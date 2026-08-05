#include "render.h"

#include <math.h>
#include <string.h>

#include "config.h"
#include "display.h"

// The panel takes RGB565 with the bytes the other way round from how the CPU
// stores a uint16, so every colour is byte swapped once, up front.
#define SWAP16(v) ((uint16_t)((((v) >> 8) & 0xFF) | (((v) & 0xFF) << 8)))

#define BAND_PIXELS (LCD_H_RES * BAND_ROWS)

static uint16_t s_color_lut[DEPTH_LEVELS * SPEED_LEVELS];
static uint16_t s_highlight_lut[DEPTH_LEVELS * SPEED_LEVELS];

// Half-width of a filled disc, indexed by radius then by row offset.
static uint8_t s_disc_span[DISC_MAX_R + 1][2 * DISC_MAX_R + 1];

// Projected particles for the current frame.
static sim_particle_view_t s_snapshot[PARTICLE_MAX];
static int16_t s_sx[PARTICLE_MAX];
static int16_t s_sy[PARTICLE_MAX];
static uint8_t s_sr[PARTICLE_MAX];
static uint16_t s_sc[PARTICLE_MAX];
static uint16_t s_sh[PARTICLE_MAX];

static inline uint16_t rgb565(int r, int g, int b)
{
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Speed ramp: still water is deep blue and brightens through lighter blues as
// it moves. White is the top of the ramp only, so it reads as spray thrown off
// by a genuinely hard shake rather than as ordinary motion.
#define RAMP_STOPS 4

static void speed_color(float t, int *r, int *g, int *b)
{
    static const float stop_t[RAMP_STOPS] = {0.00f, 0.45f, 0.78f, 1.00f};
    static const int stop_c[RAMP_STOPS][3] = {
        {10, 45, 165},
        {40, 125, 235},
        {150, 205, 250},
        {255, 255, 255},
    };

    if (t <= 0.0f) {
        *r = stop_c[0][0]; *g = stop_c[0][1]; *b = stop_c[0][2];
        return;
    }
    if (t >= 1.0f) {
        *r = stop_c[RAMP_STOPS - 1][0];
        *g = stop_c[RAMP_STOPS - 1][1];
        *b = stop_c[RAMP_STOPS - 1][2];
        return;
    }

    for (int s = 0; s < RAMP_STOPS - 1; s++) {
        if (t <= stop_t[s + 1]) {
            const float f = (t - stop_t[s]) / (stop_t[s + 1] - stop_t[s]);
            *r = (int)(stop_c[s][0] + f * (stop_c[s + 1][0] - stop_c[s][0]));
            *g = (int)(stop_c[s][1] + f * (stop_c[s + 1][1] - stop_c[s][1]));
            *b = (int)(stop_c[s][2] + f * (stop_c[s + 1][2] - stop_c[s][2]));
            return;
        }
    }
}

// One table covering every combination of depth and speed, so per-particle
// colouring is a single array read with no float maths.
static void build_color_lut(void)
{
    for (int d = 0; d < DEPTH_LEVELS; d++) {
        // Particles further into the case are darker, which is most of what
        // sells the depth on a flat screen.
        const float depth_t = (DEPTH_LEVELS > 1) ? (float)d / (float)(DEPTH_LEVELS - 1) : 0.0f;
        const float dim = DEPTH_DIM_MIN + (1.0f - DEPTH_DIM_MIN) * (1.0f - depth_t);

        for (int s = 0; s < SPEED_LEVELS; s++) {
            const float linear =
                (SPEED_LEVELS > 1) ? (float)s / (float)(SPEED_LEVELS - 1) : 0.0f;
            // The curve lives in the table, so the per-particle lookup stays a
            // plain multiply.
            const float speed_t = powf(linear, SPEED_COLOR_GAMMA);

            int r, g, b;
            speed_color(speed_t, &r, &g, &b);
            s_color_lut[d * SPEED_LEVELS + s] =
                SWAP16(rgb565((int)(r * dim), (int)(g * dim), (int)(b * dim)));

            // The specular dot: the same colour lifted towards white, then
            // dimmed by depth like everything else.
            const float lift = HIGHLIGHT_LIFT;
            const int hr = (int)((r + (255 - r) * lift) * dim);
            const int hg = (int)((g + (255 - g) * lift) * dim);
            const int hb = (int)((b + (255 - b) * lift) * dim);
            s_highlight_lut[d * SPEED_LEVELS + s] = SWAP16(rgb565(hr, hg, hb));
        }
    }

}

static void build_disc_spans(void)
{
    for (int r = 0; r <= DISC_MAX_R; r++) {
        for (int dy = -r; dy <= r; dy++) {
            const float w = sqrtf((float)(r * r - dy * dy));
            s_disc_span[r][dy + r] = (uint8_t)(w + 0.5f);
        }
    }
}

static inline void project(float x, float y, float z, int *out_x, int *out_y, float *out_scale)
{
    const float s = PROJ_FOCAL / (PROJ_FOCAL + z);
    *out_x = (int)((BOX_W * 0.5f) + (x - BOX_W * 0.5f) * s + 0.5f);
    *out_y = (int)((BOX_H * 0.5f) + (y - BOX_H * 0.5f) * s + 0.5f);
    *out_scale = s;
}

void render_init(void)
{
    build_color_lut();
    build_disc_spans();
}

static inline void draw_disc(uint16_t *buf, int band_y0, int cx, int cy, int r, uint16_t color)
{
    if (r < 1) r = 1;
    if (r > DISC_MAX_R) r = DISC_MAX_R;

    const uint8_t *spans = s_disc_span[r];

    int dy0 = -r;
    int dy1 = r;
    // Clip vertically to the band before touching any pixels.
    if (cy + dy0 < band_y0) dy0 = band_y0 - cy;
    if (cy + dy1 >= band_y0 + BAND_ROWS) dy1 = band_y0 + BAND_ROWS - 1 - cy;

    for (int dy = dy0; dy <= dy1; dy++) {
        const int hw = spans[dy + r];
        int x0 = cx - hw;
        int x1 = cx + hw;
        if (x0 < 0) x0 = 0;
        if (x1 >= LCD_H_RES) x1 = LCD_H_RES - 1;
        if (x0 > x1) {
            continue;
        }

        uint16_t *row = buf + (cy + dy - band_y0) * LCD_H_RES;
        for (int x = x0; x <= x1; x++) {
            row[x] = color;
        }
    }
}

static void project_all(int n)
{
    const float speed_scale = (float)(SPEED_LEVELS - 1) / SPEED_COLOR_MAX;
    const float depth_scale = (float)(DEPTH_LEVELS - 1) / BOX_D;

    for (int i = 0; i < n; i++) {
        int sx, sy;
        float scale;
        project(s_snapshot[i].x, s_snapshot[i].y, s_snapshot[i].z, &sx, &sy, &scale);

        s_sx[i] = (int16_t)sx;
        s_sy[i] = (int16_t)sy;

        int r = (int)(PARTICLE_RADIUS_PX * scale + 0.5f);
        if (r < 1) r = 1;
        if (r > DISC_MAX_R) r = DISC_MAX_R;
        s_sr[i] = (uint8_t)r;

        int sl = (int)(s_snapshot[i].speed * speed_scale);
        if (sl < 0) sl = 0;
        if (sl > SPEED_LEVELS - 1) sl = SPEED_LEVELS - 1;

        int dl = (int)(s_snapshot[i].z * depth_scale + 0.5f);
        if (dl < 0) dl = 0;
        if (dl > DEPTH_LEVELS - 1) dl = DEPTH_LEVELS - 1;

        const int lut = dl * SPEED_LEVELS + sl;
        s_sc[i] = s_color_lut[lut];
        s_sh[i] = s_highlight_lut[lut];
    }
}

void render_frame(void)
{
    const int n = sim_snapshot(s_snapshot, PARTICLE_MAX);
    project_all(n);

    for (int band = 0; band < BAND_COUNT; band++) {
        const int band_y0 = band * BAND_ROWS;
        const int band_y1 = band_y0 + BAND_ROWS;

        display_wait_buffer(band);
        uint16_t *buf = display_band_buffer(band);

        // Nothing is drawn but the fluid: no box, no wall, no frame. The
        // enclosure exists only in the physics, and reads purely from how the
        // particles pile up against it.
        memset(buf, 0, BAND_PIXELS * sizeof(uint16_t));

        // The simulation publishes particles sorted by depth ascending, so
        // walking backwards paints far particles first and near ones over the
        // top of them.
        for (int i = n - 1; i >= 0; i--) {
            const int r = s_sr[i];
            const int cy = s_sy[i];
            if (cy + r < band_y0 || cy - r >= band_y1) {
                continue;
            }
            draw_disc(buf, band_y0, s_sx[i], cy, r, s_sc[i]);

#if HIGHLIGHT_ENABLE
            // Offset up and left, so every particle is lit from the same
            // direction and the whole body of fluid looks rounded.
            if (r >= 3) {
                draw_disc(buf, band_y0, s_sx[i] - r / 3, cy - r / 3, r / 2, s_sh[i]);
            }
#endif
        }

        display_flush_band(band, buf);
    }
}
