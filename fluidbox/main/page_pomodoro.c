#include "page_pomodoro.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "page_manager.h"
#include "settings.h"

#define UI_REFRESH_MS 200

// How long the background flashes after a phase completes on its own.
#define ALERT_FLASH_MS 1600

// Arc resolution. High enough that the ring moves visibly every refresh even
// on the 25 minute session.
#define ARC_MAX 1000

static const char *TAG = "pomodoro";

typedef enum {
    POMO_IDLE,
    POMO_WORK,
    POMO_SHORT_BREAK,
    POMO_LONG_BREAK,
} pomo_phase_t;

// The clock of record. esp_timer timestamps, not LVGL ticks, so a sleeping or
// stalled UI cannot stretch a session. Mutated from the LVGL task (touch
// handlers) and the esp_timer task (phase deadline); a mutex rather than a
// critical section because the phase timer is re-armed while holding it.
static SemaphoreHandle_t s_lock;
static struct {
    pomo_phase_t phase;
    bool running;           // false while idle or paused
    int64_t deadline_us;    // end of phase, valid while running
    int64_t remaining_us;   // frozen remainder, valid while paused
    int works_done;         // completed work sessions this cycle, 0..4
    int work_min;           // adjustable via vertical swipe while idle
    bool flip_paused;       // paused by lying face down, resumes on pickup
} s_st = {.phase = POMO_IDLE, .work_min = POMO_WORK_MIN};

static esp_timer_handle_t s_phase_timer;

// Set by the phase timer, consumed by the next refresh_ui on the LVGL task;
// the page manager meanwhile wakes the screen and brings this page forward.
static atomic_bool s_alert_pending;

static lv_obj_t *s_screen;
static lv_obj_t *s_arc;
static lv_obj_t *s_time_label;
static lv_obj_t *s_phase_label;
static lv_obj_t *s_dots[POMO_SESSIONS_PER_CYCLE];

static int64_t phase_duration_us(pomo_phase_t phase)
{
    switch (phase) {
    case POMO_SHORT_BREAK: return (int64_t)POMO_SHORT_BREAK_MIN * 60 * 1000000;
    case POMO_LONG_BREAK: return (int64_t)POMO_LONG_BREAK_MIN * 60 * 1000000;
    default: return (int64_t)s_st.work_min * 60 * 1000000;  // WORK; idle previews it
    }
}

// ---------------------------------------------------------------------------
// State machine. Everything below runs with s_lock held; no lv_* calls here,
// the phase timer fires on the esp_timer task.
// ---------------------------------------------------------------------------

static void arm_phase_timer_locked(void)
{
    esp_timer_stop(s_phase_timer);  // harmless when not armed
    if (s_st.running) {
        const int64_t in = s_st.deadline_us - esp_timer_get_time();
        esp_timer_start_once(s_phase_timer, in > 0 ? in : 1);
    }
}

static void enter_phase_locked(pomo_phase_t phase)
{
    s_st.phase = phase;
    s_st.deadline_us = esp_timer_get_time() + phase_duration_us(phase);
    s_st.running = true;
    arm_phase_timer_locked();
}

static void on_phase_deadline(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_st.running) {
        xSemaphoreGive(s_lock);
        return;
    }

    pomo_phase_t next;
    if (s_st.phase == POMO_WORK) {
        s_st.works_done++;
        next = (s_st.works_done % POMO_SESSIONS_PER_CYCLE == 0) ? POMO_LONG_BREAK
                                                                : POMO_SHORT_BREAK;
    } else {
        if (s_st.works_done >= POMO_SESSIONS_PER_CYCLE) {
            s_st.works_done = 0;  // long break over: fresh cycle
        }
        next = POMO_WORK;
    }
    // Advance to the next phase but do NOT start its countdown: the alert
    // announces the change and the user's tap begins it. A timer that rolls
    // on unattended punishes stepping away for thirty seconds.
    s_st.phase = next;
    s_st.running = false;
    s_st.flip_paused = false;
    s_st.remaining_us = phase_duration_us(next);
    arm_phase_timer_locked();  // stops the timer, since nothing is running
    const int works = s_st.works_done;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "phase -> %d (%d works done)", next, works);

    // A phase that ends on its own should be seen: flash here, and have the
    // page manager wake the panel and bring this page to the front.
    atomic_store(&s_alert_pending, true);
    page_manager_post_event(PAGE_EVT_ALERT_TIMER);
}

