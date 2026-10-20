/* Host integration tests for the lock/unlock actions (actions.c) and log rotation.
   The Android CLIs the actions shell out to (pm, am, settings, cmd, su) are replaced
   by fake scripts on PATH that append each call to LOCKEM_DIR/calls as
   `name|arg|arg|...`, so the tests assert on the real command lines that would run.
   LOCKEM_DIR is a temp dir passed in at build time by run_tests.sh. */
#include "defs.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CALLS LOCKEM_DIR "/calls"
#define FAKEBIN LOCKEM_DIR "/fakebin"

/* One script serves every fake tool; behaviour knobs come from FAKE_* env vars. */
static const char *FAKE_SH =
    "#!/bin/sh\n"
    "n=$(basename \"$0\")\n"
    "{ printf '%s' \"$n\"; for a in \"$@\"; do printf '|%s' \"$a\"; done; echo; } >> " CALLS "\n"
    "case \"$n $*\" in\n"
    "  'cmd vibrator'*)\n"
    "    case \" $* \" in *\" $FAKE_CMD_REJECT \"*) [ -n \"$FAKE_CMD_REJECT\" ] && exit 1;; esac\n"
    "    [ -n \"$FAKE_CMD_SLEEP\" ] && sleep \"$FAKE_CMD_SLEEP\"\n"
    "    echo 'cmd-done' >> " CALLS ";;\n"
    "  'settings get'*) echo \"${FAKE_DEVOPTS:-null}\";;\n"
    "  'pm disable '*) echo \"Package $2 new state: disabled\";;\n"
    "  'pm list users'*) echo \"$FAKE_USERS\";;\n"
    "  'su 2000 -c'*) exec sh -c \"$3\";;\n"
    "esac\n"
    "exit 0\n";

static long ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void setup_fakes(void) {
    mkdir(LOCKEM_DIR, 0755);
    mkdir(FAKEBIN, 0755);
    const char *tools[] = {"pm", "am", "settings", "cmd", "su", NULL};
    for (int i = 0; tools[i]; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", FAKEBIN, tools[i]);
        FILE *f = fopen(path, "w");
        fputs(FAKE_SH, f);
        fclose(f);
        chmod(path, 0755);
    }
    char newpath[4096];
    snprintf(newpath, sizeof(newpath), "%s:%s", FAKEBIN, getenv("PATH"));
    setenv("PATH", newpath, 1);
}

static void clear_calls(void) {
    remove(CALLS);
}

static void reset(void) {
    clear_calls();
    remove(STATE_FILE);
    unsetenv("FAKE_CMD_REJECT");
    unsetenv("FAKE_CMD_SLEEP");
    unsetenv("FAKE_DEVOPTS");
    unsetenv("FAKE_USERS");
}

static char g_calls[65536];

static const char *calls(void) {
    g_calls[0] = 0;
    FILE *f = fopen(CALLS, "r");
    if (!f) return g_calls;
    size_t n = fread(g_calls, 1, sizeof(g_calls) - 1, f);
    g_calls[n] = 0;
    fclose(f);
    return g_calls;
}

/* position of a whole call line in the log, or -1 */
static long pos(const char *line) {
    const char *all = calls();
    char needle[600];
    snprintf(needle, sizeof(needle), "%s\n", line);
    for (const char *p = all; (p = strstr(p, needle)); p++)
        if (p == all || p[-1] == '\n') return p - all;
    return -1;
}

static int count(const char *line) {
    const char *all = calls();
    char needle[600];
    snprintf(needle, sizeof(needle), "%s\n", line);
    int n = 0;
    for (const char *p = all; (p = strstr(p, needle)); p++)
        if (p == all || p[-1] == '\n') n++;
    return n;
}

/* the buzz runs in a detached process: wait (bounded) for n lines to show up */
static int wait_count(const char *line, int n, int timeout_ms) {
    long end = ms() + timeout_ms;
    while (ms() < end) {
        if (count(line) >= n) return 1;
        usleep(20 * 1000);
    }
    return 0;
}

static config_t base_cfg(void) {
    config_t c;
    memset(&c, 0, sizeof(c));
    return c;
}

static void add_pkg(config_t *c, const char *pkg) {
    strcpy(c->pkgs[c->npkgs++], pkg);
}

/* ---- lock / unlock ---- */
static void test_lock_hides_and_stops_each_pkg(void) {
    reset();
    config_t c = base_cfg();
    add_pkg(&c, "com.a");
    add_pkg(&c, "com.b");
    act_lock(&c);
    CHECK(pos("pm|disable|com.a") >= 0);
    CHECK(pos("pm|hide|com.a") >= 0);
    CHECK(pos("am|force-stop|com.a") >= 0);
    CHECK(pos("pm|disable|com.b") >= 0);
    CHECK(pos("pm|hide|com.b") >= 0);
    CHECK(pos("am|force-stop|com.b") >= 0);
    CHECK(pos("pm|disable-user|--user|0|com.a") < 0); /* global disable accepted */
}

