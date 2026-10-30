/* aiquota -- AI quota dashboard for Kindle: entry point, state machine and
 * debug switches. Mirrors the KOReader plugin's flow (readingquota.koplugin/
 * quotapage.lua): paint the loading skeleton first, fetch only after the
 * first paint, then serve taps on the refresh / settings / close buttons.
 *
 * State machine: LOADING -> READY | ERROR, plus a SETTINGS sub-mode entered
 * from READY/ERROR (which itself opens KEYPAD editors for its fields). The
 * app never exits on its own: it parks on a blocked poll (CPU idle, radio
 * off, e-ink static) until the 关闭 button, a lost touch device or the
 * watchdog layers end it; the only timer is the optional auto-refresh tick.
 * Modes: no args = normal run; --demo renders embedded demo data without
 * touching the network; --dump-state prints FBInkState; --calibrate derives
 * the touch transform over SSH. A clean exit returns 0 and the wrapper script
 * (extension/bin/aiquota.sh) restores the framework. */
#define _POSIX_C_SOURCE 200809L /* sigaction */

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "fbink.h"
#include "http.h"
#include "input.h"
#include "keypad.h"
#include "light.h"
#include "net.h"
#include "quota.h"
#include "settings.h"
#include "ui.h"
#include "watchdog.h"
#include "mono.h"

#define FONT_PATH "/mnt/us/extensions/aiquota/bin/NotoSansSC-subset.ttf"
#define PING_PATH_DEFAULT "/var/tmp/aiquota.ping"
#define DETAIL_SIZE 900 /* URL / error / F03 diagnose report */

/* Session-scoped heartbeat path from the launcher; falls back for bare runs. */
static const char *ping_path(void)
{
    const char *p = getenv("AIQUOTA_PING_PATH");
    return (p && p[0]) ? p : PING_PATH_DEFAULT;
}

/* Dashboard states; the keypad is a sub-mode handled inside the main loop. */
enum { ST_LOADING = 0, ST_READY, ST_ERROR };

enum { MODE_NORMAL = 0, MODE_DEMO, MODE_DUMP_STATE, MODE_CALIBRATE };

/* Full example response from docs/superpowers/plans/usage-api.md, for --demo. */
static const char DEMO_JSON[] =
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

/* Fatal-signal handler: async-signal-safe work only. The real framework
 * restore happens in the wrapper script's EXIT trap (extension/bin/
 * aiquota.sh); this handler exists so the trap fires instead of the process
 * hanging on the default action. */
static void fatal_signal(int sig)
{
    static const char msg[] = "aiquota: fatal signal\n";

    (void)sig;
    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
}

/* Install unless the signal came in already ignored. SIG_IGN is inherited
 * across exec, and the wrapper's `trap "" TERM` (which exists to survive KUAL's
 * cascading SIGTERM) hands us exactly that: overriding it would make the app
 * die on the very group signal the wrapper set out to absorb, and the wrapper
 * would then restore the framework the moment we started. Run standalone over
 * SSH nothing is ignored, so the handler is installed as usual. */
static void install_fatal_handler(int sig, const struct sigaction *sa)
{
    struct sigaction old;

    if (sigaction(sig, NULL, &old) == 0 && old.sa_handler == SIG_IGN)
        return;
    sigaction(sig, sa, NULL);
}

static void install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fatal_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: a fatal must not leave reads blocking */
    install_fatal_handler(SIGSEGV, &sa);
    install_fatal_handler(SIGABRT, &sa);
    install_fatal_handler(SIGINT, &sa);
    install_fatal_handler(SIGTERM, &sa);
}

/* Upper bound of one poll segment while waiting for taps: the heartbeat
 * contract forbids unbounded announcements, so every wait is built from
 * bounded segments with the watchdog re-armed before each one. Segment length
 * trades wedge-detection latency against a tiny gesture risk: a touch whose
 * down and up straddle a segment boundary is dropped (state self-recovers on
 * the next touch). The same drop happens when a wait ends on a deadline
 * instead of a segment: the idle light-off / auto-refresh tick makes
 * wait_tap_until return 1 with a gesture in progress, voiding it (the next
 * touch self-recovers again). Both are known boundaries, kept explicit here
 * so nobody files them as bugs. 5 min puts worst-case wedged-app detection
 * at ~7 min here while the straddle odds are ~1 in 50 000 taps. */
#define TAP_WAIT_CHUNK_S 300

/* Idle frontlight timeout (docs/kindle-ui-and-power.md P0): the parked
 * dashboard's biggest avoidable draw is the LED, so it drops to 0 after
 * this many seconds without a touch on the dashboard and comes back on the
 * next one. First version is fixed -- no config row; if it ever wants a
 * knob, that is a new key, not a reuse of an existing one. */
#define LIGHT_IDLE_OFF_S (3 * 60)
/* After the first idle light-off, keep sampling the battery badge on
 * this low-frequency cadence so auto-refresh=off does not freeze the
 * cell count overnight (R08). Uses wait_tap_until wake points only. */
#define BADGE_SAMPLE_S (5 * 60)

static void apply_display_pref(QuotaSnapshot *snap, const AppConfig *cfg)
{
    if (!snap || !cfg)
        return;
    /* 0=follow API; 1/2 override used/remaining; 3/4 are layout prefs (F04)
     * that do not reinterpret used percentages. */
    if (cfg->display_mode_pref == 1)
        snprintf(snap->display_mode, sizeof(snap->display_mode), "used");
    else if (cfg->display_mode_pref == 2)
        snprintf(snap->display_mode, sizeof(snap->display_mode), "remaining");
}

/* Build the per-request URL with local providers= filter (F02). Never calls
 * PUT /api/providers. */
static void build_fetch_url(char *dst, size_t dst_size, const AppConfig *cfg)
{
    if (!dst || dst_size == 0 || !cfg)
        return;
    (void)config_url_with_providers(dst, dst_size, cfg->api_url,
                                    cfg->show_providers);
}

static void apply_provider_order(Ui *ui, const AppConfig *cfg)
{
    if (ui && cfg)
        ui_set_provider_order(ui, cfg->show_providers);
}

/* F03: last structured connection diagnosis (does not clear have_good). */
typedef struct {
    int wifi_ok;
    long wifi_ms;
    int json_ok;
    long json_ms;
    HttpDiag http;
    char report[DETAIL_SIZE];
} ConnDiag;

static ConnDiag g_last_diag;

static void diag_begin(void)
{
    memset(&g_last_diag, 0, sizeof(g_last_diag));
    http_diag_reset();
}

static void diag_finish(int wifi_ok, long wifi_ms, int json_ok, long json_ms)
{
    const HttpDiag *h = http_diag_last();

    g_last_diag.wifi_ok = wifi_ok;
    g_last_diag.wifi_ms = wifi_ms;
    g_last_diag.json_ok = json_ok;
    g_last_diag.json_ms = json_ms;
    if (h)
        g_last_diag.http = *h;
    http_diag_format(&g_last_diag.http, wifi_ok, wifi_ms, json_ok, json_ms,
                     g_last_diag.report, sizeof(g_last_diag.report));
}


static int light_idle_secs(const AppConfig *cfg)
{
    int m = cfg ? cfg->light_idle_off_min : CONFIG_LIGHT_IDLE_DEFAULT;
    if (m <= 0)
        return 0; /* feature off */
    return m * 60;
}

/* One tap wait, wrapped in the heartbeat contract (see watchdog.h): each
 * segment announces its own bound before blocking, then the short local-work
 * bound takes over. With this, the app has no unbounded wait left anywhere,
 * which is what lets watchdog.sh read a missing beat as "wedged" rather than
 * "the user put the Kindle down".
 * deadline is an absolute monotonic second from mono_now_s() (0 = none).
 * Wall-clock jumps no longer stretch or skip idle/auto-refresh waits (R09).
 * Returns 0 tap / 1 tick / -1 error, like input_wait_tap. */
