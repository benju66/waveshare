#pragma once

#include <stdbool.h>

#include "esp_err.h"

// User-adjustable settings, persisted in NVS, edited on the settings page.
// Getters are cheap (atomics) and safe from any task; setters persist
// immediately and are called from the LVGL task.

esp_err_t settings_init(void);

int settings_brightness(void);   // 20..255, panel brightness when awake
int settings_idle_min(void);     // 1..30, minutes without touch before idle
bool settings_fireworks(void);   // phase-end fireworks on/off (flash stays)
bool settings_wake_guard(void);  // require stillness for tap-wake when dark

void settings_set_brightness(int value);
void settings_set_idle_min(int value);
void settings_set_fireworks(bool on);
void settings_set_wake_guard(bool on);
