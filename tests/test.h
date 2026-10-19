#ifndef LOCKEM_TEST_H
#define LOCKEM_TEST_H
/* Minimal assert-based test harness (no deps). Each test is a function that
   calls CHECK(...); the runner counts failures and sets the exit code. */
#include <stdio.h>
#include <string.h>

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        g_checks++;                                                                                \
        if (!(cond)) {                                                                             \
            g_fails++;                                                                             \
            fprintf(stderr, "  FAIL %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond);               \
        }                                                                                          \
    } while (0)

#define CHECK_STREQ(a, b)                                                                          \
    do {                                                                                           \
        g_checks++;                                                                                \
        if (strcmp((a), (b)) != 0) {                                                               \
            g_fails++;                                                                             \
            fprintf(stderr, "  FAIL %s:%d  \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b));     \
        }                                                                                          \
    } while (0)

#define RUN(fn)                                                                                    \
    do {                                                                                           \
        fprintf(stderr, "- %s\n", #fn);                                                            \
        fn();                                                                                      \
    } while (0)

#define TEST_SUMMARY()                                                                             \
    do {                                                                                           \
        fprintf(stderr, "\n%s: %d checks, %d failures\n", g_fails ? "FAILED" : "PASSED", g_checks, \
                g_fails);                                                                          \
    } while (0)

#define TEST_EXIT() (g_fails ? 1 : 0)

#endif
