/* Full-screen numeric keypad for editing the Usage API address on device
 * (kindle-port.zh-CN.md section 4): Kindle's system keyboard belongs to the
 * frozen framework, so the app draws its own. The charset is 0-9 . : plus the
 * CJK labels 退格 / 确定 / 取消, all inside the bundled font subset. Targets
 * look like 192.168.1.100:46927; domains and paths go through the config.
 * The 自动刷新 field reuses the same instance in digits-only mode
 * (keypad_set_digits_only): '.' and ':' are grayed and inert there. */
#define _POSIX_C_SOURCE 200112L
#define _DEFAULT_SOURCE /* usleep under -std=c99 */

#include "keypad.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define KEYPAD_PLACEHOLDER "192.168.1.1:8787"

typedef enum { KEY_CHAR = 0, KEY_BACKSPACE, KEY_CONFIRM, KEY_CANCEL } KeyKind;

typedef struct {
    FBInkRect rect;
    KeyKind kind;
    char ch;            /* KEY_CHAR only */
    const char *label;
} Key;

struct Keypad {
    Ui *ui;             /* non-owning: geometry, font and batch-draw config */
    Key keys[15];
    int n_keys;
    FBInkRect echo_rect;
    int title_y;
    int digits_only;    /* per-field mode: '.' and ':' grayed + inert */
};

/* '.' and ':' exist for host:port targets only; a pure-number field
 * (自动刷新) grays them out and swallows their taps. */
static int key_disabled(const Keypad *kp, const Key *k)
{
    return kp->digits_only && k->kind == KEY_CHAR &&
           (k->ch == '.' || k->ch == ':');
}

/* Row 4's 确定 spans the last two columns (span = 2). */
static const struct {
    int row, col, span;
    KeyKind kind;
    char ch;
    const char *label;
} KEY_DEFS[15] = {
    { 0, 0, 1, KEY_CHAR, '1', "1" },
    { 0, 1, 1, KEY_CHAR, '2', "2" },
    { 0, 2, 1, KEY_CHAR, '3', "3" },
    { 0, 3, 1, KEY_BACKSPACE, 0, "退格" },
    { 1, 0, 1, KEY_CHAR, '4', "4" },
    { 1, 1, 1, KEY_CHAR, '5', "5" },
    { 1, 2, 1, KEY_CHAR, '6', "6" },
    { 1, 3, 1, KEY_CHAR, ':', ":" },
    { 2, 0, 1, KEY_CHAR, '7', "7" },
    { 2, 1, 1, KEY_CHAR, '8', "8" },
    { 2, 2, 1, KEY_CHAR, '9', "9" },
    { 2, 3, 1, KEY_CHAR, '.', "." },
    { 3, 0, 1, KEY_CANCEL, 0, "取消" },
    { 3, 1, 1, KEY_CHAR, '0', "0" },
    { 3, 2, 2, KEY_CONFIRM, 0, "确定" },
};

Keypad *keypad_create(Ui *ui)
{
    Keypad *kp;
    int i, gap, key_w, key_h, grid_y, avail, gx;
    int echo_h, min_echo_h, echo_font_h = 0, dummy = 0;

    if (!ui)
        return NULL;
    kp = calloc(1, sizeof(*kp));
    if (!kp)
        return NULL;
    kp->ui = ui;

    gap = ui_scale(ui, 8);
    kp->title_y = ui_scale(ui, 24);

    /* Echo line: S(22) text, left-aligned within h_padding, height sized to
     * the measured line box with S(30) as the floor. */
    ui_measure(ui, "0", ui->px_title, &dummy, &echo_font_h);
    echo_h = echo_font_h + 2;
    min_echo_h = ui_scale(ui, 30);
    if (echo_h < min_echo_h)
        echo_h = min_echo_h;
    kp->echo_rect.left = (unsigned short)ui->h_padding;
    kp->echo_rect.top = (unsigned short)(kp->title_y + ui_scale(ui, 30) + gap);
    kp->echo_rect.width = (unsigned short)ui->content_w;
    kp->echo_rect.height = (unsigned short)echo_h;

    /* 4x4 grid, S(8) gaps, h_padding side margins, key height >= S(56). */
    grid_y = kp->echo_rect.top + kp->echo_rect.height + ui_scale(ui, 16);
    avail = ui->h - grid_y - ui_scale(ui, 16);
    key_h = (avail - 3 * gap) / 4;
    if (key_h < ui_scale(ui, 56))
        key_h = ui_scale(ui, 56);
    key_w = (ui->w - 2 * ui->h_padding - 3 * gap) / 4;
    if (key_w < 1)
        key_w = 1;
    gx = (ui->w - (4 * key_w + 3 * gap)) / 2; /* centers leftover pixels */

    for (i = 0; i < 15; i++) {
        Key *k = &kp->keys[i];
        int x = gx + KEY_DEFS[i].col * (key_w + gap);
        int y = grid_y + KEY_DEFS[i].row * (key_h + gap);

        k->rect.left = (unsigned short)x;
        k->rect.top = (unsigned short)y;
        k->rect.width = (unsigned short)(KEY_DEFS[i].span * key_w +
                                         (KEY_DEFS[i].span - 1) * gap);
        k->rect.height = (unsigned short)key_h;
        k->kind = KEY_DEFS[i].kind;
        k->ch = KEY_DEFS[i].ch;
        k->label = KEY_DEFS[i].label;
    }
    kp->n_keys = 15;
    return kp;
}

