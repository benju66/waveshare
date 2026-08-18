#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "page_manager.h"

// Owns everything LVGL: the lv_display flushing through display.c's band
// machinery, the pointer indev fed by the touch task, the LVGL task, and the
// per-page screens. No other module may call lv_* functions except page
// modules running inside this module's task (widget callbacks, lv_timers).

esp_err_t ui_lvgl_init(void);

TaskHandle_t ui_lvgl_task_handle(void);

// Asks the LVGL task to load the given page's screen (and repaint fully) on
// its next loop. Safe from any task; last writer wins.
void ui_lvgl_request_page(page_id_t page);

// Installed as the display transfer-done hook while an LVGL page owns the
// panel. Runs in interrupt context.
void ui_lvgl_trans_done_hook(void *ctx);

// Latest pointer sample, published by the touch task. LVGL's indev picks it
// up inside lv_timer_handler.
void ui_lvgl_input_publish(int16_t x, int16_t y, bool pressed);

// Delivers a complete tap (press + release over two indev reads) at the
// given point. Used for contacts that ended before they could be proven not
// to be a swipe; LVGL never saw them live.
void ui_lvgl_input_click(int16_t x, int16_t y);