static void toggle_running(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.flip_paused = false;  // an explicit tap always outranks the flip
    if (s_st.phase == POMO_IDLE) {
        ESP_LOGI(TAG, "started");
        enter_phase_locked(POMO_WORK);
    } else if (s_st.running) {
        s_st.remaining_us = s_st.deadline_us - esp_timer_get_time();
        if (s_st.remaining_us < 0) s_st.remaining_us = 0;
        s_st.running = false;
        arm_phase_timer_locked();
        ESP_LOGI(TAG, "paused with %lld s left", s_st.remaining_us / 1000000);
    } else {
        s_st.deadline_us = esp_timer_get_time() + s_st.remaining_us;
        s_st.running = true;
        arm_phase_timer_locked();
        ESP_LOGI(TAG, "resumed");
    }
    xSemaphoreGive(s_lock);
}

static void reset_to_idle(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.phase = POMO_IDLE;
    s_st.running = false;
    s_st.works_done = 0;
    s_st.flip_paused = false;
    arm_phase_timer_locked();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "reset");
}

void page_pomodoro_adjust(int delta_min)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_st.phase == POMO_IDLE) {
        int m = s_st.work_min + delta_min;
        if (m < POMO_WORK_MIN_FLOOR) m = POMO_WORK_MIN_FLOOR;
        if (m > POMO_WORK_MIN_CEIL) m = POMO_WORK_MIN_CEIL;
        s_st.work_min = m;
        ESP_LOGI(TAG, "work duration -> %d min", m);
    }
    xSemaphoreGive(s_lock);
}

void page_pomodoro_set_face_down(bool face_down)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (face_down && s_st.running && s_st.phase != POMO_IDLE) {
        s_st.remaining_us = s_st.deadline_us - esp_timer_get_time();
        if (s_st.remaining_us < 0) s_st.remaining_us = 0;
        s_st.running = false;
        s_st.flip_paused = true;
        arm_phase_timer_locked();
        ESP_LOGI(TAG, "face down, paused");
    } else if (!face_down && s_st.flip_paused) {
        s_st.deadline_us = esp_timer_get_time() + s_st.remaining_us;
        s_st.running = true;
        s_st.flip_paused = false;
        arm_phase_timer_locked();
        ESP_LOGI(TAG, "picked up, resumed");
    }
    xSemaphoreGive(s_lock);
}

// Coherent snapshot for rendering.
static void snapshot(pomo_phase_t *phase, bool *running, int64_t *remaining_us,
                     int64_t *total_us, int *works_done)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *phase = s_st.phase;
    *running = s_st.running;
    *works_done = s_st.works_done;
    *total_us = phase_duration_us(s_st.phase);
    if (s_st.phase == POMO_IDLE) {
        *remaining_us = *total_us;
    } else if (s_st.running) {
        *remaining_us = s_st.deadline_us - esp_timer_get_time();
    } else {
        *remaining_us = s_st.remaining_us;
    }
    xSemaphoreGive(s_lock);
    if (*remaining_us < 0) *remaining_us = 0;
}

// ---------------------------------------------------------------------------
// Rendering, LVGL task only
// ---------------------------------------------------------------------------

static lv_color_t phase_color(pomo_phase_t phase, bool running)
{
    if (phase == POMO_IDLE || !running) {
        return lv_palette_main(LV_PALETTE_GREY);
    }
    return phase == POMO_WORK ? lv_palette_main(LV_PALETTE_RED)
                              : lv_palette_main(LV_PALETTE_GREEN);
}

// ---------------------------------------------------------------------------
// Fireworks: dots radiating from the center when a phase completes. Plain
// lv_anim position tweens; each dot fades and deletes itself.
// ---------------------------------------------------------------------------

#define FW_WAVES 3
#define FW_DOTS_PER_WAVE 14
#define FW_MS 1300
#define FW_WAVE_GAP_MS 260
#define FW_RADIUS_MIN 80
#define FW_RADIUS_MAX 185

static void fw_set_x(void *obj, int32_t v) { lv_obj_set_x(obj, v); }
static void fw_set_y(void *obj, int32_t v) { lv_obj_set_y(obj, v); }

static void fw_done(lv_anim_t *a)
{
    lv_obj_delete((lv_obj_t *)a->var);
}

