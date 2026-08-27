/* aiquota -- AI quota dashboard for Kindle: entry point, state machine and
 * debug switches. Mirrors the KOReader plugin's flow (readingquota.koplugin/
 * quotapage.lua): paint the loading skeleton first, fetch only after the
 * first paint, then serve taps on the refresh / settings / close buttons.
 *
 * State machine: LOADING -> READY | ERROR, plus a SETTINGS sub-mode entered
 * from READY/ERROR (which itself opens KEYPAD editors for its fields).
 * Modes: no args = normal run; --demo renders embedded demo data without
 * touching the network; --dump-state prints FBInkState; --calibrate derives
 * the touch transform over SSH. A clean exit returns 0 and the wrapper script
 * (extension/bin/aiquota.sh) restores the framework. */
#define _POSIX_C_SOURCE 200809L /* sigaction */

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
#include "net.h"
#include "quota.h"
#include "settings.h"
#include "ui.h"
#include "watchdog.h"

#define FONT_PATH "/mnt/us/extensions/aiquota/bin/NotoSansSC-subset.ttf"
#define PING_PATH "/var/tmp/aiquota.ping"
#define DETAIL_SIZE 600 /* holds a 512-char URL or an error line */

/* Dashboard states; the keypad is a sub-mode handled inside the main loop. */
enum { ST_LOADING = 0, ST_READY, ST_ERROR };

enum { MODE_NORMAL = 0, MODE_DEMO, MODE_DUMP_STATE, MODE_CALIBRATE };

/* Full example response from docs/superpowers/plans/usage-api.md, for --demo. */
static const char DEMO_JSON[] =
    "{\n"
    "  \"displayMode\": \"remaining\",\n"
    "  \"alertPercent\": 85,\n"
    "  \"generatedAt\": 1785695952333,\n"
    "  \"grok\": {\n"
    "    \"fetchedAt\": 1785695890000,\n"
    "    \"five\": null,\n"
    "    \"seven\": { \"used\": 8, \"resetAt\": 1788408060000 },\n"
    "    \"monthly\": null\n"
    "  },\n"
    "  \"claude\": {\n"
    "    \"fetchedAt\": 1785695900000,\n"
    "    \"five\": { \"used\": 18, \"resetAt\": 1785712676000 },\n"
    "    \"seven\": { \"used\": 42, \"resetAt\": 1786300383000 }\n"
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
    "  \"opencode\": {\n"
    "    \"fetchedAt\": 1785695951200,\n"
    "    \"rolling\": { \"used\": 1, \"resetAt\": 1785712676000, \"status\": \"ok\" },\n"
    "    \"weekly\": { \"used\": 46, \"resetAt\": 1786000859452, \"status\": \"ok\" },\n"
    "    \"monthly\": { \"used\": 23, \"resetAt\": 1788134400000, \"status\": \"ok\" },\n"
    "    \"five\": { \"used\": 1, \"resetAt\": 1785712676000, \"status\": \"ok\" },\n"
    "    \"seven\": { \"used\": 46, \"resetAt\": 1786000859452, \"status\": \"ok\" }\n"
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

/* Upper bound of one poll segment when the idle timer is off (0): the
 * heartbeat contract forbids unbounded announcements, so "wait forever" is
 * built from bounded segments with the watchdog re-armed before each one.
 * Segment length trades wedge-detection latency against a tiny gesture risk:
 * a touch whose down and up straddle a segment boundary is dropped (state
 * self-recovers on the next touch). 5 min puts worst-case wedged-app
 * detection at ~7 min here while the straddle odds are ~1 in 50 000 taps. */
#define TAP_WAIT_CHUNK_S 300

