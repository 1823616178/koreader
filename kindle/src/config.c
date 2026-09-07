/* config load/save: key=value file on /mnt/us (often vfat). */
#define _POSIX_C_SOURCE 200809L /* fileno, fsync, unlink, access */
#define _DEFAULT_SOURCE

#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    cfg->wifi_auto_off = CONFIG_WIFI_AUTO_OFF_DEFAULT;
    cfg->auto_refresh_min = CONFIG_AUTO_REFRESH_DEFAULT;
    cfg->light_idle_off_min = CONFIG_LIGHT_IDLE_DEFAULT;
    cfg->display_mode_pref = 0;
    cfg->refresh_preset = 0;
    cfg->config_version = CONFIG_VERSION_CURRENT;
    snprintf(cfg->show_providers, sizeof(cfg->show_providers), "%s",
             CONFIG_PROVIDERS_DEFAULT);
}

int config_clamp_auto_refresh(int mins)
{
    if (mins <= 0)
        return 0; /* anything non-positive is "off" */
    if (mins > CONFIG_AUTO_REFRESH_MAX)
        return CONFIG_AUTO_REFRESH_MAX;
    return mins;
}

int config_clamp_light_idle(int mins)
{
    if (mins < 0)
        return CONFIG_LIGHT_IDLE_DEFAULT;
    if (mins > CONFIG_LIGHT_IDLE_MAX)
        return CONFIG_LIGHT_IDLE_MAX;
    return mins; /* 0 = feature off */
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
        } else if (strcmp(key, "wifi_auto_off") == 0) {
            cfg->wifi_auto_off = atoi(value) ? 1 : 0;
        } else if (strcmp(key, "auto_refresh_min") == 0) {
            cfg->auto_refresh_min = config_clamp_auto_refresh(atoi(value));
        } else if (strcmp(key, "light_idle_off_min") == 0) {
            cfg->light_idle_off_min = config_clamp_light_idle(atoi(value));
        } else if (strcmp(key, "display_mode_pref") == 0) {
            cfg->display_mode_pref = atoi(value);
            if (cfg->display_mode_pref < 0 || cfg->display_mode_pref > 4)
                cfg->display_mode_pref = 0;
        } else if (strcmp(key, "config_version") == 0) {
            cfg->config_version = atoi(value);
        } else if (strcmp(key, "refresh_preset") == 0) {
            cfg->refresh_preset = atoi(value);
            if (cfg->refresh_preset < 0 || cfg->refresh_preset > 3)
                cfg->refresh_preset = 0;
        } else if (strcmp(key, "show_providers") == 0) {
            config_providers_normalize(cfg->show_providers,
                                       sizeof(cfg->show_providers), value);
        }
        /* unknown keys are ignored */
    }
    fclose(f);

    if (have_url)
        config_normalize_url(cfg->api_url, sizeof(cfg->api_url), api_url);
    /* Ensure provider list is always valid after load. */
    if (cfg->show_providers[0] == '\0')
        config_providers_normalize(cfg->show_providers,
                                   sizeof(cfg->show_providers),
                                   CONFIG_PROVIDERS_DEFAULT);
    if (cfg->display_mode_pref < 0 || cfg->display_mode_pref > 4)
        cfg->display_mode_pref = 0;
    return 0;
}

