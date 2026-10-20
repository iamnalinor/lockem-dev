#include "defs.h"
#include "pkgname.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>

/* Haptic feedback for lock/unlock: a double buzz (two pulses) fired at the start
   of the action so it acknowledges the combo immediately, not after the work.
   80 ms pulses were imperceptible on Pixel-class LRA actuators. */
#define VIBRATE_MS 100    /* length of one pulse */
#define VIBRATE_GAP_MS 60 /* silence between the two pulses: short, so they read as one double buzz */

/* Run a shell command, logging its combined stdout+stderr (any non-empty lines)
   and its exit status under a short label, so an action's result is visible in
   lockem.log instead of being silently discarded. Returns the raw pclose status
   (use WIFEXITED/WEXITSTATUS), or -1 on spawn failure. */
static int run_logged(const char *label, const char *fmt, ...) {
    char cmd[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);

    /* merge stderr into stdout so one pipe captures everything */
    char wrapped[4200];
    snprintf(wrapped, sizeof(wrapped), "%s 2>&1", cmd);

    FILE *p = popen(wrapped, "r");
    if (!p) {
        log_msg("%s: popen failed", label);
        return -1;
    }
    char line[512];
    while (fgets(line, sizeof(line), p)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (line[0]) log_msg("  %s | %s", label, line);
    }
    int rc = pclose(p);
    int ec = (rc != -1 && WIFEXITED(rc)) ? WEXITSTATUS(rc) : -1;
    log_msg("%s: rc=%d", label, ec);
    return rc;
}

static void run_capture(const char *cmd, char *out, size_t outsz) {
    out[0] = 0;
    FILE *p = popen(cmd, "r");
    if (!p) return;
    if (fgets(out, (int)outsz, p)) {
        size_t n = strlen(out);
        while (n && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ')) out[--n] = 0;
    }
    pclose(p);
}

/* single-quote a string for safe use in `su 2000 -c '<...>'` (' -> '\'') */
static void sh_squote(const char *in, char *out, size_t outsz) {
    size_t o = 0;
    if (o + 1 < outsz) out[o++] = '\'';
    for (const char *p = in; *p; p++) {
        if (*p == '\'') {
            if (o + 4 >= outsz) break;
            out[o++] = '\'';
            out[o++] = '\\';
            out[o++] = '\'';
            out[o++] = '\'';
        } else {
            if (o + 1 >= outsz) break;
            out[o++] = *p;
        }
    }
    if (o + 1 < outsz) out[o++] = '\'';
    out[o < outsz ? o : outsz - 1] = 0;
}

/* Max wall-clock for a user custom command. It runs synchronously on the daemon's
   single-threaded event loop, so a hung command would stall combo detection — `timeout`
   caps that (a timed-out command logs rc=124). Keep custom commands quick. */
#define CUSTOM_CMD_TIMEOUT_SEC 60

/* Run a user-configured custom command as root or dropped to the shell user
   (uid 2000, via Magisk `su`), under a timeout, logging its output. No-op when empty. */
static void run_custom(const char *label, const char *cmd, int as_root) {
    if (!cmd || !cmd[0]) return;
    char q[2100];
    sh_squote(cmd, q, sizeof(q));
    if (as_root) {
        run_logged(label, "timeout %d sh -c %s", CUSTOM_CMD_TIMEOUT_SEC, q);
    } else {
        run_logged(label, "timeout %d su 2000 -c %s", CUSTOM_CMD_TIMEOUT_SEC, q);
    }
}

static int state_has_devopts(void) {
    FILE *f = fopen(STATE_FILE, "r");
    if (!f) return 0;
    char line[128];
    int found = 0;
    while (fgets(line, sizeof(line), f))
        if (strncmp(line, "devopts_prev=", 13) == 0) {
            found = 1;
            break;
        }
    fclose(f);
    return found;
}

static void save_devopts_prev(void) {
    if (state_has_devopts()) return; /* already saved on an earlier lock */
    char cur[32];
    run_capture("settings get global development_settings_enabled 2>/dev/null", cur, sizeof(cur));
    if (!cur[0]) strcpy(cur, "null");
    FILE *f = fopen(STATE_FILE, "w");
    if (f) {
        fprintf(f, "devopts_prev=%s\n", cur);
        fclose(f);
    }
}

static void restore_devopts(void) {
    FILE *f = fopen(STATE_FILE, "r");
    if (!f) return;
    char line[128], prev[32] = "";
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "devopts_prev=", 13) == 0) {
            strncpy(prev, line + 13, sizeof(prev) - 1);
            size_t n = strlen(prev);
            while (n && (prev[n - 1] == '\n' || prev[n - 1] == '\r')) prev[--n] = 0;
        }
    }
    fclose(f);
    if (!prev[0]) return;
    if (strcmp(prev, "null") == 0)
        run_logged("restore devopts", "settings delete global development_settings_enabled");
    else
        run_logged("restore devopts", "settings put global development_settings_enabled %s", prev);
    remove(STATE_FILE);
}

