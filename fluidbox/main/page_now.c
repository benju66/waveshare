#include "page_now.h"

#include <math.h>
#include <stdatomic.h>
#include <time.h>

#include "battery.h"
#include "config.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "net_task.h"
#include "page_pomodoro.h"
#include "ui_theme.h"

// Fast enough that a view swipe feels immediate.
#define UI_REFRESH_MS 250

#define VIEW_WEATHER 0
#define VIEW_SUN 1
#define VIEW_MOON 2
#define VIEW_COUNT 3

#define MOON_SIZE 160
#define MOON_R 74
#define SYNODIC_DAYS 29.530588853
// A known new moon: 2000-01-06 18:14 UTC.
#define NEW_MOON_EPOCH 947182440L

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

// Solar arc view.
static lv_obj_t *s_sun_title;
static lv_obj_t *s_sun_arc;
static lv_obj_t *s_rise_label;
static lv_obj_t *s_set_label;

// Moon view.
static lv_obj_t *s_moon_canvas;
static uint16_t *s_moon_buf;
static lv_obj_t *s_moon_name;
static lv_obj_t *s_moon_pct;
static int s_moon_drawn_day = -1;

static atomic_int s_view = VIEW_WEATHER;
static atomic_int s_view_pending;  // +-1 deltas from the touch task
static bool s_detail;              // weather card tapped: alternate fields

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

void page_now_cycle(int dir)
{
    atomic_fetch_add(&s_view_pending, dir);
}

// Tap on the weather card flips between the headline numbers and the
// feels-like / humidity / wind card. Other views ignore taps.
static void on_tap(lv_event_t *e)
{
    (void)e;
    if (atomic_load(&s_view) == VIEW_WEATHER) {
        s_detail = !s_detail;
    }
}

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void apply_view(int view)
{
    const bool wx = view != VIEW_WEATHER;
    set_hidden(s_temp_label, wx);
    set_hidden(s_cond_label, wx);
    set_hidden(s_range_label, wx);
    set_hidden(s_precip_label, wx);
    const bool sun = view != VIEW_SUN;
    set_hidden(s_sun_title, sun);
    set_hidden(s_sun_arc, sun);
    set_hidden(s_rise_label, sun);
    set_hidden(s_set_label, sun);
    const bool moon = view != VIEW_MOON;
    set_hidden(s_moon_canvas, moon);
    set_hidden(s_moon_name, moon);
    set_hidden(s_moon_pct, moon);
}

// Phase fraction 0..1 (0 = new, 0.5 = full) from the synodic month.
static float moon_phase(time_t now)
{
    double days = (double)(now - NEW_MOON_EPOCH) / 86400.0;
    double p = fmod(days, SYNODIC_DAYS) / SYNODIC_DAYS;
    if (p < 0) p += 1.0;
    return (float)p;
}

static void draw_moon(float p)
{
    const uint16_t lit = lv_color_to_u16(lv_color_make(225, 225, 210));
    const uint16_t dark = lv_color_to_u16(lv_color_make(22, 24, 30));
    const uint16_t bg = lv_color_to_u16(lv_color_black());
    const int c = MOON_SIZE / 2;
    const float ct = cosf(2.0f * (float)M_PI * p);
    const bool waxing = p < 0.5f;

    for (int y = 0; y < MOON_SIZE; y++) {
        uint16_t *row = s_moon_buf + y * MOON_SIZE;
        const int dy = y - c;
        const float w2 = (float)(MOON_R * MOON_R - dy * dy);
        const float w = w2 > 0 ? sqrtf(w2) : -1.0f;
        for (int x = 0; x < MOON_SIZE; x++) {
            const float dx = (float)(x - c);
            if (w < 0 || dx < -w || dx > w) {
                row[x] = bg;
                continue;
            }
            // Terminator: waxing lights the right of w*cos, waning the left.
            const float t = w * ct;
            const bool is_lit = waxing ? dx >= t : dx <= -t;
            row[x] = is_lit ? lit : dark;
        }
    }
    lv_obj_invalidate(s_moon_canvas);
}

static void refresh_moon_view(void)
{
    const time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (tm_now.tm_yday == s_moon_drawn_day) {
        return;
    }
    s_moon_drawn_day = tm_now.tm_yday;

    const float p = moon_phase(now);
    const float illum = (1.0f - cosf(2.0f * (float)M_PI * p)) / 2.0f;
    static const char *names[8] = {"new moon",        "waxing crescent",
                                   "first quarter",   "waxing gibbous",
                                   "full moon",       "waning gibbous",
                                   "last quarter",    "waning crescent"};
    lv_label_set_text(s_moon_name, names[(int)(p * 8.0f + 0.5f) % 8]);
    lv_label_set_text_fmt(s_moon_pct, "%d%% lit", (int)(illum * 100.0f + 0.5f));
    draw_moon(p);
}

