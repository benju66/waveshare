#include "settings.h"

#include <stdatomic.h>

#include "config.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define NS "widget"

#define BRIGHT_MIN 20
#define BRIGHT_MAX 255
#define IDLE_MIN_FLOOR 1
#define IDLE_MIN_CEIL 30

static const char *TAG = "settings";

static atomic_int s_brightness = DISPLAY_BRIGHTNESS;
static atomic_int s_idle_min = IDLE_TO_FLUID_MIN;
static atomic_bool s_fireworks = true;
static atomic_bool s_wake_guard = true;
static atomic_int s_sim_mode = SIM_MODE_FLUID;
static atomic_bool s_retro = false;

static nvs_handle_t s_nvs;

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

esp_err_t settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs unavailable, defaults only: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_open(NS, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "namespace open failed: %s", esp_err_to_name(err));
        return err;
    }

    int32_t v;
    if (nvs_get_i32(s_nvs, "bright", &v) == ESP_OK) {
        atomic_store(&s_brightness, clamp(v, BRIGHT_MIN, BRIGHT_MAX));
    }
    if (nvs_get_i32(s_nvs, "idle_min", &v) == ESP_OK) {
        atomic_store(&s_idle_min, clamp(v, IDLE_MIN_FLOOR, IDLE_MIN_CEIL));
    }
    if (nvs_get_i32(s_nvs, "fireworks", &v) == ESP_OK) {
        atomic_store(&s_fireworks, v != 0);
    }
    if (nvs_get_i32(s_nvs, "guard", &v) == ESP_OK) {
        atomic_store(&s_wake_guard, v != 0);
    }
    if (nvs_get_i32(s_nvs, "sim_mode", &v) == ESP_OK && v >= 0 &&
        v < SIM_MODE_COUNT) {
        atomic_store(&s_sim_mode, v);
    }
    if (nvs_get_i32(s_nvs, "retro", &v) == ESP_OK) {
        atomic_store(&s_retro, v != 0);
    }
    ESP_LOGI(TAG, "bright %d, idle %d min, fireworks %d, guard %d, sim mode %d",
             atomic_load(&s_brightness), atomic_load(&s_idle_min),
             atomic_load(&s_fireworks), atomic_load(&s_wake_guard),
             atomic_load(&s_sim_mode));
    return ESP_OK;
}

static void persist(const char *key, int32_t value)
{
    if (s_nvs == 0) {
        return;
    }
    nvs_set_i32(s_nvs, key, value);
    nvs_commit(s_nvs);
}

int settings_brightness(void) { return atomic_load(&s_brightness); }
int settings_idle_min(void) { return atomic_load(&s_idle_min); }
bool settings_fireworks(void) { return atomic_load(&s_fireworks); }
bool settings_wake_guard(void) { return atomic_load(&s_wake_guard); }

void settings_set_brightness(int value)
{
    value = clamp(value, BRIGHT_MIN, BRIGHT_MAX);
    atomic_store(&s_brightness, value);
    persist("bright", value);
}

void settings_set_idle_min(int value)
{
    value = clamp(value, IDLE_MIN_FLOOR, IDLE_MIN_CEIL);
    atomic_store(&s_idle_min, value);
    persist("idle_min", value);
}

void settings_set_fireworks(bool on)
{
    atomic_store(&s_fireworks, on);
    persist("fireworks", on);
}

void settings_set_wake_guard(bool on)
{
    atomic_store(&s_wake_guard, on);
    persist("guard", on);
}

bool settings_retro(void) { return atomic_load(&s_retro); }

void settings_set_retro(bool on)
{
    atomic_store(&s_retro, on);
    persist("retro", on);
}

int settings_sim_mode(void) { return atomic_load(&s_sim_mode); }

void settings_set_sim_mode(int mode)
{
    mode = ((mode % SIM_MODE_COUNT) + SIM_MODE_COUNT) % SIM_MODE_COUNT;
    atomic_store(&s_sim_mode, mode);
    persist("sim_mode", mode);
    ESP_LOGI(TAG, "sim mode -> %d", mode);
}
