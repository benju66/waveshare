# ESP32 S3 FluidBox

<img src="assets/hero.gif" alt="FluidBox hero">

A 3D particle fluid living inside the case of a [Waveshare ESP32-S3-Touch-AMOLED-1.8](https://www.waveshare.com/esp32-s3-touch-amoled-1.8.htm). The screen is the front wall of a glass box, and the depth you see receding into the display is the real thickness of the enclosure.

Measured on hardware: **900 particles, ~100 fps display, ~38 simulation steps/s.**

## What this is

This repo is custom firmware for the Waveshare ESP32-S3 Touch AMOLED board. Hundreds of glowing blue particles slosh around inside a virtual box shaped like the device itself — move, tilt, or shake the board and they follow, as if liquid were trapped behind the screen.

This repo contains:
- A **fluid simulation** that runs on the ESP32-S3 in real time
- A **renderer** that draws each particle with perspective, depth, and velocity-based colour
- **IMU integration** so gravity, shake, and rotation all affect the fluid

Press the case's **PWR** button briefly to reset the simulation. Holding PWR still powers the device off as usual.

## How it works

- **Rendering** — the display is drawn in horizontal strips and sent out while the next strip is being prepared. Particle colour comes from a precomputed table based on speed and depth.
- **Simulation** — particles push each other apart when too close and pull together when too far, which keeps the motion stable even when you shake the board hard.
- **Motion** — the onboard accelerometer and gyroscope tell the simulation which way is down and how the board is moving.

For geometry, solver internals, performance numbers, and every tunable constant, see [`fluidbox/README.md`](fluidbox/README.md).

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
