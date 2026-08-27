#ifndef AIQUOTA_QUOTA_H
#define AIQUOTA_QUOTA_H

#include <stddef.h>

typedef struct {
    int valid;            /* window object existed and had numeric used */
    double used;          /* clamped 0..100, always "used percent" semantics */
    int has_reset_at;
    long long reset_at;   /* unix epoch ms */
    int rate_limited;     /* status == "rate-limited" (currently OpenCode only) */
} QuotaWindow;

typedef struct {
    int has_fetched_at;
    long long fetched_at; /* unix epoch ms */
    QuotaWindow five;
    QuotaWindow seven;
    QuotaWindow monthly;  /* Grok/OpenCode; invalid for providers without it */
} ProviderQuota;

typedef struct {
    char display_mode[12];        /* "used" | "remaining" */
    double alert_percent;         /* clamped, default 85 */
    int has_generated_at;
    long long generated_at;       /* unix epoch ms */
    ProviderQuota grok, claude, codex, kimi, opencode;
    struct {
        int has_fetched_at;
        long long fetched_at;
        ProviderQuota gemini, third_party;
    } agy;
} QuotaSnapshot;

void quota_empty_snapshot(QuotaSnapshot *snap);
/* Parse + validate with quotadata.lua fetch() semantics. Returns 0 on valid JSON object, -1 otherwise. */
int  quota_parse_snapshot(const char *json, QuotaSnapshot *snap);
/* 100 - used when mode is "remaining"; caller guarantees w->valid. */
double quota_display_value(const QuotaWindow *w, const char *display_mode);
double quota_clamp(double v);
/* Ports of quotadata.lua formatPercent / formatReset / formatTimestamp. */
void quota_format_percent(char *buf, size_t n, double value);
void quota_format_reset(char *buf, size_t n, const QuotaWindow *w, long long now_ms);
void quota_format_timestamp(char *buf, size_t n, int has_value, long long ms);

#endif
