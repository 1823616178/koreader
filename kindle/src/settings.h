#ifndef AIQUOTA_SETTINGS_H
#define AIQUOTA_SETTINGS_H

#include "config.h"
#include "fbink.h"
#include "ui.h"

/* In-app settings page, same full-screen modal pattern as keypad.c (the system
 * UI is frozen, so every control is ours): four tappable rows over a Keypad
 * for the numeric/url fields, plus a 返回 button. Rows map 1:1 onto AppConfig
 * keys; edits land in a working copy and only reach the real config through
 * settings_commit(), so 取消-style abandonment stays trivial. */

typedef struct Settings Settings;

typedef enum {
    SET_HIT_NONE = 0,   /* consumed here (toggle flip / stray tap) */
    SET_HIT_BACK,       /* leave the page: caller commits + saves */
    SET_HIT_EDIT_URL,   /* open the shared Keypad on this field */
    SET_HIT_EDIT_REFRESH,
    SET_HIT_EDIT_IDLE
} SettingsHit;

/* Takes an internal snapshot of cfg as both the working copy and the
 * dirty-comparison baseline. Returns NULL on OOM. */
Settings *settings_create(Ui *ui, const AppConfig *cfg);
void settings_destroy(Settings *s);

/* Full-screen paint, everything batched with no_refresh; the caller issues
 * one GC16 refresh afterwards (keypad_draw contract). */
void settings_draw(Settings *s);

/* Hit-test one screen-space tap. Toggling WiFi auto-off flips the value and
 * repaints its row with a DU refresh before returning SET_HIT_NONE; editor
 * rows flash their feedback and hand control back to the caller. */
SettingsHit settings_handle_tap(Settings *s, int fbfd, int x, int y);

/* Current values, e.g. to seed the keypad's echo line. */
const char *settings_url(const Settings *s);
int         settings_refresh_min(const Settings *s);
int         settings_idle_s(const Settings *s);

/* Store one edited field (caller validates/clamps first). Returns 1 when the
 * stored value differs from the snapshot. No painting happens here: the
 * caller redraws the whole page once the editor closes. */
int settings_set_url(Settings *s, const char *normalized_url);
int settings_set_refresh_min(Settings *s, int minutes);
int settings_set_idle_s(Settings *s, int seconds);

/* Flush the working copy into cfg. Returns 1 when anything differs from the
 * snapshot taken at create time (i.e. config_save is warranted). */
int settings_commit(Settings *s, AppConfig *cfg);

#endif
