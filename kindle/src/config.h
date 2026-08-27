#ifndef AIQUOTA_CONFIG_H
#define AIQUOTA_CONFIG_H

#include <stddef.h>

#define CONFIG_DEFAULT_API_BASE "http://123.161.179.32:46927"
#define CONFIG_PATH "/mnt/us/extensions/aiquota/config"

/* Idle auto-exit: no tap for this long closes the app, which is what releases
 * the SIGSTOPped framework. 0 disables the timer entirely -- the app then
 * leaves only via 关闭 (or the watchdog layers). Mind the power angle: with
 * preventScreenSaver held and the framework frozen, the Kindle will not
 * suspend while the app sits open, so unlimited costs real battery. A missing
 * key keeps the default; any non-positive value in the file means disabled
 * (atoi cannot tell "0" from garbage, so both map to off). */
#define CONFIG_IDLE_TIMEOUT_DEFAULT 600
#define CONFIG_IDLE_TIMEOUT_MIN 60
#define CONFIG_IDLE_TIMEOUT_MAX 3600

/* Release the radio after each fetch when we were the ones who raised it.
 * The Kindle's WiFi chip idles at tens of milliamps, so on-by-default is the
 * power-correct choice; 0 keeps the radio up (user's explicit preference). */
#define CONFIG_WIFI_AUTO_OFF_DEFAULT 1

/* Periodic re-fetch in whole minutes while the dashboard is up. 0 disables:
 * the default keeps "open -> glance -> put down" free of surprise refreshes.
 * An auto-refresh never feeds the idle timer, so the app still closes itself
 * after idle_timeout_s no matter how long it has been refreshing. */
#define CONFIG_AUTO_REFRESH_DEFAULT 0
#define CONFIG_AUTO_REFRESH_MAX 60

typedef struct {
    char api_url[512];   /* normalized, always ends with /api/usage */
    int touch_swap_xy;   /* 0 or 1: swap touch X/Y axes */
    int touch_mirror_x;  /* 0 or 1: x = width  - 1 - x */
    int touch_mirror_y;  /* 0 or 1: y = height - 1 - y */
    int idle_timeout_s;  /* seconds without a tap before the app exits itself */
    int wifi_auto_off;   /* 0/1: drop the WiFi we raised once a fetch is done */
    int auto_refresh_min;/* minutes between automatic re-fetches; 0 = off */
} AppConfig;

void config_defaults(AppConfig *cfg);
/* Loads key=value file over the defaults. Missing file is fine: returns -1, defaults kept. */
int  config_load(AppConfig *cfg, const char *path);
int  config_save(const AppConfig *cfg, const char *path);
/* Port of quotadata.lua normalizeUrl (lines 49-62): trim -> default if empty ->
 * add http:// if no scheme -> strip trailing '/' -> append /api/usage if missing. */
void config_normalize_url(char *dst, size_t dst_size, const char *src);
/* Clamp into [CONFIG_IDLE_TIMEOUT_MIN, CONFIG_IDLE_TIMEOUT_MAX]; <= 0 -> 0 (unlimited). */
int  config_clamp_idle_timeout(int secs);
/* Clamp into [1, CONFIG_AUTO_REFRESH_MAX]; <= 0 -> 0 (off). */
int  config_clamp_auto_refresh(int mins);

#endif
