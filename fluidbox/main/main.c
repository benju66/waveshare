// FluidBox: a 3D particle fluid that lives inside the case of the board.
//
// Two tasks run in parallel on the two cores. The simulation task on core 1
// reads the IMU and advances the fluid; the render task on core 0 paints the
// most recent published state to the AMOLED. They only meet at a small mutex
// around the published particle snapshot, so neither waits on the other.

#include <stdio.h>
#include <string.h>

#include "balls.h"
#include "battery.h"
#include "button.h"
#include "config.h"
#include "display.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "imu.h"
#include "net_task.h"
#include "orbits.h"
#include "page_manager.h"
#include "pongwars.h"
#include "render.h"
#include "threads.h"
#include "settings.h"
#include "sim.h"
#include "touch_cst820.h"
#include "ui_lvgl.h"

#define I2C_PORT I2C_NUM_0
#define I2C_SDA GPIO_NUM_15
#define I2C_SCL GPIO_NUM_14

// The simulation runs as fast as it can and yields a single tick each pass, so
// the idle task on its core still gets scheduled and the watchdog stays fed.
#define SIM_YIELD_TICKS 1
#define STATS_PERIOD_MS 2000

static const char *TAG = "fluidbox";

static i2c_master_bus_handle_t s_i2c_bus;

// Frame and step counters, published by the two tasks and read by the stats
// logger. Plain counters are fine: they are only used for a rate estimate.
static volatile uint32_t s_frames;
static volatile uint32_t s_steps;

static esp_err_t i2c_init(void)
{
    const i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_i2c_bus);
}

static void render_task(void *arg)
{
    (void)arg;

    static int last_mode = -1;

    for (;;) {
        // Sleeps here while an LVGL page owns the panel.
        page_manager_render_gate();

        const int mode = settings_sim_mode();
        if (mode != last_mode) {
            last_mode = mode;
            // The fluid's dirty-band tracking assumes untouched bands are
            // black, and the other toys paint everywhere: wipe per switch.
            for (int band = 0; band < BAND_COUNT; band++) {
                uint16_t *buf = display_acquire_band();
                memset(buf, 0, (size_t)LCD_H_RES * BAND_ROWS * sizeof(uint16_t));
                display_flush_band(band, buf);
            }
        }

        switch (mode) {
        case SIM_MODE_BALLS: balls_render_frame(); break;
        case SIM_MODE_PONGWARS: pongwars_render_frame(); break;
        case SIM_MODE_THREADS: threads_render_frame(); break;
        case SIM_MODE_ORBITS: orbits_render_frame(); break;
        default: render_frame(); break;
        }
        s_frames++;

        // The panel transfer paces this loop; yielding one tick keeps the idle
        // task fed so the watchdog stays happy.
        vTaskDelay(1);
    }
}

static void sim_task(void *arg)
{
    (void)arg;

    // Straight down the screen until the IMU produces its first sample.
    sim_forces_t forces = {
        .gravity = {0.0f, GRAVITY_GAIN * GRAVITY_MPS2 * PX_PER_METER, 0.0f},
        .omega = {0.0f, 0.0f, 0.0f},
        .alpha = {0.0f, 0.0f, 0.0f},
    };

    int64_t last_us = esp_timer_get_time();

    for (;;) {
        // Sleeps here while an LVGL page owns the panel, so the fluid wakes
        // with current IMU gravity instead of simulating unseen.
        page_manager_sim_gate();

        const int64_t now = esp_timer_get_time();
        float dt = (float)(now - last_us) * 1e-6f;
        last_us = now;

        // A long stall (logging, flash access) must not turn into a huge step
        // that throws the fluid across the box.
        if (dt > 0.05f) {
            dt = 0.05f;
        } else if (dt < 1e-4f) {
            dt = 1e-4f;
        }

        imu_read(dt, &forces);
        switch (settings_sim_mode()) {
        case SIM_MODE_BALLS: balls_step(dt, &forces); break;
        case SIM_MODE_PONGWARS: pongwars_step(dt, &forces); break;
        case SIM_MODE_THREADS: threads_step(dt, &forces); break;
        case SIM_MODE_ORBITS: orbits_step(dt, &forces); break;
        default: sim_step(dt, &forces); break;
        }
        s_steps++;

        // The PWR button moved to the touch task (it now sleeps the screen
        // rather than reseeding the fluid), which also keeps it responsive
        // while this task is parked.

        vTaskDelay(SIM_YIELD_TICKS);
    }
}

