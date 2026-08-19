#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

// Battery percentage from the AXP2101 PMU's fuel gauge. Read on demand with
// a short cache, so UI code can call it every refresh without I2C traffic.

esp_err_t battery_init(i2c_master_bus_handle_t bus);

// 0..100, or -1 while unknown (no battery, PMU unreachable, not yet read).
int battery_percent(void);

// True when USB (VBUS) is powering the board. Drives the desk-vs-pocket
// behavior split; defaults to true when the PMU is unreachable, so a desk
// setup without a readable gauge behaves like a desk.
bool battery_on_usb(void);
