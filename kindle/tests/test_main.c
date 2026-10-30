/* Host-side unit tests for the device-independent modules (config/quota/http/input/net).
 *
 * `make test` runs this binary with an isolated fixture server. To run a
 * compiled binary directly with the same fixture:
 *
 *     python3 tests/run_host_tests.py /path/to/aiquota_test
 *
 * The 200 case fetches /index.txt; the 404 case fetches /no-such-file.
 * The runner sets AIQUOTA_TEST_BASE to its ephemeral loopback URL.
 */
/* fork/pipe/alarm/waitpid for the input_wait_tap harness under -std=c99. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <linux/input.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "http.h"
#include "input.h"
#include "light.h"
#include "net.h"
#include "quota.h"
#include "ui.h"
#include "watchdog.h"
#include "mono.h"

static int failures = 0;
static const char *current = "";

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL [%s] line %d: %s\n", current, __LINE__, #cond);     \
            failures++;                                                      \
        }                                                                    \
    } while (0)

#define RUN(fn)  \
    do {         \
        current = #fn; \
        fn();          \
    } while (0)

static int dbl_eq(double a, double b)
{
    double d = a - b;
    if (d < 0)
        d = -d;
    return d < 1e-9;
}

/* Matches "NN-NN NN:NN" (with_seconds=0) or "NN-NN NN:NN:SS" (with_seconds=1). */
static int match_ts(const char *s, int with_seconds)
{
    size_t expect = with_seconds ? 14 : 11;
    size_t i;
    if (strlen(s) != expect)
        return 0;
    for (i = 0; i < expect; i++) {
        char c = s[i];
        if (i == 2) {
            if (c != '-')
                return 0;
        } else if (i == 5) {
            if (c != ' ')
                return 0;
        } else if (i == 8 || (with_seconds && i == 11)) {
            if (c != ':')
                return 0;
        } else if (c < '0' || c > '9') {
            return 0;
        }
    }
    return 1;
}

/* Full example JSON from docs/superpowers/plans/usage-api.md. */
static const char *EXAMPLE_JSON =
    "{\n"
    "  \"displayMode\": \"remaining\",\n"
    "  \"alertPercent\": 85,\n"
    "  \"generatedAt\": 1785695952333,\n"
    "  \"providers\": {\n"
    "    \"grok\": true,\n"
    "    \"codex\": true,\n"
    "    \"kimi\": true,\n"
    "    \"agy\": true\n"
    "  },\n"
    "  \"grok\": {\n"
    "    \"fetchedAt\": 1785695890000,\n"
    "    \"five\": null,\n"
    "    \"seven\": { \"used\": 8, \"resetAt\": 1788408060000 },\n"
    "    \"monthly\": null\n"
    "  },\n"
    "  \"codex\": {\n"
    "    \"fetchedAt\": 1785695910000,\n"
    "    \"five\": { \"used\": 12, \"resetAt\": 1785712676000 },\n"
    "    \"seven\": { \"used\": 34, \"resetAt\": 1786300383000 }\n"
    "  },\n"
    "  \"kimi\": {\n"
    "    \"fetchedAt\": 1785695950844,\n"
    "    \"five\": { \"used\": 2, \"resetAt\": 1785709259452 },\n"
    "    \"seven\": { \"used\": 25, \"resetAt\": 1786000859452 }\n"
    "  },\n"
    "  \"agy\": {\n"
    "    \"fetchedAt\": 1785695952333,\n"
    "    \"gemini\": {\n"
    "      \"five\": { \"used\": 0.67791, \"resetAt\": 1785712676000 },\n"
    "      \"seven\": { \"used\": 0.135493, \"resetAt\": 1785835207000 }\n"
    "    },\n"
    "    \"thirdParty\": {\n"
    "      \"five\": { \"used\": 0, \"resetAt\": 1785713952000 },\n"
    "      \"seven\": { \"used\": 0, \"resetAt\": 1786300752000 }\n"
    "    }\n"
    "  }\n"
    "}";

static void test_normalize_url(void)
{
    char buf[512];

    config_normalize_url(buf, sizeof(buf), "");
    CHECK(strcmp(buf, "http://127.0.0.1:46927/api/usage") == 0);

    config_normalize_url(buf, sizeof(buf), " 127.0.0.1:46927/ ");
    CHECK(strcmp(buf, "http://127.0.0.1:46927/api/usage") == 0);

    config_normalize_url(buf, sizeof(buf), "http://h/api/usage");
    CHECK(strcmp(buf, "http://h/api/usage") == 0);

    config_normalize_url(buf, sizeof(buf), "http://h/x");
    CHECK(strcmp(buf, "http://h/x/api/usage") == 0);

    config_normalize_url(buf, sizeof(buf), "https://h:8443/api/usage/");
    CHECK(strcmp(buf, "https://h:8443/api/usage") == 0);

    config_normalize_url(buf, sizeof(buf),
                         "h:8787?providers=grok,codex");
    CHECK(strcmp(buf, "http://h:8787/api/usage?providers=grok,codex") == 0);

    config_normalize_url(buf, sizeof(buf),
                         "http://h/api/usage?providers=grok&providers=kimi");
    CHECK(strcmp(buf,
                 "http://h/api/usage?providers=grok&providers=kimi") == 0);

    config_normalize_url(buf, sizeof(buf), "http://h/x/?tools=codex");
    CHECK(strcmp(buf, "http://h/x/api/usage?tools=codex") == 0);

    /* Tiny dst: truncates safely and stays NUL-terminated. */
    {
        char tiny[8];
        memset(tiny, 0xAA, sizeof(tiny));
        config_normalize_url(tiny, sizeof(tiny), "http://example.com/api/usage");
        CHECK(strlen(tiny) == sizeof(tiny) - 1);
        CHECK(strcmp(tiny, "http://") == 0);
    }
}

static void test_config_roundtrip(void)
{
    const char *path = "/tmp/aiquota_cfg_test.ini";
    AppConfig cfg;
    AppConfig back;
    FILE *f;

    config_defaults(&cfg);
    CHECK(strcmp(cfg.api_url, "http://127.0.0.1:46927/api/usage") == 0);
    CHECK(cfg.touch_swap_xy == 0 && cfg.touch_mirror_x == 0 && cfg.touch_mirror_y == 0);
    CHECK(cfg.wifi_auto_off == CONFIG_WIFI_AUTO_OFF_DEFAULT);
    CHECK(cfg.auto_refresh_min == CONFIG_AUTO_REFRESH_DEFAULT);

    /* Missing file: -1, defaults kept. */
    CHECK(config_load(&cfg, "/tmp/aiquota_no_such_file_9f3b2.ini") == -1);
    CHECK(strcmp(cfg.api_url, "http://127.0.0.1:46927/api/usage") == 0);
    CHECK(cfg.touch_swap_xy == 0);

    /* Save/load roundtrip with non-default touch flags. */
    strcpy(cfg.api_url, "http://example.com:1234/api/usage");
    cfg.touch_swap_xy = 1;
    cfg.touch_mirror_x = 1;
    cfg.touch_mirror_y = 0;
    cfg.wifi_auto_off = 0;
    cfg.auto_refresh_min = 15;
    CHECK(config_save(&cfg, path) == 0);

    config_defaults(&back);
    CHECK(config_load(&back, path) == 0);
    CHECK(strcmp(back.api_url, cfg.api_url) == 0);
    CHECK(back.touch_swap_xy == 1);
    CHECK(back.touch_mirror_x == 1);
    CHECK(back.touch_mirror_y == 0);
    CHECK(back.wifi_auto_off == 0);
    CHECK(back.auto_refresh_min == 15);

    /* Auto-refresh: non-positive is "off", anything above the cap saturates. */
    CHECK(config_clamp_auto_refresh(0) == 0);
    CHECK(config_clamp_auto_refresh(-7) == 0);
    CHECK(config_clamp_auto_refresh(1) == 1);
    CHECK(config_clamp_auto_refresh(7) == 7);
    CHECK(config_clamp_auto_refresh(999999) == CONFIG_AUTO_REFRESH_MAX);

    /* Loaded api_url goes through normalizeUrl; new keys parse + clamp from
     * raw text (auto_refresh_min over the cap lands on the cap). */
    f = fopen(path, "w");
    CHECK(f != NULL);
    if (f) {
        fputs("api_url=example.com:9\ntouch_mirror_y=1\n"
              "wifi_auto_off=0\nauto_refresh_min=999\nunknown_key=zzz\n", f);
        fclose(f);
        config_defaults(&back);
        CHECK(config_load(&back, path) == 0);
        CHECK(strcmp(back.api_url, "http://example.com:9/api/usage") == 0);
        CHECK(back.touch_mirror_y == 1);
        CHECK(back.touch_swap_xy == 0);
        CHECK(back.wifi_auto_off == 0);
        CHECK(back.auto_refresh_min == CONFIG_AUTO_REFRESH_MAX);
    }

    /* Junk values parse as off; the legacy idle_timeout_s key from older
     * builds is just an unknown key now and must load (and roundtrip-drop)
     * without touching anything. */
    f = fopen(path, "w");
    CHECK(f != NULL);
    if (f) {
        fputs("wifi_auto_off=abc\nauto_refresh_min=-3\nidle_timeout_s=60\n", f);
        fclose(f);
        config_defaults(&back);
        CHECK(config_load(&back, path) == 0);
        CHECK(back.wifi_auto_off == 0); /* atoi("abc")==0 -> false -> 0 */
        CHECK(back.auto_refresh_min == 0);

        CHECK(config_save(&back, path) == 0);
        f = fopen(path, "r");
        CHECK(f != NULL);
        if (f) {
            char filebuf[512];

            CHECK(fgets(filebuf, sizeof(filebuf), f) != NULL);
            CHECK(strstr(filebuf, "api_url=") == filebuf);
            /* The stale key is dropped on save, not carried forward. */
            while (fgets(filebuf, sizeof(filebuf), f) != NULL)
                CHECK(strstr(filebuf, "idle_timeout_s") == NULL);
            fclose(f);
        }
    }
}

