#include "page_manager.h"

#include <stdatomic.h>
#include <string.h>

#include "battery.h"
#include "config.h"
#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "net_task.h"
#include "page_pomodoro.h"
#include "settings.h"
#include "ui_lvgl.h"

#define PAGE_EVT_QUEUE_LEN 4

// How long to wait for a renderer to reach its gate. The worst case is one
// full render_frame (~33 ms) or one sim_step (~15 ms); anything near the
// timeout means a renderer is wedged, which is worth a log line but not a
// deadlocked UI.
#define PARK_ACK_TIMEOUT_MS 250

static const char *TAG = "page_mgr";

static atomic_bool s_fluid_active = true;
static atomic_bool s_lvgl_active = false;
static atomic_int s_current = PAGE_FLUID;

static QueueHandle_t s_events;
static SemaphoreHandle_t s_render_parked;
static SemaphoreHandle_t s_sim_parked;
static SemaphoreHandle_t s_lvgl_parked;
static TaskHandle_t s_render_task;
static TaskHandle_t s_sim_task;
static TaskHandle_t s_lvgl_task;

// Idle-to-fluid policy. The fluid is the screensaver: moving pixels, no
// burn-in. last-touch is in esp_timer microseconds; 64-bit atomics go
// through libatomic on Xtensa, which is fine at touch-sample rates.
static _Atomic int64_t s_last_touch_us;
static atomic_bool s_idle_dimmed;
static atomic_int s_last_lvgl_page = PAGE_TIMER;

// PWR short press turns the panel off outright; any tap or another press
// brings back whatever was showing. Deeper than the idle dim: both renderers
// parked, panel DCS-off.
static atomic_bool s_screen_off;
static page_id_t s_page_before_off = PAGE_FLUID;

// ---------------------------------------------------------------------------
// Gates, run on the renderers' own tasks
// ---------------------------------------------------------------------------

