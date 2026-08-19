#include "battery.h"

#include "esp_log.h"
#include "esp_timer.h"

#define AXP2101_ADDR 0x34
#define AXP2101_SPEED_HZ 400000

// Fuel gauge output, 0..100. The gauge needs a battery attached to report
// anything meaningful; on USB-only power it reads 0 or garbage, which the
// PMU status register distinguishes.
#define AXP2101_REG_BATT_PERCENT 0xA4
#define AXP2101_REG_PMU_STATUS1 0x00
#define PMU_STATUS1_BATT_PRESENT (1 << 3)
#define PMU_STATUS1_VBUS_GOOD (1 << 5)  // verify on hardware: unplug USB on battery

#define CACHE_US (10 * 1000000LL)

static const char *TAG = "battery";

static i2c_master_dev_handle_t s_dev;
static int s_cached = -1;
static int64_t s_read_us;

esp_err_t battery_init(i2c_master_bus_handle_t bus)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = AXP2101_SPEED_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 not reachable: %s", esp_err_to_name(err));
        s_dev = NULL;
    }
    return err;
}

static esp_err_t read_reg(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, value, 1, 50);
}

bool battery_on_usb(void)
{
    if (s_dev == NULL) {
        return true;
    }
    uint8_t status = 0;
    if (read_reg(AXP2101_REG_PMU_STATUS1, &status) != ESP_OK) {
        return true;
    }
    return (status & PMU_STATUS1_VBUS_GOOD) != 0;
}

int battery_percent(void)
{
    if (s_dev == NULL) {
        return -1;
    }

    const int64_t now = esp_timer_get_time();
    if (s_read_us != 0 && now - s_read_us < CACHE_US) {
        return s_cached;
    }
    s_read_us = now;

    uint8_t status = 0;
    uint8_t percent = 0;
    if (read_reg(AXP2101_REG_PMU_STATUS1, &status) != ESP_OK ||
        (status & PMU_STATUS1_BATT_PRESENT) == 0 ||
        read_reg(AXP2101_REG_BATT_PERCENT, &percent) != ESP_OK || percent > 100) {
        s_cached = -1;
    } else {
        s_cached = percent;
    }
    return s_cached;
}