static void test_lock_skips_invalid_and_blocked_pkgs(void) {
    reset();
    config_t c = base_cfg();
    add_pkg(&c, "com.x;reboot");
    add_pkg(&c, "com.android.systemui");
    act_lock(&c);
    CHECK(strstr(calls(), "com.x") == NULL);
    CHECK(strstr(calls(), "systemui") == NULL);
}

static void test_unlock_restores_each_pkg(void) {
    reset();
    config_t c = base_cfg();
    add_pkg(&c, "com.a");
    act_unlock(&c);
    CHECK(pos("pm|unhide|com.a") >= 0);
    CHECK(pos("pm|enable|com.a") >= 0);
    CHECK(pos("pm|enable|--user|0|com.a") >= 0);
}

static void test_lockdown_and_private_space(void) {
    reset();
    setenv("FAKE_USERS",
           "Users:\n\tUserInfo{0:Owner:c13} running\n\tUserInfo{11:Private space:1410}", 1);
    config_t c = base_cfg();
    c.opt_lockdown = 1;
    c.opt_private_space = 1;
    act_lock(&c);
    CHECK(pos("am|stop-user|11") >= 0);
    CHECK(pos("am|stop-user|0") < 0);
    CHECK(pos("cmd|lock_settings|require-strong-auth|STRONG_AUTH_REQUIRED_AFTER_USER_LOCKDOWN") >=
          0);
}

/* ---- developer options ---- */
static void test_devopts_disabled_then_restored(void) {
    reset();
    setenv("FAKE_DEVOPTS", "1", 1);
    config_t c = base_cfg();
    c.opt_disable_devopts = 1;
    act_lock(&c);
    CHECK(pos("settings|put|global|development_settings_enabled|0") >= 0);
    clear_calls();
    act_unlock(&c);
    CHECK(pos("settings|put|global|development_settings_enabled|1") >= 0);
    CHECK(access(STATE_FILE, F_OK) != 0); /* state consumed */
}

static void test_devopts_second_lock_keeps_original_value(void) {
    reset();
    setenv("FAKE_DEVOPTS", "1", 1);
    config_t c = base_cfg();
    c.opt_disable_devopts = 1;
    act_lock(&c);
    setenv("FAKE_DEVOPTS", "0", 1); /* now off because we turned it off */
    act_lock(&c);
    clear_calls();
    act_unlock(&c);
    CHECK(pos("settings|put|global|development_settings_enabled|1") >= 0);
}

static void test_devopts_unset_is_deleted_on_restore(void) {
    reset();
    config_t c = base_cfg();
    c.opt_disable_devopts = 1;
    act_lock(&c); /* settings get -> "null" */
    clear_calls();
    act_unlock(&c);
    CHECK(pos("settings|delete|global|development_settings_enabled") >= 0);
}

/* ---- custom commands ---- */
static void test_custom_lock_cmd_runs_after_builtins(void) {
    reset();
    config_t c = base_cfg();
    add_pkg(&c, "com.a");
    strcpy(c.lock_cmd, "settings put custom lock");
    c.lock_cmd_as_root = 1;
    act_lock(&c);
    long custom = pos("settings|put|custom|lock");
    CHECK(custom >= 0);
    CHECK(custom > pos("am|force-stop|com.a"));
}

static void test_custom_unlock_cmd_runs_before_builtins(void) {
    reset();
    config_t c = base_cfg();
    add_pkg(&c, "com.a");
    strcpy(c.unlock_cmd, "settings put custom unlock");
    c.unlock_cmd_as_root = 1;
    act_unlock(&c);
    long custom = pos("settings|put|custom|unlock");
    CHECK(custom >= 0);
    CHECK(custom < pos("pm|unhide|com.a"));
}

static void test_custom_cmd_as_shell_goes_through_su(void) {
    reset();
    config_t c = base_cfg();
    strcpy(c.lock_cmd, "settings put custom shell");
    c.lock_cmd_as_root = 0;
    act_lock(&c);
    CHECK(pos("su|2000|-c|settings put custom shell") >= 0);
    CHECK(pos("settings|put|custom|shell") >= 0);
}

static void test_custom_cmd_quotes_survive(void) {
    reset();
    config_t c = base_cfg();
    strcpy(c.lock_cmd, "settings put q 'a b' \"it's\"");
    c.lock_cmd_as_root = 1;
    act_lock(&c);
    CHECK(pos("settings|put|q|a b|it's") >= 0);
}

