/* Dashboard rendering: a pixel port of quotapage.lua (layout math 39-71, card
 * builders 87-204, buttons 221-234, title bar 257-287) and quotabar.lua:36-61.
 * Everything is drawn through cfg_draw (no_refresh); callers issue one
 * fbink_refresh_rect per paint cycle, like UIManager:setDirty batches. */
#define _POSIX_C_SOURCE 200112L
#define _DEFAULT_SOURCE /* usleep under -std=c99 */

#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define UI_TEXT_BUF 600 /* must hold the longest detail string (a 512-char URL) */

int ui_scale(const Ui *ui, int n)
{
    int dpi = ui->dpi > 0 ? ui->dpi : 167;

    /* lround(n * dpi / 167.0) == floor((2*n*dpi + 167) / 334) for n, dpi >= 0.
     * Integer math keeps the link line free of -lm (same stance as quota.c). */
    return (int)((2L * n * dpi + 167L) / 334L);
}

void ui_fill(const Ui *ui, int x, int y, int w, int h, uint8_t gray)
{
    FBInkRect r;

    if (w <= 0 || h <= 0)
        return;
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > ui->w)
        w = ui->w - x;
    if (y + h > ui->h)
        h = ui->h - y;
    if (w <= 0 || h <= 0)
        return;

    r.left = (unsigned short)x;
    r.top = (unsigned short)y;
    r.width = (unsigned short)w;
    r.height = (unsigned short)h;
    fbink_fill_rect_gray(ui->fbfd, &ui->cfg_draw, &r, false, gray);
}

int ui_measure(const Ui *ui, const char *text, int size_px, int *out_w, int *out_h)
{
    FBInkOTConfig cfg;
    FBInkOTFit fit;

    *out_w = 0;
    *out_h = 0;
    if (!text || !*text || size_px <= 0)
        return 0;

    memset(&cfg, 0, sizeof(cfg));
    memset(&fit, 0, sizeof(fit));
    cfg.style = FNT_REGULAR;
    cfg.size_px = (unsigned short)size_px;
    cfg.compute_only = true;
    /* No margins: the printable area is the full screen, so nothing wraps. */
    if (fbink_print_ot(ui->fbfd, text, &cfg, &ui->cfg_draw, &fit) < 0)
        return -1;
    *out_w = fit.bbox.width;
    *out_h = fit.bbox.height;
    return 0;
}

/* Copy src into dst, shortening at UTF-8 char boundaries with a trailing "…"
 * until it renders within max_w at size_px. dst must be UI_TEXT_BUF bytes. */
