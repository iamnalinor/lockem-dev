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
    /* write to a temp file and rename, so a crash mid-write can't truncate config */
    FILE *w = fopen(CFG_FILE ".tmp", "w");
    if (w) {
        for (int i = 0; i < n; i++) fputs(lines[i], w);
        fclose(w);
        rename(CFG_FILE ".tmp", CFG_FILE);
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

/* Write the record response atomically (temp + rename) so the WebUI never reads a
   half-written file. Echoes the request id (so the WebUI can match its own request)
   and a status: "ok" (captured; kv_line carries the pattern) or "empty" (nothing
   captured — config was NOT touched). id may be NULL for a CLI-initiated record. */
static void write_rec_result(const char *id, const char *status, const char *kv_line) {
    FILE *rf = fopen(REC_RESULT ".tmp", "w");
    if (!rf) return;
    if (id && id[0]) fprintf(rf, "id=%s\n", id);
    fprintf(rf, "status=%s\n", status);
    if (kv_line && kv_line[0]) fprintf(rf, "%s\n", kv_line);
    fclose(rf);
    rename(REC_RESULT ".tmp", REC_RESULT);
}

/* Called when the ctl inotify fd is readable during a record grab: drain the pending
   events and return 1 iff a cancel request (action=cancel, matching our id when both
   are present) has been written by the WebUI's "Stop" button. */
static int record_cancel_requested(int cfd, const char *id) {
    char buf[4096];
    ssize_t len = read(cfd, buf, sizeof(buf));
    if (len <= 0) return 0;
    int saw_request = 0;
    for (char *p = buf; p < buf + len;) {
        struct inotify_event *e = (struct inotify_event *)p;
        if (e->len > 0 && !strcmp(e->name, "request")) saw_request = 1;
        p += sizeof(struct inotify_event) + e->len;
    }
    if (!saw_request) return 0;

    char path[300];
    snprintf(path, sizeof(path), "%s/request", CTL_DIR);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256], action[32] = "", rid[64] = "";
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (!strncmp(line, "action=", 7))
            strncpy(action, line + 7, sizeof(action) - 1);
        else if (!strncmp(line, "id=", 3))
            strncpy(rid, line + 3, sizeof(rid) - 1);
    }
    fclose(f);
    if (strcmp(action, "cancel") != 0) return 0;
    if (id && id[0] && rid[0] && strcmp(id, rid) != 0) return 0; /* different session */
    return 1;
}

/* action is "record_lock" or "record_unlock"; id is the WebUI request id (or NULL).
   Grabs input and captures a pattern, while also watching the ctl inotify fd so an
   `action=cancel` request can abort the grab mid-recording (the WebUI "Stop" button). */
static void do_record(input_t *in, const char *action, const char *id) {
    log_msg("record: start (%s) — grabbing input", action);
    input_grab(in, 1);

    /* Private inotify fd for cancel detection, separate from the main loop's fd:
       events consumed here never rob the main loop of lock_now/reload/record signals
       that happen to arrive during the grab. */
    int cfd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (cfd >= 0) inotify_add_watch(cfd, CTL_DIR, IN_MOVED_TO | IN_CLOSE_WRITE);

    rawev_t evs[MAX_REC_EVENTS];
    int n = 0;
    int cancelled = 0;

    /* wait up to 15s for the first press, then end after 1.5s of inactivity */
    long deadline = now_ms() + 15000;
    struct pollfd pfd[MAX_INPUT_FDS + 1];

    while (n < MAX_REC_EVENTS && !g_stop) {
        int np = 0;
        for (int i = 0; i < in->nfds; i++) {
            pfd[np].fd = in->fds[i];
            pfd[np].events = POLLIN;
            pfd[np].revents = 0;
            np++;
        }
        int ctl_idx = -1;
        if (cfd >= 0) {
            pfd[np].fd = cfd;
            pfd[np].events = POLLIN;
            pfd[np].revents = 0;
            ctl_idx = np;
            np++;
        }

        int to = (int)(deadline - now_ms());
        if (to < 0) to = 0;
        int r = poll(pfd, np, to);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) break; /* first-press or inactivity timeout */

        if (ctl_idx >= 0 && (pfd[ctl_idx].revents & POLLIN)) {
            if (record_cancel_requested(cfd, id)) {
                cancelled = 1;
                break;
            }
        }

        int got = 0;
        for (int i = 0; i < in->nfds; i++) {
            if (!(pfd[i].revents & POLLIN)) continue;
            int key, dir;
            if (input_read_event(in->fds[i], &key, &dir)) {
                evs[n].key = key;
                evs[n].dir = dir;
                evs[n].t = now_ms();
                n++;
                got = 1;
                if (n >= MAX_REC_EVENTS) break;
            }
        }
        if (got) deadline = now_ms() + 1500; /* reset inactivity window after each event */
    }

    input_grab(in, 0);
    if (cfd >= 0) close(cfd);

    if (cancelled) {
        write_rec_result(id, "cancelled", NULL);
        log_msg("record: cancelled (%s) -> kept previous pattern", action);
        return;
    }

    config_t tmp;
    config_load(&tmp);
    pattern_t pat;
    pattern_record_normalize(evs, n, tmp.hold_ms, &pat);

    const char *key_name_cfg =
        (strcmp(action, "record_lock") == 0) ? "lock_pattern" : "unlock_pattern";

    /* Nothing captured: do NOT clobber an existing good pattern with an empty one. */
    if (pat.n == 0) {
        write_rec_result(id, "empty", NULL);
        log_msg("record: captured 0 events (%s) -> kept previous pattern", action);
        return;
    }

    char s[512];
    pattern_to_string(&pat, s, sizeof(s));
    config_write_kv(key_name_cfg, s);

    char kv[600];
    snprintf(kv, sizeof(kv), "%s=%s", key_name_cfg, s);
    write_rec_result(id, "ok", kv);

    log_msg("record: captured %d events -> %s=%s", n, key_name_cfg, s);
}