static void test_config_save_atomic(void)
{
    const char *path = "/tmp/aiquota_cfg_atomic.ini";
    const char *tmp = "/tmp/aiquota_cfg_atomic.ini.tmp";
    const char *bak = "/tmp/aiquota_cfg_atomic.ini.bak";
    AppConfig cfg;
    AppConfig back;
    FILE *f;
    char buf[256];

    unlink(path);
    unlink(tmp);
    unlink(bak);

    config_defaults(&cfg);
    strcpy(cfg.api_url, "http://keep.example/api/usage");
    cfg.touch_swap_xy = 1;
    CHECK(config_save(&cfg, path) == 0);
    CHECK(access(path, R_OK) == 0);
    /* Temp must not linger after a successful save. */
    CHECK(access(tmp, F_OK) != 0);

    /* Second save keeps a .bak of the previous good file. */
    strcpy(cfg.api_url, "http://new.example/api/usage");
    cfg.touch_swap_xy = 0;
    CHECK(config_save(&cfg, path) == 0);
    CHECK(access(bak, R_OK) == 0);
    f = fopen(bak, "r");
    CHECK(f != NULL);
    if (f) {
        CHECK(fgets(buf, sizeof(buf), f) != NULL);
        CHECK(strstr(buf, "http://keep.example/api/usage") != NULL);
        fclose(f);
    }

    config_defaults(&back);
    CHECK(config_load(&back, path) == 0);
    CHECK(strcmp(back.api_url, "http://new.example/api/usage") == 0);
    CHECK(back.touch_swap_xy == 0);

    /* Refuse a path that cannot be written: existing file stays readable and
     * the caller must see failure (never a silent truncate). */
    {
        const char *ro = "/tmp/aiquota_cfg_readonly.ini";
        AppConfig victim;

        unlink(ro);
        config_defaults(&victim);
        strcpy(victim.api_url, "http://old.example/api/usage");
        victim.wifi_auto_off = 0;
        CHECK(config_save(&victim, ro) == 0);

        /* Make the directory briefly unwritable by aiming at a non-dir parent
         * component: saving under a file-as-directory must fail. */
        CHECK(config_save(&victim, "/tmp/aiquota_cfg_readonly.ini/nested") != 0);

        config_defaults(&back);
        CHECK(config_load(&back, ro) == 0);
        CHECK(strcmp(back.api_url, "http://old.example/api/usage") == 0);
        CHECK(back.wifi_auto_off == 0);
        unlink(ro);
        unlink("/tmp/aiquota_cfg_readonly.ini.bak");
    }

    /* Empty / null path must fail without crashing. */
    CHECK(config_save(&cfg, "") != 0);
    CHECK(config_save(&cfg, NULL) != 0);

    unlink(path);
    unlink(bak);
    unlink(tmp);
}

static void test_parse_full_example(void)
{
    QuotaSnapshot snap;

    CHECK(quota_parse_snapshot(EXAMPLE_JSON, &snap) == 0);

    CHECK(strcmp(snap.display_mode, "remaining") == 0);
    CHECK(dbl_eq(snap.alert_percent, 85.0));
    CHECK(snap.has_generated_at == 1);
    CHECK(snap.generated_at == 1785695952333LL);
    CHECK(snap.providers.grok == 1);
    CHECK(snap.providers.codex == 1);
    CHECK(snap.providers.kimi == 1);
    CHECK(snap.providers.agy == 1);

    CHECK(snap.grok.has_fetched_at == 1);
    CHECK(snap.grok.fetched_at == 1785695890000LL);
    CHECK(snap.grok.five.valid == 0);
    CHECK(snap.grok.seven.valid == 1 && dbl_eq(snap.grok.seven.used, 8.0));
    CHECK(snap.grok.monthly.valid == 0);

    CHECK(snap.codex.has_fetched_at == 1);
    CHECK(snap.codex.fetched_at == 1785695910000LL);
    CHECK(snap.codex.five.valid == 1 && dbl_eq(snap.codex.five.used, 12.0));
    CHECK(snap.codex.seven.valid == 1 && dbl_eq(snap.codex.seven.used, 34.0));
    CHECK(snap.kimi.five.valid == 1 && dbl_eq(snap.kimi.five.used, 2.0));
    CHECK(snap.kimi.five.reset_at == 1785709259452LL);
    CHECK(snap.kimi.seven.valid == 1 && dbl_eq(snap.kimi.seven.used, 25.0));

    CHECK(snap.agy.has_fetched_at == 1);
    CHECK(snap.agy.fetched_at == 1785695952333LL);
    CHECK(snap.agy.gemini.has_fetched_at == 1);
    CHECK(snap.agy.gemini.five.valid == 1 && dbl_eq(snap.agy.gemini.five.used, 0.67791));
    CHECK(snap.agy.gemini.seven.valid == 1 && dbl_eq(snap.agy.gemini.seven.used, 0.135493));
    CHECK(snap.agy.gemini.seven.reset_at == 1785835207000LL);

    /* used == 0 is a valid value (type check, not truthiness). */
    CHECK(snap.agy.third_party.five.valid == 1);
    CHECK(dbl_eq(snap.agy.third_party.five.used, 0.0));
    CHECK(snap.agy.third_party.five.reset_at == 1785713952000LL);
    CHECK(snap.agy.third_party.seven.valid == 1);
    CHECK(dbl_eq(snap.agy.third_party.seven.used, 0.0));
}

static void test_parse_provider_selection(void)
{
    QuotaSnapshot snap;
    const char *json =
        "{ \"providers\": {"
        " \"grok\": true, \"codex\": false,"
        " \"kimi\": true, \"agy\": false } }";

    CHECK(quota_parse_snapshot(json, &snap) == 0);
    CHECK(snap.providers.grok == 1);
    CHECK(snap.providers.codex == 0);
    CHECK(snap.providers.kimi == 1);
    CHECK(snap.providers.agy == 0);

    /* Missing map/keys and malformed values retain the backward-compatible
     * all-enabled default instead of unexpectedly hiding cards. */
    CHECK(quota_parse_snapshot(
        "{ \"providers\": { \"grok\": false, \"codex\": \"false\" } }",
        &snap) == 0);
    CHECK(snap.providers.grok == 0);
    CHECK(snap.providers.codex == 1);
    CHECK(snap.providers.kimi == 1);
    CHECK(snap.providers.agy == 1);

    CHECK(quota_parse_snapshot("{}", &snap) == 0);
    CHECK(snap.providers.grok == 1 && snap.providers.agy == 1);
}

