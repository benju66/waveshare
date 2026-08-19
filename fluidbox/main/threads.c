#include "threads.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define SWAP16(v) ((uint16_t)((((v) >> 8) & 0xFF) | (((v) & 0xFF) << 8)))

// Trail occupancy at 2 px resolution: 184 x 224 cells, one byte each
// (0 empty, 1 or 2 = owner). ~41 KB, lives in PSRAM; the render task reads
// it without a lock - a cell torn by one frame is invisible.
#define CELL 2
#define TCOLS (LCD_H_RES / CELL)
#define TROWS (LCD_V_RES / CELL)

#define TH_SPEED 340.0f
#define TH_BALL_R 4
#define TH_TILT_BEND 0.45f
#define DT_MAX 0.02f
// Weave density at which the canvas dissolves and starts over.
#define TH_RESET_FILL 0.72f

typedef struct {
    float x, y, vx, vy;
} th_ball_t;

static uint8_t *s_cells;  // TROWS * TCOLS, PSRAM
static th_ball_t s_ball[2];
static int s_filled;

static SemaphoreHandle_t s_view_lock;
static struct {
    int16_t x, y;
} s_view_ball[2];

static uint16_t rgb565s(int r, int g, int b)
{
    const uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return SWAP16(v);
}

static void reseed(void)
{
    memset(s_cells, 0, (size_t)TROWS * TCOLS);
    s_filled = 0;
    for (int i = 0; i < 2; i++) {
        const float a = (float)(rand() % 628) / 100.0f;
        s_ball[i].x = LCD_H_RES * (i ? 0.75f : 0.25f);
        s_ball[i].y = LCD_V_RES * 0.5f;
        s_ball[i].vx = TH_SPEED * cosf(a);
        s_ball[i].vy = TH_SPEED * sinf(a);
    }
}

