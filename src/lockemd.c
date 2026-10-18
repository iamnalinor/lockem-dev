#include "defs.h"
#include "input.h"
#include "pattern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <signal.h>
#include <sys/inotify.h>

static volatile int g_stop = 0;
static void on_sig(int s) {
    (void)s;
    g_stop = 1;
}

/* ---- config pattern write-back ---- */
static void config_write_kv(const char *key, const char *val) {
    FILE *f = fopen(CFG_FILE, "r");
    char *lines[512];
    int n = 0, replaced = 0;
    size_t klen = strlen(key);
    if (f) {
        char buf[1024];
        while (n < 512 && fgets(buf, sizeof(buf), f)) {
            if (strncmp(buf, key, klen) == 0 && buf[klen] == '=') {
                char tmp[1024];
                snprintf(tmp, sizeof(tmp), "%s=%s\n", key, val);
                lines[n++] = strdup(tmp);
                replaced = 1;
            } else {
                lines[n++] = strdup(buf);
            }
        }
        fclose(f);
    }
    if (!replaced && n < 512) {
        char tmp[1024];
        snprintf(tmp, sizeof(tmp), "%s=%s\n", key, val);
        lines[n++] = strdup(tmp);
    }
    FILE *w = fopen(CFG_FILE, "w");
    if (w) {
        for (int i = 0; i < n; i++) fputs(lines[i], w);
        fclose(w);
    }
    for (int i = 0; i < n; i++) free(lines[i]);
}

/* ---- boot-combo escape ---- */
static int boot_escape(input_t *in, const config_t *c) {
    long deadline = now_ms() + (long)c->escape_window_sec * 1000L;
    int consec = 0;
    while (now_ms() < deadline && !g_stop) {
        if (input_key_down(in, c->escape_key)) {
            if (++consec >= 3) return 1; /* ~held, not a stray blip */
        } else {
            consec = 0;
        }
        usleep(200 * 1000);
    }
    return 0;
}

/* ---- record mode ---- */
static void do_record(input_t *in, const char *cfg_key) {
    log_msg("record: start (%s) — grabbing input", cfg_key);
    input_grab(in, 1);

    rawev_t evs[MAX_REC_EVENTS];
    int n = 0, key, dir;

    /* wait up to 15s for the first press, then end after 1.5s of inactivity */
    int r = input_next(in, &key, &dir, 15000);
    while (r == 1 && n < MAX_REC_EVENTS) {
        evs[n].key = key;
        evs[n].dir = dir;
        evs[n].t = now_ms();
        n++;
        r = input_next(in, &key, &dir, 1500);
    }

    input_grab(in, 0);

    config_t tmp;
    config_load(&tmp);
    pattern_t pat;
    pattern_record_normalize(evs, n, tmp.hold_ms, &pat);

    char s[512];
    pattern_to_string(&pat, s, sizeof(s));
    const char *key_name_cfg =
        (strcmp(cfg_key, "record_lock") == 0) ? "lock_pattern" : "unlock_pattern";
    config_write_kv(key_name_cfg, s);

    FILE *rf = fopen(REC_RESULT, "w");
    if (rf) {
        fprintf(rf, "%s=%s\n", key_name_cfg, s);
        fclose(rf);
    }

    log_msg("record: captured %d events -> %s=%s", n, key_name_cfg, s);
}

/* ---- ctl handling ---- */
static void drain_ctl_and_handle(int ifd, input_t *in, config_t *c, matcher_t *ml, matcher_t *mu,
                                 int *dirty) {
    char buf[4096];
    for (;;) {
        ssize_t len = read(ifd, buf, sizeof(buf));
        if (len <= 0) break;
        for (char *p = buf; p < buf + len;) {
            struct inotify_event *e = (struct inotify_event *)p;
            if (e->len > 0) {
                const char *nm = e->name;
                if (!strcmp(nm, "reload")) {
                    *dirty = 1;
                } else if (!strcmp(nm, "lock_now")) {
                    act_lock(c);
                } else if (!strcmp(nm, "record_lock")) {
                    do_record(in, "record_lock");
                    *dirty = 1;
                } else if (!strcmp(nm, "record_unlock")) {
                    do_record(in, "record_unlock");
                    *dirty = 1;
                }
                char path[300];
                snprintf(path, sizeof(path), "%s/%s", CTL_DIR, nm);
                remove(path);
            }
            p += sizeof(struct inotify_event) + e->len;
        }
    }
    if (*dirty) {
        config_load(c);
        matcher_init(ml, &c->lock_pat, c->hold_ms);
        matcher_init(mu, &c->unlock_pat, c->hold_ms);
        *dirty = 0;
        log_msg("config reloaded (%d pkgs)", c->npkgs);
    }
}