static void test_parse_window_status(void)
{
    QuotaSnapshot snap;
    const char *json =
        "{ \"codex\": {"
        " \"five\": { \"used\": 100, \"status\": \"rate-limited\" },"
        " \"seven\": { \"used\": 34, \"status\": \"ok\" } } }";

    CHECK(quota_parse_snapshot(json, &snap) == 0);
    CHECK(snap.codex.five.valid == 1);
    CHECK(dbl_eq(snap.codex.five.used, 100.0));
    CHECK(snap.codex.five.rate_limited == 1);
    CHECK(snap.codex.seven.valid == 1);
    CHECK(snap.codex.seven.rate_limited == 0);
}

static void test_parse_kimi_null(void)
{
    /* kimi provider null must not affect any other card. */
    const char *json =
        "{ \"displayMode\": \"used\","
        " \"codex\": { \"five\": { \"used\": 18, \"resetAt\": 1785712676000 },"
        "             \"seven\": { \"used\": 42, \"resetAt\": 1786300383000 } },"
        " \"kimi\": null,"
        " \"agy\": { \"gemini\": { \"five\": null, \"seven\": null },"
        "          \"thirdParty\": { \"five\": { \"used\": 0, \"resetAt\": 1785713952000 },"
        "                          \"seven\": { \"used\": 0, \"resetAt\": 1786300752000 } } } }";
    QuotaSnapshot snap;

    CHECK(quota_parse_snapshot(json, &snap) == 0);
    CHECK(snap.kimi.five.valid == 0);
    CHECK(snap.kimi.seven.valid == 0);
    CHECK(snap.codex.five.valid == 1 && dbl_eq(snap.codex.five.used, 18.0));
    CHECK(snap.codex.seven.valid == 1 && dbl_eq(snap.codex.seven.used, 42.0));
    CHECK(snap.agy.gemini.five.valid == 0);
    CHECK(snap.agy.gemini.seven.valid == 0);
    CHECK(snap.agy.third_party.five.valid == 1 && dbl_eq(snap.agy.third_party.five.used, 0.0));
    CHECK(snap.alert_percent == 85.0); /* missing -> default 85 */
    CHECK(snap.has_generated_at == 0);
    CHECK(strcmp(snap.display_mode, "used") == 0);
}

static void test_parse_remaining_mode(void)
{
    const char *json =
        "{ \"displayMode\": \"remaining\","
        " \"codex\": { \"five\": { \"used\": 42 } } }";
    QuotaSnapshot snap;

    CHECK(quota_parse_snapshot(json, &snap) == 0);
    CHECK(strcmp(snap.display_mode, "remaining") == 0);
    CHECK(snap.alert_percent == 85.0);
    CHECK(snap.codex.five.valid == 1 && dbl_eq(snap.codex.five.used, 42.0));
    CHECK(dbl_eq(quota_display_value(&snap.codex.five, snap.display_mode), 58.0));
    CHECK(dbl_eq(quota_display_value(&snap.codex.five, "used"), 42.0));

    CHECK(quota_parse_snapshot(
        "{ \"displayMode\": \"used\", \"codex\": { \"five\": { \"used\": 42 } } }",
        &snap) == 0);
    CHECK(strcmp(snap.display_mode, "used") == 0);
    CHECK(dbl_eq(quota_display_value(&snap.codex.five, snap.display_mode), 42.0));

    CHECK(quota_parse_snapshot("{}", &snap) == 0);
    CHECK(strcmp(snap.display_mode, "remaining") == 0);

    /* Non-numeric used -> window invalid. */
    CHECK(quota_parse_snapshot("{ \"codex\": { \"five\": { \"used\": \"42\" } } }", &snap) == 0);
    CHECK(snap.codex.five.valid == 0);
}

static void test_parse_garbage(void)
{
    QuotaSnapshot snap;

    CHECK(quota_parse_snapshot("not json", &snap) == -1);
    CHECK(quota_parse_snapshot("[1,2,3]", &snap) == -1);
    CHECK(quota_parse_snapshot("\"hello\"", &snap) == -1);
    CHECK(quota_parse_snapshot("", &snap) == -1);
    CHECK(quota_parse_snapshot("{} trailing", &snap) == -1);
    CHECK(quota_parse_snapshot("{} \n\t", &snap) == 0);
}

static void test_parse_out_of_range(void)
{
    /* cJSON accepts 1e300; casting that double to long long would be UB, so
     * out-of-range timestamps are treated as absent while the window that
     * carries them stays valid. */
    const char *json =
        "{ \"generatedAt\": 1e300,"
        " \"codex\": { \"five\": { \"used\": 18, \"resetAt\": 1e300 },"
        "             \"seven\": { \"used\": 42, \"resetAt\": -1e300 } } }";
    QuotaSnapshot snap;

    CHECK(quota_parse_snapshot(json, &snap) == 0);
    CHECK(snap.has_generated_at == 0);
    CHECK(snap.codex.five.valid == 1);
    CHECK(snap.codex.five.has_reset_at == 0);
    CHECK(snap.codex.seven.valid == 1);
    CHECK(snap.codex.seven.has_reset_at == 0);

    /* In-range values still parse. */
    CHECK(quota_parse_snapshot("{ \"generatedAt\": 1785695952333 }", &snap) == 0);
    CHECK(snap.has_generated_at == 1);
    CHECK(snap.generated_at == 1785695952333LL);
}

static void test_format_percent(void)
{
    char buf[32];

    quota_format_percent(buf, sizeof(buf), 42.0);
    CHECK(strcmp(buf, "42%") == 0);
    quota_format_percent(buf, sizeof(buf), 0.67791);
    CHECK(strcmp(buf, "0.7%") == 0);
    quota_format_percent(buf, sizeof(buf), 42.5);
    CHECK(strcmp(buf, "42.5%") == 0);
    quota_format_percent(buf, sizeof(buf), 99.97);
    CHECK(strcmp(buf, "100%") == 0);
}

static void test_format_reset(void)
{
    const long long now = 1785695952333LL; /* fixed epoch ms, timezone independent */
    char buf[64];
    QuotaWindow w;

    w.valid = 1;
    w.used = 0.0;

    w.has_reset_at = 0;
    w.reset_at = 0;
    quota_format_reset(buf, sizeof(buf), &w, now);
    CHECK(strcmp(buf, "重置时间未知") == 0);

    w.has_reset_at = 1;
    w.reset_at = now - 1000;
    quota_format_reset(buf, sizeof(buf), &w, now);
    CHECK(strcmp(buf, "即将重置") == 0);

    w.reset_at = now;
    quota_format_reset(buf, sizeof(buf), &w, now);
    CHECK(strcmp(buf, "即将重置") == 0);

    w.reset_at = now + 30000LL; /* 30 s -> max(1, ceil(0.5)) = 1 minute */
    quota_format_reset(buf, sizeof(buf), &w, now);
    CHECK(strcmp(buf, "1 分钟后") == 0);

    w.reset_at = now + 300000LL; /* 300 s */
    quota_format_reset(buf, sizeof(buf), &w, now);
    CHECK(strcmp(buf, "5 分钟后") == 0);

    w.reset_at = now + 4200000LL; /* 4200 s */
    quota_format_reset(buf, sizeof(buf), &w, now);
    CHECK(strcmp(buf, "1 小时 10 分后") == 0);

    w.reset_at = now + 93600000LL; /* 93600 s */
    quota_format_reset(buf, sizeof(buf), &w, now);
    CHECK(strcmp(buf, "1 天 2 小时后") == 0);

    /* >= 7 days: absolute local time; assert the shape, not the value (host TZ varies). */
    w.reset_at = now + 8LL * 86400 * 1000;
    quota_format_reset(buf, sizeof(buf), &w, now);
    CHECK(match_ts(buf, 0));
}

static void test_format_timestamp(void)
{
    char buf[64];

    quota_format_timestamp(buf, sizeof(buf), 0, 0);
    CHECK(strcmp(buf, "--") == 0);
    quota_format_timestamp(buf, sizeof(buf), 1, 1785695952333LL);
    CHECK(match_ts(buf, 1));
}