static int wait_tap_until(int fd, int scr_w, int scr_h, const AppConfig *cfg,
                          long deadline, int *x, int *y)
{
    for (;;) {
        int chunk = TAP_WAIT_CHUNK_S;
        int rc;

        if (deadline > 0) {
            long left = deadline - mono_now_s();

            if (left <= 0)
                return 1;
            if (left < chunk)
                chunk = (int)left;
        }
        /* Every chunk boundary also refreshes the screensaver hold (the
         * call rate-limits itself): the parked dashboard must never roll
         * into the lock screen, whatever powerd thinks of the hold the
         * wrapper armed at startup. */
        net_hold_screensaver();
        wd_expect(chunk + WD_SLACK_S);
        rc = input_wait_tap(fd, scr_w, scr_h, cfg, chunk, x, y);
        wd_expect(WD_STEP_S);
        if (rc != 1)
            return rc; /* tap or device error; a segment expiry re-arms */
    }
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [--demo | --dump-state | --calibrate]\n"
            "  --demo        render embedded demo data, no network\n"
            "  --dump-state  print FBInkState fields and exit\n"
            "  --calibrate   touch calibration helper (run over SSH)\n",
            prog);
}

/* Authority part of the API URL ("http://h:8123/api/usage" -> "h:8123"),
 * shown in the title bar. Static storage; recomputed when cfg.api_url changes. */
static const char *api_host(const char *url)
{
    static char host[512];
    const char *p = url;
    const char *scheme = strstr(url, "://");
    size_t n;

    if (scheme)
        p = scheme + 3;
    n = strcspn(p, "/");
    if (n >= sizeof(host))
        n = sizeof(host) - 1;
    memcpy(host, p, n);
    host[n] = '\0';
    return host;
}

/* One full-screen refresh after a complete redraw. The ui/keypad
 * modules draw everything with their own no_refresh configs; main only
 * issues refreshes once a full frame is drawn (UIManager:setDirty style).
 * The waveform is the caller's call: GC16 for real content changes, GL16
 * when the frame was repainted from identical data and fidelity does not
 * justify a flash. */
static void full_refresh_mode(int fbfd, int wfm_mode)
{
    FBInkConfig rcfg;

    memset(&rcfg, 0, sizeof(rcfg));
    rcfg.no_refresh = false;
    rcfg.wfm_mode = (WFM_MODE_INDEX_T)wfm_mode;
    /* An empty region (0x0 @ 0,0) means a full-screen refresh; the header
     * marks fbink_refresh_rect's rect nonnull, so use fbink_refresh. */
    fbink_refresh(fbfd, 0, 0, 0, 0, &rcfg);
}

static void full_refresh(int fbfd)
{
    full_refresh_mode(fbfd, WFM_GC16); /* full high-fidelity re-render */
}

/* --- what the panel currently shows ---------------------------------------
 * The app never exits on its own anymore, so most of its life is a parked
 * wait: CPU blocked in poll, radio down, e-ink holding a static image for
 * free. The one recurring cost left is the auto-refresh cycle, and a cycle
 * whose fetch brings back the same data with an unchanged battery badge
 * does not need the panel to flash at all -- these track the visible frame
 * well enough to decide that (FNV-1a64 over status + body-or-error text,
 * plus the title-bar battery cells and charging flag as painted). */
static int frame_is_dashboard;        /* a dashboard (not skeleton/page) is up */
static unsigned long long frame_sig;  /* signature of the painted content */
static int frame_batt, frame_chg;     /* cells (-1..5) and charging flag as painted */

static int batt_chg_flag(int chg)
{
    return chg == 1 ? 1 : 0;
}

static void remember_batt(int pct, int chg)
{
    frame_batt = ui_batt_cells(pct);
    frame_chg = batt_chg_flag(chg);
}

/* Idle-deadline hitchhiker (docs/kindle-light-and-battery.md G1): the
 * frontlight timeout already wakes the loop, so sample the battery then
 * and DU only the badge strip if the cell count or plug state moved.
 * No new timer, no full-screen flash. */
static void maybe_refresh_badges(int fbfd, Ui *ui)
{
    char ssid[33];
    int batt, chg;
    FBInkRect r;

    if (!frame_is_dashboard)
        return;
    net_battery(&batt, &chg);
    if (ui_batt_cells(batt) == frame_batt && batt_chg_flag(chg) == frame_chg)
        return;
    net_wifi_ssid(ssid, sizeof(ssid));
    memset(&r, 0, sizeof(r));
    ui_redraw_badges(ui, net_wifi_state(), ssid, batt, chg, &r);
    if (r.width > 0 && r.height > 0)
        ui_refresh(fbfd, &r, WFM_DU);
    remember_batt(batt, chg);
}

static void paint_current_dashboard(int fbfd, Ui *ui, const QuotaSnapshot *snap,
                                    const char *host, int st, const char *detail,
                                    int auto_refresh_min, int wfm_mode)
{
    char ssid[33];
    int batt, chg;

    net_battery(&batt, &chg);
    net_wifi_ssid(ssid, sizeof(ssid));
    ui_draw_dashboard(ui, snap, host, st == ST_ERROR ? "失败" : "已连接",
                      detail, 0, net_wifi_state(), ssid, batt, chg,
                      auto_refresh_min);
    frame_is_dashboard = 1;
    remember_batt(batt, chg);
    full_refresh_mode(fbfd, wfm_mode);
}

/* Display-content signature (R03): hash what the panel actually shows, not
 * the raw JSON. generatedAt-only churn within the same minute no longer
 * forces a full refresh. */
static unsigned long long frame_sig_of(const QuotaSnapshot *snap,
                                      const char *status, const char *detail)
{
    return quota_display_sig(snap, status, detail);
}

/* LOADING paint: empty skeleton + "connecting" card, like quotapage.lua's
 * onShow drawing the skeleton before fetch() runs. */
static void paint_loading(int fbfd, Ui *ui, const char *host, const char *api_url,
                          int auto_refresh_min)
{
    QuotaSnapshot empty;
    char ssid[33]; /* 802.11 SSIDs cap at 32 octets */
    int batt, chg;

    quota_empty_snapshot(&empty);
    net_battery(&batt, &chg);
    net_wifi_ssid(ssid, sizeof(ssid));
    ui_draw_dashboard(ui, &empty, host, "连接中", api_url, 1,
                      net_wifi_state(), ssid, batt, chg, auto_refresh_min);
    full_refresh(fbfd);
    frame_is_dashboard = 0; /* whatever lands next must be painted in full */
}

/* One fetch+paint cycle: WiFi check, HTTP GET, JSON parse, dashboard redraw.
 * A successful parse replaces *snap; on error the last good snapshot is kept
 * (or an empty one when nothing ever succeeded), and detail carries the URL
 * or the error text. release_wifi: hand the radio back after the attempt
 * (only effective when net.c actually raised it). auto_cycle: the cycle was
 * timer-driven -- its repaint lands with GL16 (nobody asked for the update,
 * so it does not justify a GC16 flash; the signature test above already
 * removed the no-change case). User-driven cycles (refresh tap, URL edit)
 * keep GC16, whose full flash is also what periodically clears ghosting.
 * Returns ST_READY/ST_ERROR.
 * The repaint at the end is skipped when the panel already shows exactly
 * this content (same signature, same battery badge and a dashboard rather
 * than a skeleton is up): an auto-refresh cycle that changed nothing then
 * costs the radio wake-up and nothing else -- no e-ink flash. */
/* Background GET so the UI thread can still accept 关闭 during a slow
 * request (R06). The worker only runs http_get; WiFi/parse stay on main. */
typedef struct {
    const char *url;
    const char *token; /* optional Bearer token; NULL/empty = no header */
    char *body;
    int status;
    char err[256];
    volatile int done;
} FetchJob;

static void *fetch_job_main(void *arg)
{
    FetchJob *job = (FetchJob *)arg;

    job->body = http_get_with_auth(job->url, job->token,
                                   &job->status, job->err, sizeof(job->err));
    job->done = 1;
    return NULL;
}

