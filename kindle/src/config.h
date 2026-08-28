#ifndef AIQUOTA_CONFIG_H
#define AIQUOTA_CONFIG_H

#include <stddef.h>

#define CONFIG_DEFAULT_API_BASE "http://123.161.179.32:46927"
#define CONFIG_PATH "/mnt/us/extensions/aiquota/config"

/* Release the radio after each fetch when we were the ones who raised it.
 * The Kindle's WiFi chip idles at tens of milliamps, so on-by-default is the
 * power-correct choice; 0 keeps the radio up (user's explicit preference). */
#define CONFIG_WIFI_AUTO_OFF_DEFAULT 1

/* Periodic re-fetch in whole minutes while the dashboard is up. 0 disables:
 * the default keeps "open -> glance -> put down" free of surprise refreshes.
 * The app never exits on its own anymore (the idle timer is gone), so this
 * is the only recurring cost while parked on the nightstand: CPU sleeps in
 * poll, the radio wakes only per fetch, and a cycle that fetches identical
 * data with an unchanged battery badge skips the e-ink repaint too. */
#define CONFIG_AUTO_REFRESH_DEFAULT 0
#define CONFIG_AUTO_REFRESH_MAX 60

typedef struct {
    char api_url[512];   /* normalized path ends with /api/usage; optional query follows */
    int touch_swap_xy;   /* 0 or 1: swap touch X/Y axes */
    int touch_mirror_x;  /* 0 or 1: x = width  - 1 - x */
    int touch_mirror_y;  /* 0 or 1: y = height - 1 - y */
    int wifi_auto_off;   /* 0/1: drop the WiFi we raised once a fetch is done */
    int auto_refresh_min;/* minutes between automatic re-fetches; 0 = off */
} AppConfig;

void config_defaults(AppConfig *cfg);
/* Loads key=value file over the defaults. Missing file is fine: returns -1, defaults kept. */
int  config_load(AppConfig *cfg, const char *path);
int  config_save(const AppConfig *cfg, const char *path);
/* Normalize URL: trim -> default if empty -> add http:// if no scheme ->
 * strip the path's trailing '/' -> append /api/usage if missing -> preserve query. */
void config_normalize_url(char *dst, size_t dst_size, const char *src);
/* Clamp into [1, CONFIG_AUTO_REFRESH_MAX]; <= 0 -> 0 (off). */
int  config_clamp_auto_refresh(int mins);

/* Legacy note: config files written by older builds may still contain an
 * idle_timeout_s=N line. The loader ignores unknown keys, so such files load
 * cleanly and the next config_save drops the line. */

#endif
