# FluidBox

A 3D particle fluid living inside the case of a Waveshare ESP32-S3-Touch-AMOLED-1.8.
Tilt the board and the liquid pours; shake it and it sprays and flashes orange.
The screen is the front wall of a glass box, and the depth you see receding into
the display is the real thickness of the enclosure.

Measured on hardware: **900 particles, 90 fps display, 38 simulation steps/s.**

---

## 1. Getting it running

```bash
. ~/esp/esp-idf/export.sh
cd fluidbox
idf.py -p /dev/cu.usbmodem101 flash monitor
```

Press `Ctrl-]` to leave the monitor.

### First-time setup

ESP-IDF is Espressif's native C framework. It installs once, into its own
directory, and brings its own compiler, CMake, Ninja and Python environment —
nothing lands in your system Python or PATH permanently.

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.5 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3
```

`export.sh` is what puts the toolchain on your PATH; it has to be sourced in
each new shell. Version 5.5.5 is the one Waveshare's own CI builds against.

### Why ESP-IDF and not Arduino

Waveshare supports both. Arduino is easier to start with, but its display path
goes through the Arduino TFT library, and Waveshare measures their own LVGL demo
at 50-60 fps there versus 200-300 fps under ESP-IDF, because ESP-IDF lets you
drive the panel with DMA directly. This app needs that control, and it does not
need a widget toolkit at all.

### How flashing actually works

The ESP32-S3 has a USB peripheral built into the chip, wired straight to the
Type-C port. No USB-to-serial adapter, no driver on macOS, and no button
presses: `esptool` pulses the USB control lines to reset the chip into its
bootloader, writes flash, and resets again.

The board enumerates as `/dev/cu.usbmodem101`. If you have several boards, run
`ls /dev/cu.*` before and after plugging one in.

**If flashing ever fails**, the app has almost certainly crashed in a way that
takes the USB peripheral down with it. Hold **BOOT**, plug the cable in, release.
That starts the chip in ROM download mode, which cannot be broken by application
code, and you can flash normally. Nothing in this project touches GPIO0, the USB
pins, or the console configuration, so plain `idf.py flash` and a normal power
cycle both keep working.

### Dependencies

`main/idf_component.yml` works like a `package.json`: the build fetches
`espressif/esp_lcd_co5300` (display driver) and `waveshare/qmi8658` (IMU) from
Espressif's component registry on first build. Nothing is vendored.

### Board revisions

Two versions of this board exist: the original with an SH8601 display and FT3168
touch, and the V2 (shipping since May 2026) with a CO5300 and CST820. They differ
in the touch controller's I2C address and a 16-pixel horizontal panel offset.
`display_init()` probes for the CST820 at address 0x15 and applies the offset if
it answers, so one binary runs on both. The detected revision is printed at boot.

---

## 2. How the rendering works

### No framebuffer

A full frame is 368 x 448 x 2 bytes = 322 KB. That does not fit in the 512 KB of
internal SRAM twice, and putting it in PSRAM would mean the CPU writing pixels
and the DMA engine reading them fight over the same external memory bus.

Instead the screen is painted in **16 horizontal bands of 28 rows**. Each band is
20 KB, small enough that two of them live comfortably in internal SRAM. The loop
in `render_frame()` is:

```
for each band:
    wait until this band's buffer is free
    clear it to black
    draw every particle that overlaps this band
    hand the buffer to DMA and move on immediately
