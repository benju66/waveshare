#include "page_now.h"

#include <time.h>

#include "battery.h"
#include "config.h"
#include "esp_timer.h"
#include "net_task.h"
#include "page_pomodoro.h"

#define UI_REFRESH_MS 1000

static lv_obj_t *s_screen;
static lv_obj_t *s_clock_label;
static lv_obj_t *s_temp_label;
static lv_obj_t *s_cond_label;
static lv_obj_t *s_range_label;
static lv_obj_t *s_precip_label;
static lv_obj_t *s_stale_dot;
static lv_obj_t *s_stale_label;
static lv_obj_t *s_badge_label;
static lv_obj_t *s_battery_label;

// WMO weather interpretation codes, coarsened to what fits on one line.
static const char *weather_text(int code)
{
    if (code == 0) return "clear";
    if (code <= 2) return "partly cloudy";
    if (code == 3) return "overcast";
    if (code <= 48) return "fog";
    if (code <= 57) return "drizzle";
    if (code <= 67) return "rain";
    if (code <= 77) return "snow";
    if (code <= 82) return "showers";
    if (code <= 86) return "snow showers";
    return "thunderstorm";
}

static void refresh_ui(lv_timer_t *timer)
{
    (void)timer;

    // Clock. Before SNTP syncs, the epoch is 1970 and showing that helps
    // nobody; the em dash reads as "not yet".
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year > 100) {
        lv_label_set_text_fmt(s_clock_label, "%d:%02d", tm_now.tm_hour,
                              tm_now.tm_min);
    } else {
        lv_label_set_text(s_clock_label, "--:--");
    }

    weather_model_t wx;
    if (weather_get(&wx)) {
        lv_label_set_text_fmt(s_temp_label, "%d\xC2\xB0", (int)(wx.temp_c + 0.5f));
        lv_label_set_text(s_cond_label, weather_text(wx.weather_code));
        lv_label_set_text_fmt(s_range_label, "%d\xC2\xB0 / %d\xC2\xB0",
                              (int)(wx.today_hi_c + 0.5f),
                              (int)(wx.today_lo_c + 0.5f));
        lv_label_set_text_fmt(s_precip_label, "rain %d%%", wx.precip_prob_pct);

        const int age_min =
            (int)((esp_timer_get_time() - wx.fetched_us) / 60000000LL);
        const bool stale = age_min > WEATHER_STALE_MIN;
        if (stale) {
            lv_obj_remove_flag(s_stale_dot, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_stale_label, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text_fmt(s_stale_label, "%dm ago", age_min);
        } else {
            lv_obj_add_flag(s_stale_dot, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_stale_label, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
#if WEATHER_COORDS_SET
        lv_label_set_text(s_cond_label, "waiting for weather...");
#else
        lv_label_set_text(s_cond_label, "set coords in config.h");
#endif
    }

    char badge[8];
    if (page_pomodoro_badge_text(badge, sizeof(badge))) {
        lv_label_set_text(s_badge_label, badge);
        lv_obj_remove_flag(s_badge_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_badge_label, LV_OBJ_FLAG_HIDDEN);
    }

    const int batt = battery_percent();
    if (batt >= 0) {
        lv_label_set_text_fmt(s_battery_label, "%d%%", batt);
        lv_obj_set_style_text_color(s_battery_label,
                                    batt <= 20 ? lv_palette_main(LV_PALETTE_RED)
                                               : lv_palette_main(LV_PALETTE_GREY),
                                    0);
        lv_obj_remove_flag(s_battery_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_battery_label, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t *page_now_create(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    s_clock_label = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_clock_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_clock_label, lv_color_white(), 0);
    lv_label_set_text(s_clock_label, "--:--");
    lv_obj_align(s_clock_label, LV_ALIGN_TOP_MID, 0, 36);

    s_temp_label = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_temp_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_temp_label, lv_color_white(), 0);
    lv_label_set_text(s_temp_label, "--\xC2\xB0");
    lv_obj_align(s_temp_label, LV_ALIGN_CENTER, 0, -20);

    s_cond_label = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_cond_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_cond_label, lv_palette_lighten(LV_PALETTE_GREY, 2), 0);
    lv_label_set_text(s_cond_label, "waiting for weather...");
    lv_obj_align(s_cond_label, LV_ALIGN_CENTER, 0, 24);

    s_range_label = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_range_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_range_label, lv_color_white(), 0);
    lv_label_set_text(s_range_label, "");
    lv_obj_align(s_range_label, LV_ALIGN_CENTER, 0, 66);

    s_precip_label = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_precip_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_precip_label, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_label_set_text(s_precip_label, "");
    lv_obj_align(s_precip_label, LV_ALIGN_CENTER, 0, 100);

    // Grey dot plus age, only shown once the data is stale.
    s_stale_dot = lv_obj_create(s_screen);
    lv_obj_set_size(s_stale_dot, 10, 10);
    lv_obj_set_style_radius(s_stale_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_stale_dot, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_set_style_border_width(s_stale_dot, 0, 0);
    lv_obj_align(s_stale_dot, LV_ALIGN_BOTTOM_LEFT, 16, -16);
    lv_obj_add_flag(s_stale_dot, LV_OBJ_FLAG_HIDDEN);

    s_stale_label = lv_label_create(s_screen);
    lv_obj_set_style_text_color(s_stale_label, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_label_set_text(s_stale_label, "");
    lv_obj_align(s_stale_label, LV_ALIGN_BOTTOM_LEFT, 34, -13);
    lv_obj_add_flag(s_stale_label, LV_OBJ_FLAG_HIDDEN);

    s_badge_label = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_badge_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_badge_label, lv_palette_main(LV_PALETTE_RED), 0);
    lv_label_set_text(s_badge_label, "");
    lv_obj_align(s_badge_label, LV_ALIGN_TOP_RIGHT, -16, 14);
    lv_obj_add_flag(s_badge_label, LV_OBJ_FLAG_HIDDEN);

    s_battery_label = lv_label_create(s_screen);
    lv_obj_set_style_text_color(s_battery_label, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_label_set_text(s_battery_label, "");
    lv_obj_align(s_battery_label, LV_ALIGN_TOP_LEFT, 16, 16);
    lv_obj_add_flag(s_battery_label, LV_OBJ_FLAG_HIDDEN);

    lv_timer_create(refresh_ui, UI_REFRESH_MS, NULL);
    return s_screen;
}
