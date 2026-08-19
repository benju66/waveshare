#include "balls.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "display.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// The panel takes byte-swapped RGB565, same as render.c.
#define SWAP16(v) ((uint16_t)((((v) >> 8) & 0xFF) | (((v) & 0xFF) << 8)))

#define SUBSTEPS 2
#define DT_MAX 0.02f

// Gravity vector scale: forces->gravity arrives in the fluid's slow-motion
// px/s^2 (GRAVITY_GAIN * g * PX_PER_METER at rest); rescale so 1 g maps to
// BALLS_GRAVITY while shake keeps its proportional punch.
#define FORCE_SCALE (BALLS_GRAVITY / (GRAVITY_GAIN * GRAVITY_MPS2 * PX_PER_METER))

// The finger, as physics: an immovable disc with the finger's velocity.
#define FINGER_R 26.0f
// Impact speed (px/s of closing velocity) past which a marble flashes.
#define FLASH_SPEED 900.0f
#define FLASH_STEPS 5

typedef struct {
    float x, y;
    float vx, vy;
    float r;
    float inv_m;  // 1/mass, mass ~ r^2: big marbles barge small ones aside
    uint8_t color;
    uint8_t flash;  // steps of impact flash remaining
} ball_t;

typedef struct {
    int16_t x, y;
    uint8_t r;
    uint8_t color;
    uint8_t flash;
} ball_view_t;

static ball_t s_balls[BALLS_COUNT];
static ball_view_t s_view[BALLS_COUNT];
static SemaphoreHandle_t s_view_lock;

static portMUX_TYPE s_touch_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    float x, y;
    float vx, vy;
    bool active;
} s_touch;

void balls_touch(int16_t x, int16_t y, bool pressed)
{
    static int16_t last_x, last_y;
    static int64_t last_us;

    const int64_t now = esp_timer_get_time();
    float vx = 0, vy = 0;
    if (pressed && last_us != 0 && now > last_us) {
        const float dt = (float)(now - last_us) / 1e6f;
        if (dt < 0.25f) {  // stale gaps carry no velocity
            vx = (float)(x - last_x) / dt;
            vy = (float)(y - last_y) / dt;
        }
    }
    last_x = x;
    last_y = y;
    last_us = pressed ? now : 0;

    portENTER_CRITICAL(&s_touch_mux);
    s_touch.x = x;
    s_touch.y = y;
    // Light smoothing keeps a jittery sample from reading as a violent flick.
    s_touch.vx = s_touch.active ? s_touch.vx * 0.5f + vx * 0.5f : vx;
    s_touch.vy = s_touch.active ? s_touch.vy * 0.5f + vy * 0.5f : vy;
    s_touch.active = pressed;
    portEXIT_CRITICAL(&s_touch_mux);
}

// Marble body colors and their top-left glint, pre-swapped for the panel.
#define PALETTE_N 6
static uint16_t s_body[PALETTE_N];
static uint16_t s_glint[PALETTE_N];

static uint16_t rgb565s(int r, int g, int b)
{
    const uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return SWAP16(v);
}

static void palette_init(void)
{
    static const int base[PALETTE_N][3] = {
        {220, 60, 50},   // red
        {245, 160, 30},  // amber
        {60, 180, 90},   // green
        {60, 130, 235},  // blue
        {170, 90, 220},  // purple
        {230, 230, 235}, // pearl
    };
    for (int i = 0; i < PALETTE_N; i++) {
        s_body[i] = rgb565s(base[i][0], base[i][1], base[i][2]);
        s_glint[i] = rgb565s((base[i][0] + 255 * 2) / 3, (base[i][1] + 255 * 2) / 3,
                             (base[i][2] + 255 * 2) / 3);
    }
}

