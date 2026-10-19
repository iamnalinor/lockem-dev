#include "pattern.h"
#include <string.h>

#define RESET_MS 4000 /* drop a partial match after this idle gap */

static void push_exp(matcher_t *m, int key, int dir, int dur) {
    if (m->nexp >= (int)(sizeof(m->exp) / sizeof(m->exp[0]))) return;
    m->exp[m->nexp].key = key;
    m->exp[m->nexp].dir = dir;
    m->exp[m->nexp].dur = dur;
    m->nexp++;
}

void matcher_init(matcher_t *m, const pattern_t *p, int hold_ms) {
    memset(m, 0, sizeof(*m));
    m->hold_ms = hold_ms > 0 ? hold_ms : 500;
    for (int i = 0; i < p->n; i++) {
        int k = p->toks[i].key;
        switch (p->toks[i].type) {
        case TOK_CLICK:
            push_exp(m, k, 1, DUR_NONE);
            push_exp(m, k, 0, DUR_LT);
            break;
        case TOK_HOLD:
            push_exp(m, k, 1, DUR_NONE);
            push_exp(m, k, 0, DUR_GE);
            break;
        case TOK_PRESS: push_exp(m, k, 1, DUR_NONE); break;
        case TOK_UNPRESS: push_exp(m, k, 0, DUR_NONE); break;
        }
    }
    m->idx = 0;
    m->last_match = 0;
}

static int exp_matches(matcher_t *m, int ix, int key, int dir, long t) {
    if (m->exp[ix].key != key || m->exp[ix].dir != dir) return 0;
    if (dir == 0 && m->exp[ix].dur != DUR_NONE) {
        long dt = t - m->down_time[key & (KEYSPACE - 1)];
        if (m->exp[ix].dur == DUR_LT && !(dt < m->hold_ms)) return 0;
        if (m->exp[ix].dur == DUR_GE && !(dt >= m->hold_ms)) return 0;
    }
    return 1;
}

int matcher_feed(matcher_t *m, int key, int dir, long t) {
    if (m->nexp == 0) return 0;
    if (dir == 1) m->down_time[key & (KEYSPACE - 1)] = t;

    if (m->idx > 0 && (t - m->last_match) > RESET_MS) m->idx = 0;

    if (exp_matches(m, m->idx, key, dir, t)) {
        m->idx++;
        m->last_match = t;
        if (m->idx >= m->nexp) {
            m->idx = 0;
            return 1;
        }
        return 0;
    }

    /* mismatch: try to restart at 0 using the current transition */
    m->idx = 0;
    if (exp_matches(m, 0, key, dir, t)) {
        m->idx = 1;
        m->last_match = t;
        if (m->idx >= m->nexp) {
            m->idx = 0;
            return 1;
        }
    }
    return 0;
}

/* ---- recording: raw events -> token pattern ---- */

int pattern_record_normalize(const rawev_t *ev, int n, int hold_ms, pattern_t *out) {
    out->n = 0;
    if (hold_ms <= 0) hold_ms = 500;

    long last_down[KEYSPACE];
    int wrapper_open[KEYSPACE];
    memset(last_down, 0, sizeof(last_down));
    memset(wrapper_open, 0, sizeof(wrapper_open));

    for (int i = 0; i < n && out->n < MAX_TOKENS; i++) {
        int K = ev[i].key;
        int ki = K & (KEYSPACE - 1);
        if (ev[i].dir == 1) {
            last_down[ki] = ev[i].t;
            /* find this down's matching up */
            int j = -1;
            for (int k = i + 1; k < n; k++) {
                if (ev[k].key == K && ev[k].dir == 0) {
                    j = k;
                    break;
                }
            }
            int wrapper = 0;
            if (j >= 0) {
                for (int k = i + 1; k < j; k++) {
                    if (ev[k].key != K) {
                        wrapper = 1;
                        break;
                    }
                }
            }
            if (wrapper) {
                wrapper_open[ki] = 1;
                out->toks[out->n].type = TOK_PRESS;
                out->toks[out->n].key = K;
                out->n++;
            }
            /* non-wrapper: defer emit to the up event */
        } else { /* up */
            if (wrapper_open[ki]) {
                wrapper_open[ki] = 0;
                if (out->n < MAX_TOKENS) {
                    out->toks[out->n].type = TOK_UNPRESS;
                    out->toks[out->n].key = K;
                    out->n++;
                }
            } else {
                long dur = ev[i].t - last_down[ki];
                if (out->n < MAX_TOKENS) {
                    out->toks[out->n].type = (dur >= hold_ms) ? TOK_HOLD : TOK_CLICK;
                    out->toks[out->n].key = K;
                    out->n++;
                }
            }
        }
    }
    return out->n;
}
