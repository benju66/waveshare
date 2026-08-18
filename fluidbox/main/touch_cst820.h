#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

// Always-on CST820 reader and swipe detector. LVGL is asleep whenever the
// fluid page owns the panel, so touch cannot live inside LVGL's input layer;
// this task runs regardless of page, turns horizontal swipes into page
// manager events, and forwards everything else to LVGL as pointer samples.
//
// Interrupt driven off TOUCH_INT_GPIO with a 50 ms polling fallback, so a
// missed release edge degrades to one late sample instead of a stuck press.

esp_err_t touch_init(i2c_master_bus_handle_t bus);
