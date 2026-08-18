#include "ota.h"

#include <string.h>

#include "config.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";

void ota_mark_healthy(void)
{
    // Errors just mean no rollback was pending; nothing to do about them.
    esp_ota_mark_app_valid_cancel_rollback();
}

esp_err_t ota_check_and_apply(void)
{
    const esp_app_desc_t *running = esp_app_get_description();

    esp_http_client_config_t http = {
        .url = OTA_RELEASE_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        // GitHub's release redirect points at a signed CDN URL that can run
        // past 2 KB; a transmit buffer that cannot hold the whole rewritten
        // request gets the connection dropped mid-headers (observed).
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
    };
    const esp_https_ota_config_t cfg = {
        .http_config = &http,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no update reachable: %s", esp_err_to_name(err));
        return err;
    }

    esp_app_desc_t incoming;
    err = esp_https_ota_get_img_desc(handle, &incoming);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "image header unreadable: %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        return err;
    }

    if (strncmp(incoming.version, running->version, sizeof(incoming.version)) == 0) {
        ESP_LOGI(TAG, "up to date (%s)", running->version);
        esp_https_ota_abort(handle);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "updating %s -> %s", running->version, incoming.version);

    int last_pct = -1;
    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        const int total = esp_https_ota_get_image_size(handle);
        if (total > 0) {
            const int pct = 100 * esp_https_ota_get_image_len_read(handle) / total;
            if (pct / 10 != last_pct / 10) {
                last_pct = pct;
                ESP_LOGI(TAG, "download %d%%", pct);
            }
        }
    }

    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGE(TAG, "download failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        return err != ESP_OK ? err : ESP_ERR_INVALID_SIZE;
    }

    err = esp_https_ota_finish(handle);  // validates signature/checksum, sets boot slot
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "finish failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "update applied, rebooting into %s", incoming.version);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;  // unreached
}
