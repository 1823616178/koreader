/* Settings page: one full-screen form for the config keys that matter on
 * device (api_url, wifi_auto_off, auto_refresh_min). Layout and feedback
 * follow keypad.c -- framed rows, invert-flash on press, DU refreshes for
 * value updates, one GC16 per page transition issued by the caller. Value
 * strings are rebuilt from the working copy after every change, so the page
 * can never drift from the state it will commit. */
#define _POSIX_C_SOURCE 200112L
#define _DEFAULT_SOURCE /* usleep under -std=c99 */

#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { ROW_URL = 0, ROW_WIFI, ROW_REFRESH, ROW_COUNT };

struct Settings {
    Ui *ui;                 /* non-owning: geometry, font, batch-draw config */
    AppConfig work;         /* edited here, committed on 返回 */
    AppConfig orig;         /* dirty-compare baseline */
    FBInkRect row_rect[ROW_COUNT];
    FBInkRect back_rect;
    char val_text[ROW_COUNT][128];
};

static const struct {
    const char *label;
} ROW_DEFS[ROW_COUNT] = {
    { "接口地址" },
    { "刷新后关WiFi" },
    { "自动刷新" },
};

/* Authority part of api_url ("http://h:8123/api/usage" -> "h:8123"), shown as
 * row value. Mirrors main.c's api_host(); kept separate so settings.c stays
 * independent of main's statics. */
static void host_of(const char *url, char *dst, size_t dst_size)
{
    const char *p = url;
    const char *scheme = strstr(url, "://");
    size_t n;

    if (!dst || dst_size == 0)
        return;
    if (scheme)
        p = scheme + 3;
    n = strcspn(p, "/");
    if (n >= dst_size)
        n = dst_size - 1;
    memcpy(dst, p, n);
    dst[n] = '\0';
}

/* Rebuild every row's value string from the working copy. */
static void refresh_values(Settings *s)
{
    char host[64];

    host_of(s->work.api_url, host, sizeof(host));
    snprintf(s->val_text[ROW_URL], sizeof(s->val_text[0]), "%s", host);
    snprintf(s->val_text[ROW_WIFI], sizeof(s->val_text[0]), "%s",
             s->work.wifi_auto_off ? "开" : "关");
    if (s->work.auto_refresh_min > 0)
        snprintf(s->val_text[ROW_REFRESH], sizeof(s->val_text[0]), "%d 分钟",
                 s->work.auto_refresh_min);
    else
        snprintf(s->val_text[ROW_REFRESH], sizeof(s->val_text[0]), "%s", "关闭");
}

Settings *settings_create(Ui *ui, const AppConfig *cfg)
{
    Settings *s;
    int i, gap, row_h, rows_y, y;

    if (!ui || !cfg)
        return NULL;
    s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->ui = ui;
    s->work = *cfg;
    s->orig = *cfg;
    refresh_values(s);

    gap = ui_scale(ui, 10);
    row_h = ui_scale(ui, 56);

    /* Top-aligned under the title bar, like a form; leftover space stays at
     * the bottom (the keypad fills its screen, this page is a short list). */
    rows_y = ui_scale(ui, 88);
    y = rows_y;
    for (i = 0; i < ROW_COUNT; i++) {
        s->row_rect[i].left = (unsigned short)ui->h_padding;
        s->row_rect[i].top = (unsigned short)y;
        s->row_rect[i].width = (unsigned short)ui->content_w;
        s->row_rect[i].height = (unsigned short)row_h;
        y += row_h + gap;
    }
    y += ui_scale(ui, 20) - gap;
    s->back_rect.left = (unsigned short)ui->h_padding;
    s->back_rect.top = (unsigned short)y;
    s->back_rect.width = (unsigned short)ui->content_w;
    s->back_rect.height = (unsigned short)ui_scale(ui, 48);

    if (y + s->back_rect.height > ui->h) { /* tiny screens: refuse, caller keeps dashboard */
        free(s);
        return NULL;
    }
    return s;
}

void settings_destroy(Settings *s)
{
    free(s);
}

/* Framed row face: black frame + white interior, keypad draw_key style. */
static void draw_frame(const Ui *ui, const FBInkRect *r)
{
    ui_fill(ui, r->left, r->top, r->width, r->height, UI_INK_BLACK);
    ui_fill(ui, r->left + ui->border, r->top + ui->border,
            r->width - 2 * ui->border, r->height - 2 * ui->border, UI_INK_WHITE);
}

/* One row: label left, current value right-aligned. fg picks the value color
 * (gray for the "off" states). An over-wide value is clamped into the room
 * left of the right padding and let ui_text truncate it with an ellipsis. */
