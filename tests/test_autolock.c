/* Unit tests for the main loop's auto-lock bookkeeping (autolock.c): the
   screen-probe rate limit, the poll timeout that keeps probes on schedule, and
   the screen-off / timeout lock decisions. */
#include "autolock.h"
#include "test.h"

#include <string.h>

static config_t cfg(int screenoff, int timeout, int minutes) {
    config_t c;
    memset(&c, 0, sizeof(c));
    c.opt_autolock_screenoff = screenoff;
    c.opt_autolock_timeout = timeout;
    c.autolock_timeout_min = minutes;
    return c;
}

/* ---- probe scheduling ---- */
static void test_probe_off_when_autolock_disabled(void) {
    config_t c = cfg(0, 0, 0);
    autolock_t a;
    autolock_init(&a);
    CHECK(!autolock_probe_due(&a, &c, 1000));
    CHECK(autolock_poll_timeout(&a, &c, 1000, 5000) == 5000);
}

static void test_first_probe_immediate(void) {
    config_t c = cfg(1, 0, 0);
    autolock_t a;
    autolock_init(&a);
    CHECK(autolock_probe_due(&a, &c, 1000));
    CHECK(autolock_poll_timeout(&a, &c, 1000, 5000) == 5000); /* never probed yet */
}

static void test_probe_rate_limited(void) {
    config_t c = cfg(1, 0, 0);
    autolock_t a;
    autolock_init(&a);
    autolock_step(&a, &c, 1, 1000); /* probe at t=1000 */
    /* key events right after: no dumpsys per key press */
    CHECK(!autolock_probe_due(&a, &c, 1001));
    CHECK(!autolock_probe_due(&a, &c, 1000 + AUTOLOCK_PROBE_MS - 1));
    CHECK(autolock_probe_due(&a, &c, 1000 + AUTOLOCK_PROBE_MS));
}

static void test_poll_timeout_wakes_for_next_probe(void) {
    config_t c = cfg(0, 1, 5);
    autolock_t a;
    autolock_init(&a);
    autolock_step(&a, &c, 1, 1000);
    /* a key wakeup 2s after the probe: sleep only until the next probe is due */
    CHECK(autolock_poll_timeout(&a, &c, 3000, 5000) == AUTOLOCK_PROBE_MS - 2000);
    /* overdue: don't sleep at all, never negative */
    CHECK(autolock_poll_timeout(&a, &c, 1000 + AUTOLOCK_PROBE_MS + 50, 5000) == 0);
    /* base timeout still caps it */
    CHECK(autolock_poll_timeout(&a, &c, 1000, 100) == 100);
}

/* ---- lock decisions ---- */
static void test_screenoff_locks_once_per_sleep(void) {
    config_t c = cfg(1, 0, 0);
    autolock_t a;
    autolock_init(&a);
    CHECK(autolock_step(&a, &c, 1, 0) == 0);
    CHECK(autolock_step(&a, &c, 0, 3000) == AUTOLOCK_SCREENOFF);
    CHECK(autolock_step(&a, &c, 0, 6000) == 0); /* still asleep: no repeat */
    CHECK(autolock_step(&a, &c, 1, 9000) == 0);
    CHECK(autolock_step(&a, &c, 0, 12000) == AUTOLOCK_SCREENOFF); /* next sleep */
}

static void test_screen_off_at_start_locks(void) {
    /* boot assumes awake; an already-dark screen on the first probe is a transition */
    config_t c = cfg(1, 0, 0);
    autolock_t a;
    autolock_init(&a);
    CHECK(autolock_step(&a, &c, 0, 0) == AUTOLOCK_SCREENOFF);
}

static void test_timeout_locks_after_minutes(void) {
    config_t c = cfg(0, 1, 2);
    autolock_t a;
    autolock_init(&a);
    CHECK(autolock_step(&a, &c, 1, 0) == 0);
    CHECK(autolock_step(&a, &c, 0, 1000) == 0); /* fell asleep at 1000 */
    CHECK(autolock_step(&a, &c, 0, 1000 + 2 * 60000 - 1) == 0);
    CHECK(autolock_step(&a, &c, 0, 1000 + 2 * 60000) == AUTOLOCK_TIMEOUT);
    CHECK(autolock_step(&a, &c, 0, 1000 + 3 * 60000) == 0); /* once per sleep */
}

static void test_wake_resets_timeout(void) {
    config_t c = cfg(0, 1, 1);
    autolock_t a;
    autolock_init(&a);
    autolock_step(&a, &c, 1, 0);
    autolock_step(&a, &c, 0, 1000);
    autolock_step(&a, &c, 1, 50000); /* woke before the minute was up */
    autolock_step(&a, &c, 0, 60000); /* asleep again */
    CHECK(autolock_step(&a, &c, 0, 61001) == 0);
    CHECK(autolock_step(&a, &c, 0, 120000) == AUTOLOCK_TIMEOUT);
}

static void test_screenoff_suppresses_timeout(void) {
    config_t c = cfg(1, 1, 1);
    autolock_t a;
    autolock_init(&a);
    autolock_step(&a, &c, 1, 0);
    CHECK(autolock_step(&a, &c, 0, 1000) == AUTOLOCK_SCREENOFF);
    CHECK(autolock_step(&a, &c, 0, 1000 + 60000) == 0); /* already locked this sleep */
}

int main(void) {
    RUN(test_probe_off_when_autolock_disabled);
    RUN(test_first_probe_immediate);
    RUN(test_probe_rate_limited);
    RUN(test_poll_timeout_wakes_for_next_probe);
    RUN(test_screenoff_locks_once_per_sleep);
    RUN(test_screen_off_at_start_locks);
    RUN(test_timeout_locks_after_minutes);
    RUN(test_wake_resets_timeout);
    RUN(test_screenoff_suppresses_timeout);
    TEST_SUMMARY();
    return TEST_EXIT();
}
