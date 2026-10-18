#ifndef LOCKEM_INPUT_H
#define LOCKEM_INPUT_H
#include "defs.h"

typedef struct {
    int fds[MAX_INPUT_FDS];
    int nfds;
} input_t;

int  input_open(input_t *in);              /* open all key devices; returns nfds (<0 err) */
void input_close(input_t *in);
int  input_grab(input_t *in, int grab);    /* EVIOCGRAB all; 1=grab 0=release */
int  input_key_down(input_t *in, int key); /* EVIOCGKEY: 1 if currently pressed */

/* Read one key transition. *key = evdev code, *dir = 1 down / 0 up.
   Returns 1 got a transition, 0 timeout, -1 error. timeout_ms < 0 blocks. */
int  input_next(input_t *in, int *key, int *dir, int timeout_ms);

/* Read one event from a specific fd (already known readable). Returns 1 if it was a
   relevant key transition (fills *key/*dir), 0 otherwise. */
int  input_read_event(int fd, int *key, int *dir);

long now_ms(void);
#endif
