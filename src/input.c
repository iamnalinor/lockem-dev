#include "input.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#define BITS_PER_LONG (sizeof(long) * 8)
#define NBITS(x) ((((x) - 1) / BITS_PER_LONG) + 1)

static int test_bit(int bit, const unsigned long *arr) {
    return (arr[bit / BITS_PER_LONG] >> (bit % BITS_PER_LONG)) & 1UL;
}

long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int dev_has_our_keys(int fd) {
    unsigned long evbit[NBITS(EV_MAX)];
    memset(evbit, 0, sizeof(evbit));
    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit) < 0) return 0;
    if (!test_bit(EV_KEY, evbit)) return 0;

    unsigned long keybit[NBITS(KEY_MAX)];
    memset(keybit, 0, sizeof(keybit));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit) < 0) return 0;

    return test_bit(KEY_POWER, keybit) || test_bit(KEY_VOLUMEUP, keybit) ||
           test_bit(KEY_VOLUMEDOWN, keybit);
}

int input_open(input_t *in) {
    in->nfds = 0;
    DIR *d = opendir("/dev/input");
    if (!d) return -1;
    struct dirent *de;
    while ((de = readdir(d)) && in->nfds < MAX_INPUT_FDS) {
        if (strncmp(de->d_name, "event", 5) != 0) continue;
        char path[300];
        snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        if (dev_has_our_keys(fd)) {
            in->fds[in->nfds++] = fd;
        } else {
            close(fd);
        }
    }
    closedir(d);
    return in->nfds;
}

void input_close(input_t *in) {
    for (int i = 0; i < in->nfds; i++) close(in->fds[i]);
    in->nfds = 0;
}

int input_grab(input_t *in, int grab) {
    int ok = 0;
    for (int i = 0; i < in->nfds; i++) {
        if (ioctl(in->fds[i], EVIOCGRAB, grab ? 1 : 0) == 0) ok++;
    }
    return ok;
}

int input_key_down(input_t *in, int key) {
    unsigned long keys[NBITS(KEY_MAX)];
    for (int i = 0; i < in->nfds; i++) {
        memset(keys, 0, sizeof(keys));
        if (ioctl(in->fds[i], EVIOCGKEY(sizeof(keys)), keys) < 0) continue;
        if (test_bit(key, keys)) return 1;
    }
    return 0;
}

static int is_our_key(int code) {
    return code == KEY_POWER || code == KEY_VOLUMEUP || code == KEY_VOLUMEDOWN;
}

int input_read_event(int fd, int *key, int *dir) {
    struct input_event ev;
    ssize_t n = read(fd, &ev, sizeof(ev));
    if (n != (ssize_t)sizeof(ev)) return 0;
    if (ev.type != EV_KEY) return 0;
    if (ev.value != 0 && ev.value != 1) return 0;
    if (!is_our_key(ev.code)) return 0;
    *key = ev.code;
    *dir = ev.value;
    return 1;
}

int input_next(input_t *in, int *key, int *dir, int timeout_ms) {
    long deadline = (timeout_ms < 0) ? -1 : now_ms() + timeout_ms;
    struct pollfd pfd[MAX_INPUT_FDS];

    for (;;) {
        for (int i = 0; i < in->nfds; i++) {
            pfd[i].fd = in->fds[i];
            pfd[i].events = POLLIN;
            pfd[i].revents = 0;
        }
        int to = -1;
        if (deadline >= 0) {
            to = (int)(deadline - now_ms());
            if (to < 0) to = 0;
        }
        int r = poll(pfd, in->nfds, to);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return 0; /* timeout */

        for (int i = 0; i < in->nfds; i++) {
            if (!(pfd[i].revents & POLLIN)) continue;
            struct input_event ev;
            ssize_t n = read(in->fds[i], &ev, sizeof(ev));
            if (n != (ssize_t)sizeof(ev)) continue;
            if (ev.type != EV_KEY) continue;
            if (ev.value != 0 && ev.value != 1) continue; /* ignore autorepeat (2) */
            if (!is_our_key(ev.code)) continue;
            *key = ev.code;
            *dir = ev.value; /* 1 down, 0 up */
            return 1;
        }
        /* readable but nothing relevant -> loop (respecting deadline) */
        if (deadline >= 0 && now_ms() >= deadline) return 0;
    }
}
