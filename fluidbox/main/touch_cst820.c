#include "touch_cst820.h"

#include <stdlib.h>
#include <string.h>

#include "button.h"
#include "config.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "imu.h"
#include "page_pomodoro.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "page_manager.h"
#include "ui_lvgl.h"

#define CST820_ADDR 0x15
#define CST820_SPEED_HZ 400000

// One burst read starting at GestureID covers everything we use.
#define REG_REPORT_START 0x01  // GestureID, FingerNum, XposH/L, YposH/L
#define REG_CHIP_ID 0xA7
#define REG_IRQ_CTL 0xFA
#define IRQ_CTL_EN_TOUCH 0x40   // pulse on every report while touched
#define IRQ_CTL_EN_CHANGE 0x20  // pulse on press and release edges

// Poll period. Bring-up showed INT cannot be relied on, so this cadence IS
// the sample rate; 20 ms still catches a fast flick with a few samples.
#define TOUCH_POLL_MS 20

// CST816-family gesture ids, reported by the chip's internal ~100 Hz engine.
// Used as a fallback for flicks too quick for our polling to track.
#define GESTURE_SLIDE_LEFT 0x03
#define GESTURE_SLIDE_RIGHT 0x04

// A contact only reaches LVGL live once it has stayed near its origin this
// long — i.e. it is a hold, not a possible swipe. Shorter stationary contacts
// become deferred clicks at release. Keeps a swipe from ever counting as a
// click on the widget it started over.
#define LV_PRESS_DELAY_MS 150
#define LV_STATIONARY_PX 10

// PWR button cadence. The debouncer needs two agreeing samples, so a press
// registers in about twice this.
#define BUTTON_POLL_MS 50

static const char *TAG = "touch";

static i2c_master_dev_handle_t s_dev;
static TaskHandle_t s_task;

// Gesture state. A swipe fires the moment it crosses the distance threshold
// rather than on release, which reads as snappier; the rest of that contact
// is then consumed so one drag cannot turn into two page changes.
static bool s_down;
static int16_t s_x0, s_y0;
static int64_t s_t0_us;
static bool s_swipe_fired;
static bool s_lv_suppressed;
static bool s_lv_published;  // LVGL has seen this contact as a live press

// True while the device has been motionless for STATIONARY_HOLD_MS: the
// desk-vs-pocket discriminator for touch wake. Updated by poll_imu.
static bool s_stationary;

static void IRAM_ATTR touch_isr(void *arg)
{
    (void)arg;
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &woken);
    portYIELD_FROM_ISR(woken);
}

static esp_err_t read_report(int *fingers, int16_t *x, int16_t *y, uint8_t *gesture)
{
    uint8_t reg = REG_REPORT_START;
    uint8_t buf[6];
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, buf, sizeof(buf), 50);
    if (err != ESP_OK) {
        return err;
    }
    *gesture = buf[0];
    *fingers = buf[1] & 0x0F;
    *x = (int16_t)(((buf[2] & 0x0F) << 8) | buf[3]);
    *y = (int16_t)(((buf[4] & 0x0F) << 8) | buf[5]);
    return ESP_OK;
}

static void fire_swipe(int dx)
{
    s_swipe_fired = true;
    page_manager_post_event(dx < 0 ? PAGE_EVT_SWIPE_LEFT : PAGE_EVT_SWIPE_RIGHT);
}

