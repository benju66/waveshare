#pragma once

#include <stdint.h>

// Per-particle state handed to the renderer. Positions are in simulation
// space: x and y in screen pixels, z in pixels of depth into the case.
typedef struct {
    float x, y, z;
    float speed;  // px/s, used to pick a colour
} sim_particle_view_t;

// External accelerations for one step, expressed in simulation axes.
typedef struct {
    float gravity[3];  // px/s^2, gravity plus the shake pseudo-force
    float omega[3];    // rad/s, box angular velocity
    float alpha[3];    // rad/s^2, box angular acceleration
} sim_forces_t;

// Diagnostics, logged periodically so the solver can be tuned from the serial
// console without watching the screen. The timings are microseconds spent in
// each pass of the most recent substep.
typedef struct {
    float mean_density;
    float rest_density;
    float mean_speed;
    float max_speed;
    int us_grid;
    int us_density;  // includes viscosity; the two share one pass
    int us_relax;

    // Front and back fillet bands compared against each other. The fluid
    // settles against the back of the case but not against the glass, and these
    // are here to say why: whether the front is denser, moving faster, being
    // pushed further by the wall, or saturating the per-pair displacement cap.
    float front_density, back_density;
    float front_speed, back_speed;
    float front_push, back_push;  // mean wall projection distance, px
    int front_count, back_count;
    int front_hits, back_hits;    // particles the wall had to move
    int clamped;                  // pairs that saturated MAX_DISPLACEMENT
    int pairs;                    // neighbour pairs cached for relaxation
} sim_stats_t;

void sim_init(void);

// Re-seeds the fluid as a settled block at the bottom of the box.
void sim_reset(void);

// Advances the fluid by dt_real seconds of wall-clock time.
void sim_step(float dt_real, const sim_forces_t *forces);

// Copies the most recently published particle state. Returns the count.
int sim_snapshot(sim_particle_view_t *out, int max);

void sim_stats(sim_stats_t *out);