static int do_fetch(int fbfd, Ui *ui, const char *api_url, const char *host,
                    QuotaSnapshot *snap, int *have_good, int demo,
                    int auto_refresh_min, int auto_cycle,
                    char *detail, size_t detail_size, int release_wifi,
                    const char *token)
{
    QuotaSnapshot fresh;
    unsigned long long sig;
    int st = ST_READY;

    if (demo) {
        /* Demo mode re-renders the embedded sample; the network is never
         * touched, so refresh cycles cannot fail here. */
        if (quota_parse_snapshot(DEMO_JSON, &fresh) == 0) {
            *snap = fresh;
            *have_good = 1;
        }
        snprintf(detail, detail_size, "demo");
        sig = frame_sig_of(&fresh, "已连接", detail);
    } else {
        int status = 0;
        char errbuf[256];
        char *body = NULL;

        errbuf[0] = '\0';
        if (net_ensure_wifi() != 0) {
            /* Checked every cycle: WiFi may drop while the app is open. */
            st = ST_ERROR;
            snprintf(detail, detail_size, "WiFi 未连接");
            sig = frame_sig_of(snap, "失败", detail);
        } else {
            body = http_get_with_auth(api_url, token, &status, errbuf, sizeof(errbuf));
            if (!body) {
                st = ST_ERROR;
                snprintf(detail, detail_size, "%s", errbuf);
                sig = frame_sig_of(snap, "失败", detail);
            } else if (status != 200) {
                st = ST_ERROR;
                snprintf(detail, detail_size, "HTTP %d", status);
                sig = frame_sig_of(snap, "失败", detail);
            } else if (quota_parse_snapshot(body, &fresh) != 0) {
                st = ST_ERROR;
                snprintf(detail, detail_size, "响应不是有效 JSON");
                sig = frame_sig_of(snap, "失败", detail);
            } else {
                *snap = fresh;
                /* display_mode_pref applied by caller when cfg is known */
                *have_good = 1;
                snprintf(detail, detail_size, "%s", api_url);
                sig = frame_sig_of(&fresh, "已连接", detail);
            }
            free(body); /* free(NULL) is fine */
        }

        /* The fetch is over either way: if we raised the radio for it, drop
         * it again. The dashboard keeps showing data with the WiFi off --
         * e-ink holds the image, nothing else needs the network. */
        if (release_wifi)
            net_release_wifi();
    }

    if (st == ST_ERROR && !*have_good)
        quota_empty_snapshot(snap);
    {
        /* Sampled once per cycle, after the radio decision above: with
         * wifi_auto_off the connection is already down here, so the SSID
         * probe inside net_wifi_ssid spawns nothing and the badge shows
         * the plain gray "WiFi" dot -- exactly the state the radio is in. */
        char ssid[33];
        int batt, chg;

        net_battery(&batt, &chg);
        net_wifi_ssid(ssid, sizeof(ssid));
        if (frame_is_dashboard && sig == frame_sig &&
            ui_batt_cells(batt) == frame_batt &&
            batt_chg_flag(chg) == frame_chg) {
            /* Identical cycle: keep the frame, skip the flash. Cell-count
             * compare (not raw percent) so a 1% drip does not force a
             * visually identical redraw. */
            return st;
        }
        ui_draw_dashboard(ui, snap, host, st == ST_ERROR ? "失败" : "已连接",
                          detail, 0, net_wifi_state(), ssid, batt, chg,
                          auto_refresh_min);
        frame_is_dashboard = 1;
        frame_sig = sig;
        remember_batt(batt, chg);
    }
    full_refresh_mode(fbfd, auto_cycle ? WFM_GL16 : WFM_GC16);
    return st;
}

/* --dump-state: every FBInkState field, name=value, one per line. */
static void dump_state(const FBInkState *st)
{
    printf("user_hz=%ld\n", st->user_hz);
    printf("font_name=%s\n", st->font_name ? st->font_name : "");
    printf("view_width=%u\n", st->view_width);
    printf("view_height=%u\n", st->view_height);
    printf("screen_width=%u\n", st->screen_width);
    printf("screen_height=%u\n", st->screen_height);
    printf("scanline_stride=%u\n", st->scanline_stride);
    printf("bpp=%u\n", st->bpp);
    printf("inverted_grayscale=%d\n", st->inverted_grayscale);
    printf("device_name=%s\n", st->device_name);
    printf("device_codename=%s\n", st->device_codename);
    printf("device_platform=%s\n", st->device_platform);
    printf("device_id=%d\n", st->device_id);
    printf("pen_fg_color=%d\n", st->pen_fg_color);
    printf("pen_bg_color=%d\n", st->pen_bg_color);
    printf("screen_dpi=%d\n", st->screen_dpi);
    printf("font_w=%d\n", st->font_w);
    printf("font_h=%d\n", st->font_h);
    printf("max_cols=%d\n", st->max_cols);
    printf("max_rows=%d\n", st->max_rows);
    printf("view_hori_origin=%d\n", st->view_hori_origin);
    printf("view_vert_origin=%d\n", st->view_vert_origin);
    printf("view_vert_offset=%d\n", st->view_vert_offset);
    printf("fontsize_mult=%d\n", st->fontsize_mult);
    printf("glyph_width=%d\n", st->glyph_width);
    printf("glyph_height=%d\n", st->glyph_height);
    printf("is_perfect_fit=%d\n", st->is_perfect_fit);
    printf("is_mtk=%d\n", st->is_mtk);
    printf("is_sunxi=%d\n", st->is_sunxi);
    printf("is_tolino=%d\n", st->is_tolino);
    printf("sunxi_has_fbdamage=%d\n", st->sunxi_has_fbdamage);
    printf("sunxi_force_rota=%d\n", st->sunxi_force_rota);
    printf("is_kindle_legacy=%d\n", st->is_kindle_legacy);
    printf("is_kobo_non_mt=%d\n", st->is_kobo_non_mt);
    printf("unreliable_wait_for=%d\n", st->unreliable_wait_for);
    printf("can_wake_epdc=%d\n", st->can_wake_epdc);
    printf("ntx_boot_rota=%d\n", st->ntx_boot_rota);
    printf("ntx_rota_quirk=%d\n", st->ntx_rota_quirk);
    printf("rotation_map=%d,%d,%d,%d\n", st->rotation_map[0],
           st->rotation_map[1], st->rotation_map[2], st->rotation_map[3]);
    printf("touch_swap_axes=%d\n", st->touch_swap_axes);
    printf("touch_mirror_x=%d\n", st->touch_mirror_x);
    printf("touch_mirror_y=%d\n", st->touch_mirror_y);
    printf("is_ntx_quirky_landscape=%d\n", st->is_ntx_quirky_landscape);
    printf("current_rota=%d\n", st->current_rota);
    printf("can_rotate=%d\n", st->can_rotate);
    printf("can_hw_invert=%d\n", st->can_hw_invert);
    printf("has_eclipse_wfm=%d\n", st->has_eclipse_wfm);
    printf("has_color_panel=%d\n", st->has_color_panel);
    printf("pixel_format=%d\n", st->pixel_format);
    printf("can_wait_for_submission=%d\n", st->can_wait_for_submission);

    /* Not FBInkState, but the same job: install.md §4.1 checks these before
     * the first real launch. */
    {
        int batt = -1, chg = -1;

        net_battery(&batt, &chg);
        printf("battery_level=%d\n", batt);
        printf("battery_charging=%d\n", chg);
        printf("frontlight_intensity=%d\n", light_get_intensity());
        printf("frontlight_max=%d\n", light_get_max());
    }
}

