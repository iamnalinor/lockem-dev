#include "defs.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <linux/input.h>

struct keymap { const char *name; int code; };
static const struct keymap KEYS[] = {
    { "KEY_POWER",      KEY_POWER },
    { "KEY_VOLUMEUP",   KEY_VOLUMEUP },
    { "KEY_VOLUMEDOWN", KEY_VOLUMEDOWN },
};
static const int NKEYS = (int)(sizeof(KEYS)/sizeof(KEYS[0]));

int key_from_name(const char *name) {
    for (int i = 0; i < NKEYS; i++)
        if (strcmp(KEYS[i].name, name) == 0) return KEYS[i].code;
    return -1;
}
const char *key_name(int code) {
    for (int i = 0; i < NKEYS; i++)
        if (KEYS[i].code == code) return KEYS[i].name;
    return "KEY_?";
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

int pattern_parse(const char *s, pattern_t *p) {
    p->n = 0;
    if (!s) return 0;
    char buf[512];
    strncpy(buf, s, sizeof(buf)-1);
    buf[sizeof(buf)-1] = 0;

    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        tok = trim(tok);
        if (!*tok) continue;
        char *colon = strchr(tok, ':');
        if (!colon) continue;
        *colon = 0;
        char *tname = trim(tok);
        char *kname = trim(colon + 1);
        tok_type tt;
        if      (!strcmp(tname, "click"))   tt = TOK_CLICK;
        else if (!strcmp(tname, "hold"))    tt = TOK_HOLD;
        else if (!strcmp(tname, "press"))   tt = TOK_PRESS;
        else if (!strcmp(tname, "unpress")) tt = TOK_UNPRESS;
        else continue;
        int kc = key_from_name(kname);
        if (kc < 0) continue;
        if (p->n >= MAX_TOKENS) break;
        p->toks[p->n].type = tt;
        p->toks[p->n].key  = kc;
        p->n++;
    }
    return p->n;
}

int pattern_to_string(const pattern_t *p, char *out, size_t outsz) {
    out[0] = 0;
    size_t used = 0;
    for (int i = 0; i < p->n; i++) {
        const char *tn =
            p->toks[i].type == TOK_CLICK   ? "click" :
            p->toks[i].type == TOK_HOLD    ? "hold" :
            p->toks[i].type == TOK_PRESS   ? "press" : "unpress";
        int w = snprintf(out + used, outsz - used, "%s%s:%s",
                         i ? "," : "", tn, key_name(p->toks[i].key));
        if (w < 0 || (size_t)w >= outsz - used) break;
        used += (size_t)w;
    }
    return (int)used;
}

static int parse_bool(const char *v) { return (v && (v[0]=='1')); }

int config_load(config_t *c) {
    memset(c, 0, sizeof(*c));
    /* defaults */
    c->strict_disable = 0;
    c->hold_ms = 500;
    c->escape_key = KEY_VOLUMEUP;
    c->escape_window_sec = 6;
    c->autolock_timeout_min = 5;

    FILE *f = fopen(CFG_FILE, "r");
    if (f) {
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            char *l = trim(line);
            if (!*l || *l == '#') continue;
            char *eq = strchr(l, '=');
            if (!eq) continue;
            *eq = 0;
            char *k = trim(l);
            char *v = trim(eq + 1);
            if      (!strcmp(k,"strict_disable"))        c->strict_disable = parse_bool(v);
            else if (!strcmp(k,"opt_vibrate"))           c->opt_vibrate = parse_bool(v);
            else if (!strcmp(k,"opt_private_space"))     c->opt_private_space = parse_bool(v);
            else if (!strcmp(k,"opt_lockdown"))          c->opt_lockdown = parse_bool(v);
            else if (!strcmp(k,"opt_disable_devopts"))   c->opt_disable_devopts = parse_bool(v);
            else if (!strcmp(k,"opt_autolock_screenoff"))c->opt_autolock_screenoff = parse_bool(v);
            else if (!strcmp(k,"opt_autolock_timeout"))  c->opt_autolock_timeout = parse_bool(v);
            else if (!strcmp(k,"autolock_timeout_min"))  c->autolock_timeout_min = atoi(v);
            else if (!strcmp(k,"hold_ms"))               c->hold_ms = atoi(v);
            else if (!strcmp(k,"escape_window_sec"))     c->escape_window_sec = atoi(v);
            else if (!strcmp(k,"escape_key")) { int kc = key_from_name(v); if (kc>=0) c->escape_key = kc; }
            else if (!strcmp(k,"lock_pattern"))          pattern_parse(v, &c->lock_pat);
            else if (!strcmp(k,"unlock_pattern"))        pattern_parse(v, &c->unlock_pat);
        }
        fclose(f);
    }
    if (c->hold_ms <= 0) c->hold_ms = 500;

    /* packages */
    FILE *pf = fopen(PKG_FILE, "r");
    if (pf) {
        char line[256];
        while (fgets(line, sizeof(line), pf) && c->npkgs < MAX_PKGS) {
            char *l = trim(line);
            if (!*l || *l == '#') continue;
            strncpy(c->pkgs[c->npkgs], l, sizeof(c->pkgs[0]) - 1);
            c->npkgs++;
        }
        fclose(pf);
    }
    return 0;
}
