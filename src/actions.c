#include "defs.h"
#include "pkgname.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/wait.h>

/* Haptic pulse length for lock/unlock feedback (ms). 80 ms was imperceptible on
   Pixel-class LRA actuators. */
#define VIBRATE_MS 200

static int run(const char *fmt, ...) {
    char cmd[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    int rc = system(cmd);
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
        run("settings delete global development_settings_enabled >/dev/null 2>&1");
    else
        run("settings put global development_settings_enabled %s >/dev/null 2>&1", prev);
    remove(STATE_FILE);
}

static void vibrate(void) {
    /* Android's vibrator CLI changed across versions and the right syntax varies by
       device, so try candidates in order and stop at the first that exits 0. `-f`
       forces the pulse past haptic-intensity settings. Log which path worked (or that
       all failed) so a silent no-op on another device is visible in the log. */
    static const char *cmds[] = {
        "cmd vibrator_manager synced -f oneshot %d >/dev/null 2>&1", /* A12+, forced */
        "cmd vibrator_manager synced oneshot %d >/dev/null 2>&1",    /* A12+, if -f rejected */
        "cmd vibrator vibrate %d >/dev/null 2>&1",                   /* legacy service, <=A11 */
        "cmd vibrator_manager vibrate %d >/dev/null 2>&1",           /* old form, likely dead */
        NULL,
    };
    for (int i = 0; cmds[i]; i++) {
        int rc = run(cmds[i], VIBRATE_MS);
        if (rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 0) {
            log_msg("vibrate: ok via '%s'", cmds[i]);
            return;
        }
    }
    log_msg("vibrate: all methods failed");
}

static void stop_private_space(void) {
    /* find the Private space user id and stop (lock) it */
    run("for id in $(pm list users | awk -F'[{:]' '/[Pp]rivate/{print $2}'); do "
        "am stop-user \"$id\" >/dev/null 2>&1; done");
}

static void do_lockdown(void) {
    /* Real user-lockdown: set the strong-auth flag so the next unlock requires
       PIN/pattern and biometrics are blocked (the point of lockdown). This does NOT
       touch the display — the device locks on its own and an already-off screen is
       never woken. The flag clears on the next successful PIN/pattern auth. */
    run("cmd lock_settings require-strong-auth STRONG_AUTH_REQUIRED_AFTER_USER_LOCKDOWN "
        "--user 0 >/dev/null 2>&1");
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

    if (c->strict_disable) {
        char out[256];
        char cmd[320];
        snprintf(cmd, sizeof(cmd), "pm disable %s 2>&1", pkg);
        run_capture(cmd, out, sizeof(out));
        /* shell/user builds reject global disable -> fall back */
        if (strstr(out, "Shell cannot") || strstr(out, "Exception") || strstr(out, "Error") ||
            strstr(out, "cannot be") || out[0] == 0) {
            run("pm disable-user --user 0 %s >/dev/null 2>&1", pkg);
        }
    } else {
        run("pm disable-user --user 0 %s >/dev/null 2>&1", pkg);
    }
    run("pm hide %s >/dev/null 2>&1", pkg);
    run("am force-stop %s >/dev/null 2>&1", pkg); /* also clears the recents card */
    log_msg("locked: %s", pkg);
}

static void unlock_one(const char *pkg) {
    if (!pkg_valid(pkg)) {
        log_msg("skip invalid pkg name");
        return;
    }
    run("pm unhide %s >/dev/null 2>&1", pkg);
    run("pm enable %s >/dev/null 2>&1", pkg);
    run("pm enable --user 0 %s >/dev/null 2>&1", pkg);
    log_msg("unlocked: %s", pkg);
}

void act_lock(const config_t *c) {
    log_msg("LOCK (%d pkgs)", c->npkgs);
    if (c->opt_disable_devopts) save_devopts_prev();
    for (int i = 0; i < c->npkgs; i++) lock_one(c, c->pkgs[i]);
    if (c->opt_disable_devopts)
        run("settings put global development_settings_enabled 0 >/dev/null 2>&1");
    if (c->opt_private_space) stop_private_space();
    if (c->opt_lockdown) do_lockdown();
    if (c->opt_vibrate) vibrate();
}

void act_unlock(const config_t *c) {
    log_msg("UNLOCK (%d pkgs)", c->npkgs);
    for (int i = 0; i < c->npkgs; i++) unlock_one(c->pkgs[i]);
    if (c->opt_disable_devopts) restore_devopts();
    if (c->opt_vibrate) vibrate();
}
