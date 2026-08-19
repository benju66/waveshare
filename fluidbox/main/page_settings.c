#include "page_settings.h"

#include "display.h"
#include "esp_app_desc.h"
#include "settings.h"

#define BRIGHT_STEP 25
#define ROW_H 74
#define ROW_TOP 64

static lv_obj_t *s_screen;
static lv_obj_t *s_bright_value;
static lv_obj_t *s_idle_value;

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

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

static lv_obj_t *make_row_label(int row, const char *text)
{
    lv_obj_t *label = lv_label_create(s_screen);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_label_set_text(label, text);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 16, ROW_TOP + row * ROW_H);
    return label;
}

static void make_stepper(int row, lv_event_cb_t cb, lv_obj_t **value_label)
{
    const int y = ROW_TOP + row * ROW_H - 6;

    lv_obj_t *minus = lv_button_create(s_screen);
    lv_obj_set_size(minus, 48, 40);
    lv_obj_align(minus, LV_ALIGN_TOP_RIGHT, -128, y);
    lv_obj_add_event_cb(minus, cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    lv_obj_t *ml = lv_label_create(minus);
    lv_label_set_text(ml, "-");
    lv_obj_center(ml);

    *value_label = lv_label_create(s_screen);
    lv_obj_set_style_text_color(*value_label, lv_color_white(), 0);
    lv_obj_align(*value_label, LV_ALIGN_TOP_RIGHT, -66, y + 12);

    lv_obj_t *plus = lv_button_create(s_screen);
    lv_obj_set_size(plus, 48, 40);
    lv_obj_align(plus, LV_ALIGN_TOP_RIGHT, -8, y);
    lv_obj_add_event_cb(plus, cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    lv_obj_t *pl = lv_label_create(plus);
    lv_label_set_text(pl, "+");
    lv_obj_center(pl);
}

static void make_switch(int row, bool state, lv_event_cb_t cb)
{
    lv_obj_t *sw = lv_switch_create(s_screen);
    lv_obj_align(sw, LV_ALIGN_TOP_RIGHT, -8, ROW_TOP + row * ROW_H - 4);
    if (state) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
}

lv_obj_t *page_settings_create(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(s_screen);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_label_set_text(title, "settings");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);

    make_row_label(0, "brightness");
    make_stepper(0, on_bright, &s_bright_value);

    make_row_label(1, "idle after");
    make_stepper(1, on_idle, &s_idle_value);

    make_row_label(2, "fireworks");
    make_switch(2, settings_fireworks(), on_fireworks);

    make_row_label(3, "pocket guard");
    make_switch(3, settings_wake_guard(), on_guard);

    // Which firmware is this, without a serial cable.
    lv_obj_t *version = lv_label_create(s_screen);
    lv_obj_set_style_text_color(version, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_label_set_text_fmt(version, "%s", esp_app_get_description()->version);
    lv_obj_align(version, LV_ALIGN_BOTTOM_MID, 0, -12);

    refresh_values();
    return s_screen;
}