static int fit_text(const Ui *ui, char *dst, const char *src, int size_px, int max_w,
                    int *out_w, int *out_h)
{
    size_t len = strlen(src);
    int w = 0, h = 0;

    if (len > UI_TEXT_BUF - 4) { /* keep room for the ellipsis + NUL */
        len = UI_TEXT_BUF - 4;
        while (len > 0 && (src[len] & 0xC0) == 0x80) /* don't split a UTF-8 sequence */
            len--;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';

    if (ui_measure(ui, dst, size_px, &w, &h) != 0)
        return -1;
    while (w > max_w && len > 0) {
        char tmp[UI_TEXT_BUF];
        int tw = 0, th = 0;

        /* Drop the last UTF-8 character (continuation bytes first). */
        do {
            len--;
        } while (len > 0 && (dst[len] & 0xC0) == 0x80);
        dst[len] = '\0';

        memcpy(tmp, dst, len);
        memcpy(tmp + len, "…", 3); /* U+2026 */
        tmp[len + 3] = '\0';
        if (ui_measure(ui, tmp, size_px, &tw, &th) != 0)
            return -1;
        w = tw;
        h = th;
        if (w <= max_w) {
            memcpy(dst, tmp, len + 4);
            break;
        }
    }
    if (len == 0 && w > max_w) {
        /* Not even "…" fits: draw nothing. */
        dst[0] = '\0';
        w = 0;
    }
    *out_w = w;
    *out_h = h;
    return 0;
}

void ui_text(const Ui *ui, const char *text, int size_px, int x, int y, int w, int h,
             int align, int fg_index, int inverted)
{
    char buf[UI_TEXT_BUF];
    int tw = 0, th = 0;
    int area_w, area_h, right, bottom;
    FBInkOTConfig cfg;
    FBInkConfig fbc;

    if (!text || !*text || w <= 0 || h <= 0 || size_px <= 0)
        return;
    if (fit_text(ui, buf, text, size_px, w, &tw, &th) != 0 || tw <= 0)
        return;

    if (align == UI_ALIGN_RIGHT)
        x += w - tw;
    else if (align == UI_ALIGN_CENTER)
        x += (w - tw) / 2;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;

    /* Printable area: the measured width plus slack, so the fitted single line
     * can never wrap; at least one full line box tall, but never two lines. */
    area_w = tw + 4;
    area_h = th + 2;
    if (area_h < size_px + size_px / 2)
        area_h = size_px + size_px / 2;

    memset(&cfg, 0, sizeof(cfg));
    cfg.margins.left = (short)x;
    cfg.margins.top = (short)y;
    right = ui->w - x - area_w;
    bottom = ui->h - y - area_h;
    cfg.margins.right = (short)(right > 0 ? right : 0);
    cfg.margins.bottom = (short)(bottom > 0 ? bottom : 0);
    cfg.style = FNT_REGULAR;
    cfg.size_px = (unsigned short)size_px;

    fbc = ui->cfg_draw;
    fbc.fg_color = (FG_COLOR_INDEX_T)fg_index;
    fbc.is_inverted = inverted ? true : false;
    fbink_print_ot(ui->fbfd, buf, &cfg, &fbc, NULL);
}

void ui_refresh(int fbfd, const FBInkRect *rect, int wfm_mode)
{
    FBInkConfig cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.wfm_mode = (WFM_MODE_INDEX_T)wfm_mode;
    fbink_refresh_rect(fbfd, rect, &cfg);
}

/* Segmented bar, quotabar.lua:36-61 with segments = 12. Any progress lights at
 * least one block; only a full bar lights the last one. */
static void draw_bar(const Ui *ui, int x, int y, double pct, int valid)
{
    int filled, i;
    double pitch;

    if (pct <= 0.0)
        filled = 0;
    else if (pct >= 1.0)
        filled = 12;
    else {
        filled = (int)(pct * 12.0 + 0.5); /* == floor for pct >= 0 */
        if (filled < 1)
            filled = 1;
        if (filled > 11)
            filled = 11;
    }

    /* Each block owns `pitch` pixels and gives up gap2 of them to its right
     * neighbour; both edges derive from the same rounding so the last block
     * lands exactly on x + card_inner. */
    pitch = (double)(ui->card_inner + ui->gap2) / 12.0;
    for (i = 0; i < 12; i++) {
        int left = (int)(i * pitch + 0.5);
        int right = (int)((i + 1) * pitch + 0.5) - ui->gap2;
        int bw = right - left;
        uint8_t gray;

        if (bw < 1)
            bw = 1;
        gray = i < filled ? UI_INK_BLACK : (valid ? UI_INK_LIGHT : UI_INK_GRAY);
        ui_fill(ui, x + left, y, bw, ui->bar_h, gray);
    }
}

/* FrameContainer port: border-thin black frame around a white card. */
static void draw_card_frame(const Ui *ui, int x, int y)
{
    ui_fill(ui, x, y, ui->card_w, ui->card_h, UI_INK_BLACK);
    ui_fill(ui, x + ui->border, y + ui->border,
            ui->card_w - 2 * ui->border, ui->card_h - 2 * ui->border, UI_INK_WHITE);
}

/* buildValueLine port: label left (label font), value right (percent font). */
static void draw_value_line(const Ui *ui, int x, int y, const char *label, const char *value)
{
    int label_w = (int)(ui->card_inner * 0.42);

    ui_text(ui, label, ui->px_label, x, y, label_w, ui->line_h,
            UI_ALIGN_LEFT, FG_BLACK, 0);
    ui_text(ui, value, ui->px_percent, x + label_w, y, ui->card_inner - label_w,
            ui->line_h, UI_ALIGN_RIGHT, FG_BLACK, 0);
}

/* buildWindow port (quotapage.lua:102-128): value line, bar, reset detail. */
static void draw_window_block(const Ui *ui, int x, int y, const char *label,
                              const QuotaWindow *win, const QuotaSnapshot *snap)
{
    char value_text[32];
    char detail[96];
    double value = 0.0;
    int dy = y + ui->line_h;

    if (win->valid) {
        long long now_ms = (long long)time(NULL) * 1000LL;
        size_t vl;

        value = quota_display_value(win, snap->display_mode);
        quota_format_percent(value_text, sizeof(value_text), value);
        if (win->rate_limited) {
            char reset[64];
            quota_format_reset(reset, sizeof(reset), win, now_ms);
            snprintf(detail, sizeof(detail), "已限流 · %s", reset);
        } else {
            quota_format_reset(detail, sizeof(detail), win, now_ms);
        }
        /* Alert marker: raw used percent vs alertPercent, not the display value. */
        if (win->used >= snap->alert_percent) {
            vl = strlen(value_text);
            if (vl + 3 <= sizeof(value_text)) {
                value_text[vl] = ' ';
                value_text[vl + 1] = '!';
                value_text[vl + 2] = '\0';
            }
        }
    } else {
        snprintf(value_text, sizeof(value_text), "%s", "--");
        snprintf(detail, sizeof(detail), "%s", "暂无数据");
    }

    draw_value_line(ui, x, y, label, value_text);
    draw_bar(ui, x, dy, win->valid ? value / 100.0 : 0.0, win->valid);
    dy += ui->bar_h + ui->gap3;
    ui_text(ui, detail, ui->px_detail, x, dy, ui->card_inner, ui->line_h,
            UI_ALIGN_RIGHT, FG_BLACK, 0);
}

static void draw_info_block(const Ui *ui, int x, int y, const char *label,
                            const char *value, const char *detail);

/* Provider card. Grok/OpenCode use the third block for monthly quota; the
 * other providers use it for their collection timestamp, keeping all eight
 * cards the same height and the grid stable while data changes. */
static void draw_provider_card(const Ui *ui, int x, int y, const char *title,
                               const ProviderQuota *pq, const QuotaSnapshot *snap,
                               const char *five_label, const char *monthly_label)
{
    const char *mode = strcmp(snap->display_mode, "remaining") == 0 ? "剩余" : "已用";
    char fetched[24];
    int ix = x + ui->border + ui->card_padding;
    int iy = y + ui->border + ui->card_padding;
    int title_w = (int)(ui->card_inner * 0.72);

    draw_card_frame(ui, x, y);
    /* Header: title left (72%), mode label right (the remaining 28%). */
    ui_text(ui, title, ui->px_card_title, ix, iy, title_w, ui->line_h,
            UI_ALIGN_LEFT, FG_BLACK, 0);
    ui_text(ui, mode, ui->px_detail, ix + title_w, iy, ui->card_inner - title_w,
            ui->line_h, UI_ALIGN_RIGHT, FG_BLACK, 0);
    iy += ui->line_h + ui->gap5;
    draw_window_block(ui, ix, iy, five_label, &pq->five, snap);
    iy += ui->wb_h + ui->gap5;
    draw_window_block(ui, ix, iy, "周额度", &pq->seven, snap);
    iy += ui->wb_h + ui->gap5;
    if (monthly_label) {
        draw_window_block(ui, ix, iy, monthly_label, &pq->monthly, snap);
    } else {
        quota_format_timestamp(fetched, sizeof(fetched),
                               pq->has_fetched_at, pq->fetched_at);
        draw_info_block(ui, ix, iy, "采集", fetched, "提供商快照");
    }
}

/* buildInfoBlock port (quotapage.lua:158-171): value line, full-width
 * light-gray separator (LineWidget), left-aligned detail. */
static void draw_info_block(const Ui *ui, int x, int y, const char *label,
                            const char *value, const char *detail)
{
    int dy = y + ui->line_h;

    draw_value_line(ui, x, y, label, value);
    ui_fill(ui, x, dy, ui->card_inner, ui->bar_h, UI_INK_LIGHT);
    dy += ui->bar_h + ui->gap3;
    ui_text(ui, detail, ui->px_detail, x, dy, ui->card_inner, ui->line_h,
            UI_ALIGN_LEFT, FG_BLACK, 0);
}

/* buildInfoCard port (quotapage.lua:174-204). */
static void draw_info_card(const Ui *ui, int x, int y, const QuotaSnapshot *snap,
                           const char *status_state, const char *status_detail)
{
    const char *mode = strcmp(snap->display_mode, "remaining") == 0 ? "剩余" : "已用";
    char generated[96];
    char ts[24];
    char pct[16];
    char selected[16];
    int ix = x + ui->border + ui->card_padding;
    int iy = y + ui->border + ui->card_padding;

    quota_format_timestamp(ts, sizeof(ts), snap->has_generated_at, snap->generated_at);
    quota_format_percent(pct, sizeof(pct), snap->alert_percent);
    snprintf(generated, sizeof(generated), "快照 %s · 告警 %s", ts, pct);
    snprintf(selected, sizeof(selected), "%d / 6",
             snap->providers.grok + snap->providers.claude +
             snap->providers.codex + snap->providers.kimi +
             snap->providers.opencode + snap->providers.agy);

    draw_card_frame(ui, x, y);
    ui_text(ui, "数据接口", ui->px_card_title, ix, iy, ui->card_inner, ui->line_h,
            UI_ALIGN_LEFT, FG_BLACK, 0);
    iy += ui->line_h + ui->gap5;
    draw_info_block(ui, ix, iy, "状态", status_state ? status_state : "--",
                    status_detail ? status_detail : "");
    iy += ui->wb_h + ui->gap5;
    draw_info_block(ui, ix, iy, "显示", mode, generated);
    iy += ui->wb_h + ui->gap5;
    draw_info_block(ui, ix, iy, "采集", selected, "按接口名单显示");
}

static const ProviderQuota *card_quota(const QuotaSnapshot *snap, UiCard id)
{
    switch (id) {
    case UI_CARD_GROK:
        return &snap->grok;
    case UI_CARD_CLAUDE:
        return &snap->claude;
    case UI_CARD_CODEX:
        return &snap->codex;
    case UI_CARD_KIMI:
        return &snap->kimi;
    case UI_CARD_OPENCODE:
        return &snap->opencode;
    case UI_CARD_AGY_GEMINI:
        return &snap->agy.gemini;
    case UI_CARD_AGY_THIRD:
        return &snap->agy.third_party;
    default:
        return NULL;
    }
}

static void card_labels(UiCard id, const char **title, const char **five,
                        const char **monthly)
{
    *title = "";
    *five = "5 小时";
    *monthly = NULL;
    switch (id) {
    case UI_CARD_GROK:
        *title = "Grok";
        *monthly = "月额度";
        break;
    case UI_CARD_CLAUDE:
        *title = "Claude Code";
        break;
    case UI_CARD_CODEX:
        *title = "Codex";
        break;
    case UI_CARD_KIMI:
        *title = "Kimi Code";
        break;
    case UI_CARD_OPENCODE:
        *title = "OpenCode Go";
        *five = "滚动 5h";
        *monthly = "月额度";
        break;
    case UI_CARD_AGY_GEMINI:
        *title = "AGY · Gemini";
        break;
    case UI_CARD_AGY_THIRD:
        *title = "AGY · Claude/GPT";
        break;
    default:
        break;
    }
}

static void draw_card_at(const Ui *ui, int x, int y, UiCard id,
                         const QuotaSnapshot *snap,
                         const char *status_state, const char *status_detail)
{
    const char *title, *five, *monthly;
    const ProviderQuota *pq;

    if (id == UI_CARD_INFO) {
        draw_info_card(ui, x, y, snap, status_state, status_detail);
        return;
    }
    pq = card_quota(snap, id);
    if (!pq)
        return;
    card_labels(id, &title, &five, &monthly);
    draw_provider_card(ui, x, y, title, pq, snap, five, monthly);
}

static void slot_xy(const Ui *ui, int slot, int y0, int *x, int *y)
{
    *x = ui->h_padding + (slot % 2) * (ui->card_w + ui->card_gap);
    *y = y0 + (slot / 2) * (ui->card_h + ui->row_gap);
}

static void register_card(Ui *ui, int slot, int y0, UiCard id)
{
    int x, y, i;

    if (!ui || ui->card_drawn >= UI_CARD_SLOTS)
        return;
    slot_xy(ui, slot, y0, &x, &y);
    i = ui->card_drawn;
    ui->card_id[i] = id;
    ui->card_rect[i].left = (unsigned short)x;
    ui->card_rect[i].top = (unsigned short)y;
    ui->card_rect[i].width = (unsigned short)ui->card_w;
    ui->card_rect[i].height = (unsigned short)ui->card_h;
    ui->card_drawn = i + 1;
}

/* Five-cell battery at the title-bar right edge. Pure rectangles (no new
 * glyphs). Height matches the "充电" label; width is ui_scale(30) plus a
 * 2px-logical nipple. 0 cells invert the body unless we are on wall power.
 * Returns the width consumed from `right` leftward, 0 if nothing drawn. */
static int draw_battery(const Ui *ui, int right, int y, int pct, int charging,
                        int *out_h)
{
    const int cells = ui_batt_cells(pct);
    const int invert = (cells == 0 && charging != 1);
    int th = 0, tw = 0;
    int frame, bar_w, bar_h, nip_w, nip_h, inner_w, inner_h, gap1;
    int bar_x, left, i, cell_w, cx;

    *out_h = 0;
    if (cells < 0)
        return 0;
    if (ui_measure(ui, "充电", ui->px_detail, &tw, &th) != 0 || th <= 0)
        th = ui->px_detail;
    *out_h = th;

    frame = ui_scale(ui, 1);
    if (frame < 1)
        frame = 1;
    bar_h = th;
    bar_w = ui_scale(ui, 30);
    if (bar_w < 5 * frame + 5)
        bar_w = 5 * frame + 5;
    nip_w = ui_scale(ui, 2);
    if (nip_w < 1)
        nip_w = 1;
    nip_h = bar_h / 2;
    if (nip_h < 2)
        nip_h = 2;

    bar_x = right - bar_w - nip_w;
    ui_fill(ui, bar_x, y, bar_w, bar_h, UI_INK_BLACK);
    inner_w = bar_w - 2 * frame;
    inner_h = bar_h - 2 * frame;
    if (inner_w > 0 && inner_h > 0)
        ui_fill(ui, bar_x + frame, y + frame, inner_w, inner_h,
                invert ? UI_INK_BLACK : UI_INK_WHITE);
    ui_fill(ui, bar_x + bar_w, y + (bar_h - nip_h) / 2, nip_w, nip_h,
            UI_INK_BLACK);

    gap1 = 1; /* 1px seam; DU is 1-bit, so this stays a hard white/black cut */
    if (inner_w > 0 && inner_h > 0) {
        int usable = inner_w - 4 * gap1;

        if (usable < 5) {
            usable = inner_w;
            gap1 = 0;
        }
        cell_w = usable / 5;
        if (cell_w < 1)
            cell_w = 1;
        cx = bar_x + frame;
        for (i = 0; i < 5; i++) {
            int cw = (i == 4) ? (bar_x + frame + inner_w) - cx : cell_w;

            if (cw < 1)
                cw = 1;
            if (invert)
                ui_fill(ui, cx, y + frame, cw, inner_h, UI_INK_WHITE);
            else if (i < cells)
                ui_fill(ui, cx, y + frame, cw, inner_h, UI_INK_BLACK);
            cx += cw + gap1;
        }
    }

    left = bar_x;
    if (charging == 1 && tw > 0) {
        int lg = ui_scale(ui, 5);

        left = bar_x - lg - tw;
        ui_text(ui, "充电", ui->px_detail, left, y, tw, th,
                UI_ALIGN_LEFT, FG_BLACK, 0);
    }
    return right - left;
}

/* Title-bar right corner, drawn right to left: five-cell battery (with a
 * "充电" label on wall power), then the WiFi badge -- state dot + label,
 * where the label is the current SSID when we have one and "WiFi" otherwise.
 * Everything comes from samples the caller took for this very render; the
 * badge adds no probes, timers or wakeups of its own.
 * Battery out of range and state < 0 each draw nothing: before the first
 * probe (or off-device) there is nothing to say. */
static void draw_status_badges(Ui *ui, int wifi_state, const char *ssid,
                               int batt_pct, int batt_charging)
{
    const int dot = ui_scale(ui, 6);
    const int gap = ui_scale(ui, 5);
    const int y = ui_scale(ui, 18);
    char text[48];
    int tw = 0, th = 0;
    int x = ui->w - ui->h_padding;
    int right = x;
    int min_x = x;
    int max_h = 0;

    ui->badge_rect.left = 0;
    ui->badge_rect.top = 0;
    ui->badge_rect.width = 0;
    ui->badge_rect.height = 0;

    {
        int bh = 0;
        int bw = draw_battery(ui, x, y, batt_pct, batt_charging, &bh);

        if (bw > 0) {
            x -= bw;
            min_x = x;
            if (bh > max_h)
                max_h = bh;
            x -= gap;
        }
    }

    if (wifi_state >= 0) {
        snprintf(text, sizeof(text), "%s",
                 (ssid && *ssid) ? ssid : "WiFi");
        if (ui_measure(ui, text, ui->px_detail, &tw, &th) == 0 && th > 0) {
            /* Cap the label so a long SSID cannot run over the battery or
             * the centered title; ui_text truncates with an ellipsis. */
            int cap = ui->w * 3 / 10;
            int box_w;

            if (cap > x - dot - gap)
                cap = x - dot - gap;
            if (cap >= 1) {
                box_w = tw < cap ? tw : cap;
                x -= box_w;
                ui_text(ui, text, ui->px_detail, x, y, box_w, th,
                        UI_ALIGN_RIGHT, wifi_state > 0 ? FG_BLACK : FG_GRAY8, 0);
                x -= gap + dot;
                ui_fill(ui, x, y + (th - dot) / 2, dot, dot,
                        wifi_state > 0 ? UI_INK_BLACK : UI_INK_GRAY);
                min_x = x;
                if (th > max_h)
                    max_h = th;
            }
        }
    }

    if (min_x < right && max_h > 0) {
        ui->badge_rect.left = (unsigned short)min_x;
        ui->badge_rect.top = (unsigned short)y;
        ui->badge_rect.width = (unsigned short)(right - min_x);
        ui->badge_rect.height = (unsigned short)max_h;
    }
}

/* TitleBar port (quotapage.lua:257-287): centered title + subtitle, then a 1px
 * black hairline across the full width. No close icon (the 关闭 button covers
 * it; the subset font has no X glyph). A second subtitle line carries dead
 * text for the auto-refresh state (docs/kindle-ui-and-power.md P1): parked
 * users can tell at a glance whether the page will update itself, without a
 * countdown that would force periodic redraws. */
static void draw_title_bar(const Ui *ui, const char *host, int auto_refresh_min)
{
    const char *title = "AI 额度";
    const char *sub = (host && *host) ? host : "未配置";
    char auto_line[32];
    int tw = 0, th = 0, sw = 0, sh = 0, ah = 0;
    int total, ty;

    snprintf(auto_line, sizeof(auto_line),
             auto_refresh_min > 0 ? "自动刷新 %d 分钟" : "自动刷新关闭",
             auto_refresh_min);

    if (ui_measure(ui, title, ui->px_title, &tw, &th) != 0 || th <= 0)
        th = ui->px_title + ui->px_title / 2;
    if (ui_measure(ui, sub, ui->px_subtitle, &sw, &sh) != 0 || sh <= 0)
        sh = ui->px_subtitle + ui->px_subtitle / 2;
    if (ui_measure(ui, auto_line, ui->px_subtitle, &sw, &ah) != 0 || ah <= 0)
        ah = sh;

    total = th + ui->gap2 + sh + ui->gap2 + ah;
    ty = (ui->title_h - 1 - total) / 2;
    if (ty < 0)
        ty = 0;

    ui_text(ui, title, ui->px_title, 0, ty, ui->w, th, UI_ALIGN_CENTER, FG_BLACK, 0);
    ui_text(ui, sub, ui->px_subtitle, 0, ty + th + ui->gap2, ui->w, sh,
            UI_ALIGN_CENTER, FG_BLACK, 0);
    ui_text(ui, auto_line, ui->px_subtitle, 0, ty + th + ui->gap2 + sh + ui->gap2,
            ui->w, ah, UI_ALIGN_CENTER,
            auto_refresh_min > 0 ? FG_BLACK : FG_GRAY8, 0);
    ui_fill(ui, 0, ui->title_h - 1, ui->w, 1, UI_INK_BLACK);
}

static void draw_button_text(const Ui *ui, int i, int inverted)
{
    const FBInkRect *r = &ui->btn_rect[i];
    int tw = 0, th = 0;
    int x, y;

    if (ui_measure(ui, ui->btn_label[i], ui->px_percent, &tw, &th) != 0 || tw <= 0)
        return;
    x = r->left + (r->width - tw) / 2;
    y = r->top + (r->height - th) / 2;
    /* Inverted press feedback: the rect was filled black by the caller and
     * is_inverted flips the glyph pixels to white (no OT bg painting). */
    ui_text(ui, ui->btn_label[i], ui->px_percent, x, y, tw, th,
            UI_ALIGN_LEFT, inverted ? FG_BLACK : ui->btn_fg[i], inverted);
}

/* ButtonTable port (quotapage.lua:221-234): three borderless text buttons,
 * 1/3 of content_w each, with thin vertical separators between them. */
static void draw_buttons(Ui *ui, int y, int loading)
{
    static const char *const labels[3] = { "刷新", "设置", "关闭" };
    int bw = ui->content_w / 3;
    int bx = ui->h_padding;
    int i;

    for (i = 0; i < 3; i++) {
        int w = (i == 2) ? ui->content_w - 2 * bw : bw;
        const char *label = (i == 0 && loading) ? "加载中…" : labels[i];
        int fg = (i == 0 && loading) ? FG_GRAY8 : FG_BLACK;

        ui->btn_rect[i].left = (unsigned short)bx;
        ui->btn_rect[i].top = (unsigned short)y;
        ui->btn_rect[i].width = (unsigned short)w;
        ui->btn_rect[i].height = (unsigned short)ui->button_h;
        snprintf(ui->btn_label[i], sizeof(ui->btn_label[i]), "%s", label);
        ui->btn_fg[i] = fg;
        ui->btn_enabled[i] = (i == 0 && loading) ? 0 : 1;
        draw_button_text(ui, i, 0);
        bx += w;
    }
    /* 1px gray hairlines spanning the button height, on the button boundaries. */
    ui_fill(ui, ui->btn_rect[1].left, y, 1, ui->button_h, UI_INK_GRAY);
    ui_fill(ui, ui->btn_rect[2].left, y, 1, ui->button_h, UI_INK_GRAY);
    ui->btn_row_drawn = 1;
}

static int scaled_metric(const Ui *ui, int logical, int percent, int minimum)
{
    int value = (ui_scale(ui, logical) * percent + 50) / 100;
    return value < minimum ? minimum : value;
}

/* Calculate the vertical metrics at a percentage of the normal KOReader-like
 * scale. Four rows fit unmodified on the target 1072x1448 devices. Older
 * 600x800 Kindles automatically use a denser but still complete layout. */
static void configure_vertical_metrics(Ui *ui, int percent)
{
    ui->row_gap = scaled_metric(ui, 8, percent, 2);
    ui->card_padding = scaled_metric(ui, 9, percent, 3);
    ui->line_h = scaled_metric(ui, 27, percent, 13);
    ui->gap3 = scaled_metric(ui, 3, percent, 1);
    ui->gap5 = scaled_metric(ui, 5, percent, 1);
    ui->bar_h = scaled_metric(ui, 8, percent, 3);
    ui->button_h = scaled_metric(ui, 48, percent, 24);

    ui->px_card_title = scaled_metric(ui, 19, percent, 10);
    ui->px_label = scaled_metric(ui, 14, percent, 8);
    ui->px_percent = scaled_metric(ui, 16, percent, 9);
    ui->px_detail = scaled_metric(ui, 12, percent, 7);

    ui->card_inner = ui->card_w - 2 * (ui->card_padding + ui->border);
    ui->wb_h = ui->line_h + ui->bar_h + ui->gap3 + ui->line_h;
    ui->card_h = 2 * (ui->card_padding + ui->border) + ui->line_h +
                 3 * ui->gap5 + 3 * ui->wb_h;
    ui->content_h = 4 * ui->card_h + 4 * ui->row_gap + ui->button_h;
}

Ui *ui_create(int fbfd, int screen_w, int screen_h, int dpi, const char *font_path)
{
    Ui *ui = calloc(1, sizeof(*ui));
    int fit_percent;

    if (!ui)
        return NULL;
    ui->fbfd = fbfd;
    ui->w = screen_w;
    ui->h = screen_h;
    ui->dpi = dpi > 0 ? dpi : 167;

    if (!font_path || !*font_path || fbink_add_ot_font(font_path, FNT_REGULAR) < 0) {
        free(ui);
        return NULL;
    }

    ui->h_padding = ui_scale(ui, 16);
    ui->card_gap = ui_scale(ui, 8);
    ui->border = ui_scale(ui, 1);
    if (ui->border < 1)
        ui->border = 1;
    ui->gap2 = ui_scale(ui, 2);
    ui->title_h = ui_scale(ui, 64);

    ui->content_w = screen_w - 2 * ui->h_padding;
    ui->card_w = (ui->content_w - ui->card_gap) / 2;
    ui->px_title = ui_scale(ui, 22);
    ui->px_subtitle = ui_scale(ui, 14);

    for (fit_percent = 100; fit_percent >= 55; fit_percent--) {
        configure_vertical_metrics(ui, fit_percent);
        if (ui->content_h <= screen_h - ui->title_h)
            break;
    }
    {
        /* CenterContainer semantics: content centered below the title bar. */
        int slack = screen_h - ui->title_h - ui->content_h;
        ui->body_y = ui->title_h + (slack > 0 ? slack / 2 : 0);
    }

    ui->cfg_draw.no_refresh = true; /* everything else zero: black on white */
    return ui;
}

void ui_destroy(Ui *ui)
{
    if (!ui)
        return;
    fbink_free_ot_fonts(); /* the font set is process-global, owned by Ui */
    free(ui);
}

void ui_draw_dashboard(Ui *ui, const QuotaSnapshot *snap, const char *host,
                       const char *status_state, const char *status_detail, int loading,
                       int wifi_state, const char *ssid, int batt_pct, int batt_charging,
                       int auto_refresh_min)
{
    UiCard ids[UI_CARD_SLOTS];
    int card_count, rows, drawn_h, slack, y0, i;

    if (!ui || !snap)
        return;

    ui->card_drawn = 0;
    ui->btn_row_drawn = 0;
    fbink_cls(ui->fbfd, &ui->cfg_draw, NULL, false); /* white canvas, no refresh */

    draw_title_bar(ui, host, auto_refresh_min);
    draw_status_badges(ui, wifi_state, ssid, batt_pct, batt_charging);

    /* One card per ordinary provider, two for AGY, plus the always-visible
     * connection card. Disabled providers are omitted and the remaining
     * cards pack left-to-right, top-to-bottom. Identity is stored next to
     * each rect so a later tap cannot confuse slot number with card. */
    card_count = ui_cards_from_providers(&snap->providers, ids, UI_CARD_SLOTS);
    rows = (card_count + 1) / 2;
    drawn_h = rows * ui->card_h + rows * ui->row_gap + ui->button_h;
    slack = ui->h - ui->title_h - drawn_h;
    y0 = ui->title_h + (slack > 0 ? slack / 2 : 0);

    for (i = 0; i < card_count; i++) {
        int x, y;

        slot_xy(ui, i, y0, &x, &y);
        draw_card_at(ui, x, y, ids[i], snap, status_state, status_detail);
        register_card(ui, i, y0, ids[i]);
    }

    draw_buttons(ui, y0 + rows * ui->card_h + rows * ui->row_gap, loading);
}

UiAction ui_hit_button(const Ui *ui, int x, int y)
{
    static const UiAction actions[3] = { UI_ACTION_REFRESH, UI_ACTION_SETTINGS,
                                          UI_ACTION_CLOSE };
    int i;

    if (!ui || !ui->btn_row_drawn)
        return UI_ACTION_NONE;
    for (i = 0; i < 3; i++) {
        const FBInkRect *r = &ui->btn_rect[i];

        if (ui_rect_contains(r, x, y))
            return ui->btn_enabled[i] ? actions[i] : UI_ACTION_NONE;
    }
    return UI_ACTION_NONE;
}

void ui_flash_button(Ui *ui, int fbfd, UiAction action)
{
    FBInkRect row;
    int i, idx;

    if (!ui || !ui->btn_row_drawn)
        return;
    switch (action) {
    case UI_ACTION_REFRESH:
        idx = 0;
        break;
    case UI_ACTION_SETTINGS:
        idx = 1;
        break;
    case UI_ACTION_CLOSE:
        idx = 2;
        break;
    default:
        return;
    }
    if (!ui->btn_enabled[idx])
        return;

    /* Press: black rect + white label, fast partial refresh. */
    ui_fill(ui, ui->btn_rect[idx].left, ui->btn_rect[idx].top,
            ui->btn_rect[idx].width, ui->btn_rect[idx].height, UI_INK_BLACK);
    draw_button_text(ui, idx, 1);
    ui_refresh(fbfd, &ui->btn_rect[idx], WFM_DU);
    usleep(120 * 1000);

    /* Release: repaint the whole row (separators included), refresh once. */
    row.left = (unsigned short)ui->h_padding;
    row.top = ui->btn_rect[0].top;
    row.width = (unsigned short)ui->content_w;
    row.height = (unsigned short)ui->button_h;
    ui_fill(ui, row.left, row.top, row.width, row.height, UI_INK_WHITE);
    for (i = 0; i < 3; i++)
        draw_button_text(ui, i, 0);
    ui_fill(ui, ui->btn_rect[1].left, row.top, 1, ui->button_h, UI_INK_GRAY);
    ui_fill(ui, ui->btn_rect[2].left, row.top, 1, ui->button_h, UI_INK_GRAY);
    ui_refresh(fbfd, &row, WFM_DU);
}

/* Popup chrome is one step thicker than a dashboard card's 1px frame. */
static int popup_frame(const Ui *ui)
{
    int b = ui_scale(ui, 2);
    return b < 2 ? 2 : b;
}

/* Scale a Ui copy so one card fills content_w minus the chrome. Derived
 * heights (wb_h, card_h) are recomputed, not multiplied -- same as
 * configure_vertical_metrics. The copy is paint-only. */
static void scale_popup_ui(Ui *big, const Ui *ui)
{
    int frame = popup_frame(ui);
    int inner_w = ui->content_w - 2 * frame;
    int num, den;

    *big = *ui;
    if (inner_w < 1)
        inner_w = ui->card_w;
    den = ui->card_w > 0 ? ui->card_w : 1;
    num = inner_w;
    big->card_w = inner_w;
    big->card_padding = ui->card_padding * num / den;
    if (big->card_padding < 1)
        big->card_padding = 1;
    big->border = ui->border * num / den;
    if (big->border < 1)
        big->border = 1;
    big->line_h = ui->line_h * num / den;
    big->bar_h = ui->bar_h * num / den;
    if (big->bar_h < 1)
        big->bar_h = 1;
    big->gap2 = ui->gap2 * num / den;
    if (big->gap2 < 1)
        big->gap2 = 1;
    big->gap3 = ui->gap3 * num / den;
    if (big->gap3 < 1)
        big->gap3 = 1;
    big->gap5 = ui->gap5 * num / den;
    if (big->gap5 < 1)
        big->gap5 = 1;
    big->px_card_title = ui->px_card_title * num / den;
    if (big->px_card_title < 1)
        big->px_card_title = 1;
    big->px_label = ui->px_label * num / den;
    if (big->px_label < 1)
        big->px_label = 1;
    big->px_percent = ui->px_percent * num / den;
    if (big->px_percent < 1)
        big->px_percent = 1;
    big->px_detail = ui->px_detail * num / den;
    if (big->px_detail < 1)
        big->px_detail = 1;
    big->card_inner = big->card_w - 2 * (big->card_padding + big->border);
    if (big->card_inner < 1)
        big->card_inner = 1;
    big->wb_h = big->line_h + big->bar_h + big->gap3 + big->line_h;
    big->card_h = 2 * (big->card_padding + big->border) + big->line_h +
                  3 * big->gap5 + 3 * big->wb_h;
}

static void popup_geom(const Ui *ui, const Ui *big, int *px, int *py, FBInkRect *out)
{
    int frame = popup_frame(ui);
    int pop_w = big->card_w + 2 * frame;
    int pop_h = big->card_h + 2 * frame;
    int body_top = ui->title_h;
    int body_h = ui->h - ui->title_h;
    int x = ui->h_padding;
    int y = body_top + (body_h - pop_h) / 2;

    if (y < body_top)
        y = body_top;
    if (y + pop_h > ui->h)
        y = ui->h - pop_h;
    if (y < 0)
        y = 0;
    if (x < 0)
        x = 0;
    if (x + pop_w > ui->w)
        x = ui->w - pop_w;
    if (x < 0)
        x = 0;
    *px = x + frame;
    *py = y + frame;
    out->left = (unsigned short)x;
    out->top = (unsigned short)y;
    out->width = (unsigned short)pop_w;
    out->height = (unsigned short)pop_h;
}

void ui_popup_rect(const Ui *ui, FBInkRect *out)
{
    Ui big;
    int px, py;

    if (!ui || !out)
        return;
    scale_popup_ui(&big, ui);
    popup_geom(ui, &big, &px, &py, out);
}

void ui_popup_draw(const Ui *ui, const QuotaSnapshot *snap,
                   const char *status_state, const char *status_detail,
                   UiCard id, FBInkRect *out)
{
    Ui big;
    FBInkRect r;
    int px, py;

    if (!ui || !snap || id == UI_CARD_NONE)
        return;
    scale_popup_ui(&big, ui);
    popup_geom(ui, &big, &px, &py, &r);
    ui_fill(ui, r.left, r.top, r.width, r.height, UI_INK_BLACK);
    draw_card_at(&big, px, py, id, snap, status_state, status_detail);
    if (out)
        *out = r;
}

void ui_redraw_badges(Ui *ui, int wifi_state, const char *ssid,
                      int batt_pct, int batt_charging, FBInkRect *out)
{
    FBInkRect old;
    int x0, y0, w0, h0, right;

    if (!ui)
        return;

    old = ui->badge_rect;
    right = ui->w - ui->h_padding;
    x0 = old.width ? (int)old.left : ui->w * 7 / 10;
    y0 = old.height ? (int)old.top : ui_scale(ui, 18);
    h0 = old.height ? (int)old.height : ui->px_detail;
    if (x0 < 0)
        x0 = 0;
    w0 = right - x0;
    if (w0 < 1 || h0 < 1)
        return;

    /* Clear only the previous badge strip, not the whole title-bar corner:
     * a taller wipe would punch a hole in the centered title/subtitle. */
    ui_fill(ui, x0, y0, w0, h0, UI_INK_WHITE);
    draw_status_badges(ui, wifi_state, ssid, batt_pct, batt_charging);

    if (out) {
        int left = x0, top = y0, bot = y0 + h0, rgt = x0 + w0;
        const FBInkRect *n = &ui->badge_rect;

        if (n->width) {
            if ((int)n->left < left)
                left = (int)n->left;
            if ((int)n->top < top)
                top = (int)n->top;
            if ((int)n->left + (int)n->width > rgt)
                rgt = (int)n->left + (int)n->width;
            if ((int)n->top + (int)n->height > bot)
                bot = (int)n->top + (int)n->height;
        }
        if (left < 0)
            left = 0;
        if (top < 0)
            top = 0;
        out->left = (unsigned short)left;
        out->top = (unsigned short)top;
        out->width = (unsigned short)(rgt > left ? rgt - left : 0);
        out->height = (unsigned short)(bot > top ? bot - top : 0);
    }
}