esp_err_t threads_init(void)
{
    s_view_lock = xSemaphoreCreateMutex();
    if (s_view_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_cells = heap_caps_malloc((size_t)TROWS * TCOLS,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_cells == NULL) {
        s_cells = malloc((size_t)TROWS * TCOLS);
    }
    if (s_cells == NULL) {
        return ESP_ERR_NO_MEM;
    }
    reseed();
    return ESP_OK;
}

static uint8_t cell_at(float x, float y)
{
    int c = (int)(x / CELL);
    int r = (int)(y / CELL);
    if (c < 0) c = 0;
    if (c >= TCOLS) c = TCOLS - 1;
    if (r < 0) r = 0;
    if (r >= TROWS) r = TROWS - 1;
    return s_cells[r * TCOLS + c];
}

static void mark_at(float x, float y, uint8_t owner)
{
    const int c = (int)(x / CELL);
    const int r = (int)(y / CELL);
    if (c >= 0 && c < TCOLS && r >= 0 && r < TROWS) {
        uint8_t *cell = &s_cells[r * TCOLS + c];
        if (*cell == 0) {
            s_filled++;
        }
        *cell = owner;
    }
}

// Hitting the enemy thread bites a hole in it: the weave is a duel, not
// just an accumulation. Radius in cells.
#define TH_BLAST_CELLS 3

static void erase_blast(float x, float y, uint8_t foe)
{
    const int cc = (int)(x / CELL);
    const int cr = (int)(y / CELL);
    for (int r = cr - TH_BLAST_CELLS; r <= cr + TH_BLAST_CELLS; r++) {
        for (int c = cc - TH_BLAST_CELLS; c <= cc + TH_BLAST_CELLS; c++) {
            if (r < 0 || r >= TROWS || c < 0 || c >= TCOLS) {
                continue;
            }
            uint8_t *cell = &s_cells[r * TCOLS + c];
            if (*cell == foe) {
                *cell = 0;
                s_filled--;
            }
        }
    }
}

static void step_ball(int i, float dt, float gx, float gy)
{
    th_ball_t *b = &s_ball[i];
    const uint8_t me = (uint8_t)(i + 1);
    const uint8_t foe = (uint8_t)(2 - i);

    b->vx += gx * TH_TILT_BEND * dt;
    b->vy += gy * TH_TILT_BEND * dt;
    const float sp = sqrtf(b->vx * b->vx + b->vy * b->vy);
    if (sp > 1.0f) {
        b->vx *= TH_SPEED / sp;
        b->vy *= TH_SPEED / sp;
    }

    // Axis-separated probes, bouncing off walls and the OTHER thread only;
    // crossing our own keeps the weave growing instead of boxing us in.
    // A whisker of angle noise on each bounce kills orbit loops.
    const float lead = (float)TH_BALL_R;
    bool bounced = false;

    float nx = b->x + b->vx * dt;
    const float px = nx + (b->vx > 0 ? lead : -lead);
    if (px < 0 || px >= LCD_H_RES) {
        b->vx = -b->vx;
        bounced = true;
    } else if (cell_at(px, b->y) == foe) {
        erase_blast(px, b->y, foe);
        b->vx = -b->vx;
        bounced = true;
    } else {
        b->x = nx;
    }

    float ny = b->y + b->vy * dt;
    const float py = ny + (b->vy > 0 ? lead : -lead);
    if (py < 0 || py >= LCD_V_RES) {
        b->vy = -b->vy;
        bounced = true;
    } else if (cell_at(b->x, py) == foe) {
        erase_blast(b->x, py, foe);
        b->vy = -b->vy;
        bounced = true;
    } else {
        b->y = ny;
    }

    if (bounced) {
        const float jitter = ((float)(rand() % 100) / 100.0f - 0.5f) * 0.22f;
        const float ca = cosf(jitter), sa = sinf(jitter);
        const float vx = b->vx * ca - b->vy * sa;
        b->vy = b->vx * sa + b->vy * ca;
        b->vx = vx;
    }

    mark_at(b->x, b->y, me);
}

void threads_step(float dt, const sim_forces_t *forces)
{
    if (dt > DT_MAX) dt = DT_MAX;
    const float scale = BALLS_GRAVITY / (GRAVITY_GAIN * GRAVITY_MPS2 * PX_PER_METER);
    const float gx = forces->gravity[0] * scale;
    const float gy = forces->gravity[1] * scale;

    // Sub-steps so the trail is a line, not dashes: at 340 px/s and ~30
    // steps/s a single step crosses five cells.
    for (int s = 0; s < 6; s++) {
        step_ball(0, dt / 6, gx, gy);
        step_ball(1, dt / 6, gx, gy);
    }

    if (s_filled > (int)((float)TROWS * TCOLS * TH_RESET_FILL)) {
        reseed();  // the weave is done; begin the next one
    }

    xSemaphoreTake(s_view_lock, portMAX_DELAY);
    for (int i = 0; i < 2; i++) {
        s_view_ball[i].x = (int16_t)s_ball[i].x;
        s_view_ball[i].y = (int16_t)s_ball[i].y;
    }
    xSemaphoreGive(s_view_lock);
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

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

void threads_render_frame(void)
{
    struct {
        int16_t x, y;
    } balls[2];
    xSemaphoreTake(s_view_lock, portMAX_DELAY);
    memcpy(balls, s_view_ball, sizeof(balls));
    xSemaphoreGive(s_view_lock);

    // Trails dimmer than their ball, so the moving heads glow.
    const uint16_t trail[3] = {0x0000, rgb565s(0, 150, 120), rgb565s(150, 0, 105)};
    const uint16_t head[2] = {rgb565s(0, 255, 200), rgb565s(255, 0, 180)};

    for (int band = 0; band < BAND_COUNT; band++) {
        const int y0 = band * BAND_ROWS;
        uint16_t *buf = display_acquire_band();

        for (int y = y0; y < y0 + BAND_ROWS; y++) {
            uint16_t *row = buf + (y - y0) * LCD_H_RES;
            const uint8_t *cells = &s_cells[(y / CELL) * TCOLS];
            for (int c = 0; c < TCOLS; c++) {
                const uint16_t color = trail[cells[c]];
                row[c * CELL] = color;
                row[c * CELL + 1] = color;
            }
        }

        draw_disc(buf, y0, balls[0].x, balls[0].y, TH_BALL_R, head[0]);
        draw_disc(buf, y0, balls[1].x, balls[1].y, TH_BALL_R, head[1]);
        display_flush_band(band, buf);
    }
}