/* Small black crosshair centered on (x, y), refreshed with fast WFM_DU. */
static void draw_crosshair(int fbfd, int scr_w, int scr_h, int x, int y)
{
    const int arm = 20; /* pixels each way from the center */
    const int th = 3;   /* bar thickness */
    FBInkConfig dcfg;
    FBInkConfig rcfg;
    FBInkRect hbar, vbar, box;
    int x0 = x - arm, x1 = x + arm;
    int y0 = y - arm, y1 = y + arm;
    int htop = y - th / 2, vleft = x - th / 2;

    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > scr_w - 1)
        x1 = scr_w - 1;
    if (y1 > scr_h - 1)
        y1 = scr_h - 1;
    if (htop < 0)
        htop = 0;
    if (htop > scr_h - th)
        htop = scr_h - th;
    if (vleft < 0)
        vleft = 0;
    if (vleft > scr_w - th)
        vleft = scr_w - th;

    memset(&dcfg, 0, sizeof(dcfg));
    dcfg.no_refresh = true; /* batch both bars, refresh once below */
    hbar.left = (unsigned short)x0;
    hbar.top = (unsigned short)htop;
    hbar.width = (unsigned short)(x1 - x0 + 1);
    hbar.height = th;
    vbar.left = (unsigned short)vleft;
    vbar.top = (unsigned short)y0;
    vbar.width = th;
    vbar.height = (unsigned short)(y1 - y0 + 1);
    fbink_fill_rect_gray(fbfd, &dcfg, &hbar, false, 0x00u);
    fbink_fill_rect_gray(fbfd, &dcfg, &vbar, false, 0x00u);

    box.left = (unsigned short)x0;
    box.top = (unsigned short)y0;
    box.width = (unsigned short)(x1 - x0 + 1);
    box.height = (unsigned short)(y1 - y0 + 1);
    memset(&rcfg, 0, sizeof(rcfg));
    rcfg.no_refresh = false;
    rcfg.wfm_mode = WFM_DU;
    fbink_refresh_rect(fbfd, &box, &rcfg);
}

/* --calibrate: SSH debug helper. Tap the 4 corners, then derive the touch
 * transform. input.c does not expose raw evdev coordinates, so this runs
 * with a zeroed transform config: the returned points are the raw values
 * linearly mapped into screen range (identity mapping), which preserves axis
 * assignment and direction -- exactly what the analysis below needs. It is a
 * best-effort debug tool, not an exact measurement. */
static int run_calibrate(int fbfd, int scr_w, int scr_h, int input_fd)
{
    static const char *names[4] = { "top-left", "top-right", "bottom-right",
                                    "bottom-left" };
    AppConfig ccfg;
    int px[4], py[4];
    int i, swap, mirror_x, mirror_y;
    int dx, dy;

    if (input_fd < 0) {
        fprintf(stderr, "aiquota: --calibrate needs a touch input device\n");
        return 1;
    }

    config_defaults(&ccfg);
    ccfg.touch_swap_xy = 0; /* explicit: identity mapping, see note above */
    ccfg.touch_mirror_x = 0;
    ccfg.touch_mirror_y = 0;

    { /* White canvas first, so the crosshairs are readable over any content. */
        FBInkConfig ccfg_fb;
        memset(&ccfg_fb, 0, sizeof(ccfg_fb));
        ccfg_fb.no_refresh = false;
        ccfg_fb.wfm_mode = WFM_GC16;
        fbink_cls(fbfd, &ccfg_fb, NULL, false);
    }

    printf("aiquota touch calibration: tap the 4 screen corners in order\n");
    for (i = 0; i < 4; i++)
        printf("  %d: %s\n", i + 1, names[i]);
    fflush(stdout); /* the next read blocks; make sure prompts are out */

    for (i = 0; i < 4; i++) {
        /* Fixed 600 s budget per corner: a session nobody finishes must not
         * pin the screen forever now that the app has no idle exit to lean
         * on. Run over SSH, this is also where the operator watches. */
        int wr = wait_tap_until(input_fd, scr_w, scr_h, &ccfg,
                                (long)time(NULL) + 600, &px[i], &py[i]);

        if (wr != 0) {
            fprintf(stderr, "aiquota: %s\n",
                    wr > 0 ? "calibration timed out" : "touch input device lost");
            return 1;
        }
        /* raw == screen under the identity mapping (see note above). */
        printf("raw=(%d,%d) screen=(%d,%d)\n", px[i], py[i], px[i], py[i]);
        fflush(stdout);
        draw_crosshair(fbfd, scr_w, scr_h, px[i], py[i]);
    }

    /* Axis assignment: TL->TR is a pure screen-X movement, so the raw axis
     * that moved most between taps 0 and 1 is the one wired to screen X.
     * Mostly raw X = no swap; mostly raw Y = the panel reports swapped axes. */
    dx = px[1] - px[0];
    if (dx < 0)
        dx = -dx;
    dy = py[1] - py[0];
    if (dy < 0)
        dy = -dy;
    swap = dy > dx;

    /* Direction: the same step should increase the axis carrying screen X
     * (and TL->BL the one carrying screen Y); a decreasing axis is mirrored.
     * With swap, screen X rides on raw Y and screen Y on raw X. */
    if (!swap) {
        mirror_x = (px[1] - px[0]) < 0;
        mirror_y = (py[3] - py[0]) < 0;
    } else {
        mirror_x = (py[1] - py[0]) < 0;
        mirror_y = (px[3] - px[0]) < 0;
    }

    printf("suggested config lines:\n");
    printf("touch_swap_xy=%d\n", swap);
    printf("touch_mirror_x=%d\n", mirror_x);
    printf("touch_mirror_y=%d\n", mirror_y);
    return 0;
}

/* Local modal: enlarge one dashboard card (docs/kindle-card-popup.md).
 * Auto-refresh is paused for the duration -- an overdue deadline returns
 * 1 immediately from wait_tap_until once we leave, so the missed cycle
 * is made up without painting under the popup. Idle frontlight still
 * ticks: the LED does not care what is on screen. Returns -1 if the
 * touch device disappears. */
static int run_card_popup(int fbfd, int input_fd, int scr_w, int scr_h,
                          const AppConfig *cfg, Ui *ui,
                          const QuotaSnapshot *snap, const char *host,
                          int st, const char *detail, UiCard card,
                          long *idle_deadline)
{
    FBInkDump dump;
    FBInkRect prect;
    int have_dump = 0;
    int wr, x = 0, y = 0;
    int page = 0;
    int pages;
    const char *status = st == ST_ERROR ? "失败" : "已连接";

    memset(&dump, 0, sizeof(dump));
    pages = (card == UI_CARD_INFO) ? ui_popup_info_pages(ui, detail) : 1;
    ui_popup_rect_for(ui, card, &prect);
    if (fbink_rect_dump(fbfd, &prect, &dump) == 0)
        have_dump = 1;
    ui_popup_draw_ex(ui, snap, status, detail, card, page, &prect);
    ui_refresh(fbfd, &prect, WFM_GC16);

    for (;;) {
        wr = wait_tap_until(input_fd, scr_w, scr_h, cfg, *idle_deadline,
                            &x, &y);
        if (wr > 0) {
            long now = (long)time(NULL);

            if (*idle_deadline > 0 && now >= *idle_deadline) {
                light_idle_off();
                *idle_deadline = 0;
                maybe_refresh_badges(fbfd, ui);
            }
            continue;
        }
        if (wr < 0)
            return -1;
        light_idle_restore();
        {
            int lis = light_idle_secs(cfg);
            *idle_deadline = lis > 0 ? mono_now_s() + lis : 0;
        }
        {
            int hit = ui_popup_hit(ui, &prect, card, pages, x, y);
            if (hit == 1 && page > 0) {
                page--;
                ui_popup_draw_ex(ui, snap, status, detail, card, page, &prect);
                ui_refresh(fbfd, &prect, WFM_GC16);
                continue;
            }
            if (hit == 2 && page + 1 < pages) {
                page++;
                ui_popup_draw_ex(ui, snap, status, detail, card, page, &prect);
                ui_refresh(fbfd, &prect, WFM_GC16);
                continue;
            }
        }
        break;
    }

    if (have_dump) {
        FBInkConfig rcfg;
        int rr;

        memset(&rcfg, 0, sizeof(rcfg));
        rcfg.wfm_mode = WFM_GC16;
        rr = fbink_restore(fbfd, &rcfg, &dump);
        fbink_free_dump_data(&dump);
        if (rr == 0)
            return 0;
    }
    /* Dump failed (OOM / no IMAGE) or restore rejected the buffer
     * (bitdepth/rotation): fall back to a full GL16 redraw. */
    paint_current_dashboard(fbfd, ui, snap, host, st, detail,
                            cfg->auto_refresh_min, WFM_GL16);
    return 0;
}


