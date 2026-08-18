#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

// Wi-Fi, SNTP and the Open-Meteo client, all confined to one low-priority
// task on core 1 so a fetch can never stall a renderer. UI code reads the
// latest weather through weather_get(); nothing here touches LVGL.

typedef struct {
    bool valid;            // false until the first successful fetch
    float temp_c;          // current temperature
    int weather_code;      // WMO code, see page_now.c for the glyph mapping
    float today_hi_c;
    float today_lo_c;
    int precip_prob_pct;   // probability next hour, 0..100
    int64_t fetched_us;    // esp_timer_get_time() at last success, for staleness
} weather_model_t;

// Starts Wi-Fi (credentials from secrets.h), SNTP and the fetch loop. Safe to
// call when secrets.h is absent: it logs and returns ESP_ERR_NOT_FOUND, and
// weather_get() then never turns valid.
esp_err_t net_task_start(void);

// Copies the latest model. Returns model.valid.
bool weather_get(weather_model_t *out);