/* One tap wait, wrapped in the heartbeat contract (see watchdog.h): announce
 * the whole idle budget before blocking, then drop back to the short local-
 * work bound. With this, the app has no unbounded wait left anywhere, which
 * is what lets watchdog.sh read a missing beat as "wedged" rather than "the
 * user put the Kindle down".
 * timeout_s <= 0 means unlimited (idle timer off): polls in TAP_WAIT_CHUNK_S
 * segments forever, so rc==1 never escapes -- only taps or device errors do.
 * Returns 0 tap / 1 timeout / -1 device error, like input_wait_tap. */
static int wait_tap_for(int fd, int scr_w, int scr_h, const AppConfig *cfg,
                        int timeout_s, int *x, int *y)
{
    int rc;

    if (timeout_s > 0) {
        wd_expect(timeout_s + WD_SLACK_S);
        rc = input_wait_tap(fd, scr_w, scr_h, cfg, timeout_s, x, y);
        wd_expect(WD_STEP_S);
        return rc;
    }

    for (;;) {
        wd_expect(TAP_WAIT_CHUNK_S + WD_SLACK_S);
        rc = input_wait_tap(fd, scr_w, scr_h, cfg, TAP_WAIT_CHUNK_S, x, y);
        wd_expect(WD_STEP_S);
        if (rc != 1)
            return rc; /* tap or device error; chunk expiry just re-arms */
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

/* One full-screen GC16 refresh after a complete redraw. The ui/keypad
 * modules draw everything with their own no_refresh configs; main only
 * issues refreshes once a full frame is drawn (UIManager:setDirty style). */
static void full_refresh(int fbfd)
{
    FBInkConfig rcfg;

    memset(&rcfg, 0, sizeof(rcfg));
    rcfg.no_refresh = false;
    rcfg.wfm_mode = WFM_GC16; /* full high-fidelity re-render */
    /* An empty region (0x0 @ 0,0) means a full-screen refresh; the header
     * marks fbink_refresh_rect's rect nonnull, so use fbink_refresh. */
    fbink_refresh(fbfd, 0, 0, 0, 0, &rcfg);
}

/* LOADING paint: empty skeleton + "connecting" card, like quotapage.lua's
 * onShow drawing the skeleton before fetch() runs. */
static void paint_loading(int fbfd, Ui *ui, const char *host, const char *api_url)
{
    QuotaSnapshot empty;

    quota_empty_snapshot(&empty);
    ui_draw_dashboard(ui, &empty, host, "连接中", api_url, 1, net_wifi_state());
    full_refresh(fbfd);
}

/* One fetch+paint cycle: WiFi check, HTTP GET, JSON parse, dashboard redraw.
 * A successful parse replaces *snap; on error the last good snapshot is kept
 * (or an empty one when nothing ever succeeded), and detail carries the URL
 * or the error text. release_wifi: hand the radio back after the attempt
 * (only effective when net.c actually raised it). Returns ST_READY/ST_ERROR. */
static int do_fetch(int fbfd, Ui *ui, const char *api_url, const char *host,
                    QuotaSnapshot *snap, int *have_good, int demo,
                    char *detail, size_t detail_size, int release_wifi)
{
    QuotaSnapshot fresh;
    int st = ST_READY;

    if (demo) {
        /* Demo mode re-renders the embedded sample; the network is never
         * touched, so refresh cycles cannot fail here. */
        if (quota_parse_snapshot(DEMO_JSON, &fresh) == 0) {
            *snap = fresh;
            *have_good = 1;
        }
        snprintf(detail, detail_size, "demo");
    } else {
        int status = 0;
        char errbuf[256];
        char *body = NULL;

        errbuf[0] = '\0';
        if (net_ensure_wifi() != 0) {
            /* Checked every cycle: WiFi may drop while the app is open. */
            st = ST_ERROR;
            snprintf(detail, detail_size, "WiFi 未连接");
        } else {
            body = http_get(api_url, &status, errbuf, sizeof(errbuf));
            if (!body) {
                st = ST_ERROR;
                snprintf(detail, detail_size, "%s", errbuf);
            } else if (status != 200) {
                st = ST_ERROR;
                snprintf(detail, detail_size, "HTTP %d", status);
            } else if (quota_parse_snapshot(body, &fresh) != 0) {
                st = ST_ERROR;
                snprintf(detail, detail_size, "响应不是有效 JSON");
            } else {
                *snap = fresh;
                *have_good = 1;
                snprintf(detail, detail_size, "%s", api_url);
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
    ui_draw_dashboard(ui, snap, host, st == ST_ERROR ? "失败" : "已连接",
                      detail, 0, net_wifi_state());
    full_refresh(fbfd);
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
        int wr = wait_tap_for(input_fd, scr_w, scr_h, &ccfg,
                              ccfg.idle_timeout_s, &px[i], &py[i]);

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
    time_t last_fetch;      /* when the current dashboard data was fetched */
    int have_good = 0;
    int have_cfg_file;
    int mode = MODE_NORMAL;
    int fbfd = -1, input_fd = -1;
    int scr_w = 0, scr_h = 0;
    int st = ST_LOADING;
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
    have_cfg_file = (config_load(&cfg, CONFIG_PATH) == 0);

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
    input_fd = input_open(); /* first: input_wait_tap uses its statics */
    if (input_fd < 0 && mode != MODE_DEMO && mode != MODE_CALIBRATE) {
        /* No interaction is possible without the touch device: fatal. */
        fprintf(stderr, "aiquota: no touch input device found\n");
        rc = 1;
        goto out;
    }

    /* Every interactive mode; extension/bin/watchdog.sh reads the file. */
    wd_start(PING_PATH);
    wd_expect(WD_STEP_S);

    if (mode == MODE_CALIBRATE) {
        rc = run_calibrate(fbfd, scr_w, scr_h, input_fd);
        goto out;
    }

    host = api_host(cfg.api_url);
    quota_empty_snapshot(&snap);
    detail[0] = '\0';
    last_fetch = time(NULL);

    if (mode == MODE_DEMO) {
        /* Render the embedded sample, once, even without a touch device. */
        QuotaSnapshot fresh;

        if (quota_parse_snapshot(DEMO_JSON, &fresh) == 0) {
            snap = fresh;
            have_good = 1;
        }
        snprintf(detail, sizeof(detail), "demo");
        ui_draw_dashboard(ui, &snap, host, "已连接", detail, 0,
                          net_wifi_state());
        full_refresh(fbfd);
        if (input_fd < 0) {
            rc = 0; /* rendered; nothing to interact with */
            goto out;
        }
        st = ST_READY; /* the event loop re-renders demo data on REFRESH */
    }

    /* --- event loop ------------------------------------------------------- */
    for (;;) {
        int x = 0, y = 0;
        int wr;
        int wait_s;
        UiAction action;

        if (st == ST_LOADING) {
            wd_expect(WD_STEP_S); /* net.c and http.c re-arm per blocking phase */
            paint_loading(fbfd, ui, host, cfg.api_url);
            st = do_fetch(fbfd, ui, cfg.api_url, host, &snap, &have_good,
                          mode == MODE_DEMO, detail, sizeof(detail),
                          cfg.wifi_auto_off);
            last_fetch = time(NULL); /* auto-refresh counts from this data */
        }

        /* READY/ERROR: wait for a tap on the button row. The poll bound is
         * whichever comes first -- the idle deadline (0 = unlimited) or the
         * next auto-refresh. Taps made while a blocking fetch runs stay
         * queued in the kernel evdev buffer and are hit-tested here against
         * the NEW screen. */
        wait_s = cfg.idle_timeout_s;
        if (cfg.auto_refresh_min > 0) {
            long until = last_fetch + (long)cfg.auto_refresh_min * 60 -
                         (long)time(NULL);

            if (until <= 0) {
                st = ST_LOADING;
                continue;
            }
            if (wait_s <= 0 || until < wait_s)
                wait_s = (int)until;
        }

        wr = wait_tap_for(input_fd, scr_w, scr_h, &cfg, wait_s, &x, &y);
        if (wr > 0) {
            /* Timeout. With auto-refresh on, let the loop top decide: it
             * turns an overdue refresh into a fetch and otherwise waits
             * again (poll can wake a hair early; re-checking the clock
             * there avoids closing on a rounding artifact). An auto-refresh
             * never feeds the idle timer, so with a finite idle timeout a
             * Kindle put face-up still closes the app on schedule; with
             * idle off, only 关闭 ends the session here. */
            if (cfg.auto_refresh_min > 0)
                continue;
            break; /* idle: close ourselves so the framework comes back */
        }
        if (wr < 0) {
            fprintf(stderr, "aiquota: touch input device lost\n");
            rc = 1;
            goto out;
        }
        action = ui_hit_button(ui, x, y);
        if (action == UI_ACTION_NONE)
            continue;
        ui_flash_button(ui, fbfd, action);

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
                wr = wait_tap_for(input_fd, scr_w, scr_h, &cfg,
                                  cfg.idle_timeout_s, &x, &y);
                if (wr > 0) {
                    settings_destroy(sp);
                    goto out; /* idle: drop edits, release the framework */
                }
                if (wr < 0) {
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

                /* --- keypad editor over one field ------------------------ */
                {
                    char edit[sizeof(cfg.api_url)];
                    int kr = 0;

                    if (!kp)
                        kp = keypad_create(ui);
                    if (!kp) {
                        /* OOM: keep the settings page up, skip the editor. */
                        fprintf(stderr, "aiquota: keypad_create failed\n");
                        continue;
                    }

                    if (hit == SET_HIT_EDIT_URL) {
                        snprintf(edit, sizeof(edit), "%s", settings_url(sp));
                        keypad_draw(kp, "接口地址", edit);
                    } else if (hit == SET_HIT_EDIT_REFRESH) {
                        snprintf(edit, sizeof(edit), "%d",
                                 settings_refresh_min(sp));
                        keypad_draw(kp, "自动刷新(分钟)", edit);
                    } else {
                        snprintf(edit, sizeof(edit), "%d",
                                 settings_idle_s(sp));
                        keypad_draw(kp, "自动退出(0=无限)", edit);
                    }
                    full_refresh(fbfd);

                    while (kr == 0) {
                        wr = wait_tap_for(input_fd, scr_w, scr_h, &cfg,
                                          cfg.idle_timeout_s, &x, &y);
                        if (wr > 0) {
                            settings_destroy(sp);
                            goto out; /* idle: drop edits, release framework */
                        }
                        if (wr < 0) {
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
                            settings_set_refresh_min(sp, atoi(edit));
                        } else {
                            settings_set_idle_s(sp, atoi(edit));
                        }
                    }
                    /* Confirm and cancel both land back on the page, redrawn
                     * from the working copy. */
                    settings_draw(sp);
                    full_refresh(fbfd);
                }
            }

            changed = settings_commit(sp, &cfg);
            settings_destroy(sp);
            if (url_changed)
                host = api_host(cfg.api_url);
            if (changed && config_save(&cfg, CONFIG_PATH) != 0)
                fprintf(stderr, "aiquota: cannot write %s\n", CONFIG_PATH);
            if (url_changed) {
                /* New endpoint: keep the confirm-address-refetches semantic. */
                st = ST_LOADING;
            } else {
                ui_draw_dashboard(ui, &snap, host,
                                  st == ST_ERROR ? "失败" : "已连接",
                                  detail, 0, net_wifi_state());
                full_refresh(fbfd);
            }
        }
    }

out:
    /* Clean exit (0) or device loss (1): either way the wrapper script's
     * EXIT trap brings the Kindle framework back. */
    if (kp)
        keypad_destroy(kp);
    if (ui)
        ui_destroy(ui);
    if (input_fd >= 0)
        close(input_fd);
    fbink_close(fbfd); /* fbfd >= 0 here: the open-failure path returned earlier */
    return rc;
}