/* ---- vibration ---- */
static void test_no_buzz_when_disabled(void) {
    reset();
    config_t c = base_cfg();
    act_lock(&c);
    usleep(200 * 1000);
    CHECK(strstr(calls(), "vibrator") == NULL);
}

static void test_buzz_is_one_waveform(void) {
    reset();
    config_t c = base_cfg();
    c.opt_vibrate = 1;
    act_unlock(&c);
    CHECK(wait_count("cmd-done", 1, 3000));
    CHECK(pos("cmd|vibrator_manager|synced|-f|waveform|-a|100|255|60|0|100|255") >= 0);
    CHECK(strstr(calls(), "oneshot") == NULL);
}

static void test_buzz_falls_back_to_two_oneshots(void) {
    reset();
    setenv("FAKE_CMD_REJECT", "waveform", 1);
    config_t c = base_cfg();
    c.opt_vibrate = 1;
    act_lock(&c);
    CHECK(wait_count("cmd-done", 2, 3000));
    CHECK(count("cmd|vibrator_manager|synced|-f|oneshot|100") == 2);
}

static void test_buzz_does_not_delay_the_action(void) {
    reset();
    setenv("FAKE_CMD_SLEEP", "1", 1); /* a vibrator call that blocks for 1 s */
    config_t c = base_cfg();
    c.opt_vibrate = 1;
    add_pkg(&c, "com.a");
    long t0 = ms();
    act_lock(&c);
    long took = ms() - t0;
    CHECK(took < 800);
    CHECK(pos("pm|hide|com.a") >= 0);
    CHECK(pos("cmd-done") < 0); /* buzz still in progress in the background */
    CHECK(wait_count("cmd-done", 1, 3000));
}

/* ---- log rotation ---- */
static void make_big_log(void) {
    FILE *f = fopen(LOG_FILE, "w");
    for (int i = 0; i < 300 * 1024 / 64; i++) fprintf(f, "%063d\n", i);
    fclose(f);
}

static void log_lines(int n) {
    for (int i = 0; i < n; i++) log_msg("line %d", i);
}

static int file_has(const char *path, const char *needle) {
    static char buf[512 * 1024];
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    return strstr(buf, needle) != NULL;
}

/* The buzz runs in a forked copy of the daemon that shares its log. If that copy
   rotated the log, the daemon would keep writing into lockem.log.1 forever. Walk the
   daemon's 50-line rotation counter through every value with a buzz at each step,
   then check the daemon's own lines still land in lockem.log. */
static void test_buzz_child_never_rotates_daemon_log(void) {
    reset();
    make_big_log();
    for (int i = 0; i < 200 && access(LOG_FILE ".1", F_OK) != 0; i++) log_msg("sync %d", i);
    CHECK(access(LOG_FILE ".1", F_OK) == 0); /* counter now at 0 */
    remove(LOG_FILE ".1");
    make_big_log();

    config_t c = base_cfg();
    c.opt_vibrate = 1;
    for (int i = 1; i <= 60; i++) {
        act_lock(&c);
        CHECK(wait_count("cmd-done", i, 3000));
    }
    log_msg("daemon-marker");
    CHECK(file_has(LOG_FILE, "daemon-marker"));
}

static void test_daemon_rotates_big_log(void) {
    remove(LOG_FILE ".1");
    make_big_log();
    log_lines(120);
    CHECK(access(LOG_FILE ".1", F_OK) == 0);
}

int main(void) {
    setup_fakes();
    RUN(test_lock_hides_and_stops_each_pkg);
    RUN(test_lock_skips_invalid_and_blocked_pkgs);
    RUN(test_unlock_restores_each_pkg);
    RUN(test_lockdown_and_private_space);
    RUN(test_devopts_disabled_then_restored);
    RUN(test_devopts_second_lock_keeps_original_value);
    RUN(test_devopts_unset_is_deleted_on_restore);
    RUN(test_custom_lock_cmd_runs_after_builtins);
    RUN(test_custom_unlock_cmd_runs_before_builtins);
    RUN(test_custom_cmd_as_shell_goes_through_su);
    RUN(test_custom_cmd_quotes_survive);
    RUN(test_no_buzz_when_disabled);
    RUN(test_buzz_is_one_waveform);
    RUN(test_buzz_falls_back_to_two_oneshots);
    RUN(test_buzz_does_not_delay_the_action);
    RUN(test_buzz_child_never_rotates_daemon_log);
    RUN(test_daemon_rotates_big_log);
    TEST_SUMMARY();
    return TEST_EXIT();
}
