#include "pongwars.h"

#include <math.h>
#include <string.h>

#include "config.h"
#include "display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define SWAP16(v) ((uint16_t)((((v) >> 8) & 0xFF) | (((v) & 0xFF) << 8)))

#define COLS (LCD_H_RES / PW_CELL)  // 23
#define ROWS (LCD_V_RES / PW_CELL)  // 28
#define DT_MAX 0.02f

// Neon on true black. The dark territory's pixels are off entirely.
#define NEON rgb565s(0, 255, 200)
#define BLACK 0x0000

typedef struct {
    float x, y, vx, vy;
} pw_ball_t;

// cells[r][c] = owning side, 0 (black) or 1 (neon). Ball i lives in
// territory i and flips foreign cells at the frontier, pong-wars style.
static uint8_t s_cells[ROWS][COLS];
static pw_ball_t s_ball[2];

static SemaphoreHandle_t s_view_lock;
static uint8_t s_view_cells[ROWS][COLS];
static struct {
    int16_t x, y;
} s_view_ball[2];

static uint16_t rgb565s(int r, int g, int b)
{
    const uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return SWAP16(v);
}

esp_err_t pongwars_init(void)
{
    s_view_lock = xSemaphoreCreateMutex();
    if (s_view_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    // Top half black, bottom half neon; each ball starts mid-territory on a
    // diagonal.
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            s_cells[r][c] = r < ROWS / 2 ? 0 : 1;
        }
    }
    const float v = PW_SPEED * 0.7071f;
    s_ball[0] = (pw_ball_t){LCD_H_RES / 2.0f, LCD_V_RES / 4.0f, v, v};
    s_ball[1] = (pw_ball_t){LCD_H_RES / 2.0f, LCD_V_RES * 3 / 4.0f, -v, -v};
    return ESP_OK;
}

static uint8_t cell_at(float x, float y)
{
    int c = (int)(x / PW_CELL);
    int r = (int)(y / PW_CELL);
    if (c < 0) c = 0;
    if (c >= COLS) c = COLS - 1;
    if (r < 0) r = 0;
    if (r >= ROWS) r = ROWS - 1;
    return s_cells[r][c];
}

static void flip_at(float x, float y, uint8_t to)
{
    int c = (int)(x / PW_CELL);
    int r = (int)(y / PW_CELL);
    if (c >= 0 && c < COLS && r >= 0 && r < ROWS) {
        s_cells[r][c] = to;
    }
}

static void step_ball(int i, float dt, float gx, float gy)
{
    pw_ball_t *b = &s_ball[i];

    // Tilt steers: gravity bends the velocity, then speed renormalizes so
    // the war never stalls or runs away.
    b->vx += gx * PW_TILT_BEND * dt;
    b->vy += gy * PW_TILT_BEND * dt;
    const float sp = sqrtf(b->vx * b->vx + b->vy * b->vy);
    if (sp > 1.0f) {
        b->vx *= PW_SPEED / sp;
        b->vy *= PW_SPEED / sp;
    }

    // Axis-separated movement, pong-wars style: crossing into foreign ground
    // flips that cell and reflects the crossing axis.
    const float lead = (float)PW_BALL_R;
    float nx = b->x + b->vx * dt;
    const float probe_x = nx + (b->vx > 0 ? lead : -lead);
    if (probe_x < 0 || probe_x >= LCD_H_RES) {
        b->vx = -b->vx;
    } else if (cell_at(probe_x, b->y) != i) {
        flip_at(probe_x, b->y, (uint8_t)i);
        b->vx = -b->vx;
    } else {
        b->x = nx;
    }

    float ny = b->y + b->vy * dt;
    const float probe_y = ny + (b->vy > 0 ? lead : -lead);
    if (probe_y < 0 || probe_y >= LCD_V_RES) {
        b->vy = -b->vy;
    } else if (cell_at(b->x, probe_y) != i) {
        flip_at(b->x, probe_y, (uint8_t)i);
        b->vy = -b->vy;
    } else {
        b->y = ny;
    }
}

void pongwars_step(float dt, const sim_forces_t *forces)
{
    if (dt > DT_MAX) dt = DT_MAX;
    // Same rescale the marbles use: 1 g of tilt ~ BALLS_GRAVITY px/s^2.
    const float scale = BALLS_GRAVITY / (GRAVITY_GAIN * GRAVITY_MPS2 * PX_PER_METER);
    const float gx = forces->gravity[0] * scale;
    const float gy = forces->gravity[1] * scale;

    step_ball(0, dt, gx, gy);
    step_ball(1, dt, gx, gy);

    xSemaphoreTake(s_view_lock, portMAX_DELAY);
    memcpy(s_view_cells, s_cells, sizeof(s_cells));
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

void pongwars_render_frame(void)
{
    static uint8_t cells[ROWS][COLS];
    static struct {
        int16_t x, y;
    } balls[2];

    xSemaphoreTake(s_view_lock, portMAX_DELAY);
    memcpy(cells, s_view_cells, sizeof(cells));
    memcpy(balls, s_view_ball, sizeof(balls));
    xSemaphoreGive(s_view_lock);

    const uint16_t neon = NEON;

    for (int band = 0; band < BAND_COUNT; band++) {
        const int y0 = band * BAND_ROWS;
        uint16_t *buf = display_acquire_band();

        // Territory: whole pixel rows per cell row, with a 1 px black seam
        // between neon cells so the grid reads as tiles.
        for (int y = y0; y < y0 + BAND_ROWS; y++) {
            uint16_t *row = buf + (y - y0) * LCD_H_RES;
            const int r = y / PW_CELL;
            const bool seam_row = (y % PW_CELL) == 0;
            for (int c = 0; c < COLS; c++) {
                const uint16_t color =
                    (cells[r][c] && !seam_row) ? neon : BLACK;
                uint16_t *px = row + c * PW_CELL;
                for (int k = 0; k < PW_CELL; k++) {
                    px[k] = color;
                }
                if (cells[r][c] && !seam_row) {
                    px[0] = BLACK;  // vertical seam
                }
            }
        }

        // Ball 0 defends black ground: draw it neon. Ball 1 defends neon
        // ground: draw it black with a neon ring so it reads on both.
        draw_disc(buf, y0, balls[0].x, balls[0].y, PW_BALL_R, neon);
        draw_disc(buf, y0, balls[1].x, balls[1].y, PW_BALL_R, neon);
        draw_disc(buf, y0, balls[1].x, balls[1].y, PW_BALL_R - 3, BLACK);

        display_flush_band(band, buf);
    }
}
