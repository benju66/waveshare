#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

// Owns the AMOLED panel and the pair of band buffers that feed it.
//
// The screen is pushed out one horizontal band at a time. Callers render into
// the buffer returned by display_band_buffer() and hand it to
// display_flush_band(), which starts a DMA transfer and returns immediately.
// The next call to display_wait_buffer() blocks only if both buffers are still
// in flight, so drawing band N overlaps with transmitting band N-1.

esp_err_t display_init(i2c_master_bus_handle_t i2c_bus);

// Blocks until the buffer for the given band index is free to draw into.
void display_wait_buffer(int band_index);

// Buffers alternate by band index; BAND_ROWS * LCD_H_RES pixels each.
uint16_t *display_band_buffer(int band_index);

// Queues an asynchronous transfer of one band. Does not block.
esp_err_t display_flush_band(int band_index, const uint16_t *buffer);

// 0..255. The panel dims itself; there is no backlight pin on an AMOLED.
esp_err_t display_set_brightness(uint8_t level);