/* ---- auto-lock housekeeping (best-effort; refine on device) ---- */
static int screen_awake(void) {
    char out[64];
    FILE *p = popen("dumpsys power 2>/dev/null | grep -o 'mWakefulness=[A-Za-z]*' | head -n1", "r");
    out[0] = 0;
    if (p) {
        if (fgets(out, sizeof(out), p)) {
        }
        pclose(p);
    }
    return strstr(out, "Awake") != NULL;
}

int main(void) {
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    signal(SIGHUP, on_sig);

    log_init();
    log_msg("lockemd starting");

    config_t cfg;
    config_load(&cfg);
    log_msg("config: %d pkgs, strict_disable=%d, hold_ms=%d", cfg.npkgs, cfg.strict_disable,
            cfg.hold_ms);

    input_t in;
    if (input_open(&in) <= 0) {
        log_msg("ERROR: no input devices with power/volume keys found");
        return 1;
    }
    log_msg("opened %d input device(s)", in.nfds);

    /* boot-combo escape */
    if (boot_escape(&in, &cfg)) {
        log_msg("boot-escape: %s held -> unlocking all", key_name(cfg.escape_key));
        act_unlock(&cfg);
    }

    int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ifd >= 0) inotify_add_watch(ifd, CTL_DIR, IN_CREATE | IN_MOVED_TO | IN_CLOSE_WRITE);

    matcher_t ml, mu;
    matcher_init(&ml, &cfg.lock_pat, cfg.hold_ms);
    matcher_init(&mu, &cfg.unlock_pat, cfg.hold_ms);

    int dirty = 0;
    int prev_awake = 1;
    long asleep_since = 0;
    int autolocked_this_sleep = 0;

    struct pollfd pfd[MAX_INPUT_FDS + 1];
    while (!g_stop) {
        int np = 0;
        for (int i = 0; i < in.nfds; i++) {
            pfd[np].fd = in.fds[i];
            pfd[np].events = POLLIN;
            pfd[np].revents = 0;
            np++;
        }
        int ifd_idx = -1;
        if (ifd >= 0) {
            pfd[np].fd = ifd;
            pfd[np].events = POLLIN;
            pfd[np].revents = 0;
            ifd_idx = np;
            np++;
        }

        int r = poll(pfd, np, 5000);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }

        long t = now_ms();

        if (r > 0) {
            for (int i = 0; i < in.nfds; i++) {
                if (!(pfd[i].revents & POLLIN)) continue;
                int key, dir;
                if (input_read_event(in.fds[i], &key, &dir)) {
                    if (matcher_feed(&ml, key, dir, t)) {
                        log_msg("lock pattern matched");
                        act_lock(&cfg);
                    }
                    if (matcher_feed(&mu, key, dir, t)) {
                        log_msg("unlock pattern matched");
                        act_unlock(&cfg);
                    }
                }
            }
            if (ifd_idx >= 0 && (pfd[ifd_idx].revents & POLLIN)) {
                drain_ctl_and_handle(ifd, &in, &cfg, &ml, &mu, &dirty);
            }
        }

        /* auto-lock housekeeping */
        if (cfg.opt_autolock_screenoff || cfg.opt_autolock_timeout) {
            int awake = screen_awake();
            if (prev_awake && !awake) { /* just went to sleep */
                asleep_since = t;
                autolocked_this_sleep = 0;
                if (cfg.opt_autolock_screenoff) {
                    log_msg("auto-lock: screen off");
                    act_lock(&cfg);
                    autolocked_this_sleep = 1;
                }
            }
            if (!awake && cfg.opt_autolock_timeout && !autolocked_this_sleep) {
                if (asleep_since && (t - asleep_since) >= (long)cfg.autolock_timeout_min * 60000L) {
                    log_msg("auto-lock: timeout");
                    act_lock(&cfg);
                    autolocked_this_sleep = 1;
                }
            }
            prev_awake = awake;
        }
    }

    log_msg("lockemd stopping");
    if (ifd >= 0) close(ifd);
    input_close(&in);
    return 0;
}