static const char *test_url(const char *path)
{
    static char url[512];
    const char *base = getenv("AIQUOTA_TEST_BASE");
    if (!base) {
        fprintf(stderr, "Run with tests/run_host_tests.py (AIQUOTA_TEST_BASE missing)\n");
        exit(1);
    }
    snprintf(url, sizeof(url), "%s%s", base, path);
    return url;
}

static void test_http_get(void)
{
    int status = 0;
    char err[128];
    char *body;

    body = http_get(test_url("/index.txt"), &status, err, sizeof(err));
    CHECK(body != NULL);
    if (body) {
        CHECK(status == 200);
        CHECK(strstr(body, "hello-aiquota") != NULL);
        free(body);
    }

    body = http_get(test_url("/index.txt?providers=grok,codex"),
                    &status, err, sizeof(err));
    CHECK(body != NULL);
    if (body) {
        CHECK(status == 200);
        CHECK(strstr(body, "hello-aiquota") != NULL);
        free(body);
    }

    /* Non-200 is not a transport failure: body returned, status carries the code. */
    status = 0;
    body = http_get(test_url("/no-such-file"), &status, err, sizeof(err));
    CHECK(body != NULL);
    if (body) {
        CHECK(status == 404);
        free(body);
    }

    err[0] = '\0';
    body = http_get("http://127.0.0.1:9/", &status, err, sizeof(err));
    CHECK(body == NULL);
    CHECK(err[0] != '\0');

    err[0] = '\0';
    body = http_get("https://127.0.0.1:18080/", &status, err, sizeof(err));
    CHECK(body == NULL);
    CHECK(err[0] != '\0');

    err[0] = '\0';
    body = http_get("ftp://x/", &status, err, sizeof(err));
    CHECK(body == NULL);
    CHECK(err[0] != '\0');
}

static void check_transform(int swap, int mirror_x, int mirror_y,
                            int scr_w, int scr_h,
                            long raw_x, long raw_y,
                            long min_x, long max_x, long min_y, long max_y,
                            int want_x, int want_y)
{
    AppConfig cfg;
    int x = -1;
    int y = -1;

    config_defaults(&cfg);
    cfg.touch_swap_xy = swap;
    cfg.touch_mirror_x = mirror_x;
    cfg.touch_mirror_y = mirror_y;
    input_transform(&cfg, scr_w, scr_h, raw_x, raw_y,
                    min_x, max_x, min_y, max_y, &x, &y);
    CHECK(x == want_x);
    CHECK(y == want_y);
}

static void test_input_transform(void)
{
    /* Identity mapping onto a 600x800 screen, raw range 0..4095. */
    check_transform(0, 0, 0, 600, 800, 0, 0, 0, 4095, 0, 4095, 0, 0);
    check_transform(0, 0, 0, 600, 800, 4095, 4095, 0, 4095, 0, 4095, 599, 799);
    check_transform(0, 0, 0, 600, 800, 2048, 2048, 0, 4095, 0, 4095, 299, 399);

    /* swap_xy: raw axes (and their ranges) swap BEFORE scaling, so a
     * non-square panel maps against the correct target dimension.
     * Regression: the old scale-then-swap path produced (0,599)/(399,299). */
    check_transform(1, 0, 0, 600, 800, 4095, 0, 0, 4095, 0, 4095, 0, 799);
    check_transform(1, 0, 0, 600, 800, 2048, 2048, 0, 4095, 0, 4095, 299, 399);
    check_transform(1, 0, 0, 600, 800, 0, 4095, 0, 4095, 0, 4095, 599, 0);
    check_transform(1, 0, 0, 600, 800, 0, 0, 0, 4095, 0, 4095, 0, 0);
    check_transform(1, 0, 0, 600, 800, 4095, 4095, 0, 4095, 0, 4095, 599, 799);

    /* mirror_x / mirror_y flip within the screen bounds. */
    check_transform(0, 1, 0, 600, 800, 0, 0, 0, 4095, 0, 4095, 599, 0);
    check_transform(0, 0, 1, 600, 800, 0, 0, 0, 4095, 0, 4095, 0, 799);
    check_transform(0, 1, 1, 600, 800, 0, 0, 0, 4095, 0, 4095, 599, 799);

    /* All swap/mirror combinations on a non-square panel (corners). */
    check_transform(1, 1, 0, 600, 800, 4095, 0, 0, 4095, 0, 4095, 599, 799);
    check_transform(1, 0, 1, 600, 800, 4095, 0, 0, 4095, 0, 4095, 0, 0);
    check_transform(1, 1, 1, 600, 800, 4095, 0, 0, 4095, 0, 4095, 599, 0);
    check_transform(1, 1, 1, 600, 800, 0, 4095, 0, 4095, 0, 4095, 0, 799);

    /* swap + both mirrors on a square screen stays exact. */
    check_transform(1, 1, 1, 800, 800, 0, 4095, 0, 4095, 0, 4095, 0, 799);

    /* Raw values outside the reported range clamp to the edges. */
    check_transform(0, 0, 0, 600, 800, 5000, -100, 0, 4095, 0, 4095, 599, 0);
    check_transform(1, 0, 0, 600, 800, 5000, -100, 0, 4095, 0, 4095, 0, 799);

    /* max == min must not divide by zero; the dead axis maps to 0. */
    check_transform(0, 0, 0, 600, 800, 100, 2048, 100, 100, 0, 4095, 0, 399);
    check_transform(1, 0, 0, 600, 800, 100, 2048, 100, 100, 0, 4095, 299, 0);
}

static void test_light_clamp_and_no_lipc(void)
{
    CHECK(light_clamp(-7, 24) == 0);
    CHECK(light_clamp(0, 24) == 0);
    CHECK(light_clamp(12, 24) == 12);
    CHECK(light_clamp(24, 24) == 24);
    CHECK(light_clamp(99, 24) == 24);
    /* max < 1 falls back to LIGHT_MAX_FALLBACK (24). */
    CHECK(light_clamp(5, 0) == 5);
    CHECK(light_clamp(99, -3) == LIGHT_MAX_FALLBACK);
    CHECK(light_clamp(-1, 0) == 0);

    /* Host has no lipc: get/set must return -1 without crashing. */
    CHECK(light_get_intensity() == -1);
    CHECK(light_get_max() == -1);
    CHECK(light_set_intensity(10) == -1);
    CHECK(light_set_intensity(0) == -1);

    /* Idle-off bookkeeping degrades to a no-op without lipc: nothing can be
     * dimmed, nothing stays remembered, restore/cancel stay safe. */
    CHECK(light_idle_off() == 0);
    CHECK(light_idle_level() == 0);
    CHECK(light_idle_restore() == 0);
    light_idle_cancel();
    CHECK(light_idle_level() == 0);

    /* The last-on slot only takes positive levels through
     * light_set_last_on; 0/negative never clobber it (a descent to off must
     * not decay the restore target). */
    CHECK(light_last_on() == 0);
    light_set_last_on(0);
    light_set_last_on(-3);
    CHECK(light_last_on() == 0);
    light_set_last_on(7);
    CHECK(light_last_on() == 7);
    light_set_last_on(0);
    CHECK(light_last_on() == 7);
    light_set_last_on(15);
    CHECK(light_last_on() == 15);
    /* A failed set must not record: no lipc on host. */
    CHECK(light_set_intensity(3) == -1);
    CHECK(light_last_on() == 15);
}

static void test_ui_batt_cells(void)
{
    CHECK(ui_batt_cells(-1) == -1);
    CHECK(ui_batt_cells(101) == -1);
    CHECK(ui_batt_cells(-100) == -1);
    CHECK(ui_batt_cells(0) == 0);
    CHECK(ui_batt_cells(5) == 0);
    CHECK(ui_batt_cells(10) == 0);
    CHECK(ui_batt_cells(11) == 1);
    CHECK(ui_batt_cells(29) == 1);
    CHECK(ui_batt_cells(30) == 2);
    CHECK(ui_batt_cells(49) == 2);
    CHECK(ui_batt_cells(50) == 3);
    CHECK(ui_batt_cells(69) == 3);
    CHECK(ui_batt_cells(70) == 4);
    CHECK(ui_batt_cells(89) == 4);
    CHECK(ui_batt_cells(90) == 5);
    CHECK(ui_batt_cells(100) == 5);
}

