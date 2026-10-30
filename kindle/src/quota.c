#include "quota.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"

double quota_clamp(double v)
{
    if (v < 0.0)
        return 0.0;
    if (v > 100.0)
        return 100.0;
    return v;
}

void quota_empty_snapshot(QuotaSnapshot *snap)
{
    memset(snap, 0, sizeof(*snap));
    snprintf(snap->display_mode, sizeof(snap->display_mode), "%s", "remaining");
    snap->alert_percent = 85.0;
    /* Backward compatibility: responses predating the providers map implied
     * that every known tool was enabled. Loading skeletons use the same set. */
    snap->providers.grok = 1;
    snap->providers.codex = 1;
    snap->providers.kimi = 1;
    snap->providers.agy = 1;
}

/* Cast a JSON number to long long only when it fits (and is not NaN); cJSON
 * accepts literals like 1e300, and casting an out-of-range double is UB.
 * Bounds are exactly -2^63 (LLONG_MIN, representable) and 2^63 (excluded). */
static int double_to_ll(double v, long long *out)
{
    if (!(v >= -9223372036854775808.0 && v < 9223372036854775808.0))
        return -1;
    *out = (long long)v;
    return 0;
}

/* Type checks, not truthiness: used == 0 is valid (mirrors validWindow in quotadata.lua). */
static void parse_window(const cJSON *obj, QuotaWindow *w)
{
    const cJSON *used;
    const cJSON *reset_at;

    w->valid = 0;
    w->used = 0.0;
    w->has_reset_at = 0;
    w->reset_at = 0;
    w->rate_limited = 0;

    if (!cJSON_IsObject(obj))
        return;
    used = cJSON_GetObjectItemCaseSensitive(obj, "used");
    if (!cJSON_IsNumber(used))
        return;

    w->valid = 1;
    w->used = quota_clamp(used->valuedouble);
    reset_at = cJSON_GetObjectItemCaseSensitive(obj, "resetAt");
    /* Out-of-range resetAt is treated as absent, window stays valid. */
    if (cJSON_IsNumber(reset_at) && double_to_ll(reset_at->valuedouble, &w->reset_at) == 0)
        w->has_reset_at = 1;

    {
        const cJSON *status = cJSON_GetObjectItemCaseSensitive(obj, "status");
        if (cJSON_IsString(status) && status->valuestring &&
            strcmp(status->valuestring, "rate-limited") == 0)
            w->rate_limited = 1;
    }
}

static void parse_provider(const cJSON *obj, ProviderQuota *p)
{
    const cJSON *fetched_at;

    if (!cJSON_IsObject(obj))
        return;
    fetched_at = cJSON_GetObjectItemCaseSensitive(obj, "fetchedAt");
    if (cJSON_IsNumber(fetched_at) &&
        double_to_ll(fetched_at->valuedouble, &p->fetched_at) == 0)
        p->has_fetched_at = 1;
    parse_window(cJSON_GetObjectItemCaseSensitive(obj, "five"), &p->five);
    parse_window(cJSON_GetObjectItemCaseSensitive(obj, "seven"), &p->seven);
    parse_window(cJSON_GetObjectItemCaseSensitive(obj, "monthly"), &p->monthly);
}

static int provider_enabled(const cJSON *providers, const char *name, int fallback)
{
    const cJSON *item;

    if (!cJSON_IsObject(providers))
        return fallback;
    item = cJSON_GetObjectItemCaseSensitive(providers, name);
    return cJSON_IsBool(item) ? (cJSON_IsTrue(item) ? 1 : 0) : fallback;
}