```

`esp_lcd_panel_draw_bitmap()` queues a transfer and returns, so the CPU is
drawing band N while band N-1 is still going out over the wire. A counting
semaphore, initialised to 2 and released by the transfer-complete interrupt,
is what makes "wait until free" work: taking it means at least one of the two
buffers is idle, and because transfers complete in the order they were queued,
that is always the one about to be reused.

The panel link sets the ceiling. QSPI moves 4 bits per clock, so at the driver's
default 40 MHz a 322 KB frame takes ~16 ms — about 52 fps in practice. The
ESP32-S3's SPI peripheral will run at 80 MHz, which halves that and measures
**91 fps** with nothing but a cleared buffer. Since the only thing drawn over
that is the fluid, the shipped build sits at **90 fps** at rest, dropping to the
high seventies mid-shake when the particles spread out and cost more to
rasterise. `LCD_PIXEL_CLOCK_HZ` in `display.c` is the knob; drop it back to
40 MHz if you ever see tearing, since these signals route through the GPIO
matrix rather than dedicated IOMUX pins.

### Colour without arithmetic

Per-pixel float maths would be hopeless at this rate, so every colour a particle
can be is computed once at startup into a table of `DEPTH_LEVELS x SPEED_LEVELS`
entries (16 x 64). Speed picks a position along a deep blue -> bright blue ->
pale blue -> white ramp; depth multiplies the whole thing down towards
`DEPTH_DIM_MIN`. Drawing a particle is then one table lookup.

White sits at the very top of the ramp only, so it reads as spray thrown off a
hard shake rather than as ordinary movement. Two things keep it rare:
`SPEED_COLOR_MAX` is set to 5000 px/s, well above normal sloshing, and
`SPEED_COLOR_GAMMA` bends the curve so gentle motion still shows visible colour
change without pushing the top end up. The curve is baked into the table, so the
runtime lookup stays a single multiply.

A second table holds the same colours lifted towards white by `HIGHLIGHT_LIFT`.

The values are stored byte-swapped, because the panel wants RGB565 in the
opposite byte order from how the CPU keeps a `uint16_t`. Doing that once in the
table beats doing it per pixel.

### Selling the third dimension

Nothing is drawn but the particles — no frame, no wall, no wireframe. The
enclosure is real in the physics and invisible on the screen, so the depth has
to come entirely from how the fluid itself is drawn. Three cues, all cheap:

1. **Short focal length.** `PROJ_FOCAL` is 220 against a box 75 deep, so the far
   wall projects to about 75% of the near one. Particles at the back are visibly
   smaller and pulled towards the centre of the screen. Shortening the focal
   length further, or deepening the box, quickly starts to look like a tunnel
   rather than a slim device.
2. **Depth darkening.** `DEPTH_DIM_MIN` takes the far plane down to 20% brightness.
3. **Specular highlights.** Each particle gets a second, smaller disc offset up
   and to the left in a lightened shade. Every particle is lit from the same
   direction, so the body of fluid looks rounded rather than like confetti.

The box still reads clearly, because the fluid traces it: the pool flattens
against the bottom, climbs the rounded corners, and rides up over the fillet
into the back panel. Drawing the enclosure as well turned out to add nothing but
clutter, which is why several increasingly elaborate attempts at it — a
wireframe, then depth-shaded rings, then a lit back wall — were all backed out
again.

### The box is the shape of the case

The device is not a cuboid, so neither is the box. Three radii describe it, all
declared in millimetres in `config.h` and converted through `PX_PER_MM`, because
they are physical properties of the case rather than of the framebuffer:

| Constant | Value | What it rounds |
|---|---|---|
| `BOX_CORNER_MM` | 4.5 mm | The panel outline, in x and y |
| `BOX_BACK_FILLET_MM` | 2.0 mm | Where the walls curve into the back panel |
| `BOX_FRONT_FILLET` | 25% of the back fillet | Where the walls curve into the glass |

Getting this wrong is visible. With square corners the frame overshoots the
glass and fluid pools into four corners you physically cannot see; with a hard
back edge the box reads as a cookie cutter rather than an enclosure.

### Resolving a curved box without branches

The whole surface — flat walls, vertical corners, both fillets, and the
doubly-curved patches where a corner runs into the back panel — is one
continuous shape, and `resolve_walls()` handles all of it with the same trick
applied twice and no special cases.

**First, collapse x and y.** Clamp the point to the inner rectangle joining the
four corner-arc centres. Whatever offset remains points straight out from the
nearest part of the outline, and its length is how far out the particle is:

```
a = clamp(p.xy, arc_lo, arc_hi)
u = p.xy - a
r = |u|                    // one radial coordinate replaces two axes
```

**Then solve in (r, z).** That plane is again a rectangle with rounded corners:
`r` bounded by the side radius, `z` between the glass and the back panel, with a
fillet at each end. So clamp and project a second time, using whichever fillet
is nearer:

```
fillet, cz = front or back fillet, or zero in the straight section between them
cr = min(r, side_r - fillet)
d  = (r - cr, z - cz)
if |d| > fillet:
    (r, z) = (cr, cz) + fillet * d / |d|