static void test_ui_cards_from_providers(void)
{
    ProviderSelection p;
    UiCard ids[UI_CARD_SLOTS];
    int n;

    memset(&p, 0, sizeof(p));
    n = ui_cards_from_providers(&p, ids, UI_CARD_SLOTS);
    CHECK(n == 1);
    CHECK(ids[0] == UI_CARD_INFO);

    p.grok = 1;
    p.kimi = 1;
    n = ui_cards_from_providers(&p, ids, UI_CARD_SLOTS);
    CHECK(n == 3);
    CHECK(ids[0] == UI_CARD_GROK);
    CHECK(ids[1] == UI_CARD_KIMI);
    CHECK(ids[2] == UI_CARD_INFO);

    memset(&p, 0, sizeof(p));
    p.agy = 1;
    n = ui_cards_from_providers(&p, ids, UI_CARD_SLOTS);
    CHECK(n == 3);
    CHECK(ids[0] == UI_CARD_AGY_GEMINI);
    CHECK(ids[1] == UI_CARD_AGY_THIRD);
    CHECK(ids[2] == UI_CARD_INFO);

    p.grok = 1;
    p.codex = 1;
    p.kimi = 1;
    n = ui_cards_from_providers(&p, ids, UI_CARD_SLOTS);
    CHECK(n == 6);
    CHECK(ids[0] == UI_CARD_GROK);
    CHECK(ids[1] == UI_CARD_CODEX);
    CHECK(ids[2] == UI_CARD_KIMI);
    CHECK(ids[3] == UI_CARD_AGY_GEMINI);
    CHECK(ids[4] == UI_CARD_AGY_THIRD);
    CHECK(ids[5] == UI_CARD_INFO);

    /* Slot number is not identity: dropping Codex must compact. */
    p.codex = 0;
    n = ui_cards_from_providers(&p, ids, UI_CARD_SLOTS);
    CHECK(n == 5);
    CHECK(ids[0] == UI_CARD_GROK);
    CHECK(ids[1] == UI_CARD_KIMI);
    CHECK(ids[2] == UI_CARD_AGY_GEMINI);
    CHECK(ids[3] == UI_CARD_AGY_THIRD);
    CHECK(ids[4] == UI_CARD_INFO);

    CHECK(ui_cards_from_providers(&p, ids, 1) == 1);
    CHECK(ids[0] == UI_CARD_GROK);
    CHECK(ui_cards_from_providers(NULL, ids, UI_CARD_SLOTS) == 0);
    CHECK(ui_cards_from_providers(&p, NULL, UI_CARD_SLOTS) == 0);
}

static void test_ui_rect_contains(void)
{
    FBInkRect r;

    r.left = 10;
    r.top = 20;
    r.width = 100;
    r.height = 50;
    CHECK(ui_rect_contains(&r, 10, 20));
    CHECK(ui_rect_contains(&r, 109, 69));
    CHECK(!ui_rect_contains(&r, 110, 20));
    CHECK(!ui_rect_contains(&r, 10, 70));
    CHECK(!ui_rect_contains(&r, 9, 20));
    CHECK(!ui_rect_contains(&r, 10, 19));
    CHECK(!ui_rect_contains(NULL, 10, 20));
}

static void test_ui_hit_card(void)
{
    Ui ui;

    memset(&ui, 0, sizeof(ui));
    ui.card_rect[0].left = 10;
    ui.card_rect[0].top = 10;
    ui.card_rect[0].width = 100;
    ui.card_rect[0].height = 50;
    ui.card_rect[1].left = 120;
    ui.card_rect[1].top = 10;
    ui.card_rect[1].width = 100;
    ui.card_rect[1].height = 50;
    ui.card_id[0] = UI_CARD_GROK;
    ui.card_id[1] = UI_CARD_INFO;
    ui.card_drawn = 2;

    CHECK(ui_hit_card(&ui, 60, 35) == UI_CARD_GROK);  /* card 1 center */
    CHECK(ui_hit_card(&ui, 170, 35) == UI_CARD_INFO); /* card 2 center */
    CHECK(ui_hit_card(&ui, 300, 35) == UI_CARD_NONE); /* blank space */
    CHECK(ui_hit_card(&ui, 115, 35) == UI_CARD_NONE); /* gap between cards */

    /* A hit inside a rect still misses when nothing was painted. */
    ui.card_drawn = 0;
    CHECK(ui_hit_card(&ui, 60, 35) == UI_CARD_NONE);
}

static void test_net_ensure_wifi_no_lipc(void)
{
    /* The host has no lipc binaries: net_ensure_wifi must return -1
     * without crashing and fail fast (two empty probes ~= 0.5 s), not
     * spin through the full 20 s on-device timeout. */
    time_t start = time(NULL);
    int rc = net_ensure_wifi();

    double elapsed = difftime(time(NULL), start);

    CHECK(rc == -1);
    CHECK(elapsed < 5.0);

    /* The failed ensure raised the radio on-device, so ownership says a
     * release is owed; off-device system() is a harmless no-op. Repeat
     * calls must stay safe (release clears its flag before acting). */
    net_release_wifi();
    net_release_wifi();
}

static void test_net_sanitize_ssid(void)
{
    char s[64];

    /* Plain ASCII passes through untouched. */
    strcpy(s, "HomeNet");
    net_sanitize_ssid(s);
    CHECK(strcmp(s, "HomeNet") == 0);

    /* Surrounding whitespace is trimmed (iw lines end with \n). */
    strcpy(s, "  spaced  ");
    net_sanitize_ssid(s);
    CHECK(strcmp(s, "spaced") == 0);

    /* Non-ASCII bytes cut the name: the subset font has no glyphs for them,
     * and a truncated ASCII head beats tofu boxes. */
    strcpy(s, "caf\xC3\xA9 123"); /* "café 123": é is 2 UTF-8 bytes */
    net_sanitize_ssid(s);
    CHECK(strcmp(s, "caf") == 0);

    strcpy(s, "\xE4\xB8\xAD\xE6\x96\x87"); /* CJK-only name -> nothing usable */
    net_sanitize_ssid(s);
    CHECK(s[0] == '\0');

    strcpy(s, "\n\t");
    net_sanitize_ssid(s);
    CHECK(s[0] == '\0');

    /* Hidden SSID: "SSID:" with an empty value stays empty. */
    strcpy(s, "");
    net_sanitize_ssid(s);
    CHECK(s[0] == '\0');
}

static void test_net_parse_charging(void)
{
    CHECK(net_parse_charging("1") == 1);
    CHECK(net_parse_charging("0") == 0);
    CHECK(net_parse_charging("1\n") == 1);
    CHECK(net_parse_charging("0\r\n") == 0);
    CHECK(net_parse_charging("Charging") == 1);
    CHECK(net_parse_charging("charging\n") == 1);
    CHECK(net_parse_charging("Full") == 1);
    CHECK(net_parse_charging("full") == 1);
    CHECK(net_parse_charging("Discharging") == 0);
    CHECK(net_parse_charging("  1") == 1);
    CHECK(net_parse_charging("") == -1);
    CHECK(net_parse_charging(NULL) == -1);
    CHECK(net_parse_charging("unknown") == -1);

    /* Must not crash; on a Kindle this is 0-100, on the host usually -1. */
    {
        int pct = 99, chg = 99;

        net_battery(&pct, &chg);
        CHECK(pct >= -1 && pct <= 100);
        CHECK(chg >= -1 && chg <= 1);
    }
}

/* --- input_wait_tap harness -------------------------------------------------
 * input_wait_tap is blocking, so each case runs in a forked child with an
 * alarm(2) watchdog: the full event stream is written into a pipe up front
 * (the pipe buffer easily holds a few dozen events), the write end is then
 * closed so a consumed-but-unmatched stream ends in EOF (-1) instead of
 * hanging, and the child exits 0 only when a tap was reported.
 * input_open() is never called here, so the device-range statics stay zeroed
 * (max==min): every coordinate transforms to 0 and only the RETURN VALUE is
 * meaningful; coordinate mapping itself is covered by test_input_transform. */

struct tap_event {
    unsigned short type;
    unsigned short code;
    int value;
};

static int put_tap_event(int fd, const struct tap_event *e)
{
    struct input_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = e->type;
    ev.code = e->code;
    ev.value = e->value;
    return write(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev) ? 0 : -1;
}

