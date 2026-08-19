#include "ui_lvgl.h"

#include <stdatomic.h>

#include "config.h"
#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "page_now.h"
#include "page_pomodoro.h"
#include "page_settings.h"

// LVGL renders in partial mode into the same two DMA band buffers the fluid
// renderer uses; the page manager guarantees only one of them is ever active.
// 28 rows per chunk means a full-screen repaint takes 16 flushes, but each
// QSPI transfer is ~0.5 ms and the pages are mostly static, so steady-state
// invalidations are small. If UI animation ever stutters, switch to two
// dedicated buffers here and leave everything else alone.
#define UI_BUF_BYTES (LCD_H_RES * BAND_ROWS * sizeof(uint16_t))

// lv_timer_handler asks to be called again after this many ms at most; a cap
// keeps page-change requests from waiting behind an idle UI.
#define UI_MAX_SLEEP_MS 50

static const char *TAG = "ui_lvgl";

static lv_display_t *s_disp;
static lv_indev_t *s_indev;
static TaskHandle_t s_task;
static lv_obj_t *s_screens[PAGE_COUNT];

// -1 when nothing is pending; written by the page manager, consumed by the
// LVGL task at the top of its loop.
static atomic_int s_pending_page = -1;

static portMUX_TYPE s_point_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    int16_t x, y;
    bool pressed;
} s_point;

// Deferred tap: 0 idle, 1 emit the press, 2 emit the release. Written by the
// touch task, consumed by the indev read over two cycles.
static atomic_int s_click_stage;
static int16_t s_click_x, s_click_y;

// ---------------------------------------------------------------------------
// Display plumbing
// ---------------------------------------------------------------------------

static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// The CO5300 corrupts columns when a window update starts on an odd
// coordinate (classic for this panel family). The fluid never trips it -
// full-width bands - but LVGL's small partial invalidations (a label here,
// a badge there) land anywhere, which showed up as jagged, garbled text.
// Round every invalidated area outward to even boundaries.
static void invalidate_cb(lv_event_t *e)
{
    lv_area_t *area = lv_event_get_param(e);
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    // The panel takes byte-swapped RGB565 (see SWAP16 in render.c). Swapping
    // here keeps LVGL's software renderer on its native-format fast path.
    lv_draw_sw_rgb565_swap(px_map, lv_area_get_size(area));

    const esp_err_t err =
        display_flush_area(area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
    if (err != ESP_OK) {
        // The transfer never started, so the ISR will never call flush_ready;
        // do it here or LVGL waits forever on this buffer.
        ESP_LOGE(TAG, "flush failed: %s", esp_err_to_name(err));
        lv_display_flush_ready(disp);
    }
}

void ui_lvgl_trans_done_hook(void *ctx)
{
    (void)ctx;
    lv_display_flush_ready(s_disp);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void ui_lvgl_input_publish(int16_t x, int16_t y, bool pressed)
{
    portENTER_CRITICAL(&s_point_mux);
    s_point.x = x;
    s_point.y = y;
    s_point.pressed = pressed;
    portEXIT_CRITICAL(&s_point_mux);
}

void ui_lvgl_input_click(int16_t x, int16_t y)
{
    if (!page_manager_lvgl_active()) {
        return;  // never queue a click for a page that is not even showing
    }
    s_click_x = x;
    s_click_y = y;
    atomic_store(&s_click_stage, 1);
}

static void indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    const int stage = atomic_load(&s_click_stage);
    if (stage != 0) {
        data->point.x = s_click_x;
        data->point.y = s_click_y;
        data->state = (stage == 1) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        atomic_store(&s_click_stage, stage == 1 ? 2 : 0);
        return;
    }

    portENTER_CRITICAL(&s_point_mux);
    data->point.x = s_point.x;
    data->point.y = s_point.y;
    data->state = s_point.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    portEXIT_CRITICAL(&s_point_mux);
}

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------

void ui_lvgl_request_page(page_id_t page)
{
    if (page > PAGE_FLUID && page < PAGE_COUNT) {
        atomic_store(&s_pending_page, (int)page);
    }
}

static void apply_pending_page(void)
{
    const int page = atomic_exchange(&s_pending_page, -1);
    if (page < 0) {
        return;
    }
    lv_screen_load(s_screens[page]);
    // The panel was wiped during the handoff, but LVGL believes whatever it
    // drew last is still there — and loading an already-active screen redraws
    // nothing at all. Invalidate the lot so the first frame repaints fully.
    lv_obj_invalidate(s_screens[page]);
}

// ---------------------------------------------------------------------------
// Task
// ---------------------------------------------------------------------------

static void lvgl_task(void *arg)
{
    (void)arg;

    for (;;) {
        page_manager_lvgl_gate();
        apply_pending_page();

        uint32_t sleep_ms = lv_timer_handler();
        if (sleep_ms > UI_MAX_SLEEP_MS) {
            sleep_ms = UI_MAX_SLEEP_MS;
        } else if (sleep_ms < 1) {
            sleep_ms = 1;
        }
        vTaskDelay(pdMS_TO_TICKS(sleep_ms));
    }
}

esp_err_t ui_lvgl_init(void)
{
    lv_init();
    lv_tick_set_cb(tick_cb);

    s_disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    if (s_disp == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, display_band_buffer(0), display_band_buffer(1),
                           UI_BUF_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_add_event_cb(s_disp, invalidate_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, indev_read_cb);
    lv_indev_set_display(s_indev, s_disp);
    // The spec's reset gesture is a deliberate hold, well clear of a sloppy tap.
    lv_indev_set_long_press_time(s_indev, 1000);

    s_screens[PAGE_TIMER] = page_pomodoro_create();
    s_screens[PAGE_NOW] = page_now_create();
    s_screens[PAGE_SETTINGS] = page_settings_create();

    if (xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 4, &s_task, 0) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "LVGL %d.%d.%d ready", lv_version_major(), lv_version_minor(),
             lv_version_patch());
    return ESP_OK;
}

TaskHandle_t ui_lvgl_task_handle(void)
{
    return s_task;
}
