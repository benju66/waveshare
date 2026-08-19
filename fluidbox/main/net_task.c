#include "net_task.h"

#include <string.h>

#include "cJSON.h"
#include "config.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "ota.h"

// Wi-Fi credentials live in a gitignored header; the build must still work
// for a fresh clone, so their absence only disables the network features.
#if __has_include("secrets.h")
#include "secrets.h"
#define HAVE_SECRETS 1
#else
#define HAVE_SECRETS 0
#endif

#define WIFI_CONNECTED_BIT BIT0

// The full response with these parameters is around 500 bytes; 4 KB absorbs
// any formatting drift without mattering.
#define HTTP_BUF_SIZE 4096

#define FETCH_RETRY_MS (60 * 1000)

static const char *TAG = "net";

static SemaphoreHandle_t s_model_lock;
static weather_model_t s_model;

#if HAVE_SECRETS

static EventGroupHandle_t s_wifi_events;

// ---------------------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------------------

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        // Endless retry is right for a wall widget: the AP rebooting at 3 am
        // should not require a power cycle here.
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "got IP");
        // Reaching the network is this firmware's proof of life; a fresh OTA
        // image that gets here will not be rolled back.
        ota_mark_healthy();
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase failed");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs init failed");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop failed");
    esp_netif_create_default_wifi_sta();

    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   on_wifi_event, NULL),
                        TAG, "handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   on_wifi_event, NULL),
                        TAG, "handler failed");

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid, WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "sta mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG,
                        "config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");

    const esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    ESP_RETURN_ON_ERROR(esp_netif_sntp_init(&sntp_cfg), TAG, "sntp failed");

    setenv("TZ", TZ_STRING, 1);
    tzset();
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Open-Meteo
// ---------------------------------------------------------------------------

// forecast_hours=2 so index 1 of the hourly array is the hour after this one.
#define WEATHER_URL_FMT                                                        \
    "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"     \
    "&current=temperature_2m,weather_code"                                     \
    "&daily=temperature_2m_max,temperature_2m_min"                             \
    "&hourly=precipitation_probability&forecast_hours=2&forecast_days=1"       \
    "&temperature_unit=fahrenheit&timezone=auto"

static esp_err_t parse_weather(const char *json, weather_model_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_err_t err = ESP_ERR_INVALID_RESPONSE;
    const cJSON *current = cJSON_GetObjectItem(root, "current");
    const cJSON *daily = cJSON_GetObjectItem(root, "daily");
    const cJSON *hourly = cJSON_GetObjectItem(root, "hourly");

    const cJSON *temp = cJSON_GetObjectItem(current, "temperature_2m");
    const cJSON *code = cJSON_GetObjectItem(current, "weather_code");
    const cJSON *hi = cJSON_GetArrayItem(cJSON_GetObjectItem(daily, "temperature_2m_max"), 0);
    const cJSON *lo = cJSON_GetArrayItem(cJSON_GetObjectItem(daily, "temperature_2m_min"), 0);
    const cJSON *probs = cJSON_GetObjectItem(hourly, "precipitation_probability");
    const cJSON *prob = cJSON_GetArrayItem(probs, 1);
    if (prob == NULL) {
        prob = cJSON_GetArrayItem(probs, 0);
    }

    if (cJSON_IsNumber(temp) && cJSON_IsNumber(code) && cJSON_IsNumber(hi) &&
        cJSON_IsNumber(lo)) {
        out->temp = (float)temp->valuedouble;
        out->weather_code = code->valueint;
        out->today_hi = (float)hi->valuedouble;
        out->today_lo = (float)lo->valuedouble;
        out->precip_prob_pct = cJSON_IsNumber(prob) ? prob->valueint : 0;
        out->fetched_us = esp_timer_get_time();
        out->valid = true;
        err = ESP_OK;
    }

    cJSON_Delete(root);
    return err;
}