/* Returns 1 if a tap was detected, 0 if not (stream exhausted or watchdog
 * fired), -1 on harness failure. */
static int tap_detected(const struct tap_event *events, size_t count,
                        int scr_w, int scr_h)
{
    int pfd[2];
    pid_t pid;
    int status;
    size_t i;

    if (pipe(pfd) != 0)
        return -1;
    for (i = 0; i < count; i++) {
        if (put_tap_event(pfd[1], &events[i]) != 0) {
            close(pfd[0]);
            close(pfd[1]);
            return -1;
        }
    }
    close(pfd[1]); /* child gets EOF once the stream is consumed */

    pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        return -1;
    }
    if (pid == 0) {
        AppConfig cfg;
        int x = -1;
        int y = -1;
        int rc;

        config_defaults(&cfg);
        alarm(2); /* watchdog: a stuck wait fails the case */
        /* timeout_s 0: these cases end on EOF, the timeout has its own test. */
        rc = input_wait_tap(pfd[0], scr_w, scr_h, &cfg, 0, &x, &y);
        _exit(rc == 0 ? 0 : 1);
    }

    close(pfd[0]);
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 1 : 0;
}

static void test_input_wait_tap(void)
{
    /* (a) simple tap: press with position, lift in place -> detected. */
    static const struct tap_event tap_simple[] = {
        { EV_ABS, ABS_MT_TRACKING_ID, 0 },
        { EV_ABS, ABS_MT_POSITION_X, 100 },
        { EV_ABS, ABS_MT_POSITION_Y, 200 },
        { EV_SYN, SYN_REPORT, 0 },
        { EV_ABS, ABS_MT_TRACKING_ID, -1 },
        { EV_SYN, SYN_REPORT, 0 },
    };
    /* (b) drag far over the (scr_w+scr_h)/40 threshold -> not a tap. */
    static const struct tap_event tap_drag[] = {
        { EV_ABS, ABS_MT_TRACKING_ID, 0 },
        { EV_ABS, ABS_MT_POSITION_X, 100 },
        { EV_ABS, ABS_MT_POSITION_Y, 100 },
        { EV_SYN, SYN_REPORT, 0 },
        { EV_ABS, ABS_MT_POSITION_X, 500 },
        { EV_ABS, ABS_MT_POSITION_Y, 500 },
        { EV_SYN, SYN_REPORT, 0 },
        { EV_ABS, ABS_MT_TRACKING_ID, -1 },
        { EV_SYN, SYN_REPORT, 0 },
    };
    /* (c) position arrives one frame after the press -> still a tap. */
    static const struct tap_event tap_late_pos[] = {
        { EV_ABS, ABS_MT_TRACKING_ID, 0 },
        { EV_SYN, SYN_REPORT, 0 },
        { EV_ABS, ABS_MT_POSITION_X, 300 },
        { EV_ABS, ABS_MT_POSITION_Y, 300 },
        { EV_SYN, SYN_REPORT, 0 },
        { EV_ABS, ABS_MT_TRACKING_ID, -1 },
        { EV_SYN, SYN_REPORT, 0 },
    };
    /* (d) BTN_TOUCH protocol variant instead of TRACKING_ID. */
    static const struct tap_event tap_btn[] = {
        { EV_KEY, BTN_TOUCH, 1 },
        { EV_ABS, ABS_MT_POSITION_X, 50 },
        { EV_ABS, ABS_MT_POSITION_Y, 60 },
        { EV_SYN, SYN_REPORT, 0 },
        { EV_KEY, BTN_TOUCH, 0 },
        { EV_SYN, SYN_REPORT, 0 },
    };
    /* (e) regression: contact A drags out and is ignored, contact B's press
     * frame carries no position and its position arrives one frame later.
     * A stale position left over from A must not poison B's down point. */
    static const struct tap_event tap_stale_pos[] = {
        { EV_ABS, ABS_MT_TRACKING_ID, 0 },
        { EV_ABS, ABS_MT_POSITION_X, 100 },
        { EV_ABS, ABS_MT_POSITION_Y, 100 },
        { EV_SYN, SYN_REPORT, 0 },
        { EV_ABS, ABS_MT_POSITION_X, 1000 },
        { EV_ABS, ABS_MT_POSITION_Y, 1000 },
        { EV_SYN, SYN_REPORT, 0 },
        { EV_ABS, ABS_MT_TRACKING_ID, -1 },
        { EV_SYN, SYN_REPORT, 0 },
        { EV_ABS, ABS_MT_TRACKING_ID, 1 },
        { EV_SYN, SYN_REPORT, 0 },
        { EV_ABS, ABS_MT_POSITION_X, 2000 },
        { EV_ABS, ABS_MT_POSITION_Y, 2000 },
        { EV_SYN, SYN_REPORT, 0 },
        { EV_ABS, ABS_MT_TRACKING_ID, -1 },
        { EV_SYN, SYN_REPORT, 0 },
    };

    CHECK(tap_detected(tap_simple, sizeof(tap_simple) / sizeof(tap_simple[0]), 600, 800) == 1);
    CHECK(tap_detected(tap_drag, sizeof(tap_drag) / sizeof(tap_drag[0]), 758, 1024) == 0);
    CHECK(tap_detected(tap_late_pos, sizeof(tap_late_pos) / sizeof(tap_late_pos[0]), 600, 800) == 1);
    CHECK(tap_detected(tap_btn, sizeof(tap_btn) / sizeof(tap_btn[0]), 1072, 1448) == 1);
    CHECK(tap_detected(tap_stale_pos, sizeof(tap_stale_pos) / sizeof(tap_stale_pos[0]), 600, 800) == 1);
}

/* The timeout path must fire on a silent-but-open device, not only on EOF:
 * with the idle exit gone, the auto-refresh tick and every bounded chunk of
 * the parked tap-wait are built on exactly this behaviour. The write end
 * stays open in the parent: without a timeout this would hang until the
 * alarm kills the child. */
static void test_input_wait_tap_timeout(void)
{
    AppConfig cfg;
    int pfd[2];
    pid_t pid;
    int status;
    time_t started;

    config_defaults(&cfg);
    if (pipe(pfd) != 0) {
        CHECK(0);
        return;
    }

    started = time(NULL);
    pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        CHECK(0);
        return;
    }
    if (pid == 0) {
        int x = -1, y = -1, rc;

        close(pfd[1]);
        alarm(10); /* a wait that ignores its timeout must not hang the suite */
        rc = input_wait_tap(pfd[0], 600, 800, &cfg, 1, &x, &y);
        _exit(rc == 1 ? 0 : 1);
    }

    close(pfd[0]);
    CHECK(waitpid(pid, &status, 0) >= 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(time(NULL) - started < 5);
    close(pfd[1]);
}

/* The beat must witness the main thread: past the announced deadline it stops,
 * which is what turns a main-thread hang into a watchdog.sh kill. */

static void test_quota_display_sig(void)
{
    QuotaSnapshot a, b;
    unsigned long long s1, s2;
    char *json;
    size_t n;

    /* Two JSON bodies that differ only in generatedAt milliseconds within the
     * same wall-clock minute must share a display signature. */
    CHECK(quota_parse_snapshot(EXAMPLE_JSON, &a) == 0);
    b = a;
    b.generated_at = a.generated_at + 500; /* +0.5s, same minute */
    s1 = quota_display_sig(&a, "已连接", "http://x/api/usage");
    s2 = quota_display_sig(&b, "已连接", "http://x/api/usage");
    CHECK(s1 == s2);

    /* A real quota change must dirty the signature. */
    b = a;
    b.codex.five.used = 99.0;
    s2 = quota_display_sig(&b, "已连接", "http://x/api/usage");
    CHECK(s1 != s2);

    /* Whitespace / key-order-equivalent parse of the same values matches. */
    n = strlen(EXAMPLE_JSON);
    json = malloc(n + 8);
    CHECK(json != NULL);
    if (json) {
        /* Insert a space after the first brace -- parser ignores whitespace. */
        json[0] = '{';
        json[1] = ' ';
        memcpy(json + 2, EXAMPLE_JSON + 1, n);
        CHECK(quota_parse_snapshot(json, &b) == 0);
        /* Align generatedAt to the same minute-bucket as a for a fair compare
         * in case the fixture and parse path differ on other fields. */
        b.generated_at = a.generated_at;
        s2 = quota_display_sig(&b, "已连接", "http://x/api/usage");
        CHECK(s1 == s2);
        free(json);
    }

    /* Minute formatter drops seconds. */
    {
        char full[32], mins[32];
        quota_format_timestamp(full, sizeof(full), 1, 1785695952333LL);
        quota_format_timestamp_min(mins, sizeof(mins), 1, 1785695952333LL);
        CHECK(strlen(mins) == 11); /* NN-NN NN:NN */
        CHECK(strstr(full, mins) == full);
    }
}

