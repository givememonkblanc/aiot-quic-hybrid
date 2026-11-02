#include "ota.h"
#include <string.h>
#include <stdio.h>
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota_pull";
static int health_camera = -1, health_backend = -1;
static uint32_t health_frames, health_inference_us;
void ota_set_local_health(bool camera_ready, uint32_t frames, uint32_t inference_us, int backend)
{
    health_camera = camera_ready;
    health_frames = frames;
    health_inference_us = inference_us;
    health_backend = backend;
}
static void running_hash(char hash[65])
{
    const esp_app_desc_t *desc = esp_app_get_description();
    for (unsigned i = 0; i < 32; ++i) snprintf(hash + 2*i, 3, "%02x", desc->app_elf_sha256[i]);
}


bool ota_boot_pending(void)
{
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    return running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

void ota_rollback_if_pending(void)
{
    if (!ota_boot_pending()) return;
    ESP_LOGW(TAG, "event=rollback reason=unconfirmed_image");
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

void ota_log_status(void)
{
    char hash[65]; running_hash(hash);
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "event=boot slot=%s pending=%d elf_sha256=%s",
             running ? running->label : "unknown", ota_boot_pending(), hash);
}

#ifdef AIOT_OTA_SITE_HEADER
#include AIOT_OTA_SITE_HEADER
bool ota_pull_enabled(void) { return true; }
static bool valid_hash(const char *s)
{
    if (!s || strlen(s) != 64) return false;
    for (unsigned i = 0; i < 64; ++i)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
}
static bool attempted(const char *hash)
{
    nvs_handle_t nvs; char previous[65]; size_t len = sizeof(previous);
    if (nvs_open("ota_pull", NVS_READONLY, &nvs) != ESP_OK) return false;
    bool match = nvs_get_str(nvs, "attempt", previous, &len) == ESP_OK && strcmp(hash, previous) == 0;
    nvs_close(nvs); return match;
}
static esp_err_t remember(const char *hash)
{
    nvs_handle_t nvs; esp_err_t err = nvs_open("ota_pull", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = hash ? nvs_set_str(nvs, "attempt", hash) : nvs_erase_key(nvs, "attempt");
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) err = nvs_commit(nvs);
    nvs_close(nvs); return err;
}
static esp_http_client_config_t http_config(const char *url)
{
    esp_http_client_config_t config = {
        .url = url, .cert_pem = aiot_ota_server_cert,
        .timeout_ms = 10000, .buffer_size = 2048, .buffer_size_tx = 1024,
        .disable_auto_redirect = true,
    };
    return config;
}
bool ota_pull_check(char target[65])
{
    char current[65], url[384];
    running_hash(current);
    int length = snprintf(url, sizeof(url),
        "%s/next?current=%s&pending=%d&camera=%d&frames=%lu&ai_us=%lu&backend=%d",
        AIOT_OTA_BASE_URL, current, ota_boot_pending(), health_camera,
        (unsigned long)health_frames, (unsigned long)health_inference_us, health_backend);
    if (length < 0 || length >= sizeof(url)) return false;
    esp_http_client_config_t config = http_config(url);
    esp_http_client_handle_t http = esp_http_client_init(&config);
    if (!http) return false;
    bool available = false;
    esp_err_t err = esp_http_client_open(http, 0);
    if (err == ESP_OK) {
        int64_t size = esp_http_client_fetch_headers(http);
        int status = esp_http_client_get_status_code(http);
        if (status == 200 && size == 64) {
            int n = esp_http_client_read_response(http, target, 64);
            target[n > 0 ? n : 0] = '\0';
            available = n == 64 && valid_hash(target) && strcmp(current, target) != 0;
            if (available && attempted(target)) {
                ESP_LOGW(TAG, "event=blocked_previous_attempt elf_sha256=%s", target);
                available = false; /* A rollback must not immediately reinstall the bad image. */
            }
        }
        ESP_LOGI(TAG, "event=poll status=%d update=%d", status, available);
    } else ESP_LOGW(TAG, "event=poll_failed error=%s", esp_err_to_name(err));
    esp_http_client_cleanup(http);
    return available;
}
esp_err_t ota_pull_install(const char *target)
{
    if (!valid_hash(target) || attempted(target)) return ESP_ERR_INVALID_ARG;
    if (!esp_ota_get_next_update_partition(NULL) || ota_boot_pending()) return ESP_ERR_INVALID_STATE;
    char url[256]; snprintf(url, sizeof(url), "%s/images/%s.bin", AIOT_OTA_BASE_URL, target);
    esp_http_client_config_t http = http_config(url);
    esp_https_ota_config_t config = {.http_config = &http};
    esp_https_ota_handle_t ota = NULL;
    ESP_LOGI(TAG, "event=download_begin target=%s", target);
    esp_err_t err = esp_https_ota_begin(&config, &ota);
    if (err != ESP_OK) return err;
    esp_app_desc_t desc;
    err = esp_https_ota_get_img_desc(ota, &desc);
    if (err == ESP_OK) {
        char hash[65];
        for (unsigned i = 0; i < 32; ++i) snprintf(hash + 2*i, 3, "%02x", desc.app_elf_sha256[i]);
        if (strcmp(hash, target) || strcmp(desc.project_name, esp_app_get_description()->project_name))
            err = ESP_ERR_OTA_VALIDATE_FAILED;
    }
    if (err != ESP_OK) { esp_https_ota_abort(ota); return err; }
    int last = 0;
    do {
        err = esp_https_ota_perform(ota);
        int bytes = esp_https_ota_get_image_len_read(ota);
        if (bytes - last >= 131072) {
            last = bytes; ESP_LOGI(TAG, "event=download_progress bytes=%d", bytes);
        }
        vTaskDelay(1);
    } while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);
    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(ota)) {
        esp_https_ota_abort(ota); return err == ESP_OK ? ESP_FAIL : err;
    }
    err = remember(target);
    if (err != ESP_OK) { esp_https_ota_abort(ota); return err; }
    err = esp_https_ota_finish(ota);
    if (err != ESP_OK) { remember(NULL); return err; }
    ESP_LOGI(TAG, "event=download_complete reboot=1");
    vTaskDelay(pdMS_TO_TICKS(200)); esp_restart();
    return ESP_OK;
}
#else
bool ota_pull_enabled(void) { return false; }
bool ota_pull_check(char target[65]) { (void)target; return false; }
esp_err_t ota_pull_install(const char *target) { (void)target; return ESP_ERR_NOT_SUPPORTED; }
#endif
