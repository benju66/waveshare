// Renders one FluidBox frame on the host and writes it to stdout as a PPM.
//
// This compiles the real render.c, so what you see is exactly what the panel
// would show. It exists so box geometry — corner radius, perspective, the
// wireframe, the back wall — can be checked and tuned without a flash cycle.
//
// Usage: preview [particles] > frame.ppm
//
// With 0 particles you get the empty box, which is the clearest way to look at
// the frame itself.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "render.h"
#include "sim.h"

#define BAND_PIXELS (LCD_H_RES * BAND_ROWS)

static uint16_t s_bands[2][BAND_PIXELS];
static uint16_t s_frame[LCD_V_RES * LCD_H_RES];

static int s_particle_count;

void display_wait_buffer(int band_index)
{
    (void)band_index;
}

uint16_t *display_band_buffer(int band_index)
{
    return s_bands[band_index & 1];
}

int display_flush_band(int band_index, const uint16_t *buffer)
{
    memcpy(&s_frame[band_index * BAND_ROWS * LCD_H_RES], buffer,
           BAND_PIXELS * sizeof(uint16_t));
    return 0;
}

// Stands in for the solver: a lattice filling the lower part of the box, which
// is enough to show where the fluid sits relative to the walls.
int sim_snapshot(sim_particle_view_t *out, int max)
{
    const int n = s_particle_count < max ? s_particle_count : max;
    const int per_row = 20;
    const int per_layer = per_row * 3;

    for (int i = 0; i < n; i++) {
        const int layer = i / per_layer;
        const int rem = i % per_layer;
        out[i].x = 20.0f + (float)(rem % per_row) * 17.0f;
        out[i].y = BOX_H - 20.0f - (float)layer * 17.0f;
        out[i].z = 10.0f + (float)(rem / per_row) * 22.0f;
        out[i].speed = 60.0f;
    }
    return n;
}

int main(int argc, char **argv)
{
    s_particle_count = (argc > 1) ? atoi(argv[1]) : 0;

    render_init();
    render_frame();

    printf("P6\n%d %d\n255\n", LCD_H_RES, LCD_V_RES);
    for (int i = 0; i < LCD_V_RES * LCD_H_RES; i++) {
        // The renderer stores RGB565 byte-swapped for the panel, so undo that
        // before unpacking.
        const uint16_t v = (uint16_t)((s_frame[i] >> 8) | (s_frame[i] << 8));
        const unsigned char rgb[3] = {
            (unsigned char)(((v >> 11) & 0x1F) * 255 / 31),
            (unsigned char)(((v >> 5) & 0x3F) * 255 / 63),
            (unsigned char)((v & 0x1F) * 255 / 31),
        };
        fwrite(rgb, 1, 3, stdout);
    }
    return 0;
}