static void test_net_wifi_ownership(void)
{
    /* Release without ownership is a no-op. */
    net_wifi_set_owned_for_test(0);
    CHECK(net_wifi_owned_for_test() == 0);
    net_release_wifi();
    CHECK(net_wifi_owned_for_test() == 0);

    /* On the host, lipc is missing so a release attempt fails and ownership
     * must stick for a later retry (R04). */
    net_wifi_set_owned_for_test(1);
    CHECK(net_wifi_owned_for_test() == 1);
    net_release_wifi();
    CHECK(net_wifi_owned_for_test() == 1);

    /* Clearing via the test seam still works. */
    net_wifi_set_owned_for_test(0);
    CHECK(net_wifi_owned_for_test() == 0);
}

static void test_ui_wrap_utf8_lines(void)
{
    char scratch[512];
    const char *lines[16];
    int n;

    n = ui_wrap_utf8_lines("abcdef", 3, scratch, sizeof(scratch), lines, 16);
    CHECK(n == 2);
    CHECK(strcmp(lines[0], "abc") == 0);
    CHECK(strcmp(lines[1], "def") == 0);

    /* CJK: 3-byte codepoints, budget 6 bytes => 2 chars per line. */
    n = ui_wrap_utf8_lines("中文测试", 6, scratch, sizeof(scratch), lines, 16);
    CHECK(n == 2);
    CHECK(strcmp(lines[0], "中文") == 0);
    CHECK(strcmp(lines[1], "测试") == 0);

    /* Long URL breaks on / or ? when possible. */
    n = ui_wrap_utf8_lines("http://example.com/api/usage?x=1", 20,
                           scratch, sizeof(scratch), lines, 16);
    CHECK(n >= 2);

    /* Empty / null. */
    CHECK(ui_wrap_utf8_lines("", 10, scratch, sizeof(scratch), lines, 16) == 0);
    CHECK(ui_wrap_utf8_lines(NULL, 10, scratch, sizeof(scratch), lines, 16) == 0);
}


static void test_config_cache_and_light_idle(void)
{
    const char *path = "/tmp/aiquota_cache_test.dat";
    char json[512];
    long long saved = 0;
    AppConfig cfg;

    unlink(path);
    CHECK(config_clamp_light_idle(0) == 0);
    CHECK(config_clamp_light_idle(3) == 3);
    CHECK(config_clamp_light_idle(99) == CONFIG_LIGHT_IDLE_MAX);
    CHECK(config_clamp_light_idle(-1) == CONFIG_LIGHT_IDLE_DEFAULT);

    CHECK(config_cache_save(path, "http://a/api/usage", NULL,
                            "{\"displayMode\":\"used\"}", 12345) == 0);
    CHECK(config_cache_load(path, "http://b/api/usage", NULL, json, sizeof(json),
                            &saved) != 0); /* URL mismatch */
    CHECK(config_cache_load(path, "http://a/api/usage", NULL, json, sizeof(json),
                            &saved) == 0);
    CHECK(saved == 12345);
    CHECK(strstr(json, "displayMode") != NULL);
    CHECK(config_cache_load(path, "http://a/api/usage", "new-account", json,
                            sizeof(json), &saved) != 0);
    CHECK(json[0] == '\0');
    CHECK(config_cache_save(path, "http://a/api/usage", "old-account",
                            "{\"displayMode\":\"used\"}", 12345) != 0);
    CHECK(access(path, F_OK) != 0); /* Authenticated responses are never persisted. */
    {
        FILE *f = fopen(path, "w");
        CHECK(f != NULL);
        if (f) {
            fputs("api_url=http://a/api/usage\nsaved_at=1\njson=\n{}", f);
            fclose(f);
            CHECK(config_cache_load(path, "http://a/api/usage", NULL, json,
                                    sizeof(json), &saved) != 0); /* Legacy cache. */
        }
    }

    config_defaults(&cfg);
    CHECK(cfg.light_idle_off_min == CONFIG_LIGHT_IDLE_DEFAULT);
    CHECK(cfg.display_mode_pref == 0);
    CHECK(cfg.config_version == CONFIG_VERSION_CURRENT);
    unlink(path);
}

static void test_http_cancel_flag(void)
{
    http_cancel_reset();
    CHECK(http_cancel_requested() == 0);
    http_cancel();
    CHECK(http_cancel_requested() == 1);
    http_cancel_reset();
    CHECK(http_cancel_requested() == 0);
}

static void test_mono_now_s(void)
{
    long a = mono_now_s();
    long b = mono_now_s();
    CHECK(b >= a);
    CHECK(a > 0);
}

static void test_wd_should_beat(void)
{
    long long now = (long long)time(NULL);

    CHECK(wd_should_beat(now)); /* not armed yet: beat freely */

    wd_expect(30);
    CHECK(wd_should_beat(now));
    CHECK(wd_should_beat(now + 29));
    CHECK(!wd_should_beat(now + 31));

    /* Re-arming from inside a long phase pushes the deadline out again. */
    wd_expect(120);
    CHECK(wd_should_beat(now + 31));
    CHECK(!wd_should_beat(now + 121));

    /* A shorter announcement takes effect immediately (no max() on the old
     * deadline), so a fast phase cannot inherit a slow phase's slack. */
    wd_expect(5);
    CHECK(!wd_should_beat(now + 31));

    /* Non-positive is treated as "one second", never as "forever". */
    wd_expect(0);
    CHECK(!wd_should_beat(now + 10));
}


static void test_providers_pref_and_url(void)
{
    char list[128];
    char url[768];
    ProviderSelection sel;
    UiCard ids[UI_CARD_SLOTS];
    int n;

    CHECK(config_provider_known("grok"));
    CHECK(config_provider_known("agy"));
    CHECK(!config_provider_known("nope"));

    n = config_providers_normalize(list, sizeof(list),
                                   "codex, grok, codex, unknown,agy");
    CHECK(n == 3);
    CHECK(strcmp(list, "codex,grok,agy") == 0);

    /* Removed providers in old configs are ignored without resetting the rest. */
    n = config_providers_normalize(list, sizeof(list),
                                   "grok,claude,codex,opencode,agy");
    CHECK(n == 3);
    CHECK(strcmp(list, "grok,codex,agy") == 0);

    /* Empty / all-invalid -> default (never all-off). */
    n = config_providers_normalize(list, sizeof(list), "");
    CHECK(n == CONFIG_PROVIDERS_MAX);
    CHECK(strcmp(list, CONFIG_PROVIDERS_DEFAULT) == 0);
    n = config_providers_normalize(list, sizeof(list), "foo,bar");
    CHECK(n == CONFIG_PROVIDERS_MAX);

    snprintf(list, sizeof(list), "%s", "grok");
    CHECK(config_providers_toggle(list, sizeof(list), "grok") == -1); /* refuse */
    CHECK(strcmp(list, "grok") == 0);
    CHECK(config_providers_toggle(list, sizeof(list), "codex") == 0);
    CHECK(strcmp(list, "grok,codex") == 0);
    CHECK(config_providers_toggle(list, sizeof(list), "grok") == 0);
    CHECK(strcmp(list, "codex") == 0);

    snprintf(list, sizeof(list), "%s", "grok,codex,kimi");
    CHECK(config_providers_move(list, sizeof(list), "codex", -1) == 0);
    CHECK(strcmp(list, "codex,grok,kimi") == 0);
    CHECK(config_providers_move(list, sizeof(list), "codex", +1) == 0);
    CHECK(strcmp(list, "grok,codex,kimi") == 0);

    config_providers_to_selection("kimi,agy", &sel);
    CHECK(!sel.grok && sel.kimi && sel.agy && !sel.codex);

    CHECK(config_url_with_providers(url, sizeof(url),
                                    "http://h/api/usage?foo=1&providers=old&tools=x",
                                    "codex,grok") == 0);
    CHECK(strstr(url, "foo=1") != NULL);
    CHECK(strstr(url, "providers=codex,grok") != NULL);
    CHECK(strstr(url, "providers=old") == NULL);
    CHECK(strstr(url, "tools=") == NULL);

    /* Card order follows CSV; AGY still expands to two cards. */
    memset(&sel, 0, sizeof(sel));
    sel.kimi = 1;
    sel.grok = 1;
    sel.agy = 1;
    n = ui_cards_from_providers_ex(&sel, "agy,kimi,grok", ids, UI_CARD_SLOTS);
    CHECK(n == 5);
    CHECK(ids[0] == UI_CARD_AGY_GEMINI);
    CHECK(ids[1] == UI_CARD_AGY_THIRD);
    CHECK(ids[2] == UI_CARD_KIMI);
    CHECK(ids[3] == UI_CARD_GROK);
    CHECK(ids[4] == UI_CARD_INFO);
}

