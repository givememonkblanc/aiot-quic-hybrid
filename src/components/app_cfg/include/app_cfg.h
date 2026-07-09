#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Persistent device configuration for the presence-sensor product.
 * Values live in NVS (namespace "aiotcfg") so a device is provisioned in the
 * field — over the serial console or the SoftAP setup portal — without
 * rebuilding firmware. app_config.h values act as compile-time defaults;
 * the "REPLACE_ME" placeholder counts as unset.
 */

#define APP_CFG_SSID_LEN 33
#define APP_CFG_PASSWORD_LEN 65
#define APP_CFG_HOST_LEN 64
#define APP_CFG_DEVICE_ID_LEN 32

typedef struct {
    char wifi_ssid[APP_CFG_SSID_LEN];
    char wifi_password[APP_CFG_PASSWORD_LEN];
    char server_host[APP_CFG_HOST_LEN];
    uint16_t server_port;
    char device_id[APP_CFG_DEVICE_ID_LEN];
    /* presence tuning */
    uint8_t presence_enter_conf;
    uint8_t presence_exit_conf;
    uint8_t presence_enter_frames;
    uint32_t presence_exit_timeout_ms;
    uint32_t heartbeat_ms;
    uint8_t snapshot_on_enter;
} app_cfg_t;

/* Load NVS values over compile-time defaults. Always leaves *cfg usable. */
esp_err_t app_cfg_load(app_cfg_t *cfg);

/* Persist the whole struct to NVS. */
esp_err_t app_cfg_save(const app_cfg_t *cfg);

/* Factory reset: wipe the config namespace. */
esp_err_t app_cfg_erase(void);

/* True when Wi-Fi credentials and server host are usable. */
bool app_cfg_provisioned(const app_cfg_t *cfg);

/*
 * Set a field by dotted key ("wifi.ssid", "wifi.pass", "server.host",
 * "server.port", "device.id", "presence.enter", "presence.exit",
 * "presence.frames", "presence.timeout_ms", "heartbeat.ms",
 * "snapshot.on_enter"). Returns ESP_ERR_INVALID_ARG for unknown keys or
 * out-of-range values. Does not persist — call app_cfg_save().
 */
esp_err_t app_cfg_set(app_cfg_t *cfg, const char *key, const char *value);

/* Print the config to stdout with the password masked. */
void app_cfg_print(const app_cfg_t *cfg);
