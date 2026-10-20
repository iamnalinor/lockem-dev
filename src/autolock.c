#include "autolock.h"

#include <string.h>

void autolock_init(autolock_t *a) {
    memset(a, 0, sizeof(*a));
    a->prev_awake = 1; /* assume awake at start, so a dark screen is a transition */
}

static int enabled(const config_t *c) {
    return c->opt_autolock_screenoff || c->opt_autolock_timeout;
}

int autolock_probe_due(const autolock_t *a, const config_t *c, long t) {
    return enabled(c) && (a->last_probe == 0 || t - a->last_probe >= AUTOLOCK_PROBE_MS);
}

int autolock_poll_timeout(const autolock_t *a, const config_t *c, long t, int base_ms) {
    if (!enabled(c) || a->last_probe == 0) return base_ms;
    long left = AUTOLOCK_PROBE_MS - (t - a->last_probe);
    if (left < 0) left = 0;
    return left < base_ms ? (int)left : base_ms;
}

int autolock_step(autolock_t *a, const config_t *c, int awake, long t) {
    int res = AUTOLOCK_NONE;
    a->last_probe = t;
    if (a->prev_awake && !awake) { /* just went to sleep */
        a->asleep_since = t;
        a->locked_this_sleep = 0;
        if (c->opt_autolock_screenoff) {
            res = AUTOLOCK_SCREENOFF;
            a->locked_this_sleep = 1;
        }
    }
    if (!awake && c->opt_autolock_timeout && !a->locked_this_sleep && a->asleep_since &&
        (t - a->asleep_since) >= (long)c->autolock_timeout_min * 60000L) {
        res = AUTOLOCK_TIMEOUT;
        a->locked_this_sleep = 1;
    }
    a->prev_awake = awake;
    return res;
}
