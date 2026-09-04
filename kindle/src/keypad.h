#ifndef AIQUOTA_KEYPAD_H
#define AIQUOTA_KEYPAD_H

#include <stddef.h>
#include "ui.h"

typedef struct Keypad Keypad;

Keypad *keypad_create(Ui *ui);              /* reuses Ui geometry + fonts */
void    keypad_destroy(Keypad *kp);
/* Full keypad redraw (no_refresh; caller issues the refresh). title: "接口地址"; text: current input. */
void    keypad_draw(Keypad *kp, const char *title, const char *text);
/* on != 0 restricts the field to digits (自动刷新): '.' and ':' are drawn
 * grayed and their taps ignored. The Keypad is created once and shared by
 * both editors, so the mode is set per field before each keypad_draw. */
void    keypad_set_digits_only(Keypad *kp, int on);
/* Handle a screen-space tap. Returns 0 = keep editing, 1 = confirm, -1 = cancel.
 * Mutates text in place. Key press feedback + echo update are drawn+refreshed (WFM_DU) internally. */
int     keypad_handle_tap(Keypad *kp, int fbfd, int x, int y, char *text, size_t text_size);

#endif