int quota_parse_snapshot(const char *json, QuotaSnapshot *snap)
{
    cJSON *root = cJSON_ParseWithOpts(json, NULL, 1);
    const cJSON *item;

    if (!root)
        return -1;
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return -1;
    }

    quota_empty_snapshot(snap);

    item = cJSON_GetObjectItemCaseSensitive(root, "displayMode");
    if (cJSON_IsString(item) && item->valuestring &&
        strcmp(item->valuestring, "used") == 0)
        snprintf(snap->display_mode, sizeof(snap->display_mode), "%s", "used");

    item = cJSON_GetObjectItemCaseSensitive(root, "alertPercent");
    if (cJSON_IsNumber(item))
        snap->alert_percent = quota_clamp(item->valuedouble);

    item = cJSON_GetObjectItemCaseSensitive(root, "generatedAt");
    if (cJSON_IsNumber(item) && double_to_ll(item->valuedouble, &snap->generated_at) == 0)
        snap->has_generated_at = 1;

    item = cJSON_GetObjectItemCaseSensitive(root, "providers");
    snap->providers.grok = provider_enabled(item, "grok", snap->providers.grok);
    snap->providers.codex = provider_enabled(item, "codex", snap->providers.codex);
    snap->providers.kimi = provider_enabled(item, "kimi", snap->providers.kimi);
    snap->providers.agy = provider_enabled(item, "agy", snap->providers.agy);

    parse_provider(cJSON_GetObjectItemCaseSensitive(root, "grok"), &snap->grok);
    parse_provider(cJSON_GetObjectItemCaseSensitive(root, "codex"), &snap->codex);
    parse_provider(cJSON_GetObjectItemCaseSensitive(root, "kimi"), &snap->kimi);

    {
        const cJSON *agy = cJSON_GetObjectItemCaseSensitive(root, "agy");
        const cJSON *fetched_at = cJSON_GetObjectItemCaseSensitive(agy, "fetchedAt");

        if (cJSON_IsNumber(fetched_at) &&
            double_to_ll(fetched_at->valuedouble, &snap->agy.fetched_at) == 0)
            snap->agy.has_fetched_at = 1;
        parse_provider(cJSON_GetObjectItemCaseSensitive(agy, "gemini"), &snap->agy.gemini);
        /* JSON key is camelCase thirdParty, C field is third_party. */
        parse_provider(cJSON_GetObjectItemCaseSensitive(agy, "thirdParty"), &snap->agy.third_party);
        /* AGY has one collection timestamp shared by its two quota groups. */
        snap->agy.gemini.has_fetched_at = snap->agy.has_fetched_at;
        snap->agy.gemini.fetched_at = snap->agy.fetched_at;
        snap->agy.third_party.has_fetched_at = snap->agy.has_fetched_at;
        snap->agy.third_party.fetched_at = snap->agy.fetched_at;
    }

    cJSON_Delete(root);
    return 0;
}

double quota_display_value(const QuotaWindow *w, const char *display_mode)
{
    if (display_mode && strcmp(display_mode, "remaining") == 0)
        return quota_clamp(100.0 - w->used);
    return w->used;
}

void quota_format_percent(char *buf, size_t n, double value)
{
    /* value is clamped to 0..100 before this is called, so truncation toward
     * zero equals floor here and no libm is needed (link line has no -lm). */
    double rounded = (double)(int)(value + 0.5);
    double diff = value - rounded;
    if (diff < 0)
        diff = -diff;
    if (diff < 0.05)
        snprintf(buf, n, "%d%%", (int)rounded);
    else
        snprintf(buf, n, "%.1f%%", value);
}

/* Epoch ms -> epoch s with Lua math.floor semantics (not C truncation). */
static long long floor_s(long long ms)
{
    long long s = ms / 1000;
    if (ms < 0 && ms % 1000 != 0)
        s--;
    return s;
}

void quota_format_reset(char *buf, size_t n, const QuotaWindow *w, long long now_ms)
{
    long long seconds;

    if (!w->has_reset_at) {
        snprintf(buf, n, "%s", "重置时间未知");
        return;
    }

    /* Lua: math.floor(reset_at / 1000 - os.time()); flooring each side first is
     * identical for integer inputs and avoids float cancellation near boundaries. */
    seconds = floor_s(w->reset_at) - floor_s(now_ms);

    if (seconds <= 0) {
        snprintf(buf, n, "%s", "即将重置");
    } else if (seconds < 3600) {
        long long minutes = (seconds + 59) / 60; /* max(1, ceil(s/60)); s >= 1 here */
        snprintf(buf, n, "%lld 分钟后", minutes);
    } else if (seconds < 86400) {
        snprintf(buf, n, "%lld 小时 %lld 分后", seconds / 3600, (seconds % 3600) / 60);
    } else if (seconds < 7 * 86400) {
        snprintf(buf, n, "%lld 天 %lld 小时后", seconds / 86400, (seconds % 86400) / 3600);
    } else {
        time_t t = (time_t)floor_s(w->reset_at);
        struct tm *lt = localtime(&t);
        if (!lt || strftime(buf, n, "%m-%d %H:%M", lt) == 0)
            snprintf(buf, n, "%s", "--");
    }
}

