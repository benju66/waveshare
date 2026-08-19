#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sim.h"

// Marble mode for the physics page: rigid 2D balls under the same IMU
// gravity and shake as the fluid, in the same rounded box, rendered through
// the same band pipeline. The sim task steps whichever mode the settings
// select; the render task draws the matching frame.

esp_err_t balls_init(void);

// One physics step. dt in real seconds (internally clamped and substepped);
// forces are the same structure imu_read fills for the fluid.
void balls_step(float dt, const sim_forces_t *forces);

// Draws a full frame through display_acquire_band/display_flush_band.
void balls_render_frame(void);

// Live finger feed from the touch task while the physics page is up: the
// finger acts as an immovable disc the marbles collide with, carrying the
// finger's velocity so a flick launches them. pressed=false ends contact.
void balls_touch(int16_t x, int16_t y, bool pressed);