static esp_err_t fetch_weather(char *buf)
{
    char url[384];
    snprintf(url, sizeof(url), WEATHER_URL_FMT, (double)WEATHER_LAT,
             (double)WEATHER_LON);

    const esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        const int len = esp_http_client_read_response(client, buf, HTTP_BUF_SIZE - 1);
        const int status = esp_http_client_get_status_code(client);
        if (len > 0 && status == 200) {
            buf[len] = '\0';
            weather_model_t fresh = {0};
            err = parse_weather(buf, &fresh);
            if (err == ESP_OK) {
                xSemaphoreTake(s_model_lock, portMAX_DELAY);
                s_model = fresh;
                xSemaphoreGive(s_model_lock);
                ESP_LOGI(TAG, "weather %.1fF code %d hi %.1f lo %.1f rain %d%%",
                         (double)fresh.temp, fresh.weather_code,
                         (double)fresh.today_hi, (double)fresh.today_lo,
                         fresh.precip_prob_pct);
            }
        } else {
            ESP_LOGW(TAG, "http status %d, read %d", status, len);
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }
    esp_http_client_cleanup(client);
    return err;
}

static void net_task(void *arg)
{
    char *buf = arg;
    bool ota_checked_once = false;
    int64_t last_ota_us = 0;

#if !WEATHER_COORDS_SET
    // Wi-Fi, SNTP and OTA still run; only the weather fetch is pointless
    // without real coordinates.
    ESP_LOGW(TAG, "WEATHER_COORDS_SET is 0; not fetching weather");
    (void)buf;
#endif

    for (;;) {
        xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                            portMAX_DELAY);

        const int64_t now = esp_timer_get_time();
        if (!ota_checked_once ||
            now - last_ota_us >= (int64_t)OTA_CHECK_INTERVAL_H * 3600 * 1000000) {
            last_ota_us = now;
            // Outrank the sim for the duration: the TLS handshake is pure
            // math and starves at this task's usual priority. The fluid
            // freezes for the few seconds of an update check; fair trade.
            vTaskPrioritySet(NULL, 6);
            esp_err_t err = ESP_FAIL;
            for (int attempt = 0; attempt < 3 && err != ESP_OK; attempt++) {
                if (attempt > 0) {
                    vTaskDelay(pdMS_TO_TICKS(30 * 1000));
                }
                err = ota_check_and_apply();  // reboots if it applies one
            }
            vTaskPrioritySet(NULL, 3);
            ota_checked_once = true;
        }

#if WEATHER_COORDS_SET
        const bool ok = fetch_weather(buf) == ESP_OK;
        // A failed fetch retries in a minute; a good one holds the cadence.
        vTaskDelay(pdMS_TO_TICKS(ok ? WEATHER_REFRESH_MIN * 60 * 1000
                                    : FETCH_RETRY_MS));
#else
        vTaskDelay(pdMS_TO_TICKS(60 * 60 * 1000));
#endif
    }
}

#endif  // HAVE_SECRETS

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t net_task_start(void)
{
    s_model_lock = xSemaphoreCreateMutex();
    if (s_model_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

#if !HAVE_SECRETS
    ESP_LOGW(TAG, "no secrets.h; Wi-Fi and weather disabled "
                  "(copy secrets.h.example to secrets.h)");
    return ESP_ERR_NOT_FOUND;
#else
    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = wifi_start();
    if (err != ESP_OK) {
        return err;
    }

    // The response buffer lives in PSRAM: it is parsed by the CPU, never
    // DMA'd, and internal RAM is the scarce resource here.
    char *buf = heap_caps_malloc(HTTP_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = malloc(HTTP_BUF_SIZE);
    }
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(net_task, "net", 6144, buf, 3, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
#endif
}

void net_set_enabled(bool enabled)
{
#if HAVE_SECRETS
    static bool s_enabled = true;
    if (s_wifi_events == NULL || enabled == s_enabled) {
        return;  // never started, or nothing to change
    }
    s_enabled = enabled;
    if (enabled) {
        ESP_LOGI(TAG, "radio on");
        esp_wifi_start();
    } else {
        ESP_LOGI(TAG, "radio off");
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        esp_wifi_stop();
    }
#else
    (void)enabled;
#endif
}

bool weather_get(weather_model_t *out)
{
    if (s_model_lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_model_lock, portMAX_DELAY);
    *out = s_model;
    xSemaphoreGive(s_model_lock);
    return out->valid;
}
