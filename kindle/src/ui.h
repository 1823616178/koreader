#ifndef AIQUOTA_UI_H
#define AIQUOTA_UI_H

#include "fbink.h"
#include "quota.h"

typedef struct Ui Ui;   /* fb geometry, scale factor, computed layout, button rects */

typedef enum { UI_ACTION_NONE = 0, UI_ACTION_REFRESH, UI_ACTION_SETTINGS, UI_ACTION_CLOSE } UiAction;

/* Packed dashboard cards, left-to-right, top-to-bottom. Slot index is not
 * identity: disabled providers drop out and the rest compact, so the id is
 * recorded next to the rect at paint time. INFO is always last. */
typedef enum {
    UI_CARD_NONE = 0,
    UI_CARD_GROK,
    UI_CARD_CLAUDE,
    UI_CARD_CODEX,
    UI_CARD_KIMI,
    UI_CARD_OPENCODE,
    UI_CARD_AGY_GEMINI,
    UI_CARD_AGY_THIRD,
    UI_CARD_INFO
} UiCard;

enum { UI_CARD_SLOTS = 9 };

/* dpi comes from fbink_get_state(); font_path is the bundled Noto Sans SC subset (loaded via fbink_add_ot_font).
 * Returns NULL on failure (bad font_path/OOM); caller must check. */
Ui  *ui_create(int fbfd, int screen_w, int screen_h, int dpi, const char *font_path);
void ui_destroy(Ui *ui);

/* Full dashboard redraw (everything drawn with no_refresh); caller issues one fbink_refresh_rect.
 * host: title-bar subtitle (may be NULL -> "未配置").
 * status_state: "连接中" | "失败" | "已连接"; status_detail: url or error text; loading: 0/1.
 * wifi_state: 1 connected (dark dot), 0 off (gray dot), -1 unknown (no badge).
 * ssid: sanitized SSID shown instead of the "WiFi" label when non-empty.
 * batt_pct: 0-100, out of range -> no battery icon; batt_charging: 1 shows
 * a "充电" label left of the five-cell bar. All three are sampled by the
 * caller at render points only -- drawing them adds no wakeups on the
 * e-ink device.
 * auto_refresh_min: 0 shows a gray "自动刷新关闭" second subtitle line,
 * > 0 "自动刷新 N 分钟" -- dead text, no timer behind it. */
void ui_draw_dashboard(Ui *ui, const QuotaSnapshot *snap, const char *host,
                       const char *status_state, const char *status_detail, int loading,
                       int wifi_state, const char *ssid, int batt_pct, int batt_charging,
                       int auto_refresh_min);

/* Hit-test the bottom button row (screen-space tap coords). Disabled buttons return UI_ACTION_NONE. */
UiAction ui_hit_button(const Ui *ui, int x, int y);

/* Press feedback: redraw the button inverted, refresh its rect with WFM_DU, ~120ms, restore, refresh again. */
void ui_flash_button(Ui *ui, int fbfd, UiAction action);

/* Screen rectangle the card popup will occupy (for dump/restore). */
void ui_popup_rect(const Ui *ui, FBInkRect *out);

/* Vector-redraw one card into a centered popup. Does not refresh; *out is
 * the popup rectangle (same as ui_popup_rect). The Ui copy used for the
 * scaled paint is local -- never hit-test against it. */
void ui_popup_draw(const Ui *ui, const QuotaSnapshot *snap,
                   const char *status_state, const char *status_detail,
                   UiCard id, FBInkRect *out);

/* Repaint the title-bar badges in place (white-out previous strip, draw
 * again). *out is the DU rect covering old+new. No refresh. */
void ui_redraw_badges(Ui *ui, int wifi_state, const char *ssid,
                      int batt_pct, int batt_charging, FBInkRect *out);

/* Five-cell battery: -1 when pct is unreadable/out of range, else 0..5.
 * 0-10 -> 0 (empty, drawn inverted unless charging), 11-29 -> 1, then
 * 20-point bands up to 90-100 -> 5. Host tests cover the boundaries. */
static inline int ui_batt_cells(int pct)
{
    if (pct < 0 || pct > 100)
        return -1;
    if (pct <= 10)
        return 0;
    if (pct <= 29)
        return 1;
    if (pct <= 49)
        return 2;
    if (pct <= 69)
        return 3;
    if (pct <= 89)
        return 4;
    return 5;
}

/* Pack enabled providers in dashboard order, always ending with INFO.
 * Returns the number of ids written, capped at max. */