```

Every case falls out of the arithmetic. Against a flat wall one component of `d`
is zero. In a vertical corner the first stage produces the arc, in a fillet the
second stage does, and where a corner meets the back panel both do at once,
which is exactly the doubly-curved patch. Mapping `r` back through `u` gives the
new position, and the surface normal is `(u * nr, nz)`, so restitution and drag
act on a correctly oriented normal everywhere.

Insetting a rounded rectangle by the wall margin shrinks its radius by the same
amount and leaves the arc centres alone, so the side radius is simply
`BOX_CORNER_R - WALL_MARGIN`.

### Not drawing it

The box is never drawn. `render_frame()` clears each band to black and paints
particles, and that is the whole of it.

This was arrived at by deleting things. Earlier versions drew a twelve-edge
wireframe, then three concentric rounded rectangles at decreasing brightness,
then diagonal struts between their corners, then a lit back panel with a
brighter band for the curved lip. Every one of them was less legible than the
fluid on its own. What went with them: the line and arc rasterisers, the packed
edge-point list and its counting sort, the per-row span tables, and a per-band
draw loop — roughly 25 KB of DRAM and a good chunk of the frame budget.

The geometry still matters, it just is not visible directly. It decides where
the fluid can go, and the fluid is what you see.

Two things improved for free when the corners came in. Frame rate went from 69
to about 84 fps, because the rounded back covers fewer pixels. And measured
`rho` moved from 1.09 onto the 1.17 rest density: trimming the corners shrank
the volume enough that the same 900 particles genuinely reach rest density
instead of sitting under permanent tension.

### Previewing without flashing

`tools/preview` compiles the real `render.c` on the host against stub
`display.h` and `esp_log.h`, renders one frame, and writes a PNG:

```
tools/preview/build.sh        # empty box
tools/preview/build.sh 780    # with a placeholder block of fluid
```

Geometry changes are far quicker to judge this way than by flashing, and because
it is the actual renderer, what it produces is pixel for pixel what the panel
shows. The fluid it draws is a plain lattice standing in for the solver, so
ignore its shape — only the box around it is meaningful.

### Discs and depth

Projection is an ordinary pinhole camera:

```
s  = FOCAL / (FOCAL + z)
sx = cx + (x - cx) * s
sy = cy + (y - cy) * s
```

A particle at the glass (`z = 0`) has `s = 1`; one at the back of the case has
`s = 0.75`, so it is drawn smaller, pulled towards the centre of the screen, and
darkened.

Filled circles come from a small table of half-widths per radius, so drawing one
is a handful of `memset`-like row fills with no per-pixel maths.

Correct occlusion needs far particles drawn before near ones. That falls out for
free: the simulation's grid cells are numbered with depth as the most
significant axis, so sorting particles into cells also sorts them by depth, and
the renderer just walks the array backwards.

---

## 3. How the simulation works

The solver is **Clavet's double density relaxation** (*Particle-based Viscoelastic
Fluid Simulation*, 2005). It is position based rather than force based, which
means it stays stable even when you shake the board hard — a stiff spring model
would explode.

### One step

```
1. add gravity and the rotating-frame forces to every velocity
2. remember each position, then move by velocity * dt
3. rebuild the neighbour grid
4. compute densities, and apply viscosity      <- one pass over neighbours
5. push overlapping particles apart            <- second pass over neighbours
6. bounce anything that left the box
7. velocity = (new position - remembered position) / dt
```

Step 7 is the trick that makes it stable: velocity is never integrated from
forces, it is *measured* from how far the particle actually ended up moving. Any
correction the solver makes in step 5 automatically becomes real momentum, and
nothing can run away.

### Density and pressure

For each pair of neighbours closer than the smoothing radius `h`, with
`q = 1 - r/h`:

```
density      += q^2
near_density += q^3
```

Pressure is `K_PRESSURE * (density - rest_density)` and is **signed**: a particle
in a sparse region gets a negative pressure that pulls it back towards its
neighbours, which is what gives the fluid a cohesive surface and lets it form
droplets. Near-pressure is `K_NEAR_PRESSURE * near_density`, always positive, and
exists purely to stop particles collapsing into clumps — `q^3` falls off faster,
so it only bites at very short range.

Neighbours are then pushed apart by
`0.5 * dt^2 * ((P_i + P_j) * q + (Pn_i + Pn_j) * q^2)`.
Scaling by `dt^2` makes this behave like a real acceleration, so the fluid looks
the same whether it is running at 30 or 50 steps per second.

### Known artifact: occasional corner pops

Settled fluid pooled into a corner will now and then fling a couple of particles
apart. Corners are where density, and therefore pressure, is highest, and
because velocity is recovered from displacement, one oversized correction turns
straight into momentum.

Two safeguards for this were built and then deliberately backed out, because
they cost more than the artifact did:

- **Capping each particle's total correction** (rather than each pair's) removed
  the pops, but it required accumulating corrections and applying them at the
  end, turning the pass from Gauss-Seidel into Jacobi. In a dense corner the cap
  saturates constantly, and uniformly scaling a saturated correction makes the
  fluid lock up instead of resolving. Corners ended up looking worse than the
  occasional pop.
- **Landing particles a random fraction of a pixel inside a wall** instead of
  exactly on it. Clamping to the exact boundary gives every particle in a corner
  identical coordinates on two or three axes, and coincident particles produce a
  near-infinite density spike. This is a genuine trigger and the fix is cheap,
  but it went out together with the change above.

Both are worth revisiting if the pops ever become the bigger annoyance. The
per-particle cap is the effective one; the trick would be applying it without
giving up Gauss-Seidel.

### A caveat about the thin box

`calibrate_rest_density()` sums the kernel over an *unbounded* lattice, but the
box is only 75 px deep against a 28 px smoothing radius — roughly four particle
layers. Particles near the front or back glass are missing neighbours that the
calibration assumes are there, so the nominal rest density of 1.17 is not
something the confined geometry can reach everywhere.

This used to show up as `rho` settling around 1.09, leaving the fluid under
slight tension. Rounding the corners removed enough volume that 900 particles
now pack to roughly 1.17 anyway, so the two errors happen to cancel. That is
luck, not design: change the particle count or the box depth and the tension
comes back. The honest fix, if it ever needs to be exact, is to calibrate rest
density against the confined geometry instead of an infinite one.

`rest_density` is not a magic number. At startup `calibrate_rest_density()` sums
the kernel over a perfect lattice at `REST_SPACING`, so you can change the
spacing or the radius and the fluid still settles at the density you asked for.

### Finding neighbours

Naively every particle would test every other: 810,000 pairs. Instead the box is
divided into a uniform grid whose cells are *at least* one smoothing radius
across, so a particle's neighbours can only be in the 3x3x3 block around it.
That minimum is a correctness requirement, not an optimisation: cells any
smaller and the search silently misses neighbours. `GRID_CX/CY/CZ` in `sim.c` are
fixed counts, so changing a box dimension means rechecking them — `sim_init()`
logs an error if the invariant breaks.

Every step, particles are **counting-sorted into their cells and physically
reordered in memory**. That costs 0.5 ms and buys two things: cells become
contiguous ranges of array indices, and neighbours end up next to each other in
memory instead of scattered.

Two further details make the inner loop cheap:

- Cells differing only in x are adjacent in the numbering, so the 27-cell
  neighbourhood collapses into **9 contiguous index ranges** instead of 27
  lookups.
- Every particle in a cell has the *same* neighbourhood, so it is computed once
  per cell rather than once per particle.

Each pair is visited once (`j > i`) and both particles are updated, halving the
work.

### Why it runs at this speed

It did not at first. Every number below came off the serial log:

| Change | steps/s |
|---|---|
| First working version | 16 |
| Interleaved coordinates; neighbourhood computed per cell | 16 |
| Fused viscosity into the density pass; one substep | 38 |
| Raised pressure stiffness so the fluid stopped compressing | 45 |
| 1200 particles (too slow while shaking) | 23 |
| **900 particles, pressures precomputed** | **33-41** |

Replacing `sqrt` followed by a divide with a single reciprocal square root was
also tried and measured no faster, so it was reverted rather than kept as less
obvious code for no gain.

Two findings were worth more than the rest:

**Fusing viscosity into the density pass.** Walking the grid is the expensive
part, not the arithmetic; the walk alone was ~5 ms of each pass. Viscosity and
density need exactly the same pairs and the same square root, so they share one
walk. Viscosity normally acts on velocity *before* positions move, which would
conflict — but because step 7 recovers velocity from displacement, applying it
as a position offset of `impulse * dt` afterwards is mathematically identical.
The offsets accumulate in a separate buffer, since moving a particle mid-walk
would corrupt the densities still being summed.

**Stiffness is a performance setting.** With pressure too weak the fluid
compressed to 2.2x its rest density, which pulled more particles inside every
particle's radius and made each pass ~40% slower. Fixing the physics fixed the
frame rate at the same time. `rho` in the log should sit at `rest_density`; if it
drifts above, raise `K_PRESSURE`.

---

## 4. Motion

An accelerometer at rest does not measure gravity, it measures the *reaction* to
it, so the axis pointing at the ceiling reads +9.8. Low-pass filtering the signal
therefore isolates which way is up; whatever is left over is the board's own
acceleration.

- **Gravity** is the negated, normalised low-pass output.
- **Shake** is the remainder. In the box's own reference frame the contents feel
  the board's acceleration as a push the other way, which is why `SHAKE_GAIN`
  enters with a minus sign.
- **Rotation** uses the gyro to add the three pseudo-forces a spinning frame
  produces: centrifugal, Euler (from angular acceleration) and Coriolis. This is
  what makes the liquid swirl and lag when you twist the board rather than just
  slide.

The axis mapping was measured on this hardware and is recorded in `config.h`:

```
flat, screen up          -> (0, 0, -9.9)   up is IMU -z, so sim z =  az
upright, port right      -> (+10.6, 0, 0)  up is IMU +x, so sim y = -ax
right edge to the floor  -> (0, -9.6, 0)   up is IMU -y, so sim x =  ay
```

If the liquid ever runs the wrong way, flip a sign in `IMU_MAP_*`.

### Slow motion, honestly

Real water in a 3 cm box sloshes far too fast to see, and the droplets would be
sub-pixel. Rather than fudge gravity, the simulation keeps every physical
constant real (9.81 m/s^2, 322 pixels per inch) and simply runs the **clock**
slower via `TIME_SCALE`. Gravity, shake and rotation all scale together, so they
stay in proportion and the motion still reads as a real liquid — just a much
bigger, heavier-looking one.

`TIME_SCALE` doubles as the main stability control, because it sets the effective
timestep — and specifically it is `TIME_SCALE` divided by the step rate that
matters. Gravity adds velocity every step for the relaxation to cancel, and
whatever is left over shows up as a shimmer on the surface. Measured at 900
particles:

| `TIME_SCALE` | Settled speed | Feel |
|---|---|---|
| 0.135 | ~130 px/s | Never stops simmering |
| 0.115 | ~140 px/s | Lively, visible surface noise |
| **0.100** | ~67 px/s | The shipped value |
| 0.085 | ~65 px/s | Calmer still, but the motion looks sluggish |

(The settled speeds above were measured across several solver revisions, so
compare them as a trend rather than as exact figures.)

Raising the step rate would buy both at once, but at a fixed particle count the
solver is already saturated, so this is a genuine trade. Lower `TIME_SCALE` for a
calmer surface, raise it for snappier motion.

### The PWR button

A short press re-seeds the fluid. It is read from pin 4 of the TCA9554 IO
expander over I2C, debounced, and only fires on release if the press was shorter
than 1.5 s.

Holding PWR for six seconds is wired directly into the AXP2101 power chip and
cuts power in hardware — no firmware involved, and deliberately left alone. The
init code only ever changes the direction bit of pin 4; the other pins on that
expander hold the display and SD card resets, so their state is read and
preserved.

BOOT is untouched, so it keeps its recovery role.

---

## 5. Reading the log

```
90.2 fps | 37.9 steps/s | grid 524 dens 10832 relax 10475 us |
rho 1.17/1.17 | speed avg 66 max 181 | accel 1.10 0.12 -9.87 | sram 149459
```

| Field | Healthy | Meaning |
|---|---|---|
| `fps` | ~90 at rest | Display refresh; capped by QSPI bandwidth |
| `steps/s` | 40-45 | Simulation rate; the number to watch when tuning |
| `grid/dens/relax` | microseconds per pass | Where the simulation time goes. `dens` includes viscosity, since they share a pass |
| `rho` | ~1.17, matching rest | Drifting well *above* rest means `K_PRESSURE` is too low |
| `speed avg` | ~67 at rest, steady | Fluctuating when idle means jitter; lower `TIME_SCALE` |
| `speed max` | ~160 at rest | Occasional spikes are the corner pops noted above |
| `accel` | ~9.8 total | Raw IMU, for checking the axis mapping |
| `sram` | ~149 KB | Internal heap |

---

## 6. Tuning

Everything adjustable is in [`main/config.h`](main/config.h).

| Want | Change |
|---|---|
| More particles | `PARTICLE_COUNT`. Above `PARTICLE_MAX` (1000) the static arrays overflow internal DRAM at link time |
| Thicker, more syrupy | `VISC_SIGMA` up |
| Bouncier fluid | `WALL_RESTITUTION` up |
| Faster, more frantic | `TIME_SCALE` up — watch for simmering |
| More dramatic shakes | `SHAKE_GAIN` up |
| More white / whiter sooner | `SPEED_COLOR_MAX` down (settled is ~150, hard shake exceeds 10000) |
| More colour in gentle motion | `SPEED_COLOR_GAMMA` down |
| Deeper-looking box | `BOX_D` up, then recheck `GRID_CZ`; or `PROJ_FOCAL` down for stronger perspective at the same depth |
| Flatter, less contrasty depth | `DEPTH_DIM_MIN` up towards 1 |
| Matte particles instead of glossy | `HIGHLIGHT_ENABLE` to 0, or `HIGHLIGHT_LIFT` down |
| Higher frame rate | `PARTICLE_COUNT` down, or `HIGHLIGHT_ENABLE` to 0 |
| Rounder or squarer corners | `BOX_CORNER_MM`; `BOX_BACK_FILLET_MM` for the curve into the back panel |
| Dimmer screen | `DISPLAY_BRIGHTNESS`, 0-255 |
| Disable swirl | `ROTATION_GAIN` to 0 |

### Where the memory goes

Internal SRAM is the binding constraint, not flash. Roughly 150 bytes per
particle across the position, velocity, density and snapshot arrays, plus 41 KB
of band buffers. The static DRAM segment has its own ceiling separate from the
heap, which is why 1200 particles failed to link while 120 KB of heap was still
free.

---

## 7. Layout

| File | Role |
|---|---|
| `main/main.c` | Startup, and the two tasks |
| `main/config.h` | Every tunable constant |
| `main/display.c` | Panel bring-up, band buffers, DMA |
| `main/render.c` | Projection, colour tables, particle rasteriser |
| `main/sim.c` | The fluid solver |
| `main/imu.c` | QMI8658, gravity/shake separation |
| `main/button.c` | PWR via the IO expander |

The simulation runs pinned to core 1 and the renderer to core 0, so they
genuinely run at once. They meet only at a mutex around a published particle
snapshot, held just long enough for a 14 KB copy, so neither ever waits on the
other's frame.

`../tools/capture.py` reads the serial log non-interactively for a fixed number
of seconds, which is handy for scripting measurements:

```bash
python3 ../tools/capture.py 10
```

`../tools/preview` renders a frame on the host instead of the board, for judging
geometry without a flash cycle. See [Previewing without flashing](#previewing-without-flashing).
