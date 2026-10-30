#ifndef AIQUOTA_CONFIG_H
#define AIQUOTA_CONFIG_H

#include <stddef.h>

#include "quota.h"

/* Loopback only: a real LAN address is a per-device config value, never a
 * hardcoded address in the repo. Existing devices keep their saved config;
 * fresh installs must set api_url once over USB. */
#define CONFIG_DEFAULT_API_BASE "http://127.0.0.1:46927"
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

/* F02: local ordered visibility list. Comma-separated provider ids; order is
 * card/request order. Does NOT call PUT /api/providers. */
#define CONFIG_PROVIDERS_DEFAULT "grok,codex,kimi,agy"
#define CONFIG_PROVIDERS_MAX 4

typedef struct {
    char api_url[512];   /* normalized path ends with /api/usage; optional query follows */
    /* Optional Bearer token, sent as "Authorization: Bearer <token>" when
     * non-empty. Stored in the device config file (plaintext, like the rest
     * of that file); the server must validate it. Empty = no header, which
     * preserves the old unauthenticated behaviour. Set over USB: the
     * on-device keypad cannot type token characters. */
    char api_token[256];
    int touch_swap_xy;   /* 0 or 1: swap touch X/Y axes */
    int touch_mirror_x;  /* 0 or 1: x = width  - 1 - x */
    int touch_mirror_y;  /* 0 or 1: y = height - 1 - y */
    int wifi_auto_off;   /* 0/1: drop the WiFi we raised once a fetch is done */
    int auto_refresh_min;/* minutes between automatic re-fetches; 0 = off */
    int light_idle_off_min; /* F06: 0=disable idle light-off, 1-60 minutes */
    int display_mode_pref; /* F04: 0=API, 1=used, 2=remain, 3=large, 4=compact */
    int refresh_preset;    /* F05: 0=custom/off via minutes, 1=manual, 2=normal, 3=night */
    int config_version;    /* F07: bumped on breaking config migrations */
    char show_providers[128]; /* F02: ordered CSV of enabled providers */
} AppConfig;

#define CONFIG_LIGHT_IDLE_DEFAULT 3
#define CONFIG_LIGHT_IDLE_MAX 60
#define CONFIG_VERSION_CURRENT 2
#define CONFIG_CACHE_PATH "/mnt/us/extensions/aiquota/cache.json"

void config_defaults(AppConfig *cfg);
/* 1 = sendable Bearer token, 0 = absent, -1 = malformed (drop, never send). */
int config_token_valid(const char *token);
/* Loads key=value file over the defaults. Missing file is fine: returns -1, defaults kept. */
int  config_load(AppConfig *cfg, const char *path);
/* Like config_load, but if primary fails tries path.bak and may rewrite primary. */
int  config_load_with_backup(AppConfig *cfg, const char *path);
int  config_save(const AppConfig *cfg, const char *path);
/* Normalize URL: trim -> default if empty -> add http:// if no scheme ->
 * strip the path's trailing '/' -> append /api/usage if missing -> preserve query. */
void config_normalize_url(char *dst, size_t dst_size, const char *src);
/* Clamp into [1, CONFIG_AUTO_REFRESH_MAX]; <= 0 -> 0 (off). */
int  config_clamp_auto_refresh(int mins);
int  config_clamp_light_idle(int mins);
/* F07: bump old configs to CONFIG_VERSION_CURRENT; returns 1 if mutated. */
int  config_migrate(AppConfig *cfg);
/* F01: atomic JSON snapshot cache bound to api_url. Authenticated responses
 * stay in memory only, so switching credentials never reuses another account. */
int  config_cache_save(const char *path, const char *api_url, const char *api_token,
                      const char *json_body, long long saved_at_ms);
int  config_cache_load(const char *path, const char *api_url, const char *api_token,
                      char *json_out, size_t json_out_size,
                      long long *saved_at_ms);

/* --- F02 provider preference helpers (host-testable) -------------------- */
/* Known provider name? 1/0. */
int  config_provider_known(const char *name);
/* Normalize CSV: keep known names, dedupe, preserve first-seen order.
 * Empty / all-invalid -> writes CONFIG_PROVIDERS_DEFAULT (never all-off).
 * Returns count of enabled names. */
int  config_providers_normalize(char *dst, size_t dst_size, const char *src);
int  config_providers_count(const char *list);
void config_providers_to_selection(const char *list, ProviderSelection *out);
/* Toggle one known name. Refuses clearing the last enabled name (returns -1
 * and leaves dst unchanged). Returns 0 on success, -1 on refuse/unknown. */
int  config_providers_toggle(char *dst, size_t dst_size, const char *name);
/* Move name one step toward the list head (dir<0) or tail (dir>0). */
int  config_providers_move(char *dst, size_t dst_size, const char *name, int dir);
/* Build fetch URL: drop existing providers/tools/tool/provider query keys,
 * append providers=<show_providers>. Base path/other query preserved. */
int  config_url_with_providers(char *dst, size_t dst_size,
                               const char *base_url, const char *show_providers);
/* F05: map preset to minutes; 0 custom leaves minutes alone. */
int  config_refresh_preset_minutes(int preset);
void config_apply_refresh_preset(AppConfig *cfg);

/* Legacy note: config files written by older builds may still contain an
 * idle_timeout_s=N line. The loader ignores unknown keys, so such files load
 * cleanly and the next config_save drops the line. */

#endif
