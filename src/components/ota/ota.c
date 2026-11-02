#include "include/ota.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"

#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

static const char *TAG = "ota";

void ota_confirm_boot(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        return;
    }
    esp_ota_img_states_t state;
    esp_err_t state_err = esp_ota_get_state_partition(running, &state);
    if (state_err == ESP_ERR_NOT_FOUND && running->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
        running->subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_15) {
        /* Initial USB bootstrap has erased otadata. Record this healthy slot so
         * the first wireless update has a known-good rollback destination. */
        if (esp_ota_set_boot_partition(running) == ESP_OK &&
            esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
            ESP_LOGI(TAG, "bootstrap OTA slot recorded valid");
        return;
    }
    if (state_err != ESP_OK) return; /* factory / non-OTA partition */
    if (state == ESP_OTA_IMG_PENDING_VERIFY || state == ESP_OTA_IMG_UNDEFINED || state == ESP_OTA_IMG_NEW) {
        /* Reaching app_main means the new image booted far enough to run.
         * A stricter product would gate this on end-to-end self-tests. */
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGI(TAG, "new OTA image confirmed valid (rollback cancelled)");
        } else {
            ESP_LOGW(TAG, "failed to confirm OTA image");
        }
    }
}

esp_err_t ota_run_from_url(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        /* Verify the update server against the built-in CA bundle. Enable
         * CONFIG_MBEDTLS_CERTIFICATE_BUNDLE (menuconfig) for HTTPS updates. */
        .crt_bundle_attach = esp_crt_bundle_attach,
#else
        .skip_cert_common_name_check = true,
#endif
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

#ifndef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    ESP_LOGW(TAG, "OTA server certificate NOT verified (cert bundle disabled)");
#endif

    ESP_LOGI(TAG, "OTA download starting: %s", url);
    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "OTA succeeded; rebooting into the new image");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
    }
    return err;
}
