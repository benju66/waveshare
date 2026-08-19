#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "lvgl.h"

// Pomodoro page. The state machine is esp_timer based and keeps running no
// matter which page is visible or how long the LVGL task sleeps; the widgets
// only ever render it. Durations live in config.h.
//
// Tap = start / pause / resume. Long press (1 s) = reset to idle.

// Builds and returns the page's screen. Call once, from the LVGL task's
// context (i.e. during ui_lvgl_init, before the task loop starts).
lv_obj_t *page_pomodoro_create(void);

// For the Now page's corner badge: writes "25:00"-style remaining time into
// buf and returns true while a session is active (running or paused).
bool page_pomodoro_badge_text(char *buf, size_t len);

// Vertical swipe on the idle timer page: adjust the work duration by
// delta_min minutes (clamped to POMO_WORK_MIN_FLOOR..CEIL). Ignored while a
// session is active. Safe from the touch task.
void page_pomodoro_adjust(int delta_min);

// True while a session is actively counting down (not idle, not paused).
// The idle policy dims the timer page in place instead of leaving it.
bool page_pomodoro_session_running(void);

// Lay-flat detector feed. Face-down pauses a running session; coming back up
// resumes it, unless the user toggled the timer while it lay there.
void page_pomodoro_set_face_down(bool face_down);