int config_save(const AppConfig *cfg, const char *path)
{
    char tmp[768];
    char bak[768];
    FILE *f;
    size_t plen;
    int fd;

    if (!cfg || !path || !path[0])
        return -1;

    plen = strlen(path);
    if (plen + 5 >= sizeof(tmp)) /* ".tmp" / ".bak" + NUL */
        return -1;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    snprintf(bak, sizeof(bak), "%s.bak", path);

    /* Write a sibling temp file first: never truncate the live config with
     * fopen(path, "w") in case we die mid-write (power loss / OOM / kill). */
    f = fopen(tmp, "w");
    if (!f)
        return -1;

    if (fprintf(f, "api_url=%s\n", cfg->api_url) < 0 ||
        fprintf(f, "touch_swap_xy=%d\n", cfg->touch_swap_xy) < 0 ||
        fprintf(f, "touch_mirror_x=%d\n", cfg->touch_mirror_x) < 0 ||
        fprintf(f, "touch_mirror_y=%d\n", cfg->touch_mirror_y) < 0 ||
        fprintf(f, "wifi_auto_off=%d\n", cfg->wifi_auto_off) < 0 ||
        fprintf(f, "auto_refresh_min=%d\n", cfg->auto_refresh_min) < 0 ||
        fprintf(f, "light_idle_off_min=%d\n", cfg->light_idle_off_min) < 0 ||
        fprintf(f, "display_mode_pref=%d\n", cfg->display_mode_pref) < 0 ||
        fprintf(f, "refresh_preset=%d\n", cfg->refresh_preset) < 0 ||
        fprintf(f, "show_providers=%s\n", cfg->show_providers) < 0 ||
        fprintf(f, "config_version=%d\n", cfg->config_version) < 0) {
        fclose(f);
        unlink(tmp);
        return -1;
    }

    if (fflush(f) != 0) {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    fd = fileno(f);
    if (fd >= 0)
        (void)fsync(fd); /* best-effort; FAT may no-op, still durable after rename */
    if (fclose(f) != 0) {
        unlink(tmp);
        return -1;
    }

    /* Keep the previous good file as .bak when present. rename over an
     * existing bak is fine; failure to backup is non-fatal. */
    if (access(path, F_OK) == 0)
        (void)rename(path, bak);

    if (rename(tmp, path) != 0) {
        /* Prefer restoring the backup over leaving no config at all. */
        if (access(bak, F_OK) == 0)
            (void)rename(bak, path);
        unlink(tmp);
        return -1;
    }
    return 0;
}

int config_cache_save(const char *path, const char *api_url,
                      const char *json_body, long long saved_at_ms)
{
    char tmp[768];
    FILE *f;
    size_t plen;

    if (!path || !api_url || !json_body)
        return -1;
    plen = strlen(path);
    if (plen + 5 >= sizeof(tmp))
        return -1;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "w");
    if (!f)
        return -1;
    if (fprintf(f, "api_url=%s\n", api_url) < 0 ||
        fprintf(f, "saved_at=%lld\n", saved_at_ms) < 0 ||
        fprintf(f, "json=\n%s", json_body) < 0 ||
        fflush(f) != 0) {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    {
        int fd = fileno(f);
        if (fd >= 0)
            (void)fsync(fd);
    }
    if (fclose(f) != 0) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int config_cache_load(const char *path, const char *api_url,
                      char *json_out, size_t json_out_size,
                      long long *saved_at_ms)
{
    FILE *f;
    char line[768];
    char file_url[768]; /* same bound as line[], minus the 8-byte prefix */
    long long saved = 0;
    int have_url = 0, have_json = 0;
    size_t jlen = 0;

    if (!path || !api_url || !json_out || json_out_size < 2)
        return -1;
    json_out[0] = '\0';
    f = fopen(path, "r");
    if (!f)
        return -1;
    file_url[0] = '\0';
    while (fgets(line, sizeof(line), f)) {
        if (!have_json && strncmp(line, "api_url=", 8) == 0) {
            snprintf(file_url, sizeof(file_url), "%s", line + 8);
            {
                size_t L = strlen(file_url);
                while (L > 0 && (file_url[L - 1] == '\n' || file_url[L - 1] == '\r'))
                    file_url[--L] = '\0';
            }
            have_url = 1;
        } else if (!have_json && strncmp(line, "saved_at=", 9) == 0) {
            saved = (long long)atoll(line + 9);
        } else if (!have_json && strcmp(line, "json=\n") == 0) {
            have_json = 1;
        } else if (!have_json && strncmp(line, "json=", 5) == 0) {
            /* json on same line */
            have_json = 1;
            snprintf(json_out, json_out_size, "%s", line + 5);
            jlen = strlen(json_out);
            while (jlen > 0 && (json_out[jlen - 1] == '\n' ||
                                json_out[jlen - 1] == '\r'))
                json_out[--jlen] = '\0';
        } else if (have_json) {
            size_t L = strlen(line);
            if (jlen + L + 1 >= json_out_size)
                break;
            memcpy(json_out + jlen, line, L + 1);
            jlen += L;
        }
    }
    fclose(f);
    if (!have_url || !have_json || strcmp(file_url, api_url) != 0)
        return -1;
    if (saved_at_ms)
        *saved_at_ms = saved;
    return 0;
}

/* --- F02 / F05 / F07 helpers --------------------------------------------- */

static const char *const PROVIDER_NAMES[CONFIG_PROVIDERS_MAX] = {
    "grok", "claude", "codex", "kimi", "opencode", "agy"
};

int config_provider_known(const char *name)
{
    int i;

    if (!name || !name[0])
        return 0;
    for (i = 0; i < CONFIG_PROVIDERS_MAX; i++) {
        if (strcmp(name, PROVIDER_NAMES[i]) == 0)
            return 1;
    }
    return 0;
}

static int providers_split(const char *src, char names[][16], int max)
{
    char buf[256];
    char *p, *tok;
    int n = 0;

    if (!src)
        src = "";
    snprintf(buf, sizeof(buf), "%s", src);
    p = buf;
    while (p && *p && n < max) {
        tok = p;
        while (*p && *p != ',')
            p++;
        if (*p) {
            *p = '\0';
            p++;
        } else {
            p = NULL;
        }
        while (*tok && isspace((unsigned char)*tok))
            tok++;
        {
            size_t L = strlen(tok);
            while (L > 0 && isspace((unsigned char)tok[L - 1]))
                tok[--L] = '\0';
        }
        if (*tok == '\0')
            continue;
        if (!config_provider_known(tok))
            continue;
        /* dedupe: keep first occurrence */
        {
            int j, dup = 0;
            for (j = 0; j < n; j++) {
                if (strcmp(names[j], tok) == 0) {
                    dup = 1;
                    break;
                }
            }
            if (dup)
                continue;
        }
        {
            size_t tl = strlen(tok);
            if (tl > 15)
                tl = 15;
            memcpy(names[n], tok, tl);
            names[n][tl] = '\0';
        }
        n++;
    }
    return n;
}

static void providers_join(char *dst, size_t dst_size,
                           char names[][16], int n)
{
    size_t pos = 0;
    int i;

    if (!dst || dst_size == 0)
        return;
    dst[0] = '\0';
    for (i = 0; i < n; i++) {
        size_t L = strlen(names[i]);
        if (pos && pos + 1 < dst_size)
            dst[pos++] = ',';
        if (pos + L >= dst_size)
            L = dst_size - pos - 1;
        memcpy(dst + pos, names[i], L);
        pos += L;
        dst[pos] = '\0';
    }
}

int config_providers_normalize(char *dst, size_t dst_size, const char *src)
{
    char names[CONFIG_PROVIDERS_MAX][16];
    int n = providers_split(src, names, CONFIG_PROVIDERS_MAX);

    if (n <= 0)
        n = providers_split(CONFIG_PROVIDERS_DEFAULT, names, CONFIG_PROVIDERS_MAX);
    providers_join(dst, dst_size, names, n);
    return n;
}

int config_providers_count(const char *list)
{
    char names[CONFIG_PROVIDERS_MAX][16];
    return providers_split(list ? list : "", names, CONFIG_PROVIDERS_MAX);
}

void config_providers_to_selection(const char *list, ProviderSelection *out)
{
    char names[CONFIG_PROVIDERS_MAX][16];
    int n, i;

    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    n = providers_split(list ? list : "", names, CONFIG_PROVIDERS_MAX);
    for (i = 0; i < n; i++) {
        if (strcmp(names[i], "grok") == 0)
            out->grok = 1;
        else if (strcmp(names[i], "claude") == 0)
            out->claude = 1;
        else if (strcmp(names[i], "codex") == 0)
            out->codex = 1;
        else if (strcmp(names[i], "kimi") == 0)
            out->kimi = 1;
        else if (strcmp(names[i], "opencode") == 0)
            out->opencode = 1;
        else if (strcmp(names[i], "agy") == 0)
            out->agy = 1;
    }
}

int config_providers_toggle(char *dst, size_t dst_size, const char *name)
{
    char names[CONFIG_PROVIDERS_MAX][16];
    char cur[128];
    int n, i, idx = -1;

    if (!dst || !name || !config_provider_known(name))
        return -1;
    snprintf(cur, sizeof(cur), "%s", dst);
    n = providers_split(cur, names, CONFIG_PROVIDERS_MAX);
    for (i = 0; i < n; i++) {
        if (strcmp(names[i], name) == 0) {
            idx = i;
            break;
        }
    }
    if (idx >= 0) {
        if (n <= 1)
            return -1; /* refuse all-off */
        for (i = idx; i < n - 1; i++)
            memcpy(names[i], names[i + 1], 16);
        n--;
    } else {
        if (n >= CONFIG_PROVIDERS_MAX)
            return -1;
        {
            size_t tl = strlen(name);
            if (tl > 15)
                tl = 15;
            memcpy(names[n], name, tl);
            names[n][tl] = '\0';
        }
        n++;
    }
    providers_join(dst, dst_size, names, n);
    return 0;
}

int config_providers_move(char *dst, size_t dst_size, const char *name, int dir)
{
    char names[CONFIG_PROVIDERS_MAX][16];
    char cur[128];
    char tmp[16];
    int n, i, idx = -1;

    if (!dst || !name || !config_provider_known(name))
        return -1;
    snprintf(cur, sizeof(cur), "%s", dst);
    n = providers_split(cur, names, CONFIG_PROVIDERS_MAX);
    for (i = 0; i < n; i++) {
        if (strcmp(names[i], name) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0)
        return -1;
    if (dir < 0) {
        if (idx == 0)
            return 0;
        memcpy(tmp, names[idx - 1], 16);
        memcpy(names[idx - 1], names[idx], 16);
        memcpy(names[idx], tmp, 16);
    } else if (dir > 0) {
        if (idx >= n - 1)
            return 0;
        memcpy(tmp, names[idx + 1], 16);
        memcpy(names[idx + 1], names[idx], 16);
        memcpy(names[idx], tmp, 16);
    }
    providers_join(dst, dst_size, names, n);
    return 0;
}

/* Drop providers/tools/tool/provider query keys; keep others. */
static void strip_provider_query(char *query /* starts with ? or empty */)
{
    char out[1050];
    const char *p;
    size_t pos = 0;
    int first = 1;

    if (!query || query[0] != '?') {
        if (query)
            query[0] = '\0';
        return;
    }
    p = query + 1;
    out[0] = '\0';
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t len = amp ? (size_t)(amp - p) : strlen(p);
        const char *eq = memchr(p, '=', len);
        size_t klen = eq ? (size_t)(eq - p) : len;
        int drop = 0;

        if ((klen == 9 && memcmp(p, "providers", 9) == 0) ||
            (klen == 5 && memcmp(p, "tools", 5) == 0) ||
            (klen == 4 && memcmp(p, "tool", 4) == 0) ||
            (klen == 8 && memcmp(p, "provider", 8) == 0))
            drop = 1;
        if (!drop && len > 0) {
            if (first) {
                out[pos++] = '?';
                first = 0;
            } else {
                out[pos++] = '&';
            }
            if (pos + len >= sizeof(out))
                len = sizeof(out) - pos - 1;
            memcpy(out + pos, p, len);
            pos += len;
            out[pos] = '\0';
        }
        if (!amp)
            break;
        p = amp + 1;
    }
    snprintf(query, 1050, "%s", out);
}

int config_url_with_providers(char *dst, size_t dst_size,
                              const char *base_url, const char *show_providers)
{
    char norm[1050];
    /* Worst case: path 1049 + stripped query 1049 + "&providers=" 11 +
     * prov 127 + NUL = 2237. Sizing path this way keeps the append below
     * provably non-truncating. */
    char path[2240];
    char query[1050];
    char prov[128];
    char *q;
    size_t plen;

    if (!dst || dst_size == 0)
        return -1;
    config_normalize_url(norm, sizeof(norm), base_url ? base_url : "");
    config_providers_normalize(prov, sizeof(prov),
                               show_providers ? show_providers
                                              : CONFIG_PROVIDERS_DEFAULT);
    query[0] = '\0';
    q = strchr(norm, '?');
    if (q) {
        snprintf(query, sizeof(query), "%s", q);
        *q = '\0';
        strip_provider_query(query);
    }
    snprintf(path, sizeof(path), "%s", norm);
    plen = strlen(path);
    /* Append providers= */
    if (query[0] == '\0') {
        snprintf(path + plen, sizeof(path) - plen, "?providers=%s", prov);
    } else {
        /* query already starts with ? */
        snprintf(path + plen, sizeof(path) - plen, "%s&providers=%s",
                 query, prov);
    }
    if (dst_size > 0) {
        snprintf(dst, dst_size, "%s", path);
    }
    return 0;
}

int config_refresh_preset_minutes(int preset)
{
    if (preset == 1)
        return 0; /* manual */
    if (preset == 2)
        return 5; /* normal */
    if (preset == 3)
        return 30; /* night */
    return -1; /* custom: leave minutes alone */
}

void config_apply_refresh_preset(AppConfig *cfg)
{
    int m;

    if (!cfg)
        return;
    m = config_refresh_preset_minutes(cfg->refresh_preset);
    if (m >= 0)
        cfg->auto_refresh_min = config_clamp_auto_refresh(m);
}

int config_migrate(AppConfig *cfg)
{
    int changed = 0;

    if (!cfg)
        return 0;
    if (cfg->config_version < 1) {
        cfg->config_version = 1;
        changed = 1;
    }
    if (cfg->config_version < 2) {
        if (cfg->show_providers[0] == '\0')
            snprintf(cfg->show_providers, sizeof(cfg->show_providers), "%s",
                     CONFIG_PROVIDERS_DEFAULT);
        else
            config_providers_normalize(cfg->show_providers,
                                       sizeof(cfg->show_providers),
                                       cfg->show_providers);
        if (cfg->display_mode_pref < 0 || cfg->display_mode_pref > 4)
            cfg->display_mode_pref = 0;
        if (cfg->refresh_preset < 0 || cfg->refresh_preset > 3)
            cfg->refresh_preset = 0;
        cfg->config_version = CONFIG_VERSION_CURRENT;
        changed = 1;
    }
    if (cfg->config_version > CONFIG_VERSION_CURRENT) {
        /* Forward-compat: clamp, do not rewrite unknown future fields away
         * beyond what we know. */
        cfg->config_version = CONFIG_VERSION_CURRENT;
        changed = 1;
    }
    if (cfg->show_providers[0] == '\0') {
        config_providers_normalize(cfg->show_providers,
                                   sizeof(cfg->show_providers),
                                   CONFIG_PROVIDERS_DEFAULT);
        changed = 1;
    }
    return changed;
}

int config_load_with_backup(AppConfig *cfg, const char *path)
{
    char bak[768];
    size_t plen;

    if (!cfg || !path || !path[0])
        return -1;
    if (config_load(cfg, path) == 0)
        return 0;
    plen = strlen(path);
    if (plen + 5 >= sizeof(bak))
        return -1;
    snprintf(bak, sizeof(bak), "%s.bak", path);
    if (config_load(cfg, bak) != 0)
        return -1;
    /* Prefer restoring a readable primary from the backup. */
    (void)config_save(cfg, path);
    return 0;
}
