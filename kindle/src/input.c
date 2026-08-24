/* evdev touchscreen input, following OpenReader's touch_reader.c approach:
 * probe /dev/input/event* for an MT-capable device, then group events by
 * SYN_REPORT into contacts. Devices speak one of two press protocols --
 * ABS_MT_TRACKING_ID != -1 (multitouch) or BTN_TOUCH down (legacy) -- and
 * both are accepted; likewise TRACKING_ID == -1 or BTN_TOUCH up lifts. */
#define _POSIX_C_SOURCE 200112L
#define _DEFAULT_SOURCE /* open/read/ioctl under -std=c99 */

#include "input.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define INPUT_MAX_DEVICE 15 /* probe /dev/input/event0 .. event15 */

/* Raw ABS_MT ranges of the opened device, kept for input_wait_tap. */
static long g_abs_min_x;
static long g_abs_max_x;
static long g_abs_min_y;
static long g_abs_max_y;

#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define NBITS(x) ((((x)-1) / BITS_PER_LONG) + 1)

static int test_bit(int bit, const unsigned long *bits)
{
    return (int)((bits[bit / BITS_PER_LONG] >> (bit % BITS_PER_LONG)) & 1UL);
}

int input_open(void)
{
    int i;

    for (i = 0; i <= INPUT_MAX_DEVICE; i++) {
        char path[40];
        int fd;
        unsigned long ev_bits[NBITS(EV_MAX)];
        unsigned long abs_bits[NBITS(ABS_MAX)];
        struct input_absinfo abs_x;
        struct input_absinfo abs_y;

        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;

        /* The level-0 bitmap lists the event types the device supports. */
        memset(ev_bits, 0, sizeof(ev_bits));
        memset(abs_bits, 0, sizeof(abs_bits));
        if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0 ||
            !test_bit(EV_ABS, ev_bits) ||
            ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0 ||
            !test_bit(ABS_MT_POSITION_X, abs_bits) ||
            !test_bit(ABS_MT_POSITION_Y, abs_bits) ||
            ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &abs_x) < 0 ||
            ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &abs_y) < 0) {
            close(fd);
            continue;
        }

        g_abs_min_x = abs_x.minimum;
        g_abs_max_x = abs_x.maximum;
        g_abs_min_y = abs_y.minimum;
        g_abs_max_y = abs_y.maximum;
        return fd;
    }
    return -1;
}

void input_transform(const AppConfig *cfg, int scr_w, int scr_h,
                     long raw_x, long raw_y,
                     long min_x, long max_x, long min_y, long max_y,
                     int *x, int *y)
{
    long sx = 0;
    long sy = 0;

    /* Clamp raw into the reported range before scaling, so a garbage
     * coordinate cannot overflow the multiply below (and lands on an edge). */
    if (raw_x < min_x)
        raw_x = min_x;
    if (raw_x > max_x)
        raw_x = max_x;
    if (raw_y < min_y)
        raw_y = min_y;
    if (raw_y > max_y)
        raw_y = max_y;

    /* Linear map into [0, scr-1]; a zero-width range maps to 0 (no /0). */
    if (max_x > min_x)
        sx = (raw_x - min_x) * (scr_w - 1) / (max_x - min_x);
    if (max_y > min_y)
        sy = (raw_y - min_y) * (scr_h - 1) / (max_y - min_y);

    if (cfg->touch_swap_xy) {
        long tmp = sx;
        sx = sy;
        sy = tmp;
    }
    if (cfg->touch_mirror_x)
        sx = scr_w - 1 - sx;
    if (cfg->touch_mirror_y)
        sy = scr_h - 1 - sy;

    /* Out-of-range raw values clamp into the visible area. */
    if (sx < 0)
        sx = 0;
    if (sx > scr_w - 1)
        sx = scr_w - 1;
    if (sy < 0)
        sy = 0;
    if (sy > scr_h - 1)
        sy = scr_h - 1;

    *x = (int)sx;
    *y = (int)sy;
}

int input_wait_tap(int fd, int scr_w, int scr_h, const AppConfig *cfg, int *x, int *y)
{
    const long tap_threshold = (scr_w + scr_h) / 40; /* ~2.5% of screen size */

    int tracking = 0;   /* a contact is currently down */
    int ended = 0;      /* contact ended during the current frame */
    int have_pos = 0;   /* cur_x/cur_y hold a real position */
    int have_down = 0;  /* down_x/down_y hold the press position */
    int have_last = 0;  /* last_x/last_y hold the latest tracked position */
    long cur_x = 0, cur_y = 0;
    long down_x = 0, down_y = 0;
    long last_x = 0, last_y = 0;

    for (;;) {
        struct input_event ev;
        ssize_t n = read(fd, &ev, sizeof(ev));

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1; /* device went away */
        if (n != (ssize_t)sizeof(ev))
            continue; /* partial event: resync on the next one */

        if (ev.type == EV_ABS) {
            if (ev.code == ABS_MT_POSITION_X) {
                cur_x = ev.value;
                have_pos = 1;
            } else if (ev.code == ABS_MT_POSITION_Y) {
                cur_y = ev.value;
                have_pos = 1;
            } else if (ev.code == ABS_MT_TRACKING_ID) {
                /* -1 (0xFFFFFFFF) lifts the contact, any other id presses.
                 * Known edge: a lift immediately followed by a new id within
                 * the SAME frame (very quick re-touch) mixes the old and new
                 * contacts' down points, so one tap can be missed; the state
                 * self-recovers on the following touch, and real drivers
                 * separate a re-press into the next frame. */
                if (ev.value != -1) {
                    tracking = 1;
                } else if (tracking) {
                    ended = 1;
                }
            }
            /* ABS_MT_SLOT is ignored on purpose: only the first contact
             * of a multitouch stream drives the UI. */
        } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            if (ev.value == 1) {
                tracking = 1;
            } else if (ev.value == 0 && tracking) {
                ended = 1;
            }
        } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            if (tracking && have_pos) {
                if (!have_down) {
                    /* Position may arrive a frame after the press. */
                    down_x = cur_x;
                    down_y = cur_y;
                    have_down = 1;
                }
                last_x = cur_x;
                last_y = cur_y;
                have_last = 1;
            }
            if (ended) {
                long up_x = have_last ? last_x : down_x;
                long up_y = have_last ? last_y : down_y;
                long dx = up_x - down_x;
                long dy = up_y - down_y;
                if (dx < 0)
                    dx = -dx;
                if (dy < 0)
                    dy = -dy;

                if (have_down && dx + dy <= tap_threshold) {
                    input_transform(cfg, scr_w, scr_h, up_x, up_y,
                                    g_abs_min_x, g_abs_max_x,
                                    g_abs_min_y, g_abs_max_y, x, y);
                    return 0;
                }
                /* Moved too far (scroll) or no position seen: ignore the
                 * gesture and keep waiting. Drop the position too -- the
                 * next contact's press frame may carry none, and a stale
                 * cur_x/cur_y would then latch as its down point. */
                tracking = 0;
                ended = 0;
                have_down = 0;
                have_last = 0;
                have_pos = 0;
            }
        }
    }
}