static inline int ui_cards_from_providers(const ProviderSelection *p, UiCard *ids,
                                          int max)
{
    int n = 0;

    if (!p || !ids || max <= 0)
        return 0;
    if (p->grok && n < max)
        ids[n++] = UI_CARD_GROK;
    if (p->claude && n < max)
        ids[n++] = UI_CARD_CLAUDE;
    if (p->codex && n < max)
        ids[n++] = UI_CARD_CODEX;
    if (p->kimi && n < max)
        ids[n++] = UI_CARD_KIMI;
    if (p->opencode && n < max)
        ids[n++] = UI_CARD_OPENCODE;
    if (p->agy) {
        if (n < max)
            ids[n++] = UI_CARD_AGY_GEMINI;
        if (n < max)
            ids[n++] = UI_CARD_AGY_THIRD;
    }
    if (n < max)
        ids[n++] = UI_CARD_INFO;
    return n;
}

static inline int ui_rect_contains(const FBInkRect *r, int x, int y)
{
    return r && x >= (int)r->left && x < (int)r->left + (int)r->width &&
           y >= (int)r->top && y < (int)r->top + (int)r->height;
}

/* ---------------------------------------------------------------------------
 * Internals shared with keypad.c: the on-screen numeric keypad reuses Ui's
 * geometry, loaded OT font and drawing helpers, so struct Ui and the low
 * level primitives live here instead of being duplicated. Application code
 * (main.c) must only use the functions above, plus ui_hit_card, which is
 * defined inline right after the struct so host tests can cover it without
 * linking ui.c/libfbink.
 */

/* 8-bit grays matching KOReader's Blitbuffer COLOR_BLACK / GRAY / LIGHT_GRAY / WHITE. */
#define UI_INK_BLACK 0x00u
#define UI_INK_GRAY 0x88u
#define UI_INK_LIGHT 0xCCu
#define UI_INK_WHITE 0xFFu

enum { UI_ALIGN_LEFT = 0, UI_ALIGN_CENTER, UI_ALIGN_RIGHT };

struct Ui {
    int fbfd;
    int w, h;
    int dpi;
    FBInkConfig cfg_draw;   /* batch drawing config: no_refresh, black on white */
    /* scaled metrics, quotapage.lua:45-51 with scaleBySize -> ui_scale */
    int h_padding, card_gap, row_gap, card_padding, border, line_h;
    int gap2, gap3, gap5, bar_h, title_h, button_h;
    int wb_h;   /* window block height: value line + bar + gap3 + detail line */
    int content_w, card_w, card_inner, card_h, content_h, body_y;
    /* font sizes in pixels */
    int px_card_title, px_label, px_percent, px_detail, px_title, px_subtitle;
    /* bottom button row: refresh / settings / close, valid once drawn */
    FBInkRect btn_rect[3];
    char btn_label[3][24];
    int btn_fg[3];      /* FG_COLOR_INDEX_E values */
    int btn_enabled[3];
    int btn_row_drawn;
    /* packed cards from the last ui_draw_dashboard; popup hit-testing */
    FBInkRect card_rect[UI_CARD_SLOTS];
    UiCard card_id[UI_CARD_SLOTS];
    int card_drawn;
    /* title-bar badge strip from the last paint, for in-place DU redraw */
    FBInkRect badge_rect;
};

/* Hit-test a painted provider/info card. UI_CARD_NONE if the tap missed.
 * Public API, but defined inline here rather than in ui.c: it only reads the
 * card_* fields through ui_rect_contains, and host tests link neither ui.c
 * nor libfbink. */
static inline UiCard ui_hit_card(const Ui *ui, int x, int y)
{
    int i;

    if (!ui || ui->card_drawn <= 0)
        return UI_CARD_NONE;
    for (i = 0; i < ui->card_drawn; i++) {
        if (ui_rect_contains(&ui->card_rect[i], x, y))
            return ui->card_id[i];
    }
    return UI_CARD_NONE;
}

int  ui_scale(const Ui *ui, int n);   /* lround(n * dpi / 167.0), KOReader scaleBySize */
void ui_fill(const Ui *ui, int x, int y, int w, int h, uint8_t gray);
int  ui_measure(const Ui *ui, const char *text, int size_px, int *out_w, int *out_h);
/* Draw one line of text, top-left at (x, y), truncated with U+2026 to width w.
 * align: UI_ALIGN_*; fg_index: FG_COLOR_INDEX_E; inverted: white on black. */
void ui_text(const Ui *ui, const char *text, int size_px, int x, int y, int w, int h,
             int align, int fg_index, int inverted);
void ui_refresh(int fbfd, const FBInkRect *rect, int wfm_mode);

#endif