/* Read + parse the WebUI's atomic request file (content: id=…, action=…), dedup by id,
   and dispatch. Keeps a one-shot dedup so a stray re-delivery can't double-record. */
static void handle_request(input_t *in, int *dirty) {
    static char last_id[64] = "";

    char path[300];
    snprintf(path, sizeof(path), "%s/request", CTL_DIR);
    FILE *f = fopen(path, "r");
    if (!f) return;

    char id[64] = "", action[32] = "", line[256];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (!strncmp(line, "id=", 3)) {
            strncpy(id, line + 3, sizeof(id) - 1);
        } else if (!strncmp(line, "action=", 7)) {
            strncpy(action, line + 7, sizeof(action) - 1);
        }
    }
    fclose(f);

    if (!action[0]) return;
    if (id[0] && strcmp(id, last_id) == 0) return; /* dedup repeats */
    if (id[0]) {
        strncpy(last_id, id, sizeof(last_id) - 1);
        last_id[sizeof(last_id) - 1] = 0;
    }

    if (!strcmp(action, "record_lock") || !strcmp(action, "record_unlock")) {
        do_record(in, action, id[0] ? id : NULL);
        *dirty = 1;
    }
    /* action=cancel arriving outside a grab is a no-op (handled inline in do_record). */
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
                int handled = 1;
                if (!strcmp(nm, "reload")) {
                    *dirty = 1;
                } else if (!strcmp(nm, "lock_now")) {
                    act_lock(c);
                } else if (!strcmp(nm, "request")) {
                    /* WebUI record handshake: content carries id + action */
                    handle_request(in, dirty);
                } else if (!strcmp(nm, "record_lock")) {
                    do_record(in, "record_lock", NULL);
                    *dirty = 1;
                } else if (!strcmp(nm, "record_unlock")) {
                    do_record(in, "record_unlock", NULL);
                    *dirty = 1;
                } else {
                    handled = 0;
                }
                /* Consume (delete) only recognized signal files. Never remove an
                   unrecognized name — in particular the WebUI's `request.tmp`, which
                   gets an IN_CLOSE_WRITE before its `mv` into place: deleting it here
                   would race the rename and silently break recording. */
                if (handled) {
                    char path[300];
                    snprintf(path, sizeof(path), "%s/%s", CTL_DIR, nm);
                    remove(path);
                }
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
    log_msg("config: %d pkgs, hold_ms=%d", cfg.npkgs, cfg.hold_ms);

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
    /* IN_CLOSE_WRITE catches `: > ctl/<name>` (CLI name-signals); IN_MOVED_TO catches the
       WebUI's atomic rename of the `request` file. We deliberately do NOT watch IN_CREATE:
       a single `: >` fires BOTH create and close-write, which used to run each signal twice
       (e.g. a stray 15s record grab that overwrote a good pattern). */
    if (ifd >= 0) inotify_add_watch(ifd, CTL_DIR, IN_MOVED_TO | IN_CLOSE_WRITE);

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