static void test_config_providers_roundtrip_and_migrate(void)
{
    const char *path = "/tmp/aiquota_cfg_prov.ini";
    AppConfig cfg, back;

    unlink(path);
    unlink("/tmp/aiquota_cfg_prov.ini.bak");
    config_defaults(&cfg);
    snprintf(cfg.show_providers, sizeof(cfg.show_providers), "%s",
             "agy,codex");
    cfg.display_mode_pref = 3; /* large */
    cfg.refresh_preset = 2;
    cfg.config_version = CONFIG_VERSION_CURRENT;
    CHECK(config_save(&cfg, path) == 0);
    /* Second save creates .bak from the first good file (R05). */
    CHECK(config_save(&cfg, path) == 0);

    config_defaults(&back);
    CHECK(config_load(&back, path) == 0);
    CHECK(strcmp(back.show_providers, "agy,codex") == 0);
    CHECK(back.display_mode_pref == 3);
    CHECK(back.refresh_preset == 2);
    CHECK(back.config_version == CONFIG_VERSION_CURRENT);

    /* Corrupt primary; recover from .bak written by prior save. */
    {
        FILE *f = fopen(path, "w");
        CHECK(f != NULL);
        /* Truncate to empty: load returns 0 but leaves defaults — force
         * unreadable by removing the file and leaving only .bak. */
        fclose(f);
        unlink(path);
    }
    config_defaults(&back);
    CHECK(config_load_with_backup(&back, path) == 0);
    CHECK(strcmp(back.show_providers, "agy,codex") == 0);

    /* Migrate v1 -> current. */
    config_defaults(&cfg);
    cfg.config_version = 1;
    cfg.show_providers[0] = '\0';
    CHECK(config_migrate(&cfg) == 1);
    CHECK(cfg.config_version == CONFIG_VERSION_CURRENT);
    CHECK(strcmp(cfg.show_providers, CONFIG_PROVIDERS_DEFAULT) == 0);

    CHECK(config_refresh_preset_minutes(1) == 0);
    CHECK(config_refresh_preset_minutes(2) == 5);
    CHECK(config_refresh_preset_minutes(3) == 30);
    CHECK(config_refresh_preset_minutes(0) == -1);

    unlink(path);
    unlink("/tmp/aiquota_cfg_prov.ini.bak");
}

static void test_http_diag_format(void)
{
    HttpDiag d;
    char buf[512];

    memset(&d, 0, sizeof(d));
    d.last_phase = HTTP_PHASE_DNS;
    d.failed_phase = HTTP_PHASE_DNS;
    d.dns_ms = 12;
    d.connect_ms = -1;
    d.http_ms = -1;
    d.total_ms = 15;
    snprintf(d.err, sizeof(d.err), "dns failed: nodename");
    http_diag_format(&d, 1, 3, 0, 0, buf, sizeof(buf));
    CHECK(strstr(buf, "WiFi: ok") != NULL);
    CHECK(strstr(buf, "DNS: 12 ms") != NULL);
    CHECK(strstr(buf, "Fail stage: dns") != NULL);
    CHECK(strstr(buf, "dns failed") != NULL);
}

static void test_api_token(void)
{
    const char *path = "/tmp/aiquota_cfg_token.ini";
    AppConfig cfg;
    AppConfig back;
    char err[128];
    char *body;
    int status = 0;
    FILE *f;

    /* Validation: printable, no spaces, bounded. A malformed token must
     * never reach the wire, even if a hand-edited config contains one. */
    CHECK(config_token_valid(NULL) == 0);
    CHECK(config_token_valid("") == 0);
    CHECK(config_token_valid("test-token_123") == 1);
    CHECK(config_token_valid("has space") == -1);
    CHECK(config_token_valid("bad\r\ntoken") == -1);
    CHECK(config_token_valid("\xc3\xa9") == -1);
    {
        char token[202];
        memset(token, 'a', sizeof(token));
        token[200] = '\0';
        CHECK(config_token_valid(token) == 1);
        token[200] = 'a';
        token[201] = '\0';
        CHECK(config_token_valid(token) == -1);
    }

    /* A malformed token fails before any network activity. */
    err[0] = '\0';
    body = http_get_with_auth(test_url("/index.txt"),
                              "bad token", &status, err, sizeof(err));
    CHECK(body == NULL);
    CHECK(strcmp(err, "bad token") == 0);

    body = http_get(test_url("/auth"), &status, err, sizeof(err));
    CHECK(body != NULL && status == 401);
    free(body);
    body = http_get_with_auth(test_url("/auth"), "test-token_123",
                              &status, err, sizeof(err));
    CHECK(body != NULL && status == 200);
    if (body)
        CHECK(strstr(body, "hello-aiquota") != NULL);
    free(body);

    /* Roundtrip: a valid token survives save/load; a malformed one in a
     * hand-edited file is dropped on load instead of being re-saved. */
    config_defaults(&cfg);
    CHECK(cfg.api_token[0] == '\0');
    snprintf(cfg.api_token, sizeof(cfg.api_token), "%s", "tok_abc-123");
    CHECK(config_save(&cfg, path) == 0);
    config_defaults(&back);
    CHECK(config_load(&back, path) == 0);
    CHECK(strcmp(back.api_token, "tok_abc-123") == 0);

    f = fopen(path, "w");
    CHECK(f != NULL);
    if (f) {
        fputs("api_url=http://example.com/api/usage\napi_token=bad token\n", f);
        fclose(f);
        config_defaults(&back);
        CHECK(config_load(&back, path) == 0);
        CHECK(back.api_token[0] == '\0');
    }
    unlink(path);
    unlink("/tmp/aiquota_cfg_token.ini.bak");
}

int main(void)
{
    RUN(test_api_token);
    RUN(test_normalize_url);
    RUN(test_config_roundtrip);
    RUN(test_config_save_atomic);
    RUN(test_parse_full_example);
    RUN(test_parse_provider_selection);
    RUN(test_parse_window_status);
    RUN(test_parse_kimi_null);
    RUN(test_parse_remaining_mode);
    RUN(test_parse_garbage);
    RUN(test_parse_out_of_range);
    RUN(test_format_percent);
    RUN(test_format_reset);
    RUN(test_format_timestamp);
    RUN(test_http_get);
    RUN(test_input_transform);
    RUN(test_input_wait_tap);
    RUN(test_input_wait_tap_timeout);
    RUN(test_quota_display_sig);
    RUN(test_config_cache_and_light_idle);
    RUN(test_http_cancel_flag);
    RUN(test_mono_now_s);
    RUN(test_net_wifi_ownership);
    RUN(test_ui_wrap_utf8_lines);
    RUN(test_wd_should_beat);
    RUN(test_net_sanitize_ssid);
    RUN(test_net_parse_charging);
    RUN(test_net_ensure_wifi_no_lipc);
    RUN(test_light_clamp_and_no_lipc);
    RUN(test_ui_batt_cells);
    RUN(test_ui_cards_from_providers);
    RUN(test_providers_pref_and_url);
    RUN(test_config_providers_roundtrip_and_migrate);
    RUN(test_http_diag_format);
    RUN(test_ui_rect_contains);
    RUN(test_ui_hit_card);

    if (failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d check(s) FAILED\n", failures);
    return 1;
}
