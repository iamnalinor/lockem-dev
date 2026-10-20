#include "defs.h"
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

/* Keep the on-disk log bounded so it can never eat storage: cap size, keep one
   rotated backup (.1). logcat (tag "lockem") is bounded by Android itself. */
#define LOG_MAX_BYTES (256 * 1024)

static FILE *g_logf = NULL;
static int g_count = 0;
static int g_no_rotate = 0;

static void rotate_if_needed(void) {
    if (g_no_rotate) return;
    struct stat st;
    if (stat(LOG_FILE, &st) == 0 && st.st_size > LOG_MAX_BYTES) {
        if (g_logf) {
            fclose(g_logf);
            g_logf = NULL;
        }
        rename(LOG_FILE, LOG_FILE ".1"); /* overwrite previous backup */
        g_logf = fopen(LOG_FILE, "a");
    }
}

/* For forked helpers that share the daemon's log FILE*: never rotate, or the
   daemon would keep writing to the renamed .1 file and stop rotating. */
void log_no_rotate(void) { g_no_rotate = 1; }

void log_init(void) {
    rotate_if_needed();
    if (!g_logf) g_logf = fopen(LOG_FILE, "a");
}

void log_msg(const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

#ifdef __ANDROID__
    __android_log_write(ANDROID_LOG_INFO, "lockem", msg);
#endif

    if (!g_logf) g_logf = fopen(LOG_FILE, "a");
    if (g_logf) {
        char ts[32];
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
        fprintf(g_logf, "[%s] %s\n", ts, msg);
        fflush(g_logf);
    }

    if (++g_count >= 50) {
        g_count = 0;
        rotate_if_needed();
    }
}
