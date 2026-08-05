#pragma once

// Every tunable in FluidBox lives here. Anything you might want to change to
// alter the look or the performance/quality tradeoff is a constant below.

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

#define LCD_H_RES 368
#define LCD_V_RES 448

// The renderer never holds a whole frame. It draws one horizontal band at a
// time into internal SRAM and DMAs it out while drawing the next one, which
// keeps the framebuffer off PSRAM entirely.
#define BAND_ROWS 28
#define BAND_COUNT (LCD_V_RES / BAND_ROWS)

#define DISPLAY_BRIGHTNESS 230  // 0..255, written to panel register 0x51

// ---------------------------------------------------------------------------
// Simulation box
// ---------------------------------------------------------------------------

// x and y are screen pixels. z is depth into the case: 0 is the glass, BOX_D
// is the back of the enclosure.
//
// The panel is 322 ppi, so a physically accurate case depth would be about
// 150 px. That reads as a tunnel rather than a slim device once perspective is
// applied, so the box is deliberately half that. The side benefit is that the
// same 900 particles now fill about a third of the volume instead of a fifth,
// which makes the pool look like a body of liquid rather than a puddle.
//
// If you change this, check GRID_CZ in sim.c: every grid cell has to stay at
// least one smoothing radius deep.
#define BOX_W 368.0f
#define BOX_H 448.0f
#define BOX_D 75.0f

#define PX_PER_METER 12677.0f
#define PX_PER_MM (PX_PER_METER / 1000.0f)

// The panel's corners are rounded, so the box has to be too. With a square box
// the fluid collects in four corners that are physically not visible, and the
// box overshoots the glass. Measured at roughly 4.5 mm, which is about 57 px at
// this panel's 322 ppi.
#define BOX_CORNER_MM 4.5f
#define BOX_CORNER_R (BOX_CORNER_MM * PX_PER_MM)

// The case does not meet its own back panel at a sharp edge either, so the
// walls curve into the backplane over this radius. It has to stay well under
// both BOX_D and BOX_CORNER_R: the box is only about 5.9 mm deep, so anything
// near 3 mm turns the back of the box into a dome.
#define BOX_BACK_FILLET_MM 2.0f
#define BOX_BACK_FILLET (BOX_BACK_FILLET_MM * PX_PER_MM)

// The glass meets the walls at a much tighter radius than the back panel does.
// Keep the two fillets summing to well under BOX_D, or the straight section of
// wall between them disappears.
#define BOX_FRONT_FILLET (BOX_BACK_FILLET * 0.25f)

// The simulation runs in slow motion. Real water in a 3 cm box sloshes far too
// fast to see; scaling time down turns it into the syrupy, readable motion
// that reads as "liquid" on a small screen. All physical constants stay real,
// only the clock is slowed, so gravity, shake and rotation stay consistent
// with each other.
// Lower also means a smaller effective timestep, which is what keeps the
// pressure solver quiet: gravity adds less velocity per step for the
// relaxation to cancel, so the fluid settles instead of simmering. What
// actually matters is TIME_SCALE divided by the step rate: measured at 900
// particles, 0.085 settled to 65 px/s and 0.115 only reached 140. This is the
// compromise, and it is the first thing to lower if the surface looks noisy.
#define TIME_SCALE 0.100f

#define GRAVITY_MPS2 9.81f

// ---------------------------------------------------------------------------
// Particles
// ---------------------------------------------------------------------------

// Arrays are sized for MAX; PARTICLE_COUNT is what actually runs and is the
// first thing to turn down if the frame rate drops.
//
// MAX is capped by internal SRAM, not by speed: every particle costs about
// 150 bytes across the position, velocity, density and snapshot arrays, and
// 1200 overflows the static DRAM segment at link time. COUNT is capped by
// speed: 1200 particles ran at 23 steps/s while being shaken, 900 holds
// around 30, and the extra smoothness reads better than the extra density.
#define PARTICLE_MAX 1000
#define PARTICLE_COUNT 900

// Distance between neighbouring particles when the fluid is at rest.
#define REST_SPACING 17.0f

// Interaction radius. Larger means smoother fluid but quadratically more
// neighbour work; 1.65x spacing gives roughly 15 neighbours per particle.
#define SMOOTH_RADIUS 28.0f

// One relaxation pass per frame. The solver is position based, so a single
// substep stays stable; two would look slightly smoother but halves the rate.
#define SUBSTEPS 1

