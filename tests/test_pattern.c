/* Unit tests for the pattern parser, matcher, and recording normalizer.
   Build: gcc -std=c11 -D_GNU_SOURCE -I ../src -fsanitize=address,undefined \
            test_pattern.c ../src/pattern.c ../src/config.c -o run && ./run   */
#include "defs.h"
#include "pattern.h"
#include "test.h"

#define HOLD 500

static int KUP, KDOWN, KPOW;

/* ---- key name mapping ---- */
static void test_keymap(void) {
    CHECK(key_from_name("KEY_VOLUMEUP") == KUP);
    CHECK(key_from_name("KEY_VOLUMEDOWN") == KDOWN);
    CHECK(key_from_name("KEY_POWER") == KPOW);
    CHECK(key_from_name("KEY_BOGUS") == -1);
    CHECK_STREQ(key_name(KUP), "KEY_VOLUMEUP");
    CHECK_STREQ(key_name(KPOW), "KEY_POWER");
}

/* ---- parse / to_string round-trip ---- */
static void test_parse_roundtrip(void) {
    pattern_t p;
    CHECK(pattern_parse("click:KEY_VOLUMEUP,hold:KEY_VOLUMEDOWN", &p) == 2);
    CHECK(p.toks[0].type == TOK_CLICK && p.toks[0].key == KUP);
    CHECK(p.toks[1].type == TOK_HOLD && p.toks[1].key == KDOWN);

    char buf[256];
    pattern_to_string(&p, buf, sizeof(buf));
    CHECK_STREQ(buf, "click:KEY_VOLUMEUP,hold:KEY_VOLUMEDOWN");

    /* re-parse the serialized form: identical */
    pattern_t q;
    pattern_parse(buf, &q);
    CHECK(q.n == p.n);
    for (int i = 0; i < q.n; i++) {
        CHECK(q.toks[i].type == p.toks[i].type);
        CHECK(q.toks[i].key == p.toks[i].key);
    }

    /* press/unpress tokens round-trip too */
    pattern_t r;
    CHECK(pattern_parse("press:KEY_VOLUMEUP,click:KEY_VOLUMEDOWN,unpress:KEY_VOLUMEUP", &r) == 3);
    pattern_to_string(&r, buf, sizeof(buf));
    CHECK_STREQ(buf, "press:KEY_VOLUMEUP,click:KEY_VOLUMEDOWN,unpress:KEY_VOLUMEUP");
}

static void test_parse_garbage(void) {
    pattern_t p;
    /* unknown token type / unknown key / missing colon are skipped */
    CHECK(pattern_parse("bogus:KEY_VOLUMEUP", &p) == 0);
    CHECK(pattern_parse("click:KEY_NOPE", &p) == 0);
    CHECK(pattern_parse("nocolon", &p) == 0);
    CHECK(pattern_parse("", &p) == 0);
    CHECK(pattern_parse(NULL, &p) == 0);
    /* whitespace tolerated */
    CHECK(pattern_parse("  click : KEY_POWER ", &p) == 1);
    CHECK(p.toks[0].type == TOK_CLICK && p.toks[0].key == KPOW);
}

/* ---- matcher: click then hold ---- */
static void test_matcher_click_hold(void) {
    pattern_t p;
    pattern_parse("click:KEY_VOLUMEUP,hold:KEY_VOLUMEDOWN", &p);
    matcher_t m;
    matcher_init(&m, &p, HOLD);

    CHECK(matcher_feed(&m, KUP, 1, 0) == 0);     /* VOLUP down */
    CHECK(matcher_feed(&m, KUP, 0, 100) == 0);   /* VOLUP up (100ms < 500 -> click ok) */
    CHECK(matcher_feed(&m, KDOWN, 1, 200) == 0); /* VOLDOWN down */
    CHECK(matcher_feed(&m, KDOWN, 0, 900) == 1); /* VOLDOWN up (700ms >= 500 -> hold) FIRES */
}

/* a too-short final up must NOT satisfy a hold */
static void test_matcher_hold_too_short(void) {
    pattern_t p;
    pattern_parse("click:KEY_VOLUMEUP,hold:KEY_VOLUMEDOWN", &p);
    matcher_t m;
    matcher_init(&m, &p, HOLD);
    CHECK(matcher_feed(&m, KUP, 1, 0) == 0);
    CHECK(matcher_feed(&m, KUP, 0, 100) == 0);
    CHECK(matcher_feed(&m, KDOWN, 1, 200) == 0);
    CHECK(matcher_feed(&m, KDOWN, 0, 300) == 0); /* only 100ms: not a hold, no fire */
}

/* a too-long up must NOT satisfy a click */
static void test_matcher_click_too_long(void) {
    pattern_t p;
    pattern_parse("click:KEY_VOLUMEUP", &p);
    matcher_t m;
    matcher_init(&m, &p, HOLD);
    CHECK(matcher_feed(&m, KUP, 1, 0) == 0);
    CHECK(matcher_feed(&m, KUP, 0, 800) == 0); /* 800ms: too long for a click */
}

/* single click fires on the up */
static void test_matcher_single_click(void) {
    pattern_t p;
    pattern_parse("click:KEY_VOLUMEUP", &p);
    matcher_t m;
    matcher_init(&m, &p, HOLD);
    CHECK(matcher_feed(&m, KUP, 1, 0) == 0);
    CHECK(matcher_feed(&m, KUP, 0, 50) == 1);
}

