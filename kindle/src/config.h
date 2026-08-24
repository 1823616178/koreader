#ifndef AIQUOTA_CONFIG_H
#define AIQUOTA_CONFIG_H

#include <stddef.h>

#define CONFIG_DEFAULT_API_BASE "http://123.161.179.32:46927"
#define CONFIG_PATH "/mnt/us/extensions/aiquota/config"

/* Idle auto-exit: no tap for this long closes the app, which is what releases
 * the SIGSTOPped framework. Clamped, never disabled -- an unbounded wait is
 * exactly the state the watchdog design refuses to allow. */
#define CONFIG_IDLE_TIMEOUT_DEFAULT 600
#define CONFIG_IDLE_TIMEOUT_MIN 60
#define CONFIG_IDLE_TIMEOUT_MAX 3600

typedef struct {
    char api_url[512];   /* normalized, always ends with /api/usage */
    int touch_swap_xy;   /* 0 or 1: swap touch X/Y axes */
    int touch_mirror_x;  /* 0 or 1: x = width  - 1 - x */
    int touch_mirror_y;  /* 0 or 1: y = height - 1 - y */
    int idle_timeout_s;  /* seconds without a tap before the app exits itself */
} AppConfig;

void config_defaults(AppConfig *cfg);
/* Loads key=value file over the defaults. Missing file is fine: returns -1, defaults kept. */
int  config_load(AppConfig *cfg, const char *path);
int  config_save(const AppConfig *cfg, const char *path);
/* Port of quotadata.lua normalizeUrl (lines 49-62): trim -> default if empty ->
 * add http:// if no scheme -> strip trailing '/' -> append /api/usage if missing. */
void config_normalize_url(char *dst, size_t dst_size, const char *src);
/* Clamp into [CONFIG_IDLE_TIMEOUT_MIN, CONFIG_IDLE_TIMEOUT_MAX]; <= 0 -> default. */
int  config_clamp_idle_timeout(int secs);

#endif
