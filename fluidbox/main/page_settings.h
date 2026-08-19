#pragma once

#include "lvgl.h"

// Settings page: brightness, idle timeout, fireworks, pocket wake guard.
// Everything is +/- buttons and switches on purpose: slider drags would
// collide with the horizontal swipe detector.

// Builds and returns the page's screen. Call once, from the LVGL task's
// context (during ui_lvgl_init, before the task loop starts).
lv_obj_t *page_settings_create(void);

// Vertical swipe between the steppers view and the switches view. Safe from
// the touch task.
void page_settings_cycle(int dir);