static void draw_row_content(const Settings *s, int i)
{
    const Ui *ui = s->ui;
    const FBInkRect *r = &s->row_rect[i];
    const int pad = ui_scale(ui, 14);
    int lh = 0, vh = 0, lw = 0, vw = 0, ty, vx, vw_fit, val_fg = FG_BLACK;

    ui_measure(ui, ROW_DEFS[i].label, ui->px_label, &lw, &lh);
    ui_measure(ui, s->val_text[i], ui->px_label, &vw, &vh);
    ty = r->top + (r->height - (lh > vh ? lh : vh)) / 2;
    if ((i == ROW_WIFI && !s->work.wifi_auto_off) ||
        (i == ROW_REFRESH && s->work.auto_refresh_min <= 0))
        val_fg = FG_GRAY8;

    ui_text(ui, ROW_DEFS[i].label, ui->px_label,
            r->left + pad, ty, lw ? lw : 1, lh ? lh : 1, UI_ALIGN_LEFT, FG_BLACK, 0);

    vx = r->left + r->width - pad - vw; /* ideal: flush against right pad */
    if (vx < r->left + pad + lw + pad)
        vx = r->left + pad + lw + pad;  /* collided with the label: clamp */
    vw_fit = r->left + r->width - pad - vx;
    if (vw_fit < 1)
        return;                         /* no room at all: drop the value */
    ui_text(ui, s->val_text[i], ui->px_label,
            vx, ty, vw_fit, vh ? vh : 1, UI_ALIGN_LEFT, val_fg, 0);
}

static void draw_row(const Settings *s, int i)
{
    draw_frame(s->ui, &s->row_rect[i]);
    draw_row_content(s, i);
}

void settings_draw(Settings *s)
{
    Ui *ui;
    int i;

    if (!s)
        return;
    ui = s->ui;

    fbink_cls(ui->fbfd, &ui->cfg_draw, NULL, false); /* white canvas, no refresh */
    ui_text(ui, "设置", ui->px_card_title,
            0, ui_scale(ui, 24), ui->w, ui_scale(ui, 30), UI_ALIGN_CENTER, FG_BLACK, 0);
    for (i = 0; i < ROW_COUNT; i++)
        draw_row(s, i);

    draw_frame(ui, &s->back_rect);
    ui_text(ui, "返回", ui->px_percent,
            0, s->back_rect.top, ui->w, s->back_rect.height,
            UI_ALIGN_CENTER, FG_BLACK, 0);
    /* The caller issues the GC16 refresh on entry. */
}

/* Press feedback: paint the rect solid black, DU-refresh, hold ~100 ms, then
 * let the caller of flash repaint normally and refresh again (keypad.c style). */
static void flash_press(const Settings *s, int fbfd, const FBInkRect *r)
{
    ui_fill(s->ui, r->left, r->top, r->width, r->height, UI_INK_BLACK);
    ui_refresh(fbfd, r, WFM_DU);
    usleep(100 * 1000);
}

static const FBInkRect *hit_row(const Settings *s, int x, int y)
{
    int i;

    for (i = 0; i < ROW_COUNT; i++) {
        const FBInkRect *r = &s->row_rect[i];

        if (x >= r->left && x < r->left + r->width &&
            y >= r->top && y < r->top + r->height)
            return r;
    }
    return NULL;
}

SettingsHit settings_handle_tap(Settings *s, int fbfd, int x, int y)
{
    const FBInkRect *r = hit_row(s, x, y);

    if (!r) {
        if (x >= s->back_rect.left && x < s->back_rect.left + s->back_rect.width &&
            y >= s->back_rect.top && y < s->back_rect.top + s->back_rect.height) {
            flash_press(s, fbfd, &s->back_rect);
            return SET_HIT_BACK;
        }
        return SET_HIT_NONE;
    }

    if (r == &s->row_rect[ROW_WIFI]) {
        flash_press(s, fbfd, r);
        s->work.wifi_auto_off = !s->work.wifi_auto_off;
        refresh_values(s);
        draw_row(s, ROW_WIFI);
        ui_refresh(fbfd, r, WFM_DU);
        return SET_HIT_NONE;
    }

    {
        int idx = (r == &s->row_rect[ROW_URL]) ? ROW_URL : ROW_REFRESH;

        flash_press(s, fbfd, r); /* blacked out: repaint + refresh to restore */
        draw_row(s, idx);
        ui_refresh(fbfd, r, WFM_DU);

        return idx == ROW_URL ? SET_HIT_EDIT_URL : SET_HIT_EDIT_REFRESH;
    }
}

const char *settings_url(const Settings *s)
{
    return s ? s->work.api_url : "";
}

int settings_refresh_min(const Settings *s)
{
    return s ? s->work.auto_refresh_min : 0;
}

int settings_set_url(Settings *s, const char *normalized_url)
{
    int changed;

    if (!s || !normalized_url)
        return 0;
    changed = strcmp(s->work.api_url, normalized_url) != 0;
    snprintf(s->work.api_url, sizeof(s->work.api_url), "%s", normalized_url);
    if (changed)
        refresh_values(s);
    return changed;
}

int settings_set_refresh_min(Settings *s, int minutes)
{
    int changed;

    if (!s)
        return 0;
    minutes = config_clamp_auto_refresh(minutes);
    changed = s->work.auto_refresh_min != minutes;
    s->work.auto_refresh_min = minutes;
    if (changed)
        refresh_values(s);
    return changed;
}

int settings_commit(Settings *s, AppConfig *cfg)
{
    int changed;

    if (!s || !cfg)
        return 0;
    changed = strcmp(s->work.api_url, s->orig.api_url) != 0 ||
              s->work.touch_swap_xy != s->orig.touch_swap_xy ||
              s->work.touch_mirror_x != s->orig.touch_mirror_x ||
              s->work.touch_mirror_y != s->orig.touch_mirror_y ||
              s->work.wifi_auto_off != s->orig.wifi_auto_off ||
              s->work.auto_refresh_min != s->orig.auto_refresh_min;
    if (changed)
        *cfg = s->work;
    return changed;
}