void keypad_destroy(Keypad *kp)
{
    free(kp); /* the font set belongs to Ui, not freed here */
}

void keypad_set_digits_only(Keypad *kp, int on)
{
    if (kp)
        kp->digits_only = on ? 1 : 0;
}

/* Normal: black frame + white face. Inverted (pressed): solid black face with
 * white glyphs (ui_text is_inverted flips the drawn pixels; no OT bg paint).
 * Disabled (digits-only '.'/':'): gray label, the ui.c disabled-button idiom. */
static void draw_key(const Keypad *kp, const Key *k, int inverted)
{
    const Ui *ui = kp->ui;
    int tw = 0, th = 0, x, y;
    int fg = key_disabled(kp, k) ? FG_GRAY8 : FG_BLACK;

    ui_fill(ui, k->rect.left, k->rect.top, k->rect.width, k->rect.height, UI_INK_BLACK);
    if (!inverted)
        ui_fill(ui, k->rect.left + ui->border, k->rect.top + ui->border,
                k->rect.width - 2 * ui->border, k->rect.height - 2 * ui->border,
                UI_INK_WHITE);

    if (ui_measure(ui, k->label, ui->px_card_title, &tw, &th) != 0 || tw <= 0)
        return;
    x = k->rect.left + (k->rect.width - tw) / 2;
    y = k->rect.top + (k->rect.height - th) / 2;
    ui_text(ui, k->label, ui->px_card_title, x, y, tw, th,
            UI_ALIGN_LEFT, fg, inverted);
}

/* Erase and redraw the echo line (no refresh; callers refresh as needed).
 * Empty input shows a light-gray placeholder instead of the text. */
static void draw_echo(const Keypad *kp, const char *text)
{
    const Ui *ui = kp->ui;
    const FBInkRect *r = &kp->echo_rect;

    ui_fill(ui, r->left, r->top, r->width, r->height, UI_INK_WHITE);
    if (text && *text)
        ui_text(ui, text, ui->px_title, r->left, r->top, r->width, r->height,
                UI_ALIGN_LEFT, FG_BLACK, 0);
    else
        ui_text(ui, KEYPAD_PLACEHOLDER, ui->px_title, r->left, r->top, r->width,
                r->height, UI_ALIGN_LEFT, FG_GRAYC, 0);
}

void keypad_draw(Keypad *kp, const char *title, const char *text)
{
    Ui *ui;
    int i;

    if (!kp)
        return;
    ui = kp->ui;

    fbink_cls(ui->fbfd, &ui->cfg_draw, NULL, false); /* white canvas, no refresh */
    ui_text(ui, (title && *title) ? title : "接口地址", ui->px_card_title,
            0, kp->title_y, ui->w, ui_scale(ui, 30), UI_ALIGN_CENTER, FG_BLACK, 0);
    draw_echo(kp, text);
    for (i = 0; i < kp->n_keys; i++)
        draw_key(kp, &kp->keys[i], 0);
    /* The caller issues the GC16 refresh on entry. */
}

int keypad_handle_tap(Keypad *kp, int fbfd, int x, int y, char *text, size_t text_size)
{
    const Key *k = NULL;
    int i;
    size_t len;

    if (!kp || !text || text_size == 0)
        return 0;
    for (i = 0; i < kp->n_keys; i++) {
        const FBInkRect *r = &kp->keys[i].rect;

        if (x >= r->left && x < r->left + r->width &&
            y >= r->top && y < r->top + r->height) {
            k = &kp->keys[i];
            break;
        }
    }
    if (!k)
        return 0;
    if (key_disabled(kp, k))
        return 0; /* grayed key: no press feedback, no edit, keep editing */

    /* Press feedback: invert, fast partial refresh, hold, restore, refresh. */
    draw_key(kp, k, 1);
    ui_refresh(fbfd, &k->rect, WFM_DU);
    usleep(100 * 1000);
    draw_key(kp, k, 0);
    ui_refresh(fbfd, &k->rect, WFM_DU);

    len = strlen(text);
    switch (k->kind) {
    case KEY_CHAR:
        if (len + 1 < text_size) {
            text[len] = k->ch;
            text[len + 1] = '\0';
            draw_echo(kp, text);
            ui_refresh(fbfd, &kp->echo_rect, WFM_DU);
        }
        return 0;
    case KEY_BACKSPACE:
        if (len > 0) {
            text[len - 1] = '\0'; /* charset is ASCII-only: byte-safe */
            draw_echo(kp, text);
            ui_refresh(fbfd, &kp->echo_rect, WFM_DU);
        }
        return 0;
    case KEY_CONFIRM:
        return len > 0 ? 1 : 0; /* disabled while empty: flash only, keep editing */
    case KEY_CANCEL:
        return -1;
    }
    return 0;
}
