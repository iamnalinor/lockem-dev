#ifndef LOCKEM_AUTOLOCK_H
#define LOCKEM_AUTOLOCK_H

/* Auto-lock bookkeeping for the main loop, factored out of lockemd.c so it can be
   unit-tested: when to probe the screen state (probing spawns `dumpsys power`,
   ~100+ ms, so it is rate-limited off the key-event path) and whether a probe
   result means "lock now". Times are monotonic ms (now_ms()). */

#include "defs.h"

#define AUTOLOCK_PROBE_MS 3000 /* min interval between screen-state probes */

enum { AUTOLOCK_NONE = 0, AUTOLOCK_SCREENOFF, AUTOLOCK_TIMEOUT };

typedef struct {
    int prev_awake;
    long asleep_since;
    int locked_this_sleep;
    long last_probe; /* 0 = never probed */
} autolock_t;

void autolock_init(autolock_t *a);

/* 1 if auto-lock is enabled and a screen probe is due at time t. */
int autolock_probe_due(const autolock_t *a, const config_t *c, long t);

/* poll() timeout: base_ms, shortened so the loop wakes when the next probe is due. */
int autolock_poll_timeout(const autolock_t *a, const config_t *c, long t, int base_ms);

/* Record a probe result (awake = screen on) taken at t. Returns AUTOLOCK_SCREENOFF
   or AUTOLOCK_TIMEOUT if the caller should lock now, else AUTOLOCK_NONE. */
int autolock_step(autolock_t *a, const config_t *c, int awake, long t);

#endif
