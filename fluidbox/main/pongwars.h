#pragma once

#include "esp_err.h"
#include "sim.h"

// Pong Wars: two territories, pure black and a neon, each defended by a
// ball that eats into the other's ground. An endless, watchable war built
// for this AMOLED: half the pixels are literally off at any moment. Tilt
// bends the ball paths, so the fidget input is picking a side.

esp_err_t pongwars_init(void);
void pongwars_step(float dt, const sim_forces_t *forces);
void pongwars_render_frame(void);
