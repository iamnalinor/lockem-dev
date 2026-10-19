/* Unit tests for config key=value parsing (config_apply_kv). */
#include "defs.h"
#include "test.h"

#include <string.h>

static void test_bools_and_ints(void) {
    config_t c;
    memset(&c, 0, sizeof(c));
    config_apply_kv(&c, "opt_vibrate", "1");
    config_apply_kv(&c, "opt_lockdown", "0");
    config_apply_kv(&c, "hold_ms", "750");
    config_apply_kv(&c, "autolock_timeout_min", "9");
    CHECK(c.opt_vibrate == 1);
    CHECK(c.opt_lockdown == 0);
    CHECK(c.hold_ms == 750);
    CHECK(c.autolock_timeout_min == 9);
}

static void test_escape_key(void) {
    config_t c;
    memset(&c, 0, sizeof(c));
    c.escape_key = key_from_name("KEY_VOLUMEUP");
    config_apply_kv(&c, "escape_key", "KEY_VOLUMEDOWN");
    CHECK(c.escape_key == key_from_name("KEY_VOLUMEDOWN"));
    /* bad key name leaves the previous value untouched */
    config_apply_kv(&c, "escape_key", "KEY_NONSENSE");
    CHECK(c.escape_key == key_from_name("KEY_VOLUMEDOWN"));
}

static void test_custom_command(void) {
    config_t c;
    memset(&c, 0, sizeof(c));
    config_apply_kv(&c, "lock_cmd", "input keyevent 26");
    config_apply_kv(&c, "unlock_cmd", "echo hi");
    config_apply_kv(&c, "cmd_user", "root");
    config_apply_kv(&c, "cmd_timing", "before");
    CHECK_STREQ(c.lock_cmd, "input keyevent 26");
    CHECK_STREQ(c.unlock_cmd, "echo hi");
    CHECK(c.cmd_as_root == 1);
    CHECK(c.cmd_before == 1);

    /* the enums default to shell/after for anything that is not root/before */
    config_t d;
    memset(&d, 0, sizeof(d));
    config_apply_kv(&d, "cmd_user", "shell");
    config_apply_kv(&d, "cmd_timing", "after");
    CHECK(d.cmd_as_root == 0);
    CHECK(d.cmd_before == 0);
}

static void test_cmd_bounded(void) {
    config_t c;
    memset(&c, 0, sizeof(c));
    char big[700];
    memset(big, 'x', sizeof(big));
    big[sizeof(big) - 1] = 0;
    config_apply_kv(&c, "lock_cmd", big);
    CHECK(strlen(c.lock_cmd) == sizeof(c.lock_cmd) - 1); /* truncated, not overflowed */
    CHECK(c.lock_cmd[sizeof(c.lock_cmd) - 1] == 0);      /* still NUL-terminated */
}

static void test_unknown_key_ignored(void) {
    config_t c;
    memset(&c, 0, sizeof(c));
    config_apply_kv(&c, "bogus_key", "whatever");
    CHECK(c.hold_ms == 0);
    CHECK(c.lock_cmd[0] == 0);
}

int main(void) {
    RUN(test_bools_and_ints);
    RUN(test_escape_key);
    RUN(test_custom_command);
    RUN(test_cmd_bounded);
    RUN(test_unknown_key_ignored);
    TEST_SUMMARY();
    return TEST_EXIT();
}