int main(int argc, char **argv)
{
    AppConfig cfg;
    FBInkConfig fbcfg;
    FBInkState fbstate;
    QuotaSnapshot snap;
    Keypad *kp = NULL;
    Ui *ui = NULL;
    const char *host;
    char detail[DETAIL_SIZE];
    long last_fetch;        /* mono second when current data was fetched (R09) */
    char snap_api_url[768]; /* R10: snapshot bound to fetch URL (+providers) */
    char fetch_url[768]; /* F02: api_url + local providers query */
    int first_run_setup = 0; /* F07: open settings when no config */
    int open_settings_once = 0;
    int providers_changed = 0;
    UiCard popup_after_fetch = UI_CARD_NONE; /* tapped card waiting on fresh data */
    unsigned req_gen;       /* R10: increments per fetch; drop stale results */
    int fail_streak;        /* F05: consecutive fetch failures */
    long fail_backoff_until;/* mono second until auto-refresh may retry */
    long idle_deadline;     /* wall-clock second the frontlight dims at */
    long badge_deadline;    /* next low-freq battery badge sample (R08) */
    int have_good = 0;
    int have_cfg_file;
    int mode = MODE_NORMAL;
    int fbfd = -1, input_fd = -1;
    int scr_w = 0, scr_h = 0;
    int st = ST_LOADING;
    int auto_cycle = 0; /* the LOADING about to run was tick-driven */
    int rc = 0;

    /* --- arguments ------------------------------------------------------ */
    if (argc > 2) {
        usage(argv[0]);
        return 2;
    }
    if (argc == 2) {
        if (strcmp(argv[1], "--demo") == 0)
            mode = MODE_DEMO;
        else if (strcmp(argv[1], "--dump-state") == 0)
            mode = MODE_DUMP_STATE;
        else if (strcmp(argv[1], "--calibrate") == 0)
            mode = MODE_CALIBRATE;
        else {
            usage(argv[0]);
            return 2;
        }
    }

    /* --- config ----------------------------------------------------------
     * The config file wins when present; otherwise FBInk's device quirks are
     * better touch defaults than zeros (adopted after fbink_get_state). */
    config_defaults(&cfg);
    have_cfg_file = (config_load_with_backup(&cfg, CONFIG_PATH) == 0);
    if (config_migrate(&cfg)) {
        /* Persist migrated keys (show_providers, version) when possible. */
        if (have_cfg_file)
            (void)config_save(&cfg, CONFIG_PATH);
    }

    install_signal_handlers();

    /* --- framebuffer ------------------------------------------------------
     * Init failures exit 1: the wrapper script's EXIT trap restores the
     * Kindle framework, so a bare exit is safe here. */
    fbfd = fbink_open();
    if (fbfd < 0) {
        fprintf(stderr, "aiquota: cannot open framebuffer\n");
        return 1;
    }
    memset(&fbcfg, 0, sizeof(fbcfg));
    if (fbink_init(fbfd, &fbcfg) != 0) {
        fprintf(stderr, "aiquota: fbink_init failed\n");
        fbink_close(fbfd);
        return 1;
    }
    fbink_get_state(&fbcfg, &fbstate);
    if (!have_cfg_file) {
        cfg.touch_swap_xy = fbstate.touch_swap_axes ? 1 : 0;
        cfg.touch_mirror_x = fbstate.touch_mirror_x ? 1 : 0;
        cfg.touch_mirror_y = fbstate.touch_mirror_y ? 1 : 0;
        first_run_setup = 1; /* F07: guide into settings */
        open_settings_once = 1;
    }
    scr_w = (int)fbstate.screen_width;
    scr_h = (int)fbstate.screen_height;

    if (mode == MODE_DUMP_STATE) {
        dump_state(&fbstate);
        fbink_close(fbfd);
        return 0;
    }

    /* --- ui + input ------------------------------------------------------ */
    ui = ui_create(fbfd, scr_w, scr_h, (int)fbstate.screen_dpi, FONT_PATH);
    if (!ui) {
        fprintf(stderr, "aiquota: ui_create failed (is %s readable?)\n",
                FONT_PATH);
        fbink_close(fbfd);
        return 1;
    }
    apply_provider_order(ui, &cfg);
    build_fetch_url(fetch_url, sizeof(fetch_url), &cfg);
    input_fd = input_open(); /* first: input_wait_tap uses its statics */
    if (input_fd < 0 && mode != MODE_DEMO && mode != MODE_CALIBRATE) {
        /* No interaction is possible without the touch device: fatal. */
        fprintf(stderr, "aiquota: no touch input device found\n");
        rc = 1;
        goto out;
    }

    /* Every interactive mode; extension/bin/watchdog.sh reads the file. */
    wd_start(ping_path());
    wd_expect(WD_STEP_S);

    if (mode == MODE_CALIBRATE) {
        rc = run_calibrate(fbfd, scr_w, scr_h, input_fd);
        goto out;
    }

    host = api_host(cfg.api_url);
    quota_empty_snapshot(&snap);
    detail[0] = '\0';
    last_fetch = mono_now_s();
    snap_api_url[0] = '\0';
    req_gen = 0;
    fail_streak = 0;
    fail_backoff_until = 0;
    {
        int lis = light_idle_secs(&cfg);
        idle_deadline = lis > 0 ? mono_now_s() + lis : 0;
    }
    badge_deadline = 0; /* armed after the first idle light-off */

    /* F01: show last good snapshot for this URL while the first fetch runs. */
    if (mode != MODE_DEMO && mode != MODE_CALIBRATE) {
        char cached[8192];
        long long saved_at = 0;

        if (config_cache_load(CONFIG_CACHE_PATH, fetch_url, cached,
                              sizeof(cached), &saved_at) == 0) {
            QuotaSnapshot fresh;
            if (quota_parse_snapshot(cached, &fresh) == 0) {
                snap = fresh;
                apply_display_pref(&snap, &cfg);
                have_good = 1;
                snprintf(snap_api_url, sizeof(snap_api_url), "%s", fetch_url);
                snprintf(detail, sizeof(detail), "缓存");
            }
        }
    }

    if (mode == MODE_DEMO) {
        /* Render the embedded sample, once, even without a touch device. */
        QuotaSnapshot fresh;

        if (quota_parse_snapshot(DEMO_JSON, &fresh) == 0) {
            snap = fresh;
            have_good = 1;
        }
        snprintf(detail, sizeof(detail), "demo");
        {
            char ssid[33];
            int batt, chg;

            net_battery(&batt, &chg); /* real figures; network untouched */
            net_wifi_ssid(ssid, sizeof(ssid));
            ui_draw_dashboard(ui, &snap, host, "已连接", detail, 0,
                              net_wifi_state(), ssid, batt, chg,
                              cfg.auto_refresh_min);
            frame_is_dashboard = 1;
            frame_sig = frame_sig_of(&snap, "已连接", detail);
            remember_batt(batt, chg);
        }
        full_refresh(fbfd);
        if (input_fd < 0) {
            rc = 0; /* rendered; nothing to interact with */
            goto out;
        }
        st = ST_READY; /* the event loop re-renders demo data on REFRESH */
    }

    /* F07 first-run: no config file -> open settings before the first fetch so
     * the user can set api_url. Diagnose / providers live there too. */
    if (first_run_setup && mode == MODE_NORMAL) {
        snprintf(detail, sizeof(detail), "首次运行：请配置接口地址");
        st = ST_READY; /* skip auto fetch until settings return */
    }

    /* --- event loop ------------------------------------------------------- */
    /* auto_cycle: the LOADING about to run was timer-driven -- an auto tick
     * fetches behind the frame already on screen instead of flashing the
     * connecting skeleton nobody is there to watch. */
    for (;;) {
        int x = 0, y = 0;
        int wr;
        long deadline;
        long auto_deadline;
        UiAction action;

        if (st == ST_LOADING) {
            int want_close = 0;

            wd_expect(WD_STEP_S); /* net.c and http.c re-arm per blocking phase */
            if (!auto_cycle)
                paint_loading(fbfd, ui, host, cfg.api_url,
                              cfg.auto_refresh_min);
            req_gen++;
            /* R10: if the bound URL no longer matches, drop the old snapshot
             * so a failed fetch cannot look like the new endpoint's data. */
            build_fetch_url(fetch_url, sizeof(fetch_url), &cfg);
            apply_provider_order(ui, &cfg);
            if (have_good && snap_api_url[0] &&
                strcmp(snap_api_url, fetch_url) != 0) {
                quota_empty_snapshot(&snap);
                have_good = 0;
                snap_api_url[0] = '\0';
            }
            if (mode == MODE_DEMO) {
                st = do_fetch(fbfd, ui, cfg.api_url, host, &snap, &have_good,
                              1, cfg.auto_refresh_min, auto_cycle,
                              detail, sizeof(detail), 0, cfg.api_token);
                snprintf(snap_api_url, sizeof(snap_api_url), "%s", cfg.api_url);
            } else {
                /* Cancelable path (R06): WiFi on main, GET in a worker so
                 * 关闭 still hit-tests during a slow DNS/connect/recv. */
                FetchJob job;
                pthread_t tid;
                unsigned gen = req_gen;

                memset(&job, 0, sizeof(job));
                job.url = fetch_url;
                job.token = cfg.api_token;
                http_cancel_reset();
                diag_begin();
                {
                    long tw0 = mono_now_ms();
                    int wrc = net_ensure_wifi();
                    long wifi_ms = mono_now_ms() - tw0;

                if (wrc != 0) {
                    diag_finish(0, wifi_ms, 0, 0);
                    st = ST_ERROR;
                    snprintf(detail, sizeof(detail), "WiFi 未连接");
                    if (cfg.wifi_auto_off)
                        net_release_wifi();
                    paint_current_dashboard(fbfd, ui, &snap, host, st, detail,
                                            cfg.auto_refresh_min, WFM_GC16);
                } else if (pthread_create(&tid, NULL, fetch_job_main, &job) != 0) {
                    /* Fallback: synchronous GET if threads are unavailable. */
                    st = do_fetch(fbfd, ui, fetch_url, host, &snap, &have_good,
                                  0, cfg.auto_refresh_min, auto_cycle,
                                  detail, sizeof(detail), cfg.wifi_auto_off,
                                  cfg.api_token);
                    diag_finish(1, wifi_ms, have_good, 0);
                    if (have_good) {
                        apply_display_pref(&snap, &cfg);
                        snprintf(snap_api_url, sizeof(snap_api_url), "%s",
                                 fetch_url);
                    }
                } else {
                    while (!job.done) {
                        int fx = 0, fy = 0;
                        int fr = wait_tap_until(input_fd, scr_w, scr_h, &cfg,
                                                mono_now_s() + 1, &fx, &fy);
                        if (fr == 0 &&
                            ui_hit_button(ui, fx, fy) == UI_ACTION_CLOSE) {
                            http_cancel();
                            want_close = 1;
                            ui_flash_button(ui, fbfd, UI_ACTION_CLOSE);
                        }
                    }
                    pthread_join(tid, NULL);
                    if (gen != req_gen) {
                        free(job.body);
                        job.body = NULL;
                    } else if (want_close) {
                        free(job.body);
                        if (cfg.wifi_auto_off)
                            net_release_wifi();
                        break; /* leave the event loop */
                    } else if (!job.body) {
                        st = ST_ERROR;
                        if (++fail_streak > 6)
                            fail_streak = 6;
                        fail_backoff_until = mono_now_s() +
                            (30L << (fail_streak > 5 ? 5 : fail_streak));
                        snprintf(detail, sizeof(detail), "%s",
                                 job.err[0] ? job.err : "请求失败");
                        if (cfg.wifi_auto_off)
                            net_release_wifi();
                        paint_current_dashboard(fbfd, ui, &snap, host, st,
                                                detail, cfg.auto_refresh_min,
                                                auto_cycle ? WFM_GL16 : WFM_GC16);
                    } else if (job.status != 200) {
                        st = ST_ERROR;
                        snprintf(detail, sizeof(detail), "HTTP %d", job.status);
                        free(job.body);
                        if (cfg.wifi_auto_off)
                            net_release_wifi();
                        paint_current_dashboard(fbfd, ui, &snap, host, st,
                                                detail, cfg.auto_refresh_min,
                                                auto_cycle ? WFM_GL16 : WFM_GC16);
                    } else {
                        QuotaSnapshot fresh;
                        if (quota_parse_snapshot(job.body, &fresh) != 0) {
                            st = ST_ERROR;
                            snprintf(detail, sizeof(detail),
                                     "响应不是有效 JSON");
                        } else {
                            snap = fresh;
                            apply_display_pref(&snap, &cfg);
                            have_good = 1;
                            st = ST_READY;
                            fail_streak = 0;
                            fail_backoff_until = 0;
                            snprintf(detail, sizeof(detail), "%s", fetch_url);
                            snprintf(snap_api_url, sizeof(snap_api_url), "%s",
                                     fetch_url);
                            (void)config_cache_save(CONFIG_CACHE_PATH, fetch_url,
                                                    job.body,
                                                    (long long)time(NULL) * 1000LL);
                        }
                        free(job.body);
                        if (cfg.wifi_auto_off)
                            net_release_wifi();
                        {
                            unsigned long long sig =
                                frame_sig_of(&snap,
                                             st == ST_ERROR ? "失败" : "已连接",
                                             detail);
                            char ssid[33];
                            int batt, chg;
                            net_battery(&batt, &chg);
                            net_wifi_ssid(ssid, sizeof(ssid));
                            if (!(frame_is_dashboard && sig == frame_sig &&
                                  ui_batt_cells(batt) == frame_batt &&
                                  batt_chg_flag(chg) == frame_chg)) {
                                ui_draw_dashboard(ui, &snap, host,
                                    st == ST_ERROR ? "失败" : "已连接", detail,
                                    0, net_wifi_state(), ssid, batt, chg,
                                    cfg.auto_refresh_min);
                                frame_is_dashboard = 1;
                                frame_sig = sig;
                                remember_batt(batt, chg);
                                full_refresh_mode(fbfd,
                                    auto_cycle ? WFM_GL16 : WFM_GC16);
                            }
                        }
                    }
                    /* record structured diag for this attempt (F03) */
                    diag_finish(1, wifi_ms, have_good && st == ST_READY, 0);
                }
                } /* wifi_ms scope */
            }
            last_fetch = mono_now_s();
            auto_cycle = 0;
            if (want_close)
                break;
        }

        /* A dashboard-card tap is a user-driven refresh with a destination:
         * only open the modal after that request has finished, so the large
         * card is drawn from the newest snapshot.  On failure, the regular
         * fetch path keeps the last good snapshot and exposes the error state;
         * the requested card still opens instead of swallowing the tap. */
        if (popup_after_fetch != UI_CARD_NONE) {
            UiCard card = popup_after_fetch;

            popup_after_fetch = UI_CARD_NONE;
            if (run_card_popup(fbfd, input_fd, scr_w, scr_h, &cfg, ui,
                               &snap, host, st, detail, card,
                               &idle_deadline) < 0) {
                fprintf(stderr, "aiquota: touch input device lost\n");
                rc = 1;
                goto out;
            }
            continue;
        }

        /* READY/ERROR: park on one blocked poll until the next tap or the
         * earlier of two deadlines -- the auto-refresh tick and the idle
         * frontlight (a touch re-arms both). Taps queued in the kernel evdev
         * buffer while a fetch runs stay hit-testable against the NEW
         * screen. An overdue deadline (e.g. time spent in settings) returns
         * 1 immediately and fetches right away. */
        if (open_settings_once) {
            open_settings_once = 0;
            action = UI_ACTION_SETTINGS;
            /* Paint a hint once so the empty dashboard is not silent. */
            paint_current_dashboard(fbfd, ui, &snap, host, st, detail,
                                    cfg.auto_refresh_min, WFM_GC16);
            goto handle_action;
        }
        auto_deadline = cfg.auto_refresh_min > 0
                            ? (long)last_fetch + (long)cfg.auto_refresh_min * 60
                            : 0;
        if (auto_deadline > 0 && fail_backoff_until > auto_deadline)
            auto_deadline = fail_backoff_until;
        deadline = auto_deadline;
        if (idle_deadline > 0 && (deadline == 0 || idle_deadline < deadline))
            deadline = idle_deadline;
        if (badge_deadline > 0 && (deadline == 0 || badge_deadline < deadline))
            deadline = badge_deadline;
        wr = wait_tap_until(input_fd, scr_w, scr_h, &cfg, deadline, &x, &y);
        if (wr > 0) {
            long now = mono_now_s();

            /* The expiry may have been the light, the refresh, or both:
             * test each against its own deadline instead of assuming the
             * earlier one. The light-off must not drag the app through
             * ST_LOADING -- only the refresh does. */
            if (idle_deadline > 0 && now >= idle_deadline) {
                light_idle_off(); /* remembers the level for the next touch */
                idle_deadline = 0;
                maybe_refresh_badges(fbfd, ui);
                /* Keep sampling while parked with auto-refresh off (R08). */
                badge_deadline = now + BADGE_SAMPLE_S;
            }
            if (badge_deadline > 0 && now >= badge_deadline) {
                maybe_refresh_badges(fbfd, ui);
                badge_deadline = now + BADGE_SAMPLE_S;
            }
            if (auto_deadline > 0 && now >= auto_deadline) {
                auto_cycle = 1;
                st = ST_LOADING;
            }
            continue;
        }
        if (wr < 0) {
            fprintf(stderr, "aiquota: touch input device lost\n");
            rc = 1;
            goto out;
        }
        /* Restore an idle-dimmed light before the hit-test, so the wake tap
         * still lands on whatever the user aimed at (the first tap acts, it
         * does not just wake). No-op when the light was not dimmed by us. */
        light_idle_restore();
        {
        int lis = light_idle_secs(&cfg);
        idle_deadline = lis > 0 ? mono_now_s() + lis : 0;
    }
        badge_deadline = 0; /* active use: resume via the next idle-off hitch */
        action = ui_hit_button(ui, x, y);
        if (action == UI_ACTION_NONE) {
            UiCard card = ui_hit_card(ui, x, y);

            if (card == UI_CARD_NONE)
                continue;
            /* Reuse the normal cancelable loading flow, including its visible
             * connecting state, WiFi policy, parsing, cache and error fallback.
             * The pending card is opened at the top of the next READY/ERROR
             * iteration with the resulting snapshot. */
            popup_after_fetch = card;
            auto_cycle = 0;
            st = ST_LOADING;
            continue;
        }
        ui_flash_button(ui, fbfd, action);

handle_action:
        if (action == UI_ACTION_CLOSE)
            break;
        if (action == UI_ACTION_REFRESH) {
            st = ST_LOADING;
            continue;
        }
        if (action == UI_ACTION_SETTINGS) {
            Settings *sp = settings_create(ui, &cfg);
            SettingsHit hit;
            int done = 0;
            int changed, url_changed = 0;

            if (!sp) {
                /* OOM: keep the dashboard up, just skip editing. */
                fprintf(stderr, "aiquota: settings_create failed\n");
                continue;
            }
            settings_draw(sp);
            full_refresh(fbfd);

            while (!done) {
                /* No deadline: with the idle exit gone, the page waits for a
                 * tap for as long as it takes. An auto-refresh tick that
                 * passes while editing is simply skipped -- the main loop
                 * sees the overdue deadline on return and fetches then. */
                wr = wait_tap_until(input_fd, scr_w, scr_h, &cfg, 0, &x, &y);
                if (wr != 0) {
                    fprintf(stderr, "aiquota: touch input device lost\n");
                    settings_destroy(sp);
                    rc = 1;
                    goto out;
                }
                hit = settings_handle_tap(sp, fbfd, x, y);
                if (hit == SET_HIT_BACK) {
                    done = 1;
                    continue;
                }
                if (hit == SET_HIT_NONE)
                    continue; /* toggle flipped / stray tap: page already updated */

                if (hit == SET_HIT_EDIT_PROVIDERS) {
                    int pdone = 0;
                    settings_providers_draw(sp);
                    full_refresh(fbfd);
                    while (!pdone) {
                        wr = wait_tap_until(input_fd, scr_w, scr_h, &cfg,
                                            0, &x, &y);
                        if (wr != 0) {
                            fprintf(stderr, "aiquota: touch input device lost\n");
                            settings_destroy(sp);
                            rc = 1;
                            goto out;
                        }
                        pdone = settings_providers_handle_tap(sp, fbfd, x, y);
                    }
                    settings_draw(sp);
                    full_refresh(fbfd);
                    continue;
                }

                if (hit == SET_HIT_DIAGNOSE) {
                    /* F03: cancelable probe; never clears have_good snapshot. */
                    char durl[768];
                    FetchJob djob;
                    pthread_t dtid;
                    int dclose = 0;
                    char dmsg[DETAIL_SIZE];

                    /* Diagnose against the settings working copy (not saved yet). */
                    {
                        AppConfig tmp = cfg;
                        snprintf(tmp.api_url, sizeof(tmp.api_url), "%s",
                                 settings_url(sp));
                        snprintf(tmp.show_providers, sizeof(tmp.show_providers),
                                 "%s", settings_providers(sp));
                        build_fetch_url(durl, sizeof(durl), &tmp);
                    }
                    memset(&djob, 0, sizeof(djob));
                    djob.url = durl;
                    djob.token = cfg.api_token;
                    http_cancel_reset();
                    diag_begin();
                    snprintf(dmsg, sizeof(dmsg), "Diagnosing...");
                    settings_draw(sp);
                    ui_text(ui, dmsg, ui->px_subtitle, 0, ui->h / 2, ui->w,
                            ui->line_h, UI_ALIGN_CENTER, FG_BLACK, 0);
                    full_refresh(fbfd);
                    {
                        long tw0 = mono_now_ms();
                        int wrc = net_ensure_wifi();
                        long wifi_ms = mono_now_ms() - tw0;
                        if (wrc != 0) {
                            diag_finish(0, wifi_ms, 0, 0);
                            if (cfg.wifi_auto_off)
                                net_release_wifi();
                        } else if (pthread_create(&dtid, NULL, fetch_job_main,
                                                  &djob) != 0) {
                            int stcode = 0;
                            char err[256];
                            char *body = http_get(durl, &stcode, err, sizeof(err));
                            int jok = 0;
                            if (body) {
                                QuotaSnapshot tmp;
                                jok = (quota_parse_snapshot(body, &tmp) == 0);
                                free(body);
                            }
                            diag_finish(1, wifi_ms, jok, 0);
                            if (cfg.wifi_auto_off)
                                net_release_wifi();
                        } else {
                            while (!djob.done) {
                                int fx = 0, fy = 0;
                                int fr = wait_tap_until(input_fd, scr_w, scr_h,
                                                        &cfg, mono_now_s() + 1,
                                                        &fx, &fy);
                                if (fr == 0 &&
                                    ui_hit_button(ui, fx, fy) == UI_ACTION_CLOSE) {
                                    http_cancel();
                                    dclose = 1;
                                }
                            }
                            pthread_join(dtid, NULL);
                            if (dclose) {
                                free(djob.body);
                                if (cfg.wifi_auto_off)
                                    net_release_wifi();
                                settings_draw(sp);
                                full_refresh(fbfd);
                                continue;
                            }
                            {
                                int jok = 0;
                                long j0 = mono_now_ms();
                                if (djob.body && djob.status == 200) {
                                    QuotaSnapshot tmp;
                                    jok = (quota_parse_snapshot(djob.body,
                                                                &tmp) == 0);
                                }
                                diag_finish(1, wifi_ms, jok, mono_now_ms() - j0);
                            }
                            free(djob.body);
                            if (cfg.wifi_auto_off)
                                net_release_wifi();
                        }
                    }
                    /* Show report in multi-line info popup; keep snap intact. */
                    {
                        int page = 0;
                        int pages;
                        FBInkRect prect;
                        snprintf(detail, sizeof(detail), "%s",
                                 g_last_diag.report[0] ? g_last_diag.report
                                                       : "no diag");
                        pages = ui_popup_info_pages(ui, detail);
                        if (pages < 1)
                            pages = 1;
                        ui_popup_draw_ex(ui, &snap, "诊断", detail, UI_CARD_INFO,
                                         page, &prect);
                        full_refresh(fbfd);
                        for (;;) {
                            wr = wait_tap_until(input_fd, scr_w, scr_h, &cfg,
                                                0, &x, &y);
                            if (wr != 0)
                                break;
                            {
                                int ph = ui_popup_hit(ui, &prect, UI_CARD_INFO,
                                                      pages, x, y);
                                if (ph == 1 && page > 0) {
                                    page--;
                                    ui_popup_draw_ex(ui, &snap, "诊断", detail,
                                                     UI_CARD_INFO, page, &prect);
                                    full_refresh(fbfd);
                                } else if (ph == 2 && page + 1 < pages) {
                                    page++;
                                    ui_popup_draw_ex(ui, &snap, "诊断", detail,
                                                     UI_CARD_INFO, page, &prect);
                                    full_refresh(fbfd);
                                } else if (ph == 0) {
                                    break;
                                }
                            }
                        }
                    }
                    settings_draw(sp);
                    full_refresh(fbfd);
                    continue;
                }

                /* --- keypad editor over one field ------------------------ */
                {
                    char edit[sizeof(cfg.api_url)];
                    int kr = 0;

                    if (hit != SET_HIT_EDIT_URL && hit != SET_HIT_EDIT_REFRESH &&
                        hit != SET_HIT_EDIT_IDLE) {
                        continue;
                    }

                    if (!kp)
                        kp = keypad_create(ui);
                    if (!kp) {
                        /* OOM: keep the settings page up, skip the editor. */
                        fprintf(stderr, "aiquota: keypad_create failed\n");
                        continue;
                    }

                    if (hit == SET_HIT_EDIT_URL) {
                        snprintf(edit, sizeof(edit), "%s", settings_url(sp));
                        keypad_set_digits_only(kp, 0); /* host:port charset */
                        keypad_draw(kp, "接口地址", edit);
                    } else if (hit == SET_HIT_EDIT_REFRESH) {
                        snprintf(edit, sizeof(edit), "%d",
                                 settings_refresh_min(sp));
                        keypad_set_digits_only(kp, 1); /* pure-number field */
                        keypad_draw(kp, "自动刷新(分钟)", edit);
                    } else if (hit == SET_HIT_EDIT_IDLE) {
                        snprintf(edit, sizeof(edit), "%d",
                                 settings_idle_min(sp));
                        keypad_set_digits_only(kp, 1);
                        keypad_draw(kp, "Idle light (min)", edit);
                    }
                    full_refresh(fbfd);

                    while (kr == 0) {
                        wr = wait_tap_until(input_fd, scr_w, scr_h, &cfg,
                                            0, &x, &y);
                        if (wr != 0) {
                            fprintf(stderr,
                                    "aiquota: touch input device lost\n");
                            settings_destroy(sp);
                            rc = 1;
                            goto out;
                        }
                        kr = keypad_handle_tap(kp, fbfd, x, y, edit,
                                               sizeof(edit));
                    }

                    if (kr == 1) { /* confirm: validate into the working copy */
                        if (hit == SET_HIT_EDIT_URL) {
                            char norm[sizeof(cfg.api_url)];

                            config_normalize_url(norm, sizeof(norm), edit);
                            url_changed |= settings_set_url(sp, norm);
                        } else if (hit == SET_HIT_EDIT_REFRESH) {
                            /* edit holds up to 511 keystrokes, far past int
                             * range: atoi overflow is UB and glibc wraps
                             * negative, which the downstream clamp reads as
                             * "off" -- the wrong direction for a big number.
                             * strtol saturates in the long domain instead
                             * (and stops at '.'/':', so "1.5" parses as 1);
                             * clamp there to the documented [0, 60], then
                             * hand over an int that cannot overflow. */
                            long minutes = strtol(edit, NULL, 10);

                            if (minutes < 0)
                                minutes = 0;
                            if (minutes > CONFIG_AUTO_REFRESH_MAX)
                                minutes = CONFIG_AUTO_REFRESH_MAX;
                            settings_set_refresh_min(sp, (int)minutes);
                        } else if (hit == SET_HIT_EDIT_IDLE) {
                            long minutes = strtol(edit, NULL, 10);

                            if (minutes < 0)
                                minutes = 0;
                            if (minutes > CONFIG_LIGHT_IDLE_MAX)
                                minutes = CONFIG_LIGHT_IDLE_MAX;
                            settings_set_light_idle_min(sp, (int)minutes);
                        }
                    }
                    /* Confirm and cancel both land back on the page, redrawn
                     * from the working copy. */
                    settings_draw(sp);
                    full_refresh(fbfd);
                }
            }

            {
                char prev_prov[128];
                snprintf(prev_prov, sizeof(prev_prov), "%s", cfg.show_providers);
                changed = settings_commit(sp, &cfg);
                providers_changed =
                    strcmp(prev_prov, cfg.show_providers) != 0;
            }
            if (url_changed)
                host = api_host(cfg.api_url);
            apply_provider_order(ui, &cfg);
            build_fetch_url(fetch_url, sizeof(fetch_url), &cfg);

            /* Persist before leaving the page. Memory cfg is already updated;
             * a failed write must be visible and retriable, never reported as
             * success. Any tap retries; BACK is not required again. */
            if (changed) {
                while (config_save(&cfg, CONFIG_PATH) != 0) {
                    fprintf(stderr, "aiquota: cannot write %s\n", CONFIG_PATH);
                    settings_draw(sp);
                    ui_text(ui, "保存失败，点击重试", ui->px_subtitle,
                            0, ui->h / 2, ui->w, ui->line_h,
                            UI_ALIGN_CENTER, FG_BLACK, 0);
                    full_refresh(fbfd);
                    wr = wait_tap_until(input_fd, scr_w, scr_h, &cfg,
                                        0, &x, &y);
                    if (wr != 0) {
                        fprintf(stderr,
                                "aiquota: touch input device lost\n");
                        settings_destroy(sp);
                        rc = 1;
                        goto out;
                    }
                }
            }
            settings_destroy(sp);
            if (url_changed || providers_changed || first_run_setup) {
                /* New endpoint / filter / first-run leave settings: refetch. */
                if (url_changed || providers_changed) {
                    quota_empty_snapshot(&snap);
                    have_good = 0;
                    snap_api_url[0] = '\0';
                }
                first_run_setup = 0;
                st = ST_LOADING;
            } else {
                /* Same snapshot as before settings: keep its signature,
                 * adopt the fresher battery cells so the skip test stays
                 * honest. GL16 wakes the panel without the GC16 flash. */
                paint_current_dashboard(fbfd, ui, &snap, host, st, detail,
                                        cfg.auto_refresh_min, WFM_GL16);
            }
            /* Time spent editing does not count as dashboard idle. */
            {
        int lis = light_idle_secs(&cfg);
        idle_deadline = lis > 0 ? mono_now_s() + lis : 0;
    }
        }
    }

out:
    /* Clean exit (0) or device loss (1): either way the wrapper script's
     * EXIT trap brings the Kindle framework back. An idle-dimmed light gets
     * its level back first: leaving the dashboard must not hand powerd a 0
     * the user never chose. A light turned off by hand is never pending and
     * stays off. */
    light_idle_restore();
    if (kp)
        keypad_destroy(kp);
    if (ui)
        ui_destroy(ui);
    if (input_fd >= 0)
        close(input_fd);
    fbink_close(fbfd); /* fbfd >= 0 here: the open-failure path returned earlier */
    return rc;
}
