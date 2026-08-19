#include "page_settings.h"

#include <stdatomic.h>

#include "config.h"
#include "display.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "settings.h"
#include "ui_theme.h"

// Two vertically-swiped views so every control gets a thumb-sized target on
// this 1.8" panel: view 0 = the steppers, view 1 = the switches.

#define BRIGHT_STEP 25
#define BTN_W 72
#define BTN_H 60

static lv_obj_t *s_screen;
static lv_obj_t *s_view0;  // brightness + idle steppers
static lv_obj_t *s_view1;  // switches + version
static lv_obj_t *s_bright_value;
static lv_obj_t *s_idle_value;
static atomic_int s_view_pending;
static int s_view;

// ---------------------------------------------------------------------------
// Callbacks (LVGL task)
// ---------------------------------------------------------------------------

static void refresh_values(void)
{
    lv_label_set_text_fmt(s_bright_value, "%d%%", settings_brightness() * 100 / 255);
    lv_label_set_text_fmt(s_idle_value, "%d min", settings_idle_min());
}

static void on_bright(lv_event_t *e)
{
    const int delta = (int)(intptr_t)lv_event_get_user_data(e) * BRIGHT_STEP;
    settings_set_brightness(settings_brightness() + delta);
    display_set_brightness(settings_brightness());  // apply live
    refresh_values();
}

static void on_idle(lv_event_t *e)
{
    const int delta = (int)(intptr_t)lv_event_get_user_data(e);
    settings_set_idle_min(settings_idle_min() + delta);
    refresh_values();
}

static void on_fireworks(lv_event_t *e)
{
    settings_set_fireworks(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void on_guard(lv_event_t *e)
{
    settings_set_wake_guard(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void restart_cb(lv_timer_t *t)
{
    (void)t;
    esp_restart();
}

static void on_retro(lv_event_t *e)
{
    settings_set_retro(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
    // Fonts are baked in at widget creation; a quick reboot re-dresses
    // everything. The pause lets the switch animate and NVS settle.
    lv_timer_set_repeat_count(lv_timer_create(restart_cb, 700, NULL), 1);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

static lv_obj_t *make_view(void)
{
    lv_obj_t *v = lv_obj_create(s_screen);
    lv_obj_set_size(v, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(v, 0, 0);
    lv_obj_set_style_bg_opa(v, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(v, 0, 0);
    lv_obj_set_style_pad_all(v, 0, 0);
    lv_obj_remove_flag(v, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return v;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, theme_font_mid(), 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_label_set_text(label, text);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    return label;
}

// A big stepper block: name, huge value, fat - and + buttons flanking it.
static void make_stepper(lv_obj_t *parent, const char *name, int y,
                         lv_event_cb_t cb, lv_obj_t **value_label)
{
    make_label(parent, name, y);

    lv_obj_t *minus = lv_button_create(parent);
    lv_obj_set_size(minus, BTN_W, BTN_H);
    lv_obj_set_style_radius(minus, theme_radius(10), 0);
    lv_obj_align(minus, LV_ALIGN_TOP_LEFT, 24, y + 34);
    lv_obj_add_event_cb(minus, cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    lv_obj_t *ml = lv_label_create(minus);
    lv_obj_set_style_text_font(ml, theme_font_mid(), 0);
    lv_label_set_text(ml, "-");
    lv_obj_center(ml);

    *value_label = lv_label_create(parent);
    lv_obj_set_style_text_font(*value_label, theme_font_mid(), 0);
    lv_obj_set_style_text_color(*value_label, lv_color_white(), 0);
    lv_obj_align(*value_label, LV_ALIGN_TOP_MID, 0, y + 50);

    lv_obj_t *plus = lv_button_create(parent);
    lv_obj_set_size(plus, BTN_W, BTN_H);
    lv_obj_set_style_radius(plus, theme_radius(10), 0);
    lv_obj_align(plus, LV_ALIGN_TOP_RIGHT, -24, y + 34);
    lv_obj_add_event_cb(plus, cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    lv_obj_t *pl = lv_label_create(plus);
    lv_obj_set_style_text_font(pl, theme_font_mid(), 0);
    lv_label_set_text(pl, "+");
    lv_obj_center(pl);
}

static void make_switch_row(lv_obj_t *parent, const char *name, int y,
                            bool state, lv_event_cb_t cb)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, theme_font_mid(), 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_label_set_text(label, name);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 24, y + 12);

    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_size(sw, 88, 44);  // thumb-sized
    lv_obj_set_style_radius(sw, theme_radius(22), 0);
    lv_obj_set_style_radius(sw, theme_radius(22), LV_PART_KNOB);
    lv_obj_align(sw, LV_ALIGN_TOP_RIGHT, -20, y);
    if (state) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
}

void page_settings_cycle(int dir)
{
    atomic_fetch_add(&s_view_pending, dir);
}

static void poll_view(lv_timer_t *t)
{
    (void)t;
    const int delta = atomic_exchange(&s_view_pending, 0);
    if (delta == 0) {
        return;
    }
    s_view = ((s_view + delta) % 2 + 2) % 2;
    if (s_view == 0) {
        lv_obj_remove_flag(s_view0, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_view1, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_view0, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_view1, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t *page_settings_create(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(s_screen);
    lv_obj_set_style_text_font(title, theme_font_small(), 0);
    lv_obj_set_style_text_color(title, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_label_set_text(title, "settings  (swipe up for more)");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    s_view0 = make_view();
    make_stepper(s_view0, "brightness", 56, on_bright, &s_bright_value);
    make_stepper(s_view0, "idle after", 236, on_idle, &s_idle_value);

    s_view1 = make_view();
    make_switch_row(s_view1, "fireworks", 56, settings_fireworks(), on_fireworks);
    make_switch_row(s_view1, "pocket guard", 156, settings_wake_guard(), on_guard);
    make_switch_row(s_view1, "retro pixels", 256, settings_retro(), on_retro);

    lv_obj_t *version = lv_label_create(s_view1);
    lv_obj_set_style_text_font(version, theme_font_small(), 0);
    lv_obj_set_style_text_color(version, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_label_set_text_fmt(version, "%s", esp_app_get_description()->version);
    lv_obj_align(version, LV_ALIGN_BOTTOM_MID, 0, -12);

    lv_obj_add_flag(s_view1, LV_OBJ_FLAG_HIDDEN);
    lv_timer_create(poll_view, 150, NULL);
    refresh_values();
    return s_screen;
}