static void process_sample(int fingers, int16_t x, int16_t y, uint8_t gesture)
{
    const int64_t now = esp_timer_get_time();

    if (fingers > 0) {
        page_manager_note_activity();
        if (!s_down) {
            s_down = true;
            ESP_LOGI(TAG, "press at %d,%d", x, y);
            s_x0 = x;
            s_y0 = y;
            s_t0_us = now;
            s_lv_published = false;
            if (page_manager_screen_is_off() && !s_stationary) {
                // Pocket guard: the screen is dark and the device is being
                // jostled - this "touch" is fabric. Swallow the whole
                // contact; only the PWR button wakes a moving device.
                s_lv_suppressed = true;
                s_swipe_fired = true;
            } else {
                // A press that wakes the dimmed or dark screen is consumed
                // by the wake: it must not also click whatever the page
                // shows, nor turn into a page swipe on the transition.
                s_lv_suppressed = page_manager_wake_touch();
                s_swipe_fired = s_lv_suppressed;
            }
        }

        const int dx = x - s_x0;
        const int dy = y - s_y0;

        // Clear sideways travel disqualifies this contact from ever reaching
        // LVGL: it is a page gesture, not an interaction with a widget.
        if (!s_lv_suppressed && abs(dx) > SWIPE_LV_CANCEL_DX) {
            s_lv_suppressed = true;
        }

        if (!s_swipe_fired && abs(dx) >= SWIPE_MIN_DX && abs(dy) <= SWIPE_MAX_DY &&
            now - s_t0_us <= (int64_t)SWIPE_MAX_MS * 1000) {
            fire_swipe(dx);
        }

        // LVGL sees the contact live only once it has proven to be a
        // stationary hold; that keeps long-press working without letting a
        // nascent swipe masquerade as a click.
        if (!s_lv_published && !s_lv_suppressed && !s_swipe_fired &&
            abs(dx) < LV_STATIONARY_PX && abs(dy) < LV_STATIONARY_PX &&
            now - s_t0_us >= (int64_t)LV_PRESS_DELAY_MS * 1000) {
            s_lv_published = true;
        }
        if (s_lv_published) {
            ui_lvgl_input_publish(x, y, !s_lv_suppressed);
        }
    } else if (s_down) {
        s_down = false;
        ESP_LOGI(TAG, "release at %d,%d gesture 0x%02x", x, y, gesture);

        const int dx = x - s_x0;
        const int dy = y - s_y0;

        // Fast flicks can finish inside one or two poll periods, leaving too
        // few samples for the tracker above. Two fallbacks, in order of
        // trust: total travel between first and last report, then the chip's
        // own gesture engine, which watched the whole thing at full rate.
        if (!s_swipe_fired && now - s_t0_us <= (int64_t)SWIPE_MAX_MS * 1000) {
            if (abs(dx) >= SWIPE_MIN_DX && abs(dy) <= SWIPE_MAX_DY) {
                fire_swipe(dx);
            } else if (gesture == GESTURE_SLIDE_LEFT) {
                fire_swipe(-1);
            } else if (gesture == GESTURE_SLIDE_RIGHT) {
                fire_swipe(+1);
            }
        }

        if (s_lv_published) {
            // LVGL saw the hold begin; let it see the release too.
            ui_lvgl_input_publish(x, y, false);
        } else if (!s_swipe_fired && !s_lv_suppressed &&
                   abs(dx) < SWIPE_LV_CANCEL_DX && abs(dy) < SWIPE_LV_CANCEL_DX) {
            // Short and stationary: a tap. LVGL never saw it live, so hand it
            // over as a complete click now that it cannot be a swipe.
            ui_lvgl_input_click(s_x0, s_y0);
        } else if (!s_swipe_fired && abs(dy) >= SWIPE_MIN_DY &&
                   abs(dx) <= SWIPE_MAX_DY &&
                   now - s_t0_us <= (int64_t)SWIPE_MAX_MS * 1000 &&
                   page_manager_current() == PAGE_TIMER) {
            // Vertical swipe on the timer page: up lengthens the work
            // session, down shortens it. Only the idle timer listens.
            page_pomodoro_adjust(dy < 0 ? POMO_ADJUST_STEP_MIN
                                        : -POMO_ADJUST_STEP_MIN);
        }
    }
}

// IMU housekeeping: flip-to-pause detection plus the stationary tracker.
// On the fluid page the sim keeps the cached sample fresh; everywhere else
// the sim task is parked, so a direct poll is safe. Debounced so a wobble
// while handling never pauses anything.
static void poll_imu(void)
{
    static int64_t s_last_poll_us;
    static int64_t s_flat_since_us;
    static bool s_reported;
    static float s_prev[3];
    static int64_t s_still_since_us;

    const int64_t now = esp_timer_get_time();
    if (now - s_last_poll_us < 250 * 1000) {
        return;
    }
    s_last_poll_us = now;

    float accel[3];
    if (page_manager_fluid_active()) {
        imu_raw_accel(accel);
    } else if (!imu_poll_accel(accel)) {
        return;
    }

    // Stationary tracking: compare against the previous sample rather than
    // absolute gravity, so any resting orientation counts as still.
    const float dx = accel[0] - s_prev[0];
    const float dy = accel[1] - s_prev[1];
    const float dz = accel[2] - s_prev[2];
    memcpy(s_prev, accel, sizeof(s_prev));
    if (dx * dx + dy * dy + dz * dz >
        STATIONARY_DELTA_MPS2 * STATIONARY_DELTA_MPS2) {
        s_still_since_us = 0;
        s_stationary = false;
    } else {
        if (s_still_since_us == 0) {
            s_still_since_us = now;
        }
        s_stationary = now - s_still_since_us >= (int64_t)STATIONARY_HOLD_MS * 1000;
    }

    // "Flat, screen up" reads about -g on the raw z axis (see config.h), so
    // face-down is a sustained read near +g.
    const bool flat = accel[2] > FACE_DOWN_MPS2;
    if (!flat) {
        s_flat_since_us = 0;
        if (s_reported) {
            s_reported = false;
            page_pomodoro_set_face_down(false);
        }
        return;
    }
    if (s_flat_since_us == 0) {
        s_flat_since_us = now;
    }
    if (!s_reported && now - s_flat_since_us >= (int64_t)FACE_DOWN_HOLD_MS * 1000) {
        s_reported = true;
        page_pomodoro_set_face_down(true);
    }
}

