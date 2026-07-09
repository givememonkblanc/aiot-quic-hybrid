#include "include/app_cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "presence.h"

static const char *TAG = "app_cfg";
#define APP_CFG_NAMESPACE "aiotcfg"

static bool app_cfg_is_placeholder(const char *value)
{
    return value == NULL || value[0] == '\0' || strcmp(value, "REPLACE_ME") == 0;
}

static void app_cfg_defaults(app_cfg_t *cfg)
{
    const presence_config_t presence_defaults = PRESENCE_CONFIG_DEFAULTS;

    memset(cfg, 0, sizeof(*cfg));
    if (!app_cfg_is_placeholder(APP_WIFI_SSID)) {
        strlcpy(cfg->wifi_ssid, APP_WIFI_SSID, sizeof(cfg->wifi_ssid));
    }
    if (!app_cfg_is_placeholder(APP_WIFI_PASSWORD)) {
        strlcpy(cfg->wifi_password, APP_WIFI_PASSWORD, sizeof(cfg->wifi_password));
    }
    /* Server is baked in (zero-config): a bench APP_PEER_HOST override wins,
     * otherwise the product's fixed APP_DEFAULT_SERVER_HOST. The customer
     * never enters it, so provisioning only needs Wi-Fi. */
    if (!app_cfg_is_placeholder(APP_PEER_HOST)) {
        strlcpy(cfg->server_host, APP_PEER_HOST, sizeof(cfg->server_host));
    } else if (!app_cfg_is_placeholder(APP_DEFAULT_SERVER_HOST)) {
        strlcpy(cfg->server_host, APP_DEFAULT_SERVER_HOST, sizeof(cfg->server_host));
    }
    cfg->server_port = APP_DEFAULT_SERVER_PORT;

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(cfg->device_id, sizeof(cfg->device_id), "%s-%02x%02x",
             APP_DEVICE_ID, mac[4], mac[5]);

    cfg->presence_enter_conf = presence_defaults.enter_confidence;
    cfg->presence_exit_conf = presence_defaults.exit_confidence;
    cfg->presence_enter_frames = presence_defaults.enter_frames;
    cfg->presence_exit_timeout_ms = presence_defaults.exit_timeout_ms;
    cfg->heartbeat_ms = 5000;
    cfg->snapshot_on_enter = 1;
}

static void app_cfg_get_str(nvs_handle_t handle, const char *key, char *dst, size_t cap)
{
    size_t len = cap;
    if (nvs_get_str(handle, key, dst, &len) != ESP_OK) {
        /* keep default */
    }
}

esp_err_t app_cfg_load(app_cfg_t *cfg)
{
    app_cfg_defaults(cfg);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_flash_init: %s (using defaults)", esp_err_to_name(err));
        return err;
    }

    nvs_handle_t handle;
    err = nvs_open(APP_CFG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return ESP_OK; /* nothing stored yet: defaults are the config */
    }

    app_cfg_get_str(handle, "wifi_ssid", cfg->wifi_ssid, sizeof(cfg->wifi_ssid));
    app_cfg_get_str(handle, "wifi_pass", cfg->wifi_password, sizeof(cfg->wifi_password));
    app_cfg_get_str(handle, "srv_host", cfg->server_host, sizeof(cfg->server_host));
    app_cfg_get_str(handle, "device_id", cfg->device_id, sizeof(cfg->device_id));
    nvs_get_u16(handle, "srv_port", &cfg->server_port);
    nvs_get_u8(handle, "pr_enter", &cfg->presence_enter_conf);
    nvs_get_u8(handle, "pr_exit", &cfg->presence_exit_conf);
    nvs_get_u8(handle, "pr_frames", &cfg->presence_enter_frames);
    nvs_get_u32(handle, "pr_timeout", &cfg->presence_exit_timeout_ms);
    nvs_get_u32(handle, "hb_ms", &cfg->heartbeat_ms);
    nvs_get_u8(handle, "snap", &cfg->snapshot_on_enter);
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t app_cfg_save(const app_cfg_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(APP_CFG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        return err;
    }

    nvs_set_str(handle, "wifi_ssid", cfg->wifi_ssid);
    nvs_set_str(handle, "wifi_pass", cfg->wifi_password);
    nvs_set_str(handle, "srv_host", cfg->server_host);
    nvs_set_str(handle, "device_id", cfg->device_id);
    nvs_set_u16(handle, "srv_port", cfg->server_port);
    nvs_set_u8(handle, "pr_enter", cfg->presence_enter_conf);
    nvs_set_u8(handle, "pr_exit", cfg->presence_exit_conf);
    nvs_set_u8(handle, "pr_frames", cfg->presence_enter_frames);
    nvs_set_u32(handle, "pr_timeout", cfg->presence_exit_timeout_ms);
    nvs_set_u32(handle, "hb_ms", cfg->heartbeat_ms);
    nvs_set_u8(handle, "snap", cfg->snapshot_on_enter);

    err = nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "configuration saved");
    return err;
}