static void stats_loop(void)
{
    uint32_t last_frames = 0, last_steps = 0;
    int64_t last_us = esp_timer_get_time();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(STATS_PERIOD_MS));

        const int64_t now = esp_timer_get_time();
        const float elapsed = (float)(now - last_us) * 1e-6f;
        last_us = now;

        const uint32_t frames = s_frames;
        const uint32_t steps = s_steps;

        sim_stats_t st;
        sim_stats(&st);

        float accel[3];
        imu_raw_accel(accel);

        ESP_LOGI(TAG,
                 "%.1f fps | %.1f steps/s | grid %d dens %d relax %d us | "
                 "rho %.2f/%.2f | speed avg %.0f max %.0f | accel % .2f % .2f % .2f | "
                 "sram %u",
                 (double)((float)(frames - last_frames) / elapsed),
                 (double)((float)(steps - last_steps) / elapsed),
                 st.us_grid, st.us_density, st.us_relax,
                 (double)st.mean_density, (double)st.rest_density,
                 (double)st.mean_speed, (double)st.max_speed,
                 (double)accel[0], (double)accel[1], (double)accel[2],
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

        // Front band against back band. The fluid settles against the back panel
        // but not against the glass, so these two should differ in exactly the
        // quantity responsible.
        ESP_LOGI(TAG,
                 "  front n=%3d rho %.2f v %6.0f hits %3d push %.3f | "
                 "back n=%3d rho %.2f v %6.0f hits %3d push %.3f | "
                 "clamped %d | pairs %d",
                 st.front_count, (double)st.front_density, (double)st.front_speed,
                 st.front_hits, (double)st.front_push,
                 st.back_count, (double)st.back_density, (double)st.back_speed,
                 st.back_hits, (double)st.back_push, st.clamped, st.pairs);

        last_frames = frames;
        last_steps = steps;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "FluidBox starting");

    ESP_ERROR_CHECK(i2c_init());
    settings_init();  // best effort; defaults cover an unreadable NVS
    ESP_ERROR_CHECK(display_init(s_i2c_bus));
    ESP_ERROR_CHECK(display_set_brightness(settings_brightness()));

    // The fluid still works without the IMU, it just falls straight down, so a
    // sensor failure is logged rather than fatal.
    if (imu_init(s_i2c_bus) != ESP_OK) {
        ESP_LOGW(TAG, "continuing without motion input");
    }
    if (button_init(s_i2c_bus) != ESP_OK) {
        ESP_LOGW(TAG, "continuing without the reset button");
    }
    if (battery_init(s_i2c_bus) != ESP_OK) {
        ESP_LOGW(TAG, "continuing without a battery gauge");
    }

    sim_init();
    render_init();
    ESP_ERROR_CHECK(balls_init());
    ESP_ERROR_CHECK(pongwars_init());
    ESP_ERROR_CHECK(threads_init());
    ESP_ERROR_CHECK(orbits_init());

    // Page machinery before the renderer tasks exist: their loop gates read
    // page manager state from the first iteration.
    ESP_ERROR_CHECK(page_manager_init());
    ESP_ERROR_CHECK(ui_lvgl_init());
    if (touch_init(s_i2c_bus) != ESP_OK) {
        ESP_LOGW(TAG, "continuing without touch; pages unreachable by swipe");
    }
    if (net_task_start() != ESP_OK) {
        ESP_LOGW(TAG, "continuing offline; Now page will have no weather");
    }

    TaskHandle_t sim_handle = NULL;
    TaskHandle_t render_handle = NULL;
    xTaskCreatePinnedToCore(sim_task, "sim", 4096, NULL, 5, &sim_handle, 1);
    xTaskCreatePinnedToCore(render_task, "render", 4096, NULL, 5, &render_handle, 0);
    page_manager_register_tasks(render_handle, sim_handle, ui_lvgl_task_handle());

    stats_loop();
}
