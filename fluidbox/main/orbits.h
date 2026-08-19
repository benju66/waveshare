#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sim.h"

// Orbits: a neon sun and a dozen planets falling around it. Drag the sun
// with a finger and the system reshuffles; fling a planet close and it
// slingshots. Tilt adds a uniform drift. The gravity-well toy, where the
// well is the point rather than a marble accessory.

esp_err_t orbits_init(void);
void orbits_step(float dt, const sim_forces_t *forces);
void orbits_render_frame(void);
void orbits_touch(int16_t x, int16_t y, bool pressed);