esp_err_t app_cfg_erase(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(APP_CFG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    ESP_LOGW(TAG, "configuration erased (factory reset)");
    return err;
}

bool app_cfg_provisioned(const app_cfg_t *cfg)
{
    /* The customer only provides Wi-Fi; the server endpoint is baked in at
     * build time, so field setup is "Wi-Fi only". */
    return cfg->wifi_ssid[0] != '\0' && cfg->wifi_password[0] != '\0';
}

static esp_err_t app_cfg_set_str(char *dst, size_t cap, const char *value)
{
    if (value == NULL || strlen(value) >= cap) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(dst, value, cap);
    return ESP_OK;
}

esp_err_t app_cfg_set(app_cfg_t *cfg, const char *key, const char *value)
{
    if (cfg == NULL || key == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    long num = strtol(value, NULL, 10);

    if (strcmp(key, "wifi.ssid") == 0) {
        return app_cfg_set_str(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), value);
    }
    if (strcmp(key, "wifi.pass") == 0) {
        return app_cfg_set_str(cfg->wifi_password, sizeof(cfg->wifi_password), value);
    }
    if (strcmp(key, "server.host") == 0) {
        return app_cfg_set_str(cfg->server_host, sizeof(cfg->server_host), value);
    }
    if (strcmp(key, "device.id") == 0) {
        return app_cfg_set_str(cfg->device_id, sizeof(cfg->device_id), value);
    }
    if (strcmp(key, "server.port") == 0) {
        if (num < 1 || num > 65535) return ESP_ERR_INVALID_ARG;
        cfg->server_port = (uint16_t)num;
        return ESP_OK;
    }
    if (strcmp(key, "presence.enter") == 0) {
        if (num < 1 || num > 255) return ESP_ERR_INVALID_ARG;
        cfg->presence_enter_conf = (uint8_t)num;
        return ESP_OK;
    }
    if (strcmp(key, "presence.exit") == 0) {
        if (num < 0 || num > 255) return ESP_ERR_INVALID_ARG;
        cfg->presence_exit_conf = (uint8_t)num;
        return ESP_OK;
    }
    if (strcmp(key, "presence.frames") == 0) {
        if (num < 1 || num > 100) return ESP_ERR_INVALID_ARG;
        cfg->presence_enter_frames = (uint8_t)num;
        return ESP_OK;
    }
    if (strcmp(key, "presence.timeout_ms") == 0) {
        if (num < 500 || num > 3600000) return ESP_ERR_INVALID_ARG;
        cfg->presence_exit_timeout_ms = (uint32_t)num;
        return ESP_OK;
    }
    if (strcmp(key, "heartbeat.ms") == 0) {
        if (num < 500 || num > 3600000) return ESP_ERR_INVALID_ARG;
        cfg->heartbeat_ms = (uint32_t)num;
        return ESP_OK;
    }
    if (strcmp(key, "snapshot.on_enter") == 0) {
        if (num < 0 || num > 1) return ESP_ERR_INVALID_ARG;
        cfg->snapshot_on_enter = (uint8_t)num;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

void app_cfg_print(const app_cfg_t *cfg)
{
    printf("device.id           = %s\n", cfg->device_id);
    printf("wifi.ssid           = %s\n", cfg->wifi_ssid[0] ? cfg->wifi_ssid : "(unset)");
    printf("wifi.pass           = %s\n", cfg->wifi_password[0] ? "********" : "(unset)");
    printf("server.host         = %s\n", cfg->server_host[0] ? cfg->server_host : "(unset)");
    printf("server.port         = %u\n", (unsigned)cfg->server_port);
    printf("presence.enter      = %u\n", (unsigned)cfg->presence_enter_conf);
    printf("presence.exit       = %u\n", (unsigned)cfg->presence_exit_conf);
    printf("presence.frames     = %u\n", (unsigned)cfg->presence_enter_frames);
    printf("presence.timeout_ms = %lu\n", (unsigned long)cfg->presence_exit_timeout_ms);
    printf("heartbeat.ms        = %lu\n", (unsigned long)cfg->heartbeat_ms);
    printf("snapshot.on_enter   = %u\n", (unsigned)cfg->snapshot_on_enter);
    printf("provisioned         = %s\n", app_cfg_provisioned(cfg) ? "yes" : "NO (setup required)");
}