static void refresh_sun_view(const weather_model_t *wx, bool have_wx)
{
    const time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    const int now_min = tm_now.tm_hour * 60 + tm_now.tm_min;

    if (!have_wx || wx->sunrise_min < 0 || wx->sunset_min <= wx->sunrise_min) {
        lv_label_set_text(s_sun_title, "waiting for sun data...");
        return;
    }

    char buf[20];
    int h12 = (wx->sunrise_min / 60) % 12;
    if (h12 == 0) h12 = 12;
    snprintf(buf, sizeof(buf), "%d:%02d am", h12, wx->sunrise_min % 60);
    lv_label_set_text(s_rise_label, buf);
    h12 = (wx->sunset_min / 60) % 12;
    if (h12 == 0) h12 = 12;
    snprintf(buf, sizeof(buf), "%d:%02d pm", h12, wx->sunset_min % 60);
    lv_label_set_text(s_set_label, buf);

    if (now_min < wx->sunrise_min) {
        lv_arc_set_value(s_sun_arc, 0);
        lv_label_set_text(s_sun_title, "before sunrise");
    } else if (now_min > wx->sunset_min) {
        lv_arc_set_value(s_sun_arc, 1000);
        lv_label_set_text(s_sun_title, "after sunset");
    } else {
        const int span = wx->sunset_min - wx->sunrise_min;
        lv_arc_set_value(s_sun_arc, (now_min - wx->sunrise_min) * 1000 / span);
        const int left = wx->sunset_min - now_min;
        lv_label_set_text_fmt(s_sun_title, "%dh %02dm of daylight left",
                              left / 60, left % 60);
    }
}