static long mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int cmd_ok(int rc) { return rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 0; }

static void vibrate_now(void) {
    /* Android's vibrator CLI changed across versions and the right syntax varies by
       device, so try candidates in order and stop at the first that exits 0. `-f`
       forces the pulse past haptic-intensity settings. Each attempt's output + rc is
       logged (via run_logged), and we log which path worked (or that all failed).
       Each call is wrapped in `timeout` so a hung vibrator binary can't hang us.

       Preferred: one waveform (on/off/on as duration-amplitude pairs), so the gap is
       timed by the vibrator itself and the two pulses read as one double buzz. */
    static const char *waveforms[] = {
        "timeout 3 cmd vibrator_manager synced -f waveform -a %d 255 %d 0 %d 255", /* A12+ */
        "timeout 3 cmd vibrator_manager synced waveform -a %d 255 %d 0 %d 255",
        "timeout 3 cmd vibrator waveform -a %d 255 %d 0 %d 255", /* legacy, <=A11 */
        NULL,
    };
    for (int i = 0; waveforms[i]; i++) {
        if (cmd_ok(run_logged("vibrate", waveforms[i], VIBRATE_MS, VIBRATE_GAP_MS, VIBRATE_MS))) {
            log_msg("vibrate: ok via '%s'", waveforms[i]);
            return;
        }
    }

    /* Fallback: two oneshots. `cmd vibrator_manager` blocks until the pulse ends on
       most builds, so time the first call and only sleep what's left of pulse + gap —
       otherwise the gap doubles and the buzzes feel like two separate events. */
    static const char *oneshots[] = {
        "timeout 3 cmd vibrator_manager synced -f oneshot %d", /* A12+, forced */
        "timeout 3 cmd vibrator_manager synced oneshot %d",    /* A12+, if -f rejected */
        "timeout 3 cmd vibrator vibrate %d",                   /* legacy service, <=A11 */
        "timeout 3 cmd vibrator_manager vibrate %d",           /* old form, likely dead */
        NULL,
    };
    for (int i = 0; oneshots[i]; i++) {
        long t0 = mono_ms();
        if (cmd_ok(run_logged("vibrate", oneshots[i], VIBRATE_MS))) {
            long spent = mono_ms() - t0;
            long wait = spent >= VIBRATE_MS ? VIBRATE_GAP_MS : VIBRATE_MS + VIBRATE_GAP_MS - spent;
            usleep(wait * 1000);
            run_logged("vibrate", oneshots[i], VIBRATE_MS);
            log_msg("vibrate: ok via '%s'", oneshots[i]);
            return;
        }
    }
    log_msg("vibrate: all methods failed");
}

/* Fire the buzz in a detached grandchild so it starts the instant the combo matches
   and the lock/unlock work runs in parallel instead of waiting ~0.5 s for it. Double
   fork so init reaps it: the daemon must not ignore SIGCHLD (pclose needs waitpid).
   Falls back to vibrating inline if fork fails. Two quick combos may overlap two
   buzz children; the vibrator service cancels the older vibration, so that's fine. */
