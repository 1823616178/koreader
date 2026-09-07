#ifndef AIQUOTA_LIGHT_H
#define AIQUOTA_LIGHT_H

/* Frontlight through Amazon's powerd lipc (com.lab126.powerd flIntensity).
 * Same failure stance as net.c: missing lipc/powerd returns -1 and never
 * crashes. 0 is off on PW4+ (the 5.16 target); powerd persists the value
 * itself, so this is not an AppConfig key. The scale is the stock UI's
 * 0..flMaxIntensity inclusive. */

#define LIGHT_MAX_FALLBACK 24

int light_get_intensity(void); /* 0..max, or -1 if unreadable */
int light_get_max(void);       /* inclusive max, or -1 if unreadable */
int light_set_intensity(int n); /* clamp then set; 0 ok, -1 on fail */
/* [0, max]; max < 1 uses LIGHT_MAX_FALLBACK. */
int light_clamp(int n, int max);

/* The level "on" means while the light is off: the settings page's
 * turn-back-on target. Written only through light_set_last_on, from the
 * moments that express a chosen level (settings page opened on a lit light,
 * an up-step, a toggle-off); a stepper descent to 0 deliberately does NOT
 * write it, so the target survives turning the light off. 0 = no positive
 * level known yet (callers fall back to max/2). Process-local only: never
 * persisted, never an AppConfig key. */
int  light_last_on(void);
void light_set_last_on(int n); /* n <= 0 ignored: off is not a level */

/* Idle frontlight: the parked dashboard's biggest avoidable draw is the LED,
 * so the main loop dims it to 0 after a few idle minutes and restores it on
 * the next touch (or before exit). The remembered level doubles as the
 * "dimmed by us" marker: a light the user turned off never enters it, so a
 * restore can never switch one back on. The settings page's re-open target
 * comes from light_last_on() instead -- the main loop always restores (and
 * clears) the idle slot before the settings page can open. */
int  light_idle_off(void);     /* remember current level, set 0; 1 if dimmed now */
int  light_idle_restore(void); /* re-apply the remembered level; 1 if restored */
int  light_idle_level(void);   /* level remembered by idle-off, 0 when none */
void light_idle_cancel(void);  /* forget the level (user changed the light) */

#endif
