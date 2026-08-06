#pragma once

// Host stub standing in for the real display driver, so render.c can be
// compiled and inspected on a Mac without flashing the board.
//
// Only the three entry points render.c actually calls are declared. The real
// display_flush_band() returns esp_err_t, which is a typedef of int32_t that
// does not exist off-target; render.c ignores the return value, so int is fine.

#include <stdbool.h>
#include <stdint.h>

uint16_t *display_acquire_band(void);
int display_flush_band(int band_index, const uint16_t *buffer);
