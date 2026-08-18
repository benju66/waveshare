#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

// Owns the AMOLED panel and the pair of band buffers that feed it.
//
// The screen is pushed out one horizontal band at a time. Callers render into
// the buffer returned by display_acquire_band() and hand it to
// display_flush_band(), which starts a DMA transfer and returns immediately.
// Acquiring blocks only if both buffers are still in flight, so drawing one
// band overlaps with transmitting the previous one.

esp_err_t display_init(i2c_master_bus_handle_t i2c_bus);

// Blocks until a band buffer is free, then returns it. BAND_ROWS * LCD_H_RES
// pixels. Buffers rotate per call rather than per band index, because the
// renderer skips bands that are already black on the panel, and two transfers
// in flight must never land in the same buffer.
uint16_t *display_acquire_band(void);

// Queues an asynchronous transfer of one band. Does not block.
esp_err_t display_flush_band(int band_index, const uint16_t *buffer);

// 0..255. The panel dims itself; there is no backlight pin on an AMOLED.
esp_err_t display_set_brightness(uint8_t level);

// --- Additions for the widget pages fork ---
//
// The LVGL pages reuse the same two band buffers and, crucially, the same
// counting semaphore as the fluid renderer: every transfer, from either
// renderer, takes one slot before esp_lcd_panel_draw_bitmap and the
// transfer-complete interrupt gives it back. A semaphore count of two
// therefore always means the panel DMA is idle, no matter which page owns
// the screen.

// Invoked from the transfer-complete interrupt, after the slot is given back.
// Install and clear only while the DMA is idle (see display_wait_idle).
typedef void (*display_trans_done_hook_t)(void *ctx);
void display_set_trans_done_hook(display_trans_done_hook_t hook, void *ctx);

// Queues a transfer of an arbitrary rectangle, for LVGL partial flushes.
// Takes one buffer slot, exactly like display_flush_band. Coordinates are
// logical (the panel's V2 x-gap is applied by the driver); ends exclusive.
esp_err_t display_flush_area(int x0, int y0, int x1_excl, int y1_excl, const void *buffer);

// Blocks until both buffer slots are free, i.e. no transfer is in flight and
// no acquired band is waiting to be flushed.
void display_wait_idle(void);

// Direct access to band buffer 0 or 1, so LVGL can use them as draw buffers
// while an LVGL page owns the panel. Ownership is the page manager's problem.
uint16_t *display_band_buffer(int idx);

// DCS display on/off. Off shows nothing and, this being an AMOLED, draws
// next to nothing. Panel RAM stays writable while off.
esp_err_t display_power(bool on);
