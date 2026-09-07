/* Settings page: one full-screen form for the config keys that matter on
 * device (api_url, wifi_auto_off, auto_refresh_min) plus 前光, which talks
 * to powerd immediately (one merged row: on/off toggle on the left, inline
 * -/+ level stepper on the right) and is not a config key. Layout and
 * feedback follow keypad.c -- framed rows, invert-flash on press, DU
 * refreshes for value updates, one GC16 per page transition issued by the
 * caller. Value strings are rebuilt from the working copy after every
 * change, so the page can never drift from the state it will commit. */
#define _POSIX_C_SOURCE 200112L
#define _DEFAULT_SOURCE /* usleep under -std=c99 */

#include "settings.h"

#include "light.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { ROW_URL = 0, ROW_WIFI, ROW_REFRESH, ROW_IDLE, ROW_DISPLAY, ROW_PROVIDERS,
       ROW_DIAGNOSE, ROW_LIGHT, ROW_COUNT };
enum { PROV_COUNT = CONFIG_PROVIDERS_MAX };

struct Settings {
    Ui *ui;                 /* non-owning: geometry, font, batch-draw config */
    AppConfig work;         /* edited here, committed on 返回 */
    AppConfig orig;         /* dirty-compare baseline */
    FBInkRect row_rect[ROW_COUNT];
    FBInkRect light_minus;  /* 前光 row's stepper: keys flank a level box */
    FBInkRect light_plus;
    FBInkRect light_level_box;
    FBInkRect back_rect;
    char val_text[ROW_COUNT][128];
    char level_text[16];    /* level shown between the stepper keys */
    int light_ok;           /* 1 when powerd answered flIntensity */
    int light_on;           /* intensity > 0 */
    int light_level;        /* last set / last read, 0 when off */
    int light_max;          /* flMaxIntensity, or LIGHT_MAX_FALLBACK */
    /* F02 providers sub-page geometry */
    FBInkRect prov_row[PROV_COUNT];
    FBInkRect prov_up[PROV_COUNT];
    FBInkRect prov_dn[PROV_COUNT];
    FBInkRect prov_back;
    char prov_warn[64];     /* "need >=1" flash text */
};

static const struct {
    const char *label;
} ROW_DEFS[ROW_COUNT] = {
    { "接口地址" },
    { "刷新后关WiFi" },
    { "自动刷新" },
    { "Idle light" }, /* F06: minutes; ASCII to avoid font rebuild */
    { "Display" },    /* F04: API / used / remain / large / compact */
    { "Providers" },  /* F02 */
    { "Diagnose" },   /* F03 */
    { "前光" },
};