static void refresh_ui(lv_timer_t *timer)
{
    (void)timer;

    const int delta = atomic_exchange(&s_view_pending, 0);
    int view = atomic_load(&s_view);
    if (delta != 0) {
        view = ((view + delta) % VIEW_COUNT + VIEW_COUNT) % VIEW_COUNT;
        atomic_store(&s_view, view);
        s_detail = false;
        apply_view(view);
    }

    // Clock. Before SNTP syncs, the epoch is 1970 and showing that helps
    // nobody; the em dash reads as "not yet".
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year > 100) {
        int hour12 = tm_now.tm_hour % 12;
        if (hour12 == 0) hour12 = 12;
        lv_label_set_text_fmt(s_clock_label, "%d:%02d %s", hour12, tm_now.tm_min,
                              tm_now.tm_hour < 12 ? "am" : "pm");
    } else {
        lv_label_set_text(s_clock_label, "--:--");
    }

    weather_model_t wx;
    const bool have_wx = weather_get(&wx);

    if (view == VIEW_SUN) {
        refresh_sun_view(&wx, have_wx);
    } else if (view == VIEW_MOON) {
        refresh_moon_view();
    }

    if (view == VIEW_WEATHER && have_wx) {
        lv_label_set_text_fmt(s_temp_label, "%d\xC2\xB0", (int)(wx.temp + 0.5f));
        if (s_detail) {
            // Tapped: the alternate card.
            lv_label_set_text_fmt(s_cond_label, "feels like %d\xC2\xB0",
                                  (int)(wx.feels_like + 0.5f));
            lv_label_set_text_fmt(s_range_label, "humidity %d%%", wx.humidity_pct);
            lv_label_set_text_fmt(s_precip_label, "wind %d mph",
                                  (int)(wx.wind_mph + 0.5f));
        } else {
            lv_label_set_text(s_cond_label, weather_text(wx.weather_code));
            lv_label_set_text_fmt(s_range_label, "%d\xC2\xB0 / %d\xC2\xB0",
                                  (int)(wx.today_hi + 0.5f),
                                  (int)(wx.today_lo + 0.5f));
            lv_label_set_text_fmt(s_precip_label, "rain %d%%", wx.precip_prob_pct);
        }

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
    } else if (view == VIEW_WEATHER) {
#if WEATHER_COORDS_SET
        lv_label_set_text(s_cond_label, "waiting for weather...");
#else
        lv_label_set_text(s_cond_label, "set coords in config.h");
#endif
        lv_obj_add_flag(s_stale_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_stale_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        // Off the weather view, the stale marker has no business showing.
        lv_obj_add_flag(s_stale_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_stale_label, LV_OBJ_FLAG_HIDDEN);
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
    theme_apply_big(s_clock_label);
    lv_obj_set_style_text_color(s_clock_label, lv_color_white(), 0);
    lv_label_set_text(s_clock_label, "--:--");
    lv_obj_align(s_clock_label, LV_ALIGN_TOP_MID, 0, 36);

    s_temp_label = lv_label_create(s_screen);
    theme_apply_big(s_temp_label);
    lv_obj_set_style_text_color(s_temp_label, lv_color_white(), 0);
    lv_label_set_text(s_temp_label, "--\xC2\xB0");
    lv_obj_align(s_temp_label, LV_ALIGN_CENTER, 0, -20);

    s_cond_label = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_cond_label, theme_font_mid(), 0);
    // White, not grey: the grey read as unlit AMOLED pixels at arm's length.
    lv_obj_set_style_text_color(s_cond_label, lv_color_white(), 0);
    lv_label_set_text(s_cond_label, "waiting for weather...");
    lv_obj_align(s_cond_label, LV_ALIGN_CENTER, 0, 24);

    s_range_label = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_range_label, theme_font_mid(), 0);
    lv_obj_set_style_text_color(s_range_label, lv_color_white(), 0);
    lv_label_set_text(s_range_label, "");
    lv_obj_align(s_range_label, LV_ALIGN_CENTER, 0, 66);

    s_precip_label = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_precip_label, theme_font_mid(), 0);
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
    lv_obj_set_style_text_font(s_badge_label, theme_font_mid(), 0);
    lv_obj_set_style_text_color(s_badge_label, lv_palette_main(LV_PALETTE_RED), 0);
    lv_label_set_text(s_badge_label, "");
    lv_obj_align(s_badge_label, LV_ALIGN_TOP_RIGHT, -16, 14);
    lv_obj_add_flag(s_badge_label, LV_OBJ_FLAG_HIDDEN);

    s_battery_label = lv_label_create(s_screen);
    lv_obj_set_style_text_color(s_battery_label, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_label_set_text(s_battery_label, "");
    lv_obj_align(s_battery_label, LV_ALIGN_TOP_LEFT, 16, 16);
    lv_obj_add_flag(s_battery_label, LV_OBJ_FLAG_HIDDEN);

    // --- Solar arc view ---
    s_sun_title = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_sun_title, theme_font_mid(), 0);
    lv_obj_set_style_text_color(s_sun_title, lv_color_white(), 0);
    lv_label_set_text(s_sun_title, "");
    lv_obj_align(s_sun_title, LV_ALIGN_CENTER, 0, 96);

    s_sun_arc = lv_arc_create(s_screen);
    lv_obj_set_size(s_sun_arc, 300, 300);
    lv_obj_align(s_sun_arc, LV_ALIGN_CENTER, 0, 40);
    lv_arc_set_rotation(s_sun_arc, 180);
    lv_arc_set_bg_angles(s_sun_arc, 0, 180);
    lv_arc_set_range(s_sun_arc, 0, 1000);
    lv_arc_set_value(s_sun_arc, 0);
    lv_obj_remove_flag(s_sun_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_sun_arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_sun_arc, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_sun_arc, lv_color_hex(0x1a1a2a), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_sun_arc, lv_palette_main(LV_PALETTE_AMBER),
                               LV_PART_INDICATOR);
    // The knob is the sun.
    lv_obj_set_style_bg_color(s_sun_arc, lv_palette_main(LV_PALETTE_YELLOW),
                              LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_sun_arc, 6, LV_PART_KNOB);

    s_rise_label = lv_label_create(s_screen);
    lv_obj_set_style_text_color(s_rise_label, lv_palette_main(LV_PALETTE_AMBER), 0);
    lv_label_set_text(s_rise_label, "");
    lv_obj_align(s_rise_label, LV_ALIGN_CENTER, -110, 60);

    s_set_label = lv_label_create(s_screen);
    lv_obj_set_style_text_color(s_set_label, lv_palette_main(LV_PALETTE_AMBER), 0);
    lv_label_set_text(s_set_label, "");
    lv_obj_align(s_set_label, LV_ALIGN_CENTER, 110, 60);

    // --- Moon view ---
    s_moon_buf = heap_caps_malloc((size_t)MOON_SIZE * MOON_SIZE * 2,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_moon_canvas = lv_canvas_create(s_screen);
    if (s_moon_buf != NULL) {
        lv_canvas_set_buffer(s_moon_canvas, s_moon_buf, MOON_SIZE, MOON_SIZE,
                             LV_COLOR_FORMAT_RGB565);
    }
    lv_obj_align(s_moon_canvas, LV_ALIGN_CENTER, 0, -20);

    s_moon_name = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_moon_name, theme_font_mid(), 0);
    lv_obj_set_style_text_color(s_moon_name, lv_color_white(), 0);
    lv_label_set_text(s_moon_name, "");
    lv_obj_align(s_moon_name, LV_ALIGN_CENTER, 0, 96);

    s_moon_pct = lv_label_create(s_screen);
    lv_obj_set_style_text_color(s_moon_pct, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_label_set_text(s_moon_pct, "");
    lv_obj_align(s_moon_pct, LV_ALIGN_CENTER, 0, 126);

    apply_view(VIEW_WEATHER);

    lv_timer_create(refresh_ui, UI_REFRESH_MS, NULL);
    lv_obj_add_event_cb(s_screen, on_tap, LV_EVENT_SHORT_CLICKED, NULL);
    return s_screen;
}
