#ifndef AIQUOTA_INPUT_H
#define AIQUOTA_INPUT_H

#include "config.h"

/* Scan /dev/input/event* via EVIOCGBIT for a device with EV_ABS + ABS_MT_POSITION_X.
 * Returns an open fd (O_RDONLY) or -1 if none found. */
int  input_open(void);

/* Blocking wait for one tap; returns 0 and fills screen-space x,y (config transform applied),
 * or -1 on error. Groups events by SYN_REPORT; tap = touch contact down->up with small movement.
 * Blocks indefinitely: returns only on a tap or a device error -- there is no timeout.
 * Uses the device ranges captured by input_open, so call input_open first, same process. */
int  input_wait_tap(int fd, int scr_w, int scr_h, const AppConfig *cfg, int *x, int *y);

/* Pure coordinate transform, exposed for unit tests:
 * scale raw evdev ABS_MT ranges into screen pixels, then apply swap/mirror from cfg. */
void input_transform(const AppConfig *cfg, int scr_w, int scr_h,
                     long raw_x, long raw_y,
                     long min_x, long max_x, long min_y, long max_y,
                     int *x, int *y);

#endif
