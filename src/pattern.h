#ifndef LOCKEM_PATTERN_H
#define LOCKEM_PATTERN_H
#include "defs.h"

#define KEYSPACE 1024   /* power of two; covers KEY_MAX (0x2ff) with a safe index mask */

/* duration constraint on an "up" expected-transition */
enum { DUR_NONE = 0, DUR_LT = 1, DUR_GE = 2 };

typedef struct {
    struct { int key; int dir; int dur; } exp[MAX_TOKENS * 2];
    int  nexp;
    int  idx;
    int  hold_ms;
    long down_time[KEYSPACE];
    long last_match;
} matcher_t;

void matcher_init(matcher_t *m, const pattern_t *p, int hold_ms);
/* feed one raw transition; returns 1 when the full pattern just matched */
int  matcher_feed(matcher_t *m, int key, int dir, long t);

/* raw event for recording */
typedef struct { int key; int dir; long t; } rawev_t;

/* normalize a recorded raw-event buffer into a token pattern */
int pattern_record_normalize(const rawev_t *ev, int n, int hold_ms, pattern_t *out);

#endif