esp_err_t balls_init(void)
{
    s_view_lock = xSemaphoreCreateMutex();
    if (s_view_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    palette_init();

    // Scatter across the upper half with no velocity: they rain in, sort
    // themselves out, and settle. A nice entrance every boot.
    for (int i = 0; i < BALLS_COUNT; i++) {
        s_balls[i].r = (float)(BALLS_R_MIN + rand() % (BALLS_R_MAX - BALLS_R_MIN + 1));
        s_balls[i].inv_m = 1.0f / (s_balls[i].r * s_balls[i].r);
        s_balls[i].x = s_balls[i].r + (float)(rand() % (int)(BOX_W - 2 * s_balls[i].r));
        s_balls[i].y = s_balls[i].r + (float)(rand() % (int)(BOX_H / 2));
        s_balls[i].vx = 0;
        s_balls[i].vy = 0;
        s_balls[i].color = (uint8_t)(rand() % PALETTE_N);
        s_balls[i].flash = 0;
    }
    return ESP_OK;
}

// Keeps a ball inside the rounded box, reflecting the outward velocity
// component. Corner arcs use the same radius the fluid's box does.
static void wall_collide(ball_t *b)
{
    const float R = BOX_CORNER_R;
    const float e = BALLS_WALL_RESTITUTION;

    // Corner quadrants first: the ball center must stay within R - r of the
    // corner arc's center.
    const float lim = R - b->r;
    const struct { float cx, cy; } corners[4] = {
        {R, R}, {BOX_W - R, R}, {R, BOX_H - R}, {BOX_W - R, BOX_H - R},
    };
    for (int c = 0; c < 4; c++) {
        const bool in_x = (c & 1) ? b->x > corners[c].cx : b->x < corners[c].cx;
        const bool in_y = (c & 2) ? b->y > corners[c].cy : b->y < corners[c].cy;
        if (!in_x || !in_y) {
            continue;
        }
        const float dx = b->x - corners[c].cx;
        const float dy = b->y - corners[c].cy;
        const float d2 = dx * dx + dy * dy;
        if (d2 > lim * lim && d2 > 1e-6f) {
            const float d = sqrtf(d2);
            const float nx = dx / d, ny = dy / d;
            b->x = corners[c].cx + nx * lim;
            b->y = corners[c].cy + ny * lim;
            const float vn = b->vx * nx + b->vy * ny;
            if (vn > 0) {
                b->vx -= (1 + e) * vn * nx;
                b->vy -= (1 + e) * vn * ny;
            }
        }
        return;  // a ball is in at most one corner quadrant
    }

    // Straight walls.
    if (b->x < b->r) {
        b->x = b->r;
        if (b->vx < 0) b->vx = -b->vx * e;
    } else if (b->x > BOX_W - b->r) {
        b->x = BOX_W - b->r;
        if (b->vx > 0) b->vx = -b->vx * e;
    }
    if (b->y < b->r) {
        b->y = b->r;
        if (b->vy < 0) b->vy = -b->vy * e;
    } else if (b->y > BOX_H - b->r) {
        b->y = BOX_H - b->r;
        if (b->vy > 0) b->vy = -b->vy * e;
    }
}

static void substep(float h, float ax, float ay)
{
    for (int i = 0; i < BALLS_COUNT; i++) {
        ball_t *b = &s_balls[i];
        b->vx = (b->vx + ax * h) * BALLS_DRAG;
        b->vy = (b->vy + ay * h) * BALLS_DRAG;
        const float sp2 = b->vx * b->vx + b->vy * b->vy;
        if (sp2 > BALLS_MAX_SPEED * BALLS_MAX_SPEED) {
            const float s = BALLS_MAX_SPEED / sqrtf(sp2);
            b->vx *= s;
            b->vy *= s;
        }
        b->x += b->vx * h;
        b->y += b->vy * h;
        wall_collide(b);
    }

    // Pairwise collisions: ~4000 pairs, trivial next to the fluid's solver.
    // Mass goes as r^2, so a big marble barges small ones aside.
    for (int i = 0; i < BALLS_COUNT; i++) {
        for (int j = i + 1; j < BALLS_COUNT; j++) {
            ball_t *a = &s_balls[i];
            ball_t *b = &s_balls[j];
            const float dx = b->x - a->x;
            const float dy = b->y - a->y;
            const float rsum = a->r + b->r;
            const float d2 = dx * dx + dy * dy;
            if (d2 >= rsum * rsum || d2 < 1e-6f) {
                continue;
            }
            const float d = sqrtf(d2);
            const float nx = dx / d, ny = dy / d;
            const float overlap = rsum - d;
            const float inv_sum = a->inv_m + b->inv_m;

            // Separate by inverse mass, then kill closing velocity likewise.
            a->x -= nx * overlap * (a->inv_m / inv_sum);
            a->y -= ny * overlap * (a->inv_m / inv_sum);
            b->x += nx * overlap * (b->inv_m / inv_sum);
            b->y += ny * overlap * (b->inv_m / inv_sum);

            const float vn = (b->vx - a->vx) * nx + (b->vy - a->vy) * ny;
            if (vn < 0) {
                const float imp = -(1 + BALLS_BALL_RESTITUTION) * vn / inv_sum;
                a->vx -= imp * a->inv_m * nx;
                a->vy -= imp * a->inv_m * ny;
                b->vx += imp * b->inv_m * nx;
                b->vy += imp * b->inv_m * ny;
                if (-vn > FLASH_SPEED) {
                    a->flash = FLASH_STEPS;
                    b->flash = FLASH_STEPS;
                }
            }
        }
    }

    // The finger: an immovable disc carrying the finger's velocity, so a
    // resting touch parts the pile and a fast drag launches marbles.
    portENTER_CRITICAL(&s_touch_mux);
    const bool touch_on = s_touch.active;
    const float tx = s_touch.x, ty = s_touch.y;
    const float tvx = s_touch.vx, tvy = s_touch.vy;
    portEXIT_CRITICAL(&s_touch_mux);
    if (touch_on) {
        for (int i = 0; i < BALLS_COUNT; i++) {
            ball_t *b = &s_balls[i];
            const float dx = b->x - tx;
            const float dy = b->y - ty;
            const float rsum = b->r + FINGER_R;
            const float d2 = dx * dx + dy * dy;
            if (d2 >= rsum * rsum || d2 < 1e-6f) {
                continue;
            }
            const float d = sqrtf(d2);
            const float nx = dx / d, ny = dy / d;
            b->x += nx * (rsum - d);
            b->y += ny * (rsum - d);
            const float vn = (b->vx - tvx) * nx + (b->vy - tvy) * ny;
            if (vn < 0) {
                // Infinite finger mass: the marble takes the whole impulse.
                b->vx -= (1 + BALLS_BALL_RESTITUTION) * vn * nx;
                b->vy -= (1 + BALLS_BALL_RESTITUTION) * vn * ny;
                if (-vn > FLASH_SPEED) {
                    b->flash = FLASH_STEPS;
                }
            }
            wall_collide(b);  // the finger can pin a marble against a wall
        }
    }
}

void balls_step(float dt, const sim_forces_t *forces)
{
    if (dt > DT_MAX) dt = DT_MAX;
    float ax = forces->gravity[0] * FORCE_SCALE;
    float ay = forces->gravity[1] * FORCE_SCALE;

    // Shake boost: amplify the fast-changing part of the force vector. Tilt
    // changes slowly and passes through; a shake flips sign step to step and
    // gets multiplied into a proper clatter.
    static float s_prev_ax, s_prev_ay;
    ax += (ax - s_prev_ax) * BALLS_SHAKE_BOOST;
    ay += (ay - s_prev_ay) * BALLS_SHAKE_BOOST;
    s_prev_ax = forces->gravity[0] * FORCE_SCALE;
    s_prev_ay = forces->gravity[1] * FORCE_SCALE;

    const float h = dt / SUBSTEPS;
    for (int s = 0; s < SUBSTEPS; s++) {
        substep(h, ax, ay);
    }

    xSemaphoreTake(s_view_lock, portMAX_DELAY);
    for (int i = 0; i < BALLS_COUNT; i++) {
        s_view[i].x = (int16_t)s_balls[i].x;
        s_view[i].y = (int16_t)s_balls[i].y;
        s_view[i].r = (uint8_t)s_balls[i].r;
        s_view[i].color = s_balls[i].color;
        s_view[i].flash = s_balls[i].flash;
        if (s_balls[i].flash > 0) {
            s_balls[i].flash--;
        }
    }
    xSemaphoreGive(s_view_lock);
}

// ---------------------------------------------------------------------------
// Rendering: full frame, band by band, flat discs with a top-left glint.
// ---------------------------------------------------------------------------

static void draw_disc_rows(uint16_t *buf, int band_y0, int cx, int cy, int r,
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

void balls_render_frame(void)
{
    static ball_view_t view[BALLS_COUNT];
    xSemaphoreTake(s_view_lock, portMAX_DELAY);
    memcpy(view, s_view, sizeof(view));
    xSemaphoreGive(s_view_lock);

    for (int band = 0; band < BAND_COUNT; band++) {
        const int y0 = band * BAND_ROWS;
        uint16_t *buf = display_acquire_band();
        memset(buf, 0, (size_t)LCD_H_RES * BAND_ROWS * sizeof(uint16_t));

        for (int i = 0; i < BALLS_COUNT; i++) {
            const ball_view_t *b = &view[i];
            if (b->y + b->r < y0 || b->y - b->r >= y0 + BAND_ROWS) {
                continue;
            }
            // A hard impact whitens the whole marble for a few frames.
            draw_disc_rows(buf, y0, b->x, b->y, b->r,
                           b->flash ? s_glint[b->color] : s_body[b->color]);
            // The glint that turns a disc into a marble.
            draw_disc_rows(buf, y0, b->x - b->r / 3, b->y - b->r / 3, b->r / 3,
                           b->flash ? rgb565s(255, 255, 255) : s_glint[b->color]);
        }
        display_flush_band(band, buf);
    }
}