static void gate(atomic_bool *active, SemaphoreHandle_t parked)
{
    while (!atomic_load_explicit(active, memory_order_acquire)) {
        // Ack first, then sleep. The manager sets the flag before it notifies,
        // so a notification that lands between the give and the take below is
        // latched and the loop re-check exits immediately.
        xSemaphoreGive(parked);
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

void page_manager_render_gate(void) { gate(&s_fluid_active, s_render_parked); }
void page_manager_sim_gate(void) { gate(&s_fluid_active, s_sim_parked); }
void page_manager_lvgl_gate(void) { gate(&s_lvgl_active, s_lvgl_parked); }

// ---------------------------------------------------------------------------
// Transitions, run on the page manager task only
// ---------------------------------------------------------------------------

static void wait_parked(SemaphoreHandle_t parked, const char *who)
{
    if (xSemaphoreTake(parked, pdMS_TO_TICKS(PARK_ACK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "%s never acked its park; continuing anyway", who);
    }
}

static void park_fluid(void)
{
    // Drain stale acks so the takes below wait for acks produced by *this*
    // park. A stale credit exists after any ack timeout, and draining makes
    // that self-healing instead of compounding.
    xSemaphoreTake(s_render_parked, 0);
    xSemaphoreTake(s_sim_parked, 0);
    atomic_store_explicit(&s_fluid_active, false, memory_order_release);
    wait_parked(s_render_parked, "render");
    wait_parked(s_sim_parked, "sim");
}

static void unpark_fluid(void)
{
    atomic_store_explicit(&s_fluid_active, true, memory_order_release);
    if (s_render_task) xTaskNotifyGive(s_render_task);
    if (s_sim_task) xTaskNotifyGive(s_sim_task);
}

static void park_lvgl(void)
{
    // One stale credit always exists here: the LVGL task parks once at boot
    // and that ack is never consumed by a transition.
    xSemaphoreTake(s_lvgl_parked, 0);
    atomic_store_explicit(&s_lvgl_active, false, memory_order_release);
    wait_parked(s_lvgl_parked, "lvgl");
}

static void unpark_lvgl(void)
{
    atomic_store_explicit(&s_lvgl_active, true, memory_order_release);
    if (s_lvgl_task) xTaskNotifyGive(s_lvgl_task);
}

// The fluid renderer's dirty-band tracking assumes untouched bands are black
// on the panel, and LVGL only repaints what it thinks changed, so both
// directions of a handoff need the panel wiped.
static void clear_panel_black(void)
{
    for (int band = 0; band < BAND_COUNT; band++) {
        uint16_t *buf = display_acquire_band();
        memset(buf, 0, (size_t)LCD_H_RES * BAND_ROWS * sizeof(uint16_t));
        display_flush_band(band, buf);
    }
    display_wait_idle();
}

// ---------------------------------------------------------------------------
// CRT sleep/wake: the picture collapses to a bright line, then a dot, like a
// tube television losing power; waking plays it back the other way. Runs on
// this task while every renderer is parked, drawing straight through the
// band machinery. White is 0xFFFF in either byte order.
// ---------------------------------------------------------------------------

static void crt_frame(int line_w, int line_h)
{
    const int cx = LCD_H_RES / 2;
    const int cy = LCD_V_RES / 2;
    const int y_top = cy - line_h / 2;
    const int y_bot = cy + line_h / 2;
    const int x0 = cx - line_w / 2 < 0 ? 0 : cx - line_w / 2;
    const int x1 = cx + line_w / 2 >= LCD_H_RES ? LCD_H_RES - 1 : cx + line_w / 2;

    for (int band = 0; band < BAND_COUNT; band++) {
        const int by = band * BAND_ROWS;
        uint16_t *buf = display_acquire_band();
        memset(buf, 0, (size_t)LCD_H_RES * BAND_ROWS * sizeof(uint16_t));
        for (int y = by; y < by + BAND_ROWS; y++) {
            if (y < y_top || y > y_bot) {
                continue;
            }
            uint16_t *row = buf + (y - by) * LCD_H_RES;
            for (int x = x0; x <= x1; x++) {
                row[x] = 0xFFFF;
            }
        }
        display_flush_band(band, buf);
    }
    display_wait_idle();
}

static void crt_off_animation(void)
{
    // Collapse the picture into the line...
    for (int h = 40; h >= 2; h -= 8) {
        crt_frame(LCD_H_RES, h);
        vTaskDelay(pdMS_TO_TICKS(24));
    }
    // ...then the line into a dot...
    for (int w = LCD_H_RES; w >= 10; w = w * 8 / 17) {
        crt_frame(w, 3);
        vTaskDelay(pdMS_TO_TICKS(26));
    }
    // ...and gone.
    clear_panel_black();
    vTaskDelay(pdMS_TO_TICKS(40));
}

static void crt_on_animation(void)
{
    // The dot warms up into a line, the line opens into the picture.
    for (int w = 10; w < LCD_H_RES; w = w * 17 / 8) {
        crt_frame(w, 3);
        vTaskDelay(pdMS_TO_TICKS(24));
    }
    for (int h = 2; h <= 40; h += 10) {
        crt_frame(LCD_H_RES, h);
        vTaskDelay(pdMS_TO_TICKS(22));
    }
    // start_page() wipes to black and the page paints over.
}

// Parks whichever renderer holds the panel and drains its last transfer.
// Afterwards nobody owns the panel and the DMA is idle.
static void park_current(page_id_t current)
{
    if (current == PAGE_FLUID) {
        park_fluid();
        display_wait_idle();
    } else {
        park_lvgl();
        display_wait_idle();
        display_set_trans_done_hook(NULL, NULL);
    }
}

// Hands the freshly cleared panel to the target page's renderer.
static void start_page(page_id_t target)
{
    clear_panel_black();
    if (target == PAGE_FLUID) {
        unpark_fluid();
    } else {
        display_set_trans_done_hook(ui_lvgl_trans_done_hook, NULL);
        ui_lvgl_request_page(target);
        unpark_lvgl();
    }
}

static void goto_page(page_id_t target)
{
    const page_id_t current = (page_id_t)atomic_load(&s_current);
    if (target == current) {
        return;
    }

    const int64_t t0 = esp_timer_get_time();

    if (current != PAGE_FLUID && target != PAGE_FLUID) {
        // LVGL keeps the panel; the LVGL task swaps screens on its next loop.
        ui_lvgl_request_page(target);
    } else {
        park_current(current);
        start_page(target);
    }

    atomic_store(&s_current, target);
    if (target != PAGE_FLUID) {
        atomic_store(&s_last_lvgl_page, target);
        // Any route back to an LVGL page ends the screensaver state, whether
        // it was a wake touch or a plain swipe.
        if (atomic_exchange(&s_idle_dimmed, false)) {
            display_set_brightness(settings_brightness());
        }
    }
    atomic_store(&s_last_touch_us, esp_timer_get_time());
    ESP_LOGI(TAG, "page %d -> %d in %lld ms", current, target,
             (esp_timer_get_time() - t0) / 1000);
}

static void screen_off(void)
{
    if (atomic_load(&s_screen_off)) {
        return;
    }
    const page_id_t current = (page_id_t)atomic_load(&s_current);
    s_page_before_off = current;
    park_current(current);
    crt_off_animation();
    display_power(false);
    // Screen off means pocket or night: the radio is the biggest remaining
    // drain, and weather can be half an hour stale without anyone noticing.
    net_set_enabled(false);
    atomic_store(&s_screen_off, true);
    ESP_LOGI(TAG, "screen off (was page %d)", current);
}

static void screen_wake(page_id_t target)
{
    if (!atomic_load(&s_screen_off)) {
        return;
    }
    display_power(true);
    display_set_brightness(settings_brightness());
    net_set_enabled(true);
    atomic_store(&s_idle_dimmed, false);
    crt_on_animation();
    start_page(target);
    atomic_store(&s_current, target);
    if (target != PAGE_FLUID) {
        atomic_store(&s_last_lvgl_page, target);
    }
    atomic_store(&s_last_touch_us, esp_timer_get_time());
    atomic_store(&s_screen_off, false);
    ESP_LOGI(TAG, "screen wake to page %d", target);
}

static void idle_check(void)
{
    if (atomic_load(&s_screen_off) || atomic_load(&s_idle_dimmed) ||
        page_manager_current() == PAGE_FLUID) {
        return;
    }
    const int idle_min = settings_idle_min();
    const int64_t idle_us = esp_timer_get_time() - atomic_load(&s_last_touch_us);
    if (idle_us > (int64_t)idle_min * 60 * 1000000) {
        if (!battery_on_usb()) {
            // Battery: every idle minute of screen is carry time lost. A
            // running session still alerts at full brightness when it ends.
            ESP_LOGI(TAG, "idle %d min on battery, screen off", idle_min);
            screen_off();
        } else if (page_pomodoro_session_running()) {
            // Desk with a session counting: the timer owns the idle screen,
            // dimmed but readable - never the fluid, whatever page was open.
            ESP_LOGI(TAG, "idle %d min with a session running, dimmed timer",
                     idle_min);
            goto_page(PAGE_TIMER);
            display_set_brightness(IDLE_FLUID_BRIGHTNESS);
            atomic_store(&s_idle_dimmed, true);
        } else {
            // Desk, nothing running: the dimmed fluid is the screensaver.
            ESP_LOGI(TAG, "idle %d min, dimming to the fluid", idle_min);
            goto_page(PAGE_FLUID);
            display_set_brightness(IDLE_FLUID_BRIGHTNESS);
            atomic_store(&s_idle_dimmed, true);
        }
    }
}

static void page_manager_task(void *arg)
{
    (void)arg;

    for (;;) {
        page_evt_t evt;
        if (xQueueReceive(s_events, &evt, pdMS_TO_TICKS(1000)) != pdTRUE) {
            idle_check();
            continue;
        }

        const page_id_t current = (page_id_t)atomic_load(&s_current);
        switch (evt) {
        case PAGE_EVT_SWIPE_LEFT:
            goto_page((page_id_t)((current + 1) % PAGE_COUNT));
            break;
        case PAGE_EVT_SWIPE_RIGHT:
            goto_page((page_id_t)((current + PAGE_COUNT - 1) % PAGE_COUNT));
            break;
        case PAGE_EVT_SCREEN_OFF:
            screen_off();
            break;
        case PAGE_EVT_WAKE:
            if (atomic_load(&s_screen_off)) {
                screen_wake(s_page_before_off);
            } else if ((page_id_t)atomic_load(&s_last_lvgl_page) == current) {
                // Dimmed in place (running timer): just restore brightness.
                if (atomic_exchange(&s_idle_dimmed, false)) {
                    display_set_brightness(settings_brightness());
                }
                atomic_store(&s_last_touch_us, esp_timer_get_time());
            } else {
                // Waking from the idle dim: back to the page that was left.
                goto_page((page_id_t)atomic_load(&s_last_lvgl_page));
            }
            break;
        case PAGE_EVT_ALERT_TIMER:
            // A finished pomodoro phase deserves eyes on it, at full
            // brightness, whatever the screen was doing.
            if (atomic_load(&s_screen_off)) {
                screen_wake(PAGE_TIMER);
            } else {
                if (atomic_exchange(&s_idle_dimmed, false)) {
                    display_set_brightness(settings_brightness());
                }
                goto_page(PAGE_TIMER);
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t page_manager_init(void)
{
    s_events = xQueueCreate(PAGE_EVT_QUEUE_LEN, sizeof(page_evt_t));
    s_render_parked = xSemaphoreCreateBinary();
    s_sim_parked = xSemaphoreCreateBinary();
    s_lvgl_parked = xSemaphoreCreateBinary();
    if (!s_events || !s_render_parked || !s_sim_parked || !s_lvgl_parked) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(page_manager_task, "page_mgr", 4096, NULL, 6,
                                NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void page_manager_register_tasks(TaskHandle_t render_task, TaskHandle_t sim_task,
                                 TaskHandle_t lvgl_task)
{
    s_render_task = render_task;
    s_sim_task = sim_task;
    s_lvgl_task = lvgl_task;
}

void page_manager_post_event(page_evt_t evt)
{
    if (s_events && xQueueSend(s_events, &evt, 0) != pdTRUE) {
        ESP_LOGD(TAG, "event queue full, swipe dropped");
    }
}

bool page_manager_fluid_active(void)
{
    return atomic_load_explicit(&s_fluid_active, memory_order_acquire);
}

bool page_manager_lvgl_active(void)
{
    return atomic_load_explicit(&s_lvgl_active, memory_order_acquire);
}

page_id_t page_manager_current(void)
{
    return (page_id_t)atomic_load(&s_current);
}

void page_manager_note_activity(void)
{
    atomic_store(&s_last_touch_us, esp_timer_get_time());
}

bool page_manager_wake_touch(void)
{
    page_manager_note_activity();
    if (atomic_load(&s_idle_dimmed) || atomic_load(&s_screen_off)) {
        page_manager_post_event(PAGE_EVT_WAKE);
        return true;
    }
    return false;
}

bool page_manager_screen_is_off(void)
{
    return atomic_load(&s_screen_off);
}
