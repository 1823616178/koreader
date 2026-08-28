#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Trim leading and trailing whitespace (Lua %s class == isspace). */
static char *trim(char *s)
{
    size_t len;
    while (*s && isspace((unsigned char)*s))
        s++;
    len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1]))
        s[--len] = '\0';
    return s;
}

void config_defaults(AppConfig *cfg)
{
    config_normalize_url(cfg->api_url, sizeof(cfg->api_url), CONFIG_DEFAULT_API_BASE);
    cfg->touch_swap_xy = 0;
    cfg->touch_mirror_x = 0;
    cfg->touch_mirror_y = 0;
    cfg->idle_timeout_s = CONFIG_IDLE_TIMEOUT_DEFAULT;
    cfg->wifi_auto_off = CONFIG_WIFI_AUTO_OFF_DEFAULT;
    cfg->auto_refresh_min = CONFIG_AUTO_REFRESH_DEFAULT;
}

int config_clamp_idle_timeout(int secs)
{
    if (secs <= 0)
        return 0; /* 0 = unlimited: the idle timer is off */
    if (secs < CONFIG_IDLE_TIMEOUT_MIN)
        return CONFIG_IDLE_TIMEOUT_MIN;
    if (secs > CONFIG_IDLE_TIMEOUT_MAX)
        return CONFIG_IDLE_TIMEOUT_MAX;
    return secs;
}

int config_clamp_auto_refresh(int mins)
{
    if (mins <= 0)
        return CONFIG_AUTO_REFRESH_DEFAULT; /* anything non-positive is "off" */
    if (mins > CONFIG_AUTO_REFRESH_MAX)
        return CONFIG_AUTO_REFRESH_MAX;
    return mins;
}

void config_normalize_url(char *dst, size_t dst_size, const char *src)
{
    char buf[1050]; /* scheme prefix + input + "/api/usage" + NUL */
    char query[1050];
    const char *start = src ? src : "";
    char *query_start;
    size_t len;
    size_t pos = 0;

    while (*start && isspace((unsigned char)*start))
        start++;
    len = strlen(start);
    while (len > 0 && isspace((unsigned char)start[len - 1]))
        len--;

    if (len == 0) {
        start = CONFIG_DEFAULT_API_BASE;
        len = strlen(start);
    }

    /* Lua matches ^https?:// case-sensitively; anything else gets http:// prepended. */
    if (!((len >= 7 && memcmp(start, "http://", 7) == 0) ||
          (len >= 8 && memcmp(start, "https://", 8) == 0))) {
        memcpy(buf, "http://", 7);
        pos = 7;
    }

    if (len > sizeof(buf) - pos - 11) /* keep room for "/api/usage" + NUL */
        len = sizeof(buf) - pos - 11;
    memcpy(buf + pos, start, len);
    pos += len;
    buf[pos] = '\0';

    /* The API now accepts ?providers=... (and aliases). Normalize the path
     * before the query, then put the complete query back unchanged. */
    query[0] = '\0';
    query_start = strchr(buf, '?');
    if (query_start) {
        snprintf(query, sizeof(query), "%s", query_start);
        pos = (size_t)(query_start - buf);
        buf[pos] = '\0';
    }

    while (pos > 0 && buf[pos - 1] == '/')
        pos--;
    buf[pos] = '\0';

    if (pos < 10 || memcmp(buf + pos - 10, "/api/usage", 10) != 0) {
        memcpy(buf + pos, "/api/usage", 10);
        pos += 10;
        buf[pos] = '\0';
    }

    if (query[0] != '\0') {
        size_t qlen = strlen(query);
        if (qlen > sizeof(buf) - pos - 1)
            qlen = sizeof(buf) - pos - 1;
        memcpy(buf + pos, query, qlen);
        pos += qlen;
        buf[pos] = '\0';
    }

    if (dst_size > 0) {
        size_t out = strlen(buf);
        if (out >= dst_size)
            out = dst_size - 1;
        memcpy(dst, buf, out);
        dst[out] = '\0';
    }
}

int config_load(AppConfig *cfg, const char *path)
{
    FILE *f = fopen(path, "r");
    char line[768];
    char api_url[512];
    int have_url = 0;

    if (!f)
        return -1;

    api_url[0] = '\0';
    while (fgets(line, sizeof(line), f)) {
        char *key = trim(line);
        char *eq;
        char *value;

        if (*key == '\0' || *key == '#' || *key == ';')
            continue;
        eq = strchr(key, '=');
        if (!eq)
            continue;
        *eq = '\0';
        /* key was trimmed before splitting; re-trim the part left of '=' */
        {
            size_t klen = strlen(key);
            while (klen > 0 && isspace((unsigned char)key[klen - 1]))
                key[--klen] = '\0';
        }
        value = trim(eq + 1);

        if (strcmp(key, "api_url") == 0) {
            snprintf(api_url, sizeof(api_url), "%s", value);
            have_url = 1;
        } else if (strcmp(key, "touch_swap_xy") == 0) {
            cfg->touch_swap_xy = atoi(value) ? 1 : 0;
        } else if (strcmp(key, "touch_mirror_x") == 0) {
            cfg->touch_mirror_x = atoi(value) ? 1 : 0;
        } else if (strcmp(key, "touch_mirror_y") == 0) {
            cfg->touch_mirror_y = atoi(value) ? 1 : 0;
        } else if (strcmp(key, "idle_timeout_s") == 0) {
            cfg->idle_timeout_s = config_clamp_idle_timeout(atoi(value));
        } else if (strcmp(key, "wifi_auto_off") == 0) {
            cfg->wifi_auto_off = atoi(value) ? 1 : 0;
        } else if (strcmp(key, "auto_refresh_min") == 0) {
            cfg->auto_refresh_min = config_clamp_auto_refresh(atoi(value));
        }
        /* unknown keys are ignored */
    }
    fclose(f);

    if (have_url)
        config_normalize_url(cfg->api_url, sizeof(cfg->api_url), api_url);
    return 0;
}

int config_save(const AppConfig *cfg, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "api_url=%s\n", cfg->api_url);
    fprintf(f, "touch_swap_xy=%d\n", cfg->touch_swap_xy);
    fprintf(f, "touch_mirror_x=%d\n", cfg->touch_mirror_x);
    fprintf(f, "touch_mirror_y=%d\n", cfg->touch_mirror_y);
    fprintf(f, "idle_timeout_s=%d\n", cfg->idle_timeout_s);
    fprintf(f, "wifi_auto_off=%d\n", cfg->wifi_auto_off);
    fprintf(f, "auto_refresh_min=%d\n", cfg->auto_refresh_min);
    if (fclose(f) != 0)
        return -1;
    return 0;
}
