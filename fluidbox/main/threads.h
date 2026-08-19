#pragma once

#include "esp_err.h"
#include "sim.h"

// Threads: two balls weave neon trails across the black, each passing over
// its own thread but deflecting off the other's, until the screen fills and
// the weave dissolves to begin again. No interaction by design; tilt bends
// the paths. Black-dominant like Pong Wars, but accumulation instead of war.

esp_err_t threads_init(void);
void threads_step(float dt, const sim_forces_t *forces);
void threads_render_frame(void);
