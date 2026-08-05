#include "render.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "display.h"

// The panel takes RGB565 with the bytes the other way round from how the CPU
// stores a uint16, so every colour is byte swapped once, up front.
#define SWAP16(v) ((uint16_t)((((v) >> 8) & 0xFF) | (((v) & 0xFF) << 8)))

#define BAND_PIXELS (LCD_H_RES * BAND_ROWS)
#define MAX_EDGE_POINTS 4096

static uint16_t s_color_lut[DEPTH_LEVELS * SPEED_LEVELS];
static uint16_t s_highlight_lut[DEPTH_LEVELS * SPEED_LEVELS];

// Three wireframe shades: far face, struts, near face.
static uint16_t s_edge_shade[3];
static uint16_t s_back_fill;

// Projected bounds of the far wall, filled so the box interior is not black.
static int s_back_x0, s_back_y0, s_back_x1, s_back_y1;

// Half-width of a filled disc, indexed by radius then by row offset.
static uint8_t s_disc_span[DISC_MAX_R + 1][2 * DISC_MAX_R + 1];

// The box wireframe never moves, so its pixels are rasterised once at startup
// and bucketed by band. Drawing it is then just a memory walk.
static uint32_t s_edge_pts[MAX_EDGE_POINTS];  // packed as (y << 16) | x
static int s_edge_count;
static int s_edge_band_start[BAND_COUNT + 1];

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

    for (int i = 0; i < 3; i++) {
        // Far face at 35% of the near face, struts in between.
        const float k = 0.35f + 0.65f * ((float)i / 2.0f);
        s_edge_shade[i] = SWAP16(rgb565((int)(BOX_EDGE_NEAR_R * k),
                                        (int)(BOX_EDGE_NEAR_G * k),
                                        (int)(BOX_EDGE_NEAR_B * k)));
    }

    s_back_fill = SWAP16(rgb565(BOX_BACK_FILL_R, BOX_BACK_FILL_G, BOX_BACK_FILL_B));
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

// Points pack as (shade << 30) | (y << 16) | x. x and y need 9 bits each, so
// the top two bits are free to carry which of the three wireframe shades the
// pixel uses.
#define EDGE_PACK(shade, x, y) (((uint32_t)(shade) << 30) | ((uint32_t)(y) << 16) | (uint32_t)(x))
#define EDGE_SHADE(p) ((p) >> 30)
#define EDGE_Y(p) (((p) >> 16) & 0x1FF)
#define EDGE_X(p) ((p) & 0x1FF)

static void edge_point(int x, int y, int shade)
{
    if (s_edge_count >= MAX_EDGE_POINTS) {
        return;
    }
    if (x < 0 || x >= LCD_H_RES || y < 0 || y >= LCD_V_RES) {
        return;
    }
    s_edge_pts[s_edge_count++] = EDGE_PACK(shade, x, y);
}

static void raster_line(int x0, int y0, int x1, int y1, int shade)
{
    const int dx = abs(x1 - x0);
    const int dy = -abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1;
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        edge_point(x0, y0, shade);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
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

// Twelve edges of the box. The front face lands exactly on the screen border;
// the back face is pulled towards the centre by the perspective divide, and
// the four struts between them read as depth.
static void build_box_edges(void)
{
    static const float corner[8][3] = {
        {0.0f, 0.0f, 0.0f},    {BOX_W, 0.0f, 0.0f},
        {BOX_W, BOX_H, 0.0f},  {0.0f, BOX_H, 0.0f},
        {0.0f, 0.0f, BOX_D},   {BOX_W, 0.0f, BOX_D},
        {BOX_W, BOX_H, BOX_D}, {0.0f, BOX_H, BOX_D},
    };
    // Third column is the shade: 0 far, 1 struts, 2 near.
    static const int edge[12][3] = {
        {0, 1, 2}, {1, 2, 2}, {2, 3, 2}, {3, 0, 2},  // near face
        {4, 5, 0}, {5, 6, 0}, {6, 7, 0}, {7, 4, 0},  // far face
        {0, 4, 1}, {1, 5, 1}, {2, 6, 1}, {3, 7, 1},  // struts
    };

    int px[8], py[8];
    for (int i = 0; i < 8; i++) {
        float scale;
        project(corner[i][0], corner[i][1], corner[i][2], &px[i], &py[i], &scale);
        // Keep the front face inside the panel rather than one pixel past it.
        if (px[i] >= LCD_H_RES) px[i] = LCD_H_RES - 1;
        if (py[i] >= LCD_V_RES) py[i] = LCD_V_RES - 1;
    }

    // Corner 4 is the far top-left and corner 6 the far bottom-right.
    s_back_x0 = px[4];
    s_back_y0 = py[4];
    s_back_x1 = px[6];
    s_back_y1 = py[6];

    s_edge_count = 0;
    for (int e = 0; e < 12; e++) {
        raster_line(px[edge[e][0]], py[edge[e][0]], px[edge[e][1]], py[edge[e][1]],
                    edge[e][2]);
    }

    // Counting sort the points into bands so each band can draw its slice
    // without scanning the whole list.
    int count[BAND_COUNT] = {0};
    for (int i = 0; i < s_edge_count; i++) {
        count[EDGE_Y(s_edge_pts[i]) / BAND_ROWS]++;
    }
    s_edge_band_start[0] = 0;
    for (int b = 0; b < BAND_COUNT; b++) {
        s_edge_band_start[b + 1] = s_edge_band_start[b] + count[b];
    }

    static uint32_t sorted[MAX_EDGE_POINTS];
    int cursor[BAND_COUNT];
    for (int b = 0; b < BAND_COUNT; b++) {
        cursor[b] = s_edge_band_start[b];
    }
    for (int i = 0; i < s_edge_count; i++) {
        const int band = (int)(EDGE_Y(s_edge_pts[i]) / BAND_ROWS);
        sorted[cursor[band]++] = s_edge_pts[i];
    }
    memcpy(s_edge_pts, sorted, sizeof(uint32_t) * (size_t)s_edge_count);
}

void render_init(void)
{
    build_color_lut();
    build_disc_spans();
    build_box_edges();
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

        memset(buf, 0, BAND_PIXELS * sizeof(uint16_t));

        // The far wall, so the box has an interior rather than a void behind
        // the fluid.
        {
            int fy0 = s_back_y0 > band_y0 ? s_back_y0 : band_y0;
            int fy1 = s_back_y1 < band_y1 - 1 ? s_back_y1 : band_y1 - 1;
            for (int y = fy0; y <= fy1; y++) {
                uint16_t *row = buf + (y - band_y0) * LCD_H_RES;
                for (int x = s_back_x0; x <= s_back_x1; x++) {
                    row[x] = s_back_fill;
                }
            }
        }

        for (int e = s_edge_band_start[band]; e < s_edge_band_start[band + 1]; e++) {
            const uint32_t p = s_edge_pts[e];
            const int y = (int)EDGE_Y(p) - band_y0;
            buf[y * LCD_H_RES + (int)EDGE_X(p)] = s_edge_shade[EDGE_SHADE(p)];
        }

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
