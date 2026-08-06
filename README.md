# ESP32 S3 FluidBox

<img src="assets/hero.gif" alt="FluidBox hero">

A 3D particle fluid living inside the case of a [Waveshare ESP32-S3-Touch-AMOLED-1.8](https://www.waveshare.com/esp32-s3-touch-amoled-1.8.htm). Tilt the board and the liquid pours; shake it and it sprays white. The screen is the front wall of a glass box, and the depth you see receding into the display is the real thickness of the enclosure.

Measured on hardware: **900 particles, ~100 fps display, ~38 simulation steps/s.**

## What this is

Bare ESP-IDF (no Arduino, no UI toolkit): a software rasterizer draws particles straight into DMA'd display bands, an IMU feed splits into gravity/shake/rotation, and a position-based SPH solver (Clavet's double density relaxation) moves the fluid. A short press of the case's PWR button re-seeds the fluid; holding it is wired directly into the power chip and always works.

## How it works

- **Rendering** — no framebuffer. The panel is painted in 16 horizontal bands that live in internal SRAM and go out over DMA while the next band is drawn, so nothing needs PSRAM. Colour is a lookup table indexed by depth and speed, so drawing a particle is one table read.
- **3D without a wireframe** — nothing is drawn but the particles. Perspective (a short focal length), depth darkening, and a specular highlight on each disc are the only depth cues, and the fluid itself traces the case's rounded corners and fillets.
- **Simulation** — particles are counting-sorted into a uniform grid each step so only nearby pairs are tested, and the same neighbour pairs are found once and reused by both solver passes. Position-based relaxation (rather than force integration) keeps it stable even under a hard shake.
- **Motion** — the IMU's low-pass output gives gravity's direction; the high-frequency remainder is shake; the gyro adds the centrifugal/Coriolis terms that make the fluid swirl when the board twists.

The full technical write-up — geometry, solver internals, performance numbers, and every tunable in [`config.h`](fluidbox/main/config.h) — is in [`fluidbox/README.md`](fluidbox/README.md).

## Running it

```bash
. ~/esp/esp-idf/export.sh   # ESP-IDF v5.5.5, see fluidbox/README.md for first-time setup
cd fluidbox
idf.py -p /dev/cu.usbmodem101 flash monitor
```

## Layout

| Path | Contents |
|---|---|
| `fluidbox/` | The ESP-IDF project: display, IMU, solver, renderer |
| `tools/capture.py` | Non-interactive serial log capture, for scripting measurements |
| `tools/preview/` | Host-side build of the real renderer, for judging geometry without flashing |

## License

[MIT](LICENSE)
