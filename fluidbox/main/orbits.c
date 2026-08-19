#include "orbits.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define SWAP16(v) ((uint16_t)((((v) >> 8) & 0xFF) | (((v) & 0xFF) << 8)))

#define PLANETS 12
#define SUN_R 16
#define MU 5200000.0f     // gravitational parameter, px^3/s^2-ish: sets orbit pace
#define SOFTEN 900.0f     // r^2 floor so a grazing pass slingshots, not explodes
#define O_MAX_SPEED 1400.0f
#define O_TILT 0.25f      // fraction of ball-gravity applied as uniform drift
#define DT_MAX 0.02f

typedef struct {
    float x, y, vx, vy;
    uint8_t r;      // 3..6 px
    uint8_t color;
} planet_t;

static planet_t s_p[PLANETS];
static float s_sun_x = LCD_H_RES / 2.0f;
static float s_sun_y = LCD_V_RES / 2.0f;

static portMUX_TYPE s_touch_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    int16_t x, y;
    bool active;
} s_touch;

static SemaphoreHandle_t s_view_lock;
static struct {
    int16_t px[PLANETS], py[PLANETS];
    int16_t sx, sy;
} s_view;

#define PALETTE_N 5
static uint16_t s_colors[PALETTE_N];

static uint16_t rgb565s(int r, int g, int b)
{
    const uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return SWAP16(v);
}

void orbits_touch(int16_t x, int16_t y, bool pressed)
{
    portENTER_CRITICAL(&s_touch_mux);
    s_touch.x = x;
    s_touch.y = y;
    s_touch.active = pressed;
    portEXIT_CRITICAL(&s_touch_mux);
}

esp_err_t orbits_init(void)
{
    s_view_lock = xSemaphoreCreateMutex();
    if (s_view_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_colors[0] = rgb565s(0, 255, 200);
    s_colors[1] = rgb565s(255, 0, 180);
    s_colors[2] = rgb565s(120, 160, 255);
    s_colors[3] = rgb565s(255, 220, 90);
    s_colors[4] = rgb565s(180, 255, 120);

    // Ring of planets on near-circular orbits at varied radii.
    for (int i = 0; i < PLANETS; i++) {
        const float ang = (float)i * (2.0f * (float)M_PI / PLANETS);
        const float rad = 60.0f + (float)(rand() % 120);
        s_p[i].x = s_sun_x + rad * cosf(ang);
        s_p[i].y = s_sun_y + rad * sinf(ang);
        const float v = sqrtf(MU / rad);
        s_p[i].vx = -v * sinf(ang);
        s_p[i].vy = v * cosf(ang);
        s_p[i].r = (uint8_t)(3 + rand() % 4);
        s_p[i].color = (uint8_t)(rand() % PALETTE_N);
    }
    return ESP_OK;
}

void orbits_step(float dt, const sim_forces_t *forces)
{
    if (dt > DT_MAX) dt = DT_MAX;
    const float scale =
        BALLS_GRAVITY / (GRAVITY_GAIN * GRAVITY_MPS2 * PX_PER_METER) * O_TILT;
    const float gx = forces->gravity[0] * scale;
    const float gy = forces->gravity[1] * scale;

    portENTER_CRITICAL(&s_touch_mux);
    const bool touch_on = s_touch.active;
    const float tx = s_touch.x, ty = s_touch.y;
    portEXIT_CRITICAL(&s_touch_mux);
    if (touch_on) {
        // The finger drags the sun; a light chase keeps it from teleporting.
        s_sun_x += (tx - s_sun_x) * 0.35f;
        s_sun_y += (ty - s_sun_y) * 0.35f;
    }

    for (int i = 0; i < PLANETS; i++) {
        planet_t *p = &s_p[i];
        const float dx = s_sun_x - p->x;
        const float dy = s_sun_y - p->y;
        const float r2 = dx * dx + dy * dy + SOFTEN;
        const float inv_r = 1.0f / sqrtf(r2);
        const float a = MU / r2;
        p->vx += (dx * inv_r * a + gx) * dt;
        p->vy += (dy * inv_r * a + gy) * dt;

        const float sp2 = p->vx * p->vx + p->vy * p->vy;
        if (sp2 > O_MAX_SPEED * O_MAX_SPEED) {
            const float s = O_MAX_SPEED / sqrtf(sp2);
            p->vx *= s;
            p->vy *= s;
        }
        p->x += p->vx * dt;
        p->y += p->vy * dt;

        // Soft walls: an escapee bounces back into play.
        if (p->x < p->r) { p->x = p->r; p->vx = -p->vx * 0.9f; }
        if (p->x > BOX_W - p->r) { p->x = BOX_W - p->r; p->vx = -p->vx * 0.9f; }
        if (p->y < p->r) { p->y = p->r; p->vy = -p->vy * 0.9f; }
        if (p->y > BOX_H - p->r) { p->y = BOX_H - p->r; p->vy = -p->vy * 0.9f; }
    }

    xSemaphoreTake(s_view_lock, portMAX_DELAY);
    for (int i = 0; i < PLANETS; i++) {
        s_view.px[i] = (int16_t)s_p[i].x;
        s_view.py[i] = (int16_t)s_p[i].y;
    }
    s_view.sx = (int16_t)s_sun_x;
    s_view.sy = (int16_t)s_sun_y;
    xSemaphoreGive(s_view_lock);
}

static void draw_disc(uint16_t *buf, int band_y0, int cx, int cy, int r,
                      uint16_t color)
{
    const int y_lo = cy - r < band_y0 ? band_y0 : cy - r;
    const int y_hi = cy + r >= band_y0 + BAND_ROWS ? band_y0 + BAND_ROWS - 1 : cy + r;
    for (int y = y_lo; y <= y_hi; y++) {
        const int dy = y - cy;
        const int hw = (int)sqrtf((float)(r * r - dy * dy));
        int x0 = cx - hw, x1 = cx + hw;
        if (x0 < 0) x0 = 0;
        if (x1 >= LCD_H_RES) x1 = LCD_H_RES - 1;
        uint16_t *row = buf + (y - band_y0) * LCD_H_RES;
        for (int x = x0; x <= x1; x++) {
            row[x] = color;
        }
    }
}

void orbits_render_frame(void)
{
    static int16_t px[PLANETS], py[PLANETS];
    int16_t sx, sy;
    xSemaphoreTake(s_view_lock, portMAX_DELAY);
    memcpy(px, s_view.px, sizeof(px));
    memcpy(py, s_view.py, sizeof(py));
    sx = s_view.sx;
    sy = s_view.sy;
    xSemaphoreGive(s_view_lock);

    const uint16_t sun_core = rgb565s(255, 235, 120);
    const uint16_t sun_glow = rgb565s(120, 80, 10);

    for (int band = 0; band < BAND_COUNT; band++) {
        const int y0 = band * BAND_ROWS;
        uint16_t *buf = display_acquire_band();
        memset(buf, 0, (size_t)LCD_H_RES * BAND_ROWS * sizeof(uint16_t));

        draw_disc(buf, y0, sx, sy, SUN_R + 4, sun_glow);
        draw_disc(buf, y0, sx, sy, SUN_R, sun_core);
        for (int i = 0; i < PLANETS; i++) {
            draw_disc(buf, y0, px[i], py[i], s_p[i].r, s_colors[s_p[i].color]);
        }
        display_flush_band(band, buf);
    }
}
