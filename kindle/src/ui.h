#ifndef AIQUOTA_UI_H
#define AIQUOTA_UI_H

#include "fbink.h"
#include "quota.h"

#include <stdio.h>
#include <string.h>

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
/* F02: set local provider order used by ui_draw_dashboard packing. */
void ui_set_provider_order(Ui *ui, const char *order_csv);

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
void ui_popup_rect_for(const Ui *ui, UiCard id, FBInkRect *out);

/* Vector-redraw one card into a centered popup. Does not refresh; *out is
 * the popup rectangle (same as ui_popup_rect). The Ui copy used for the
 * scaled paint is local -- never hit-test against it. */
void ui_popup_draw(const Ui *ui, const QuotaSnapshot *snap,
                   const char *status_state, const char *status_detail,
                   UiCard id, FBInkRect *out);
/* Info-card popup: multi-line detail with optional paging (R07).
 * page is 0-based; ui_popup_info_pages() reports how many pages the
 * current detail needs. ui_popup_hit() returns 0=close, 1=prev,
 * 2=next, -1=ignore (e.g. empty chrome). */
int  ui_popup_info_pages(const Ui *ui, const char *status_detail);
void ui_popup_draw_ex(const Ui *ui, const QuotaSnapshot *snap,
                     const char *status_state, const char *status_detail,
                     UiCard id, int page, FBInkRect *out);
int  ui_popup_hit(const Ui *ui, const FBInkRect *pop, UiCard id,
                 int pages, int x, int y);
/* Pure wrap helper (host-testable): split UTF-8 into lines of at most
 * max_bytes bytes (column budget by UTF-8 codepoints ~3 bytes for CJK).
 * Copies into scratch; lines[] point into scratch. Returns line count. */
static inline int ui_wrap_utf8_lines(const char *text, int max_bytes,
                                     char *scratch, size_t scratch_n,
                                     const char **lines, int max_lines)
{
    size_t pos = 0;
    int nlines = 0;
    const char *p;

    if (!text || max_bytes < 1 || !scratch || scratch_n < 2 || !lines || max_lines < 1)
        return 0;
    p = text;
    while (*p && nlines < max_lines) {
        int line_bytes = 0;
        const char *start = p;
        const char *last_break = p;

        while (*p) {
            unsigned char c = (unsigned char)*p;
            int clen = 1;
            if ((c & 0x80) == 0)
                clen = 1;
            else if ((c & 0xE0) == 0xC0)
                clen = 2;
            else if ((c & 0xF0) == 0xE0)
                clen = 3;
            else if ((c & 0xF8) == 0xF0)
                clen = 4;
            if (line_bytes + clen > max_bytes)
                break;
            p += clen;
            line_bytes += clen;
            last_break = p;
            if (c == ' ' || c == '\n' || c == '/' || c == '?' || c == '&')
                last_break = p;
            if (c == '\n')
                break;
        }
        if (last_break == start) {
            /* single overlong token: force one codepoint */
            unsigned char c = (unsigned char)*p;
            int clen = 1;
            if ((c & 0xE0) == 0xC0) clen = 2;
            else if ((c & 0xF0) == 0xE0) clen = 3;
            else if ((c & 0xF8) == 0xF0) clen = 4;
            last_break = p + clen;
            p = last_break;
            line_bytes = clen;
        } else if (*p && (unsigned char)*start != '\n') {
            p = last_break;
            line_bytes = (int)(p - start);
        }
        if (pos + (size_t)line_bytes + 1 >= scratch_n)
            break;
        lines[nlines] = scratch + pos;
        memcpy(scratch + pos, start, (size_t)line_bytes);
        pos += (size_t)line_bytes;
        scratch[pos++] = '\0';
        nlines++;
        if (*p == '\n')
            p++;
    }
    return nlines;
}

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

/* Append one provider's card(s). AGY expands to Gemini + third-party. */
static inline int ui_cards_append_provider(const ProviderSelection *p,
                                           const char *name, UiCard *ids,
                                           int n, int max)
{
    if (!p || !name || !ids || n >= max)
        return n;
    if (strcmp(name, "grok") == 0 && p->grok)
        ids[n++] = UI_CARD_GROK;
    else if (strcmp(name, "claude") == 0 && p->claude)
        ids[n++] = UI_CARD_CLAUDE;
    else if (strcmp(name, "codex") == 0 && p->codex)
        ids[n++] = UI_CARD_CODEX;
    else if (strcmp(name, "kimi") == 0 && p->kimi)
        ids[n++] = UI_CARD_KIMI;
    else if (strcmp(name, "opencode") == 0 && p->opencode)
        ids[n++] = UI_CARD_OPENCODE;
    else if (strcmp(name, "agy") == 0 && p->agy) {
        if (n < max)
            ids[n++] = UI_CARD_AGY_GEMINI;
        if (n < max)
            ids[n++] = UI_CARD_AGY_THIRD;
    }
    return n;
}

/* Pack enabled providers. order_csv (e.g. "kimi,grok,agy") controls order;
 * NULL/empty uses the default grok..agy sequence. Always ends with INFO.
 * AGY still emits two cards and is not split. */
static inline int ui_cards_from_providers_ex(const ProviderSelection *p,
                                             const char *order_csv, UiCard *ids,
                                             int max)
{
    static const char *def_order[] = {
        "grok", "claude", "codex", "kimi", "opencode", "agy"
    };
    int n = 0;
    int used[6] = {0, 0, 0, 0, 0, 0};
    int i;

    if (!p || !ids || max <= 0)
        return 0;

    if (order_csv && order_csv[0]) {
        const char *s = order_csv;
        while (*s && n < max) {
            char part[16];
            size_t L = 0;
            int j;

            while (*s == ' ' || *s == '\t' || *s == ',')
                s++;
            if (!*s)
                break;
            while (*s && *s != ',' && L + 1 < sizeof(part)) {
                if (*s != ' ' && *s != '\t')
                    part[L++] = *s;
                s++;
            }
            part[L] = '\0';
            while (*s && *s != ',')
                s++;
            if (*s == ',')
                s++;
            if (!part[0])
                continue;
            for (j = 0; j < 6; j++) {
                if (strcmp(part, def_order[j]) == 0) {
                    if (!used[j]) {
                        n = ui_cards_append_provider(p, def_order[j], ids, n, max);
                        used[j] = 1;
                    }
                    break;
                }
            }
        }
    }

    for (i = 0; i < 6; i++) {
        if (used[i])
            continue;
        n = ui_cards_append_provider(p, def_order[i], ids, n, max);
        if (n >= max)
            break;
    }
    if (n < max)
        ids[n++] = UI_CARD_INFO;
    return n;
}

/* Pack enabled providers in default dashboard order, always ending with INFO. */
static inline int ui_cards_from_providers(const ProviderSelection *p, UiCard *ids,
                                          int max)
{
    return ui_cards_from_providers_ex(p, NULL, ids, max);
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
    /* F02: optional card order CSV (empty = default). */
    char provider_order[128];
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