// Three staggered waves of dots with jittered angles, radii and sizes, each
// arcing outward with a touch of downward drift so it reads as sparks
// falling rather than a tidy expanding ring.
static void fireworks_start(void)
{
    static const lv_palette_t colors[] = {LV_PALETTE_RED, LV_PALETTE_AMBER,
                                          LV_PALETTE_GREEN, LV_PALETTE_LIGHT_BLUE,
                                          LV_PALETTE_PURPLE, LV_PALETTE_PINK};

    for (int wave = 0; wave < FW_WAVES; wave++) {
        const uint32_t delay = wave * FW_WAVE_GAP_MS;
        // Later waves start slightly off-center, like secondary shells.
        const int cx = LCD_H_RES / 2 + (rand() % 61 - 30) * wave;
        const int cy = LCD_V_RES / 2 + (rand() % 41 - 20) * wave;

        for (int i = 0; i < FW_DOTS_PER_WAVE; i++) {
            const int size = 5 + rand() % 6;
            lv_obj_t *dot = lv_obj_create(s_screen);
            lv_obj_set_size(dot, size, size);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(
                dot, lv_palette_main(colors[rand() % (sizeof(colors) / sizeof(colors[0]))]),
                0);
            lv_obj_set_style_border_width(dot, 0, 0);
            lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_pos(dot, cx, cy);
            // Born transparent; the delayed fade-in reveals it at wave start.
            lv_obj_set_style_opa(dot, LV_OPA_TRANSP, 0);

            const float jitter = ((float)(rand() % 100) / 100.0f - 0.5f) * 0.4f;
            const float angle =
                (float)i * (2.0f * (float)M_PI / FW_DOTS_PER_WAVE) + jitter;
            const int r = FW_RADIUS_MIN + rand() % (FW_RADIUS_MAX - FW_RADIUS_MIN);

            lv_anim_t ax;
            lv_anim_init(&ax);
            lv_anim_set_var(&ax, dot);
            lv_anim_set_exec_cb(&ax, fw_set_x);
            lv_anim_set_values(&ax, cx, cx + (int32_t)(r * cosf(angle)));
            lv_anim_set_duration(&ax, FW_MS);
            lv_anim_set_delay(&ax, delay);
            lv_anim_set_path_cb(&ax, lv_anim_path_ease_out);
            lv_anim_start(&ax);

            lv_anim_t ay;
            lv_anim_init(&ay);
            lv_anim_set_var(&ay, dot);
            lv_anim_set_exec_cb(&ay, fw_set_y);
            // +25 px of sag at the end of the arc: gravity, cheaply.
            lv_anim_set_values(&ay, cy, cy + (int32_t)(r * sinf(angle)) + 25);
            lv_anim_set_duration(&ay, FW_MS);
            lv_anim_set_delay(&ay, delay);
            lv_anim_set_path_cb(&ay, lv_anim_path_ease_out);
            lv_anim_set_completed_cb(&ay, fw_done);  // fires after delay + duration
            lv_anim_start(&ay);

            // Reveal at wave start, burn bright, fade over the back half.
            lv_obj_fade_in(dot, 60, delay);
            lv_obj_fade_out(dot, FW_MS / 2, delay + FW_MS / 2);
        }
    }
}

static void refresh_ui(lv_timer_t *timer)
{
    (void)timer;

    pomo_phase_t phase;
    bool running;
    int64_t remaining_us, total_us;
    int works_done;
    snapshot(&phase, &running, &remaining_us, &total_us, &works_done);

    const int remaining_s = (int)((remaining_us + 999999) / 1000000);
    lv_label_set_text_fmt(s_time_label, "%02d:%02d", remaining_s / 60,
                          remaining_s % 60);

    const char *phase_text = "tap to start";
    if (phase != POMO_IDLE) {
        const bool fresh = remaining_us == total_us;  // ended phase, unstarted next
        if (phase == POMO_WORK) {
            phase_text = running ? "work" : (fresh ? "tap to start work" : "work paused");
        } else {
            phase_text = running ? "break" : (fresh ? "tap to start break" : "break paused");
        }
    }
    lv_label_set_text(s_phase_label, phase_text);

    lv_arc_set_value(s_arc, (int32_t)(remaining_us * ARC_MAX / total_us));
    lv_obj_set_style_arc_color(s_arc, phase_color(phase, running),
                               LV_PART_INDICATOR);

    for (int i = 0; i < POMO_SESSIONS_PER_CYCLE; i++) {
        const bool filled = i < works_done;
        lv_obj_set_style_bg_opa(s_dots[i], filled ? LV_OPA_COVER : LV_OPA_20, 0);
    }

    // Alert flash: alternate the background between black and a dark tint of
    // the new phase's color for a moment. Runs on the refresh cadence, so no
    // animation machinery and nothing to cancel.
    static int64_t s_flash_until_us;
    static bool s_flash_on;
    const int64_t now = esp_timer_get_time();
    if (atomic_exchange(&s_alert_pending, false)) {
        s_flash_until_us = now + (int64_t)ALERT_FLASH_MS * 1000;
        if (settings_fireworks()) {
            fireworks_start();
        }
    }
    if (now < s_flash_until_us) {
        s_flash_on = !s_flash_on;
        lv_obj_set_style_bg_color(s_screen,
                                  s_flash_on
                                      ? lv_color_darken(phase_color(phase, running), 160)
                                      : lv_color_black(),
                                  0);
    } else if (s_flash_on) {
        s_flash_on = false;
        lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
    }
}