static void poll_power_button(void)
{
    static int64_t s_last_poll_us;
    static bool s_was_pressed;
    static bool s_ignore_release;

    const int64_t now = esp_timer_get_time();
    if (now - s_last_poll_us < (int64_t)BUTTON_POLL_MS * 1000) {
        return;
    }
    s_last_poll_us = now;

    const bool short_press = button_take_short_press();  // also runs the debouncer
    const bool pressed = button_is_pressed();
    const bool press_edge = pressed && !s_was_pressed;
    const bool release_edge = !pressed && s_was_pressed;
    s_was_pressed = pressed;

    // Waking must happen on the press EDGE, not on a classified short press:
    // people hold the button when a screen stays dark, a hold never becomes
    // a short press, and at 6 seconds the AXP2101 hard-cuts the power.
    if (press_edge && page_manager_screen_is_off()) {
        page_manager_post_event(PAGE_EVT_WAKE);
        s_ignore_release = true;  // this press's release already did its job
    }

    // Short press with the screen on: panel off. The 6 second hardware
    // power-off stays available underneath all of this.
    if (short_press && !s_ignore_release && !page_manager_screen_is_off()) {
        page_manager_post_event(PAGE_EVT_SCREEN_OFF);
    }
    if (release_edge) {
        s_ignore_release = false;
    }
}

static void touch_task(void *arg)
{
    (void)arg;

    for (;;) {
        // The dark screen needs neither 50 Hz touch tracking nor a hot I2C
        // bus; the INT line still short-circuits the slow poll on a real tap.
        const uint32_t poll_ms =
            page_manager_screen_is_off() ? TOUCH_POLL_OFF_MS : TOUCH_POLL_MS;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(poll_ms));

        poll_power_button();
        poll_imu();

        // Read unconditionally. Bring-up showed the CST820's INT line cannot
        // be relied on (the IrqCtl write appears to be ignored on this chip),
        // so the 50 ms poll is the workhorse and INT, when it does fire, is
        // just a latency improvement. One 6-byte read every 50 ms is ~0.3%
        // bus duty; not worth optimizing away ever again.
        int fingers;
        int16_t x, y;
        uint8_t gesture;
        if (read_report(&fingers, &x, &y, &gesture) == ESP_OK) {
            process_sample(fingers, x, y, gesture);
        }
    }
}

esp_err_t touch_init(i2c_master_bus_handle_t bus)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CST820_ADDR,
        .scl_speed_hz = CST820_SPEED_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CST820 not reachable: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t reg = REG_CHIP_ID;
    uint8_t chip_id = 0;
    if (i2c_master_transmit_receive(s_dev, &reg, 1, &chip_id, 1, 50) == ESP_OK) {
        ESP_LOGI(TAG, "CST820 chip id 0x%02x", chip_id);
    }

    // Pulse INT on report while touched and on press/release edges. Best
    // effort: the poll fallback carries the feature if the write is ignored.
    const uint8_t irq_cfg[2] = {REG_IRQ_CTL, IRQ_CTL_EN_TOUCH | IRQ_CTL_EN_CHANGE};
    i2c_master_transmit(s_dev, irq_cfg, sizeof(irq_cfg), 50);

    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << TOUCH_INT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "int pin config failed");

    if (xTaskCreatePinnedToCore(touch_task, "touch", 4096, NULL, 6, &s_task, 1) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    // The button module has not installed an ISR service (it polls over I2C),
    // but tolerate one existing in case that ever changes.
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    return gpio_isr_handler_add(TOUCH_INT_GPIO, touch_isr, NULL);
}
