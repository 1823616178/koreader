#ifndef AIQUOTA_UI_H
#define AIQUOTA_UI_H

#include "fbink.h"
#include "quota.h"

typedef struct Ui Ui;   /* fb geometry, scale factor, computed layout, button rects */

typedef enum { UI_ACTION_NONE = 0, UI_ACTION_REFRESH, UI_ACTION_EDIT_URL, UI_ACTION_CLOSE } UiAction;

/* dpi comes from fbink_get_state(); font_path is the bundled Noto Sans SC subset (loaded via fbink_add_ot_font).
 * Returns NULL on failure (bad font_path/OOM); caller must check. */
Ui  *ui_create(int fbfd, int screen_w, int screen_h, int dpi, const char *font_path);
void ui_destroy(Ui *ui);

/* Full dashboard redraw (everything drawn with no_refresh); caller issues one fbink_refresh_rect.
 * host: title-bar subtitle (may be NULL -> "未配置").
 * status_state: "连接中" | "失败" | "已连接"; status_detail: url or error text; loading: 0/1. */
void ui_draw_dashboard(Ui *ui, const QuotaSnapshot *snap, const char *host,
                       const char *status_state, const char *status_detail, int loading);

/* Hit-test the bottom button row (screen-space tap coords). Disabled buttons return UI_ACTION_NONE. */
UiAction ui_hit_button(const Ui *ui, int x, int y);

/* Press feedback: redraw the button inverted, refresh its rect with WFM_DU, ~120ms, restore, refresh again. */
void ui_flash_button(Ui *ui, int fbfd, UiAction action);

/* ---------------------------------------------------------------------------
 * Internals shared with keypad.c: the on-screen numeric keypad reuses Ui's
 * geometry, loaded OT font and drawing helpers, so struct Ui and the low
 * level primitives live here instead of being duplicated. Application code
 * (main.c) must only use the functions above.
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
    int h_padding, card_gap, card_padding, border, line_h;
    int gap2, gap3, gap5, bar_h, title_h, button_h;
    int wb_h;   /* window block height: value line + bar + gap3 + detail line */
    int content_w, card_w, card_inner, card_h, content_h, body_y;
    /* font sizes in pixels */
    int px_card_title, px_label, px_percent, px_detail, px_title, px_subtitle;
    /* bottom button row: refresh / edit-url / close, valid once drawn */
    FBInkRect btn_rect[3];
    char btn_label[3][24];
    int btn_fg[3];      /* FG_COLOR_INDEX_E values */
    int btn_enabled[3];
    int btn_row_drawn;
};

int  ui_scale(const Ui *ui, int n);   /* lround(n * dpi / 167.0), KOReader scaleBySize */
void ui_fill(const Ui *ui, int x, int y, int w, int h, uint8_t gray);
int  ui_measure(const Ui *ui, const char *text, int size_px, int *out_w, int *out_h);
/* Draw one line of text, top-left at (x, y), truncated with U+2026 to width w.
 * align: UI_ALIGN_*; fg_index: FG_COLOR_INDEX_E; inverted: white on black. */
void ui_text(const Ui *ui, const char *text, int size_px, int x, int y, int w, int h,
             int align, int fg_index, int inverted);
void ui_refresh(int fbfd, const FBInkRect *rect, int wfm_mode);

#endif