/* idle gap beyond RESET_MS drops a partial match */
static void test_matcher_reset(void) {
    pattern_t p;
    pattern_parse("click:KEY_VOLUMEUP,click:KEY_VOLUMEDOWN", &p);
    matcher_t m;
    matcher_init(&m, &p, HOLD);
    CHECK(matcher_feed(&m, KUP, 1, 0) == 0);
    CHECK(matcher_feed(&m, KUP, 0, 50) == 0); /* first click done, idx advanced */
    /* long gap (> RESET_MS=4000) then the second click: should have reset, no fire */
    CHECK(matcher_feed(&m, KDOWN, 1, 50 + 5000) == 0);
    CHECK(matcher_feed(&m, KDOWN, 0, 50 + 5050) == 0);

    /* but the same sequence done promptly fires */
    matcher_init(&m, &p, HOLD);
    CHECK(matcher_feed(&m, KUP, 1, 0) == 0);
    CHECK(matcher_feed(&m, KUP, 0, 50) == 0);
    CHECK(matcher_feed(&m, KDOWN, 1, 100) == 0);
    CHECK(matcher_feed(&m, KDOWN, 0, 150) == 1);
}

/* parallel recipe: press VOLUP, click VOLDOWN under it, unpress VOLUP */
static void test_matcher_parallel(void) {
    pattern_t p;
    pattern_parse("press:KEY_VOLUMEUP,click:KEY_VOLUMEDOWN,unpress:KEY_VOLUMEUP", &p);
    matcher_t m;
    matcher_init(&m, &p, HOLD);
    CHECK(matcher_feed(&m, KUP, 1, 0) == 0);     /* press VOLUP */
    CHECK(matcher_feed(&m, KDOWN, 1, 100) == 0); /* VOLDOWN down */
    CHECK(matcher_feed(&m, KDOWN, 0, 150) == 0); /* VOLDOWN up (click) */
    CHECK(matcher_feed(&m, KUP, 0, 300) == 1);   /* unpress VOLUP FIRES */
}

/* empty pattern never fires */
static void test_matcher_empty(void) {
    pattern_t p;
    p.n = 0;
    matcher_t m;
    matcher_init(&m, &p, HOLD);
    CHECK(matcher_feed(&m, KUP, 1, 0) == 0);
    CHECK(matcher_feed(&m, KUP, 0, 50) == 0);
}

/* ---- recording normalizer ---- */
static void test_normalize_clicks(void) {
    rawev_t evs[] = {
        {KUP, 1, 0},
        {KUP, 0, 50},
        {KUP, 1, 100},
        {KUP, 0, 150},
    };
    pattern_t out;
    int n = pattern_record_normalize(evs, 4, HOLD, &out);
    CHECK(n == 2);
    CHECK(out.toks[0].type == TOK_CLICK && out.toks[0].key == KUP);
    CHECK(out.toks[1].type == TOK_CLICK && out.toks[1].key == KUP);
}

static void test_normalize_hold(void) {
    rawev_t evs[] = {{KDOWN, 1, 0}, {KDOWN, 0, 700}};
    pattern_t out;
    int n = pattern_record_normalize(evs, 2, HOLD, &out);
    CHECK(n == 1);
    CHECK(out.toks[0].type == TOK_HOLD && out.toks[0].key == KDOWN);
}

static void test_normalize_parallel(void) {
    /* VOLUP held while VOLDOWN is clicked under it */
    rawev_t evs[] = {
        {KUP, 1, 0},
        {KDOWN, 1, 100},
        {KDOWN, 0, 150},
        {KUP, 0, 300},
    };
    pattern_t out;
    int n = pattern_record_normalize(evs, 4, HOLD, &out);
    CHECK(n == 3);
    CHECK(out.toks[0].type == TOK_PRESS && out.toks[0].key == KUP);
    CHECK(out.toks[1].type == TOK_CLICK && out.toks[1].key == KDOWN);
    CHECK(out.toks[2].type == TOK_UNPRESS && out.toks[2].key == KUP);
}

/* normalized recipe should match when replayed through the matcher */
static void test_normalize_then_match(void) {
    rawev_t evs[] = {
        {KUP, 1, 0},
        {KUP, 0, 50},
        {KDOWN, 1, 100},
        {KDOWN, 0, 800},
    };
    pattern_t out;
    pattern_record_normalize(evs, 4, HOLD, &out);
    CHECK(out.n == 2);
    CHECK(out.toks[0].type == TOK_CLICK);
    CHECK(out.toks[1].type == TOK_HOLD);

    matcher_t m;
    matcher_init(&m, &out, HOLD);
    CHECK(matcher_feed(&m, KUP, 1, 0) == 0);
    CHECK(matcher_feed(&m, KUP, 0, 50) == 0);
    CHECK(matcher_feed(&m, KDOWN, 1, 100) == 0);
    CHECK(matcher_feed(&m, KDOWN, 0, 800) == 1);
}

int main(void) {
    KUP = key_from_name("KEY_VOLUMEUP");
    KDOWN = key_from_name("KEY_VOLUMEDOWN");
    KPOW = key_from_name("KEY_POWER");

    RUN(test_keymap);
    RUN(test_parse_roundtrip);
    RUN(test_parse_garbage);
    RUN(test_matcher_click_hold);
    RUN(test_matcher_hold_too_short);
    RUN(test_matcher_click_too_long);
    RUN(test_matcher_single_click);
    RUN(test_matcher_reset);
    RUN(test_matcher_parallel);
    RUN(test_matcher_empty);
    RUN(test_normalize_clicks);
    RUN(test_normalize_hold);
    RUN(test_normalize_parallel);
    RUN(test_normalize_then_match);

    TEST_SUMMARY();
    return TEST_EXIT();
}