static void on_short_click(lv_event_t *e)
{
    toggle_running();
    lv_timer_ready(lv_event_get_user_data(e));  // repaint now, not in 200 ms
}

static void on_long_press(lv_event_t *e)
{
    reset_to_idle();
    lv_timer_ready(lv_event_get_user_data(e));
}

lv_obj_t *page_pomodoro_create(void)
{
    s_lock = xSemaphoreCreateMutex();
    const esp_timer_create_args_t targs = {
        .callback = on_phase_deadline,
        .name = "pomo_phase",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_phase_timer));

    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    s_arc = lv_arc_create(s_screen);
    lv_obj_set_size(s_arc, 300, 300);
    lv_obj_center(s_arc);
    lv_arc_set_rotation(s_arc, 270);
    lv_arc_set_bg_angles(s_arc, 0, 360);
    lv_arc_set_range(s_arc, 0, ARC_MAX);
    lv_arc_set_value(s_arc, ARC_MAX);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_arc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x202020), LV_PART_MAIN);

    s_time_label = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_time_label, lv_color_white(), 0);
    lv_label_set_text(s_time_label, "25:00");
    lv_obj_center(s_time_label);

    s_phase_label = lv_label_create(s_screen);
    lv_obj_set_style_text_color(s_phase_label, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_label_set_text(s_phase_label, "tap to start");
    lv_obj_align(s_phase_label, LV_ALIGN_CENTER, 0, 44);

    for (int i = 0; i < POMO_SESSIONS_PER_CYCLE; i++) {
        s_dots[i] = lv_obj_create(s_screen);
        lv_obj_set_size(s_dots[i], 12, 12);
        lv_obj_set_style_radius(s_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_dots[i], lv_palette_main(LV_PALETTE_RED), 0);
        lv_obj_set_style_bg_opa(s_dots[i], LV_OPA_20, 0);
        lv_obj_set_style_border_width(s_dots[i], 0, 0);
        lv_obj_remove_flag(s_dots[i], LV_OBJ_FLAG_SCROLLABLE);
        // Children eat clicks by default; a tap on a dot must still reach the
        // screen's start/pause handler.
        lv_obj_remove_flag(s_dots[i], LV_OBJ_FLAG_CLICKABLE);
        const int spread = 24;
        lv_obj_align(s_dots[i], LV_ALIGN_CENTER,
                     (int32_t)((i - (POMO_SESSIONS_PER_CYCLE - 1) / 2.0f) * spread),
                     90);
    }

    lv_timer_t *timer = lv_timer_create(refresh_ui, UI_REFRESH_MS, NULL);
    lv_obj_add_event_cb(s_screen, on_short_click, LV_EVENT_SHORT_CLICKED, timer);
    lv_obj_add_event_cb(s_screen, on_long_press, LV_EVENT_LONG_PRESSED, timer);

    return s_screen;
}

bool page_pomodoro_badge_text(char *buf, size_t len)
{
    if (s_lock == NULL) {
        return false;
    }

    pomo_phase_t phase;
    bool running;
    int64_t remaining_us, total_us;
    int works_done;
    snapshot(&phase, &running, &remaining_us, &total_us, &works_done);

    if (phase == POMO_IDLE) {
        return false;
    }
    const int remaining_s = (int)((remaining_us + 999999) / 1000000);
    snprintf(buf, len, "%02d:%02d", remaining_s / 60, remaining_s % 60);
    return true;
}
