#ifndef LOCKEM_DEFS_H
#define LOCKEM_DEFS_H

#include <stdio.h>
#include <stdint.h>

#define LOCKEM_DIR "/data/adb/lockem"
#define CFG_FILE LOCKEM_DIR "/config"
#define PKG_FILE LOCKEM_DIR "/packages.list"
#define STATE_FILE LOCKEM_DIR "/state"
#define LOG_FILE LOCKEM_DIR "/lockem.log"
#define CTL_DIR LOCKEM_DIR "/ctl"
#define REC_RESULT LOCKEM_DIR "/record_result"

#define MAX_TOKENS 32
#define MAX_PKGS 256
#define MAX_INPUT_FDS 16
#define MAX_REC_EVENTS 256

/* pattern token types */
typedef enum { TOK_CLICK = 0, TOK_HOLD, TOK_PRESS, TOK_UNPRESS } tok_type;

typedef struct {
    tok_type type;
    int key; /* evdev key code */
} token_t;

typedef struct {
    token_t toks[MAX_TOKENS];
    int n;
} pattern_t;

typedef struct {
    /* packages */
    char pkgs[MAX_PKGS][160];
    int npkgs;

    /* axes / options */
    int strict_disable;
    int opt_vibrate;
    int opt_private_space;
    int opt_lockdown;
    int opt_disable_devopts;
    int opt_autolock_screenoff;
    int opt_autolock_timeout;
    int autolock_timeout_min;

    int hold_ms;
    int escape_key;
    int escape_window_sec;

    pattern_t lock_pat;
    pattern_t unlock_pat;
} config_t;

/* key name <-> code helpers (pattern.c) */
int key_from_name(const char *name);
const char *key_name(int code);

/* config.c */
int config_load(config_t *c);                   /* returns 0 ok */
int pattern_parse(const char *s, pattern_t *p); /* "click:KEY,hold:KEY" */
int pattern_to_string(const pattern_t *p, char *out, size_t outsz);

/* log (util.c) */
void log_init(void);
void log_msg(const char *fmt, ...);

/* actions.c */
void act_lock(const config_t *c);
void act_unlock(const config_t *c);

#endif