static const char *const PROV_NAMES[PROV_COUNT] = {
    "grok", "claude", "codex", "kimi", "opencode", "agy"
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
    if (s->work.light_idle_off_min > 0)
        snprintf(s->val_text[ROW_IDLE], sizeof(s->val_text[0]), "%d 分钟",
                 s->work.light_idle_off_min);
    else
        snprintf(s->val_text[ROW_IDLE], sizeof(s->val_text[0]), "%s", "关闭");
    {
        const char *dm = "API";
        if (s->work.display_mode_pref == 1)
            dm = "used";
        else if (s->work.display_mode_pref == 2)
            dm = "remain";
        else if (s->work.display_mode_pref == 3)
            dm = "large";
        else if (s->work.display_mode_pref == 4)
            dm = "compact";
        snprintf(s->val_text[ROW_DISPLAY], sizeof(s->val_text[0]), "%s", dm);
    }
    {
        int n = config_providers_count(s->work.show_providers);
        snprintf(s->val_text[ROW_PROVIDERS], sizeof(s->val_text[0]),
                 "%d / %d", n, CONFIG_PROVIDERS_MAX);
    }
    snprintf(s->val_text[ROW_DIAGNOSE], sizeof(s->val_text[0]), "%s", "run");
    if (!s->light_ok) {
        snprintf(s->val_text[ROW_LIGHT], sizeof(s->val_text[0]), "%s", "--");
        snprintf(s->level_text, sizeof(s->level_text), "%s", "--");
    } else {
        snprintf(s->val_text[ROW_LIGHT], sizeof(s->val_text[0]), "%s",
                 s->light_on ? "开" : "关");
        snprintf(s->level_text, sizeof(s->level_text), "%d", s->light_level);
    }
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
    {
        int cur = light_get_intensity();
        int max = light_get_max();

        if (cur < 0) {
            s->light_ok = 0;
            s->light_max = LIGHT_MAX_FALLBACK;
        } else {
            s->light_ok = 1;
            s->light_max = max >= 1 ? max : LIGHT_MAX_FALLBACK;
            s->light_level = light_clamp(cur, s->light_max);
            s->light_on = s->light_level > 0;
            if (s->light_on)
                light_set_last_on(s->light_level); /* freshen the slot from
                                                    * powerd's live value */
            /* Off: nothing to seed -- the stepper/toggle read
             * light_last_on() themselves, with max/2 as the fallback when
             * no positive level is known. */
        }
    }
    refresh_values(s);

    /* Keep dense settings pages compact by tightening the gaps, not the
     * controls.  The light stepper needs a 48-unit visual/touch target; the
     * old 42-unit row made both keys spill outside ROW_LIGHT, while partial
     * refreshes still covered only the row and left clipped/ghosted edges. */
    gap = ui_scale(ui, ROW_COUNT > 6 ? 4 : 10);
    row_h = ui_scale(ui, ROW_COUNT > 4 ? 48 : 56);

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

    { /* 前光 row's right end (docs/kindle-ui-and-power.md P2 merge): two
       * S(48) square keys flanking a fixed-width level box, "- N +"; the
       * row's left half stays the on/off toggle. */
        const FBInkRect *r = &s->row_rect[ROW_LIGHT];
        const int pad = ui_scale(ui, 14);
        const int side = ui_scale(ui, 48);
        const int between = ui_scale(ui, 8);
        const int box_w = ui_scale(ui, 56);
        const int top = (int)r->top + ((int)r->height - side) / 2;

        s->light_minus.width = s->light_plus.width = (unsigned short)side;
        s->light_minus.height = s->light_plus.height = (unsigned short)side;
        s->light_minus.top = s->light_plus.top = (unsigned short)top;
        s->light_plus.left = (unsigned short)((int)r->left + (int)r->width -
                                              pad - side);
        s->light_minus.left = (unsigned short)((int)s->light_plus.left -
                                               between - box_w - between - side);
        s->light_level_box.left = (unsigned short)((int)s->light_minus.left +
                                                   side + between);
        s->light_level_box.top = (unsigned short)r->top;
        s->light_level_box.width = (unsigned short)box_w;
        s->light_level_box.height = (unsigned short)r->height;
    }

    if (y + s->back_rect.height > ui->h) { /* tiny screens: refuse, caller keeps dashboard */
        free(s);
        return NULL;
    }

    /* Providers sub-page: one row per tool + Back. */
    {
        int py = ui_scale(ui, 72);
        int ph = ui_scale(ui, 44);
        int pg = ui_scale(ui, 8);
        int side = ui_scale(ui, 40);
        int i;

        s->prov_warn[0] = '\0';
        for (i = 0; i < PROV_COUNT; i++) {
            s->prov_row[i].left = (unsigned short)ui->h_padding;
            s->prov_row[i].top = (unsigned short)py;
            s->prov_row[i].width = (unsigned short)ui->content_w;
            s->prov_row[i].height = (unsigned short)ph;
            s->prov_dn[i].width = s->prov_up[i].width = (unsigned short)side;
            s->prov_dn[i].height = s->prov_up[i].height = (unsigned short)side;
            s->prov_up[i].top = s->prov_dn[i].top =
                (unsigned short)(py + (ph - side) / 2);
            s->prov_dn[i].left = (unsigned short)((int)s->prov_row[i].left +
                                                  (int)s->prov_row[i].width -
                                                  ui_scale(ui, 14) - side);
            s->prov_up[i].left = (unsigned short)((int)s->prov_dn[i].left -
                                                  ui_scale(ui, 6) - side);
            py += ph + pg;
        }
        s->prov_back.left = (unsigned short)ui->h_padding;
        s->prov_back.top = (unsigned short)(py + ui_scale(ui, 8));
        s->prov_back.width = (unsigned short)ui->content_w;
        s->prov_back.height = (unsigned short)ui_scale(ui, 48);
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
    int right_edge = r->left + r->width - pad;

    if (i == ROW_LIGHT)
        right_edge = (int)s->light_minus.left - pad; /* stepper owns the right end */

    ui_measure(ui, ROW_DEFS[i].label, ui->px_label, &lw, &lh);
    ui_measure(ui, s->val_text[i], ui->px_label, &vw, &vh);
    ty = r->top + (r->height - (lh > vh ? lh : vh)) / 2;
    if ((i == ROW_WIFI && !s->work.wifi_auto_off) ||
        (i == ROW_REFRESH && s->work.auto_refresh_min <= 0) ||
        (i == ROW_LIGHT && (!s->light_ok || !s->light_on)))
        val_fg = FG_GRAY8;

    ui_text(ui, ROW_DEFS[i].label, ui->px_label,
            r->left + pad, ty, lw ? lw : 1, lh ? lh : 1, UI_ALIGN_LEFT, FG_BLACK, 0);

    vx = right_edge - vw;               /* ideal: flush against the right edge */
    if (vx < r->left + pad + lw + pad)
        vx = r->left + pad + lw + pad;  /* collided with the label: clamp */
    vw_fit = right_edge - vx;
    if (vw_fit < 1)
        return;                         /* no room at all: drop the value */
    ui_text(ui, s->val_text[i], ui->px_label,
            vx, ty, vw_fit, vh ? vh : 1, UI_ALIGN_LEFT, val_fg, 0);
}

/* Stepper key face: keypad draw_key framing -- black frame, white face,
 * centered ASCII +/- (always in the font subset, no new CJK). */
static void draw_step_key(const Settings *s, const FBInkRect *r, const char *glyph)
{
    const Ui *ui = s->ui;
    int tw = 0, th = 0, x, y;

    draw_frame(ui, r);
    if (ui_measure(ui, glyph, ui->px_card_title, &tw, &th) != 0 || tw <= 0)
        return;
    x = r->left + (r->width - tw) / 2;
    y = r->top + (r->height - th) / 2;
    ui_text(ui, glyph, ui->px_card_title, x, y, tw, th, UI_ALIGN_LEFT, FG_BLACK, 0);
}

/* Current level in the box between the stepper keys; gray while off.  Draw
 * the readout as part of the same three-piece control so its placement does
 * not depend on the width of a one- vs. two-digit value. */
static void draw_light_level(const Settings *s)
{
    const Ui *ui = s->ui;
    const FBInkRect *r = &s->light_level_box;
    int tw = 0, th = 0, y;
    int inset = ui->border;

    draw_frame(ui, r);
    if (ui_measure(ui, s->level_text, ui->px_card_title, &tw, &th) != 0 ||
        tw <= 0 || th <= 0)
        return;
    y = r->top + (r->height - th) / 2;
    ui_text(ui, s->level_text, ui->px_card_title,
            r->left + inset, y, r->width - 2 * inset, th, UI_ALIGN_CENTER,
            s->light_ok && s->light_on ? FG_BLACK : FG_GRAY8, 0);
}

static void draw_row(const Settings *s, int i)
{
    draw_frame(s->ui, &s->row_rect[i]);
    draw_row_content(s, i);
    if (i == ROW_LIGHT) {
        draw_step_key(s, &s->light_minus, "-");
        draw_step_key(s, &s->light_plus, "+");
        draw_light_level(s);
    }
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
    {
        /* ui_text does not v-center: y is the glyph-box top (keypad draw_key). */
        const FBInkRect *r = &s->back_rect;
        int tw = 0, th = 0, x, y;

        if (ui_measure(ui, "返回", ui->px_percent, &tw, &th) == 0 && tw > 0) {
            x = r->left + (r->width - tw) / 2;
            y = r->top + (r->height - th) / 2;
            ui_text(ui, "返回", ui->px_percent, x, y, tw, th,
                    UI_ALIGN_LEFT, FG_BLACK, 0);
        }
    }
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

static int rect_has(const FBInkRect *r, int x, int y)
{
    return x >= r->left && x < r->left + r->width &&
           y >= r->top && y < r->top + r->height;
}

static const FBInkRect *hit_row(const Settings *s, int x, int y)
{
    int i;

    for (i = 0; i < ROW_COUNT; i++) {
        const FBInkRect *r = &s->row_rect[i];

        if (rect_has(r, x, y))
            return r;
    }
    return NULL;
}

static void restore_row(const Settings *s, int fbfd, int i)
{
    draw_row(s, i);
    ui_refresh(fbfd, &s->row_rect[i], WFM_DU);
}

/* Adjust the light level by one click from the 前光 row's stepper: applied
 * to powerd immediately (no keypad layer). State and value strings only --
 * the caller repaints, so a no-op click (!light_ok, already at 0, at the
 * ceiling, powerd refusing) still un-inverts the flashed key. A click whose
 * clamped target changes neither level nor on/off skips the lipc child
 * process entirely -- powerd's side would not change either.
 * Only UP clicks update the remembered on-level: stepping down through 5..1
 * to 0 is how the light gets turned off here, and recording those pass-through
 * levels would decay the restore target to 1 -- which maps to ~2/4095 on the
 * backlight driver, i.e. the light comes back visibly still off. */
static void step_light(Settings *s, int up)
{
    int next = -1; /* -1: nothing to apply */

    if (s->light_ok) {
        if (up) {
            if (s->light_on) {
                next = s->light_level + 1; /* light_clamp caps at light_max */
            } else {
                next = light_last_on(); /* off -> on: same rule as the toggle */
                if (next < 1)
                    next = s->light_max / 2;
                if (next < 1)
                    next = 1;
            }
        } else if (s->light_on) {
            next = s->light_level - 1; /* 0 = off */
        }
        if (next >= 0) {
            next = light_clamp(next, s->light_max);
            /* Same level and same on/off (+ at the ceiling clamps back to
             * light_max): nothing to tell powerd, skip the lipc spawn. */
            if ((next != s->light_level || (next > 0) != s->light_on) &&
                light_set_intensity(next) == 0) {
                s->light_level = next;
                s->light_on = next > 0;
                if (up && next > 0)
                    light_set_last_on(next);
            }
        }
    }
    refresh_values(s); /* idempotent: a no-op click rewrites the same strings */
}

SettingsHit settings_handle_tap(Settings *s, int fbfd, int x, int y)
{
    const FBInkRect *r;

    if (rect_has(&s->light_minus, x, y) || rect_has(&s->light_plus, x, y)) {
        int up = rect_has(&s->light_plus, x, y);
        const FBInkRect *key = up ? &s->light_plus : &s->light_minus;

        flash_press(s, fbfd, key);
        step_light(s, up);
        /* One DU covers both outcomes: the new level, or just the flash
         * coming back out when the click was a no-op. */
        restore_row(s, fbfd, ROW_LIGHT);
        return SET_HIT_NONE;
    }

    r = hit_row(s, x, y);
    if (!r) {
        if (rect_has(&s->back_rect, x, y)) {
            flash_press(s, fbfd, &s->back_rect);
            return SET_HIT_BACK;
        }
        return SET_HIT_NONE;
    }

    if (r == &s->row_rect[ROW_WIFI]) {
        flash_press(s, fbfd, r);
        s->work.wifi_auto_off = !s->work.wifi_auto_off;
        refresh_values(s);
        restore_row(s, fbfd, ROW_WIFI);
        return SET_HIT_NONE;
    }

    if (r == &s->row_rect[ROW_DISPLAY]) {
        flash_press(s, fbfd, r);
        s->work.display_mode_pref = (s->work.display_mode_pref + 1) % 5;
        refresh_values(s);
        restore_row(s, fbfd, ROW_DISPLAY);
        return SET_HIT_NONE;
    }

    if (r == &s->row_rect[ROW_LIGHT]) {
        int next;

        /* The stepper owns the row's right end (keys handled above; the
         * level box and the key gaps are dead): only the left half toggles. */
        if (x >= (int)s->light_minus.left)
            return SET_HIT_NONE;
        flash_press(s, fbfd, r);
        if (!s->light_ok) {
            restore_row(s, fbfd, ROW_LIGHT);
            return SET_HIT_NONE;
        }
        if (s->light_on) {
            if (s->light_level > 0)
                light_set_last_on(s->light_level); /* remember where on was */
            next = 0;
        } else {
            next = light_last_on();
            if (next < 1)
                next = s->light_max / 2;
            if (next < 1)
                next = 1;
            next = light_clamp(next, s->light_max);
        }
        if (light_set_intensity(next) == 0) {
            s->light_level = next;
            s->light_on = next > 0;
            refresh_values(s);
        }
        restore_row(s, fbfd, ROW_LIGHT);
        return SET_HIT_NONE;
    }

    if (r == &s->row_rect[ROW_PROVIDERS]) {
        flash_press(s, fbfd, r);
        restore_row(s, fbfd, ROW_PROVIDERS);
        return SET_HIT_EDIT_PROVIDERS;
    }

    if (r == &s->row_rect[ROW_DIAGNOSE]) {
        flash_press(s, fbfd, r);
        restore_row(s, fbfd, ROW_DIAGNOSE);
        return SET_HIT_DIAGNOSE;
    }

    if (r == &s->row_rect[ROW_URL] || r == &s->row_rect[ROW_REFRESH] ||
        r == &s->row_rect[ROW_IDLE]) {
        int idx = ROW_URL;

        if (r == &s->row_rect[ROW_REFRESH])
            idx = ROW_REFRESH;
        else if (r == &s->row_rect[ROW_IDLE])
            idx = ROW_IDLE;
        flash_press(s, fbfd, r);
        restore_row(s, fbfd, idx);
        if (idx == ROW_URL)
            return SET_HIT_EDIT_URL;
        if (idx == ROW_IDLE)
            return SET_HIT_EDIT_IDLE;
        return SET_HIT_EDIT_REFRESH;
    }
    return SET_HIT_NONE;
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

int settings_idle_min(const Settings *s)
{
    return s ? s->work.light_idle_off_min : 0;
}

int settings_set_light_idle_min(Settings *s, int minutes)
{
    int changed;

    if (!s)
        return 0;
    minutes = config_clamp_light_idle(minutes);
    changed = s->work.light_idle_off_min != minutes;
    s->work.light_idle_off_min = minutes;
    if (changed)
        refresh_values(s);
    return changed;
}

const char *settings_providers(const Settings *s)
{
    return s ? s->work.show_providers : CONFIG_PROVIDERS_DEFAULT;
}

static int provider_enabled_in(const char *list, const char *name)
{
    ProviderSelection sel;
    config_providers_to_selection(list, &sel);
    if (strcmp(name, "grok") == 0) return sel.grok;
    if (strcmp(name, "claude") == 0) return sel.claude;
    if (strcmp(name, "codex") == 0) return sel.codex;
    if (strcmp(name, "kimi") == 0) return sel.kimi;
    if (strcmp(name, "opencode") == 0) return sel.opencode;
    if (strcmp(name, "agy") == 0) return sel.agy;
    return 0;
}

void settings_providers_draw(Settings *s)
{
    Ui *ui;
    int i;

    if (!s)
        return;
    ui = s->ui;
    fbink_cls(ui->fbfd, &ui->cfg_draw, NULL, false);
    ui_text(ui, "Providers", ui->px_card_title,
            0, ui_scale(ui, 20), ui->w, ui_scale(ui, 28),
            UI_ALIGN_CENTER, FG_BLACK, 0);
    if (s->prov_warn[0]) {
        ui_text(ui, s->prov_warn, ui->px_subtitle,
                0, ui_scale(ui, 48), ui->w, ui_scale(ui, 20),
                UI_ALIGN_CENTER, FG_BLACK, 0);
    }
    for (i = 0; i < PROV_COUNT; i++) {
        const FBInkRect *r = &s->prov_row[i];
        char line[64];
        int on = provider_enabled_in(s->work.show_providers, PROV_NAMES[i]);
        int pad = ui_scale(ui, 14);
        int tw = 0, th = 0;

        draw_frame(ui, r);
        snprintf(line, sizeof(line), "%s  %s", PROV_NAMES[i], on ? "ON" : "off");
        if (ui_measure(ui, line, ui->px_label, &tw, &th) == 0 && tw > 0) {
            int ty = r->top + (r->height - th) / 2;
            ui_text(ui, line, ui->px_label, r->left + pad, ty, tw, th,
                    UI_ALIGN_LEFT, on ? FG_BLACK : FG_GRAY8, 0);
        }
        draw_step_key(s, &s->prov_up[i], "^");
        draw_step_key(s, &s->prov_dn[i], "v");
    }
    draw_frame(ui, &s->prov_back);
    {
        const FBInkRect *r = &s->prov_back;
        int tw = 0, th = 0, x, y;
        if (ui_measure(ui, "返回", ui->px_percent, &tw, &th) == 0 && tw > 0) {
            x = r->left + (r->width - tw) / 2;
            y = r->top + (r->height - th) / 2;
            ui_text(ui, "返回", ui->px_percent, x, y, tw, th,
                    UI_ALIGN_LEFT, FG_BLACK, 0);
        }
    }
}

int settings_providers_handle_tap(Settings *s, int fbfd, int x, int y)
{
    int i;

    if (!s)
        return 1;
    if (rect_has(&s->prov_back, x, y)) {
        flash_press(s, fbfd, &s->prov_back);
        s->prov_warn[0] = '\0';
        return 1;
    }
    for (i = 0; i < PROV_COUNT; i++) {
        if (rect_has(&s->prov_up[i], x, y)) {
            flash_press(s, fbfd, &s->prov_up[i]);
            (void)config_providers_move(s->work.show_providers,
                                        sizeof(s->work.show_providers),
                                        PROV_NAMES[i], -1);
            refresh_values(s);
            settings_providers_draw(s);
            {
                FBInkRect full;
                full.left = 0; full.top = 0;
                full.width = (unsigned short)s->ui->w;
                full.height = (unsigned short)s->ui->h;
                ui_refresh(fbfd, &full, WFM_DU);
            }
            return 0;
        }
        if (rect_has(&s->prov_dn[i], x, y)) {
            flash_press(s, fbfd, &s->prov_dn[i]);
            (void)config_providers_move(s->work.show_providers,
                                        sizeof(s->work.show_providers),
                                        PROV_NAMES[i], +1);
            refresh_values(s);
            settings_providers_draw(s);
            {
                FBInkRect full;
                full.left = 0; full.top = 0;
                full.width = (unsigned short)s->ui->w;
                full.height = (unsigned short)s->ui->h;
                ui_refresh(fbfd, &full, WFM_DU);
            }
            return 0;
        }
        if (rect_has(&s->prov_row[i], x, y)) {
            flash_press(s, fbfd, &s->prov_row[i]);
            if (config_providers_toggle(s->work.show_providers,
                                        sizeof(s->work.show_providers),
                                        PROV_NAMES[i]) != 0) {
                snprintf(s->prov_warn, sizeof(s->prov_warn),
                         "need >=1 provider");
            } else {
                s->prov_warn[0] = '\0';
            }
            refresh_values(s);
            settings_providers_draw(s);
            {
                FBInkRect full;
                full.left = 0; full.top = 0;
                full.width = (unsigned short)s->ui->w;
                full.height = (unsigned short)s->ui->h;
                ui_refresh(fbfd, &full, WFM_DU);
            }
            return 0;
        }
    }
    return 0;
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
              s->work.auto_refresh_min != s->orig.auto_refresh_min ||
              s->work.light_idle_off_min != s->orig.light_idle_off_min ||
              s->work.display_mode_pref != s->orig.display_mode_pref ||
              s->work.refresh_preset != s->orig.refresh_preset ||
              strcmp(s->work.show_providers, s->orig.show_providers) != 0 ||
              s->work.config_version != s->orig.config_version;
    if (changed) {
        s->work.config_version = CONFIG_VERSION_CURRENT;
        *cfg = s->work;
    }
    return changed;
}