void quota_format_timestamp(char *buf, size_t n, int has_value, long long ms)
{
    time_t t;
    struct tm *lt;

    if (!has_value) {
        snprintf(buf, n, "%s", "--");
        return;
    }
    t = (time_t)floor_s(ms);
    lt = localtime(&t);
    if (!lt || strftime(buf, n, "%m-%d %H:%M:%S", lt) == 0)
        snprintf(buf, n, "%s", "--");
}

void quota_format_timestamp_min(char *buf, size_t n, int has_value, long long ms)
{
    time_t t;
    struct tm *lt;

    if (!has_value) {
        snprintf(buf, n, "%s", "--");
        return;
    }
    t = (time_t)floor_s(ms);
    lt = localtime(&t);
    /* Minute floor: same text for any second within the minute, so a fetch
     * that only bumps generatedAt by milliseconds does not dirty the panel. */
    if (!lt || strftime(buf, n, "%m-%d %H:%M", lt) == 0)
        snprintf(buf, n, "%s", "--");
}

static unsigned long long fnv1a_add(unsigned long long h, const char *s)
{
    if (!s)
        return h;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 0x100000001b3ULL;
    }
    return h;
}

static unsigned long long fnv1a_add_win(unsigned long long h, const QuotaWindow *w,
                                       const char *mode, long long now_ms)
{
    char pct[16];
    char reset[48];

    if (!w || !w->valid) {
        return fnv1a_add(h, "--");
    }
    quota_format_percent(pct, sizeof(pct), quota_display_value(w, mode));
    h = fnv1a_add(h, pct);
    h ^= (unsigned long long)(w->rate_limited ? 1 : 0);
    h *= 0x100000001b3ULL;
    quota_format_reset(reset, sizeof(reset), w, now_ms);
    return fnv1a_add(h, reset);
}

static unsigned long long fnv1a_add_provider(unsigned long long h,
                                            const ProviderQuota *pq,
                                            const char *mode, int with_monthly,
                                            long long now_ms)
{
    char ts[24];

    if (!pq)
        return h;
    h = fnv1a_add_win(h, &pq->five, mode, now_ms);
    h = fnv1a_add_win(h, &pq->seven, mode, now_ms);
    if (with_monthly)
        h = fnv1a_add_win(h, &pq->monthly, mode, now_ms);
    quota_format_timestamp_min(ts, sizeof(ts), pq->has_fetched_at, pq->fetched_at);
    return fnv1a_add(h, ts);
}

unsigned long long quota_display_sig(const QuotaSnapshot *snap,
                                    const char *status, const char *detail)
{
    unsigned long long h = 0xcbf29ce484222325ULL;
    char ts[24];
    char pct[16];
    long long now_ms;
    const char *mode;

    if (!snap)
        return fnv1a_add(fnv1a_add(h, status), detail);

    now_ms = (long long)time(NULL) * 1000LL;
    mode = snap->display_mode;
    h = fnv1a_add(h, status);
    h = fnv1a_add(h, detail);
    h = fnv1a_add(h, mode);
    quota_format_percent(pct, sizeof(pct), snap->alert_percent);
    h = fnv1a_add(h, pct);
    quota_format_timestamp_min(ts, sizeof(ts), snap->has_generated_at,
                               snap->generated_at);
    h = fnv1a_add(h, ts);

    h ^= (unsigned long long)(snap->providers.grok & 1);
    h ^= (unsigned long long)(snap->providers.codex & 1) << 1;
    h ^= (unsigned long long)(snap->providers.kimi & 1) << 2;
    h ^= (unsigned long long)(snap->providers.agy & 1) << 3;
    h *= 0x100000001b3ULL;

    if (snap->providers.grok)
        h = fnv1a_add_provider(h, &snap->grok, mode, 1, now_ms);
    if (snap->providers.codex)
        h = fnv1a_add_provider(h, &snap->codex, mode, 0, now_ms);
    if (snap->providers.kimi)
        h = fnv1a_add_provider(h, &snap->kimi, mode, 0, now_ms);
    if (snap->providers.agy) {
        quota_format_timestamp_min(ts, sizeof(ts), snap->agy.has_fetched_at,
                                   snap->agy.fetched_at);
        h = fnv1a_add(h, ts);
        h = fnv1a_add_provider(h, &snap->agy.gemini, mode, 1, now_ms);
        h = fnv1a_add_provider(h, &snap->agy.third_party, mode, 1, now_ms);
    }
    return h;
}
