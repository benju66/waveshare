#pragma once

#include "lvgl.h"

// The "Now" page: a clock, today's weather, and the pomodoro corner badge.
// Weather-only by design — transit was cut from v1.

// Builds and returns the page's screen. Call once, from the LVGL task's
// context (during ui_lvgl_init, before the task loop starts).
lv_obj_t *page_now_create(void);