static void vibrate(void) {
    pid_t pid = fork();
    if (pid < 0) {
        vibrate_now();
        return;
    }
    if (pid == 0) {
        log_no_rotate();
        /* grandchild buzzes; if that fork fails, buzz here (the parent waits) */
        if (fork() <= 0) vibrate_now();
        _exit(0);
    }
    waitpid(pid, NULL, 0);
}

static void stop_private_space(void) {
    /* find the Private space user id and stop (lock) it */
    run_logged("stop private space",
               "for id in $(pm list users | awk -F'[{:]' '/[Pp]rivate/{print $2}'); do "
               "am stop-user \"$id\"; done");
}

static void do_lockdown(void) {
    /* Real user-lockdown: set the strong-auth flag so the next unlock requires
       PIN/pattern and biometrics are blocked (the point of lockdown). This does NOT
       touch the display — the device locks on its own and an already-off screen is
       never woken. The flag clears on the next successful PIN/pattern auth. (No
       `--user 0`: the subcommand ignores it and applies to the lockdown context
       regardless — verified on-device it logged USER_ID -1 with the arg present.) */
    run_logged("lockdown",
               "cmd lock_settings require-strong-auth STRONG_AUTH_REQUIRED_AFTER_USER_LOCKDOWN");
}

static void lock_one(const config_t *c, const char *pkg) {
    if (!pkg_valid(pkg)) {
        log_msg("skip invalid pkg name");
        return;
    }
    if (pkg_blocked(pkg)) {
        log_msg("skip blocked pkg: %s", pkg);
        return;
    }

    /* Global `pm disable` so the app disappears from Settings → Apps entirely
       (disable-user leaves it listed as "Disabled" and re-enablable). Fall back to
       per-user disable-user if a build rejects the global disable. */
    char out[256];
    char cmd[320];
    snprintf(cmd, sizeof(cmd), "pm disable %s 2>&1", pkg);
    run_capture(cmd, out, sizeof(out));
    log_msg("  pm disable %s | %s", pkg, out[0] ? out : "(no output)");
    if (strstr(out, "Shell cannot") || strstr(out, "Exception") || strstr(out, "Error") ||
        strstr(out, "cannot be") || out[0] == 0) {
        run_logged("pm disable-user", "pm disable-user --user 0 %s", pkg);
    }
    run_logged("pm hide", "pm hide %s", pkg);
    run_logged("am force-stop", "am force-stop %s", pkg); /* also clears the recents card */
    log_msg("locked: %s", pkg);
}

static void unlock_one(const char *pkg) {
    if (!pkg_valid(pkg)) {
        log_msg("skip invalid pkg name");
        return;
    }
    run_logged("pm unhide", "pm unhide %s", pkg);
    run_logged("pm enable", "pm enable %s", pkg);
    run_logged("pm enable --user 0", "pm enable --user 0 %s", pkg);
    log_msg("unlocked: %s", pkg);
}

void act_lock(const config_t *c) {
    log_msg("LOCK (%d pkgs)", c->npkgs);
    if (c->opt_vibrate) vibrate(); /* acknowledge the combo right away */
    if (c->opt_disable_devopts) save_devopts_prev();
    for (int i = 0; i < c->npkgs; i++) lock_one(c, c->pkgs[i]);
    if (c->opt_disable_devopts)
        run_logged("disable devopts", "settings put global development_settings_enabled 0");
    if (c->opt_private_space) stop_private_space();
    if (c->opt_lockdown) do_lockdown();
    /* custom command wraps the built-ins: on lock it runs last */
    run_custom("custom lock cmd", c->lock_cmd, c->lock_cmd_as_root);
}

void act_unlock(const config_t *c) {
    log_msg("UNLOCK (%d pkgs)", c->npkgs);
    if (c->opt_vibrate) vibrate(); /* acknowledge the combo right away */
    /* custom command wraps the built-ins: on unlock it runs first */
    run_custom("custom unlock cmd", c->unlock_cmd, c->unlock_cmd_as_root);
    for (int i = 0; i < c->npkgs; i++) unlock_one(c->pkgs[i]);
    if (c->opt_disable_devopts) restore_devopts();
}