// Double density relaxation stiffness, as an acceleration in px/s^2 per unit
// of density error. Displacement is scaled by dt^2, so these behave like real
// accelerations and the solver stays stable if SUBSTEPS changes. For scale,
// slowed-down gravity is about 2400 px/s^2, so the fluid resists compression
// roughly twenty times harder than gravity squeezes it.
// PRESSURE keeps the bulk at rest density and, being signed, also pulls the
// surface together like surface tension. NEAR_PRESSURE is always repulsive and
// stops particles collapsing onto each other.
#define K_PRESSURE 400000.0f
#define K_NEAR_PRESSURE 800000.0f

// Cap on how far one neighbour pair may push a particle in a single step, in
// pixels. Deliberately a per-pair limit rather than a per-particle one; see the
// note on corner behaviour in the README.
#define MAX_DISPLACEMENT 4.0f

// Clavet viscosity: linear and quadratic terms applied to the closing speed
// between neighbours. Higher is thicker, more honey than water. Speeds here
// are in pixels per second and run into the hundreds, which is why SIGMA is
// large: a pair sheds roughly 0.5 * dt * q * SIGMA of its closing speed per
// step, so SIGMA around 30 damps a few percent per neighbour.
#define VISC_SIGMA 45.0f
#define VISC_BETA 0.03f

#define WALL_RESTITUTION 0.25f

// Tangential drag applied once per step while a particle is touching a wall.
// This is per step, not per second, so it compounds fast: 0.86 at ~34 steps/s
// left the fluid crawling along the walls instead of sliding into a corner.
#define WALL_FRICTION 0.96f

// ---------------------------------------------------------------------------
// IMU
// ---------------------------------------------------------------------------

// Time constant of the low pass that separates steady gravity from shake.
#define GRAVITY_LP_HZ 1.2f

// How hard a shake pushes the fluid, relative to the true pseudo-force.
#define SHAKE_GAIN 1.6f

// Rotating-frame forces from the gyro: centrifugal, Euler (angular
// acceleration) and Coriolis. Set to 0 to disable them.
#define ROTATION_GAIN 1.0f

// Maps QMI8658 axes onto simulation axes (x right, y down, z into the case).
// Flip a sign here if the fluid falls the wrong way.
//
// An accelerometer reports the reaction to gravity, so whichever axis points
// up reads positive. Measured on this board:
//   flat, screen up          -> (0, 0, -9.9)   up is IMU -z, so sim z = az
//   upright, port right      -> (+10.6, 0, 0)  up is IMU +x, so sim y = -ax
//   right edge to the floor  -> (0, -9.6, 0)   up is IMU -y, so sim x = ay
#define IMU_MAP_X(ax, ay, az) (ay)
#define IMU_MAP_Y(ax, ay, az) (-(ax))
#define IMU_MAP_Z(ax, ay, az) (az)

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

// Pinhole projection. A particle at the glass draws at full size; one at the
// back of the case draws at FOCAL/(FOCAL+BOX_D) of that. Shorter focal length
// means stronger perspective: at 220 against a 75 deep box, the back of the
// case works out at about 75% of the front. Since the enclosure itself is never
// drawn, this and the depth dimming are what make the fluid read as 3D.
#define PROJ_FOCAL 220.0f

#define PARTICLE_RADIUS_PX 6.5f
#define DISC_MAX_R 10

// A smaller, brighter disc offset towards the top left of each particle, which
// turns a flat circle into something that reads as a lit sphere.
#define HIGHLIGHT_ENABLE 1
#define HIGHLIGHT_LIFT 0.55f  // 0 = no lift, 1 = pure white

// Colour ramp resolution: speed is quantised to this many steps, depth to
// DEPTH_LEVELS, and the product is baked into one lookup table.
#define SPEED_LEVELS 64
#define DEPTH_LEVELS 16

// Speed (px/s in simulation time) that saturates the ramp at pure white.
// Measured on the device: settled fluid sits near 60, a tilt-driven pour runs
// a few hundred, and a hard shake peaks above 10000. Set high so that white is
// reserved for genuinely violent motion.
#define SPEED_COLOR_MAX 5000.0f

// Curve applied to speed before it indexes the ramp. Below 1 it stretches the
// low end, so gentle motion still shows colour while white stays rare.
#define SPEED_COLOR_GAMMA 0.55f

// How much the back of the box is darkened relative to the front.
#define DEPTH_DIM_MIN 0.20f
