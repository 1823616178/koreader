/* Frontlight via powerd lipc. lipc only exists on-device, so every failure
 * mode (missing binary, empty answer, out-of-range parse) degrades to -1
 * and never a crash. powerd is a separate daemon from cvm, so freezing the
 * framework does not block these calls -- the same reason preventScreenSaver
 * already works from net.c. */
#define _POSIX_C_SOURCE 200112L
#define _DEFAULT_SOURCE

#include "light.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#include "watchdog.h"

#define LIGHT_CMD_S 30 /* one hung lipc-get/set; matches NET_CMD_S */
#define LIGHT_SANITY_MAX 255


/* Property names interpolated into a shell command must be plain
 * identifiers. All current callers pass constants (flIntensity,
 * flMaxIntensity); this rejects any future caller that would smuggle a
 * shell metacharacter, space or option flag into lipc-get/set-prop. */
static int prop_valid(const char *prop)
{
    const unsigned char *p;
    if (!prop || !*prop)
        return 0;
    for (p = (const unsigned char *)prop; *p; p++) {
        if (!isalnum(*p) && *p != '_' && *p != '-' && *p != '.')
            return 0;
    }
    return 1;
}

static int lipc_get_int(const char *prop, int *out)
{
    char cmd[128];
    FILE *p;
    char buf[64];
    char *end;
    long v;

    if (!prop_valid(prop) || !out)
        return -1;
    snprintf(cmd, sizeof(cmd),
             "lipc-get-prop com.lab126.powerd %s 2>/dev/null", prop);
    wd_expect(LIGHT_CMD_S);
    p = popen(cmd, "r");
    if (!p)
        return -1;
    if (!fgets(buf, sizeof(buf), p)) {
        pclose(p);
        return -1;
    }
    pclose(p);
    v = strtol(buf, &end, 10);
    if (end == buf || v < 0 || v > LIGHT_SANITY_MAX)
        return -1;
    *out = (int)v;
    return 0;
}

static int lipc_set_int(const char *prop, int value)
{
    char cmd[160];
    int rc;

    if (!prop_valid(prop))
        return -1;
    snprintf(cmd, sizeof(cmd),
             "lipc-set-prop -i com.lab126.powerd %s %d 2>/dev/null",
             prop, value);
    wd_expect(LIGHT_CMD_S);
    rc = system(cmd);
    return rc == 0 ? 0 : -1;
}

int light_clamp(int n, int max)
{
    if (max < 1)
        max = LIGHT_MAX_FALLBACK;
    if (n < 0)
        return 0;
    if (n > max)
        return max;
    return n;
}

int light_get_intensity(void)
{
    int v;

    if (lipc_get_int("flIntensity", &v) != 0)
        return -1;
    return v;
}

/* flMaxIntensity is a per-device constant. Cache the first successful read:
 * every set and every stepper click otherwise pays a second lipc spawn just
 * to re-learn a number that cannot change. A failed read is never cached --
 * the next call retries, and light_clamp's fallback keeps sets working. */
static int cached_max;

int light_get_max(void)
{
    int v;

    if (cached_max >= 1)
        return cached_max;
    if (lipc_get_int("flMaxIntensity", &v) != 0 || v < 1)
        return -1;
    cached_max = v;
    return v;
}

/* The level "on" means when the light is currently off: what the settings
 * page's stepper/toggle offer as the turn-back-on target. Written explicitly
 * by light_set_last_on from the moments that express a chosen level (page
 * opened on a lit light, an up-step, a toggle-off) -- never by
 * light_set_intensity itself, because a stepper descent applies every
 * intermediate level on the way to 0 and would decay the slot to 1, which
 * the backlight driver maps to ~2/4095: the light would come back visibly
 * still off. A hint, not state: powerd persists the live intensity itself,
 * and this slot stays process-local on purpose -- never written to the
 * config file. */
static int user_last_on;

int light_set_intensity(int n)
{
    int max = light_get_max();

    n = light_clamp(n, max);
    if (lipc_set_int("flIntensity", n) != 0)
        return -1;
    return 0;
}

int light_last_on(void)
{
    return user_last_on;
}

void light_set_last_on(int n)
{
    if (n > 0)
        user_last_on = n;
}

/* --- idle dim --------------------------------------------------------------
 * One process-static slot does double duty: the level to restore, and the
 * "dimmed by this logic" marker (non-zero). The settings page's last-on hint
 * lives in user_last_on instead: the main loop restores (and so clears) the
 * idle slot before the settings page can open, so it would read 0 here. */

static int idle_saved; /* level to restore; 0 = the light was not dimmed by us */

int light_idle_level(void)
{
    return idle_saved;
}

void light_idle_cancel(void)
{
    idle_saved = 0;
}

int light_idle_off(void)
{
    int cur = light_get_intensity();

    if (cur <= 0)
        return 0; /* already off, or powerd unreadable: nothing to dim */
    if (light_set_intensity(0) != 0)
        return 0;
    idle_saved = cur;
    return 1;
}

int light_idle_restore(void)
{
    int saved = idle_saved;

    if (saved <= 0)
        return 0;
    /* Keep the level on failure so the next touch retries the restore. */
    if (light_set_intensity(saved) != 0)
        return 0;
    idle_saved = 0;
    return 1;
}
