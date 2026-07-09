#pragma once

#include "esp_err.h"

/*
 * Over-the-air firmware update for the presence sensor.
 *
 * Requires an OTA-capable partition table (otadata + ota_0 + ota_1); see
 * partitions_ota.csv and docs/PRODUCT.md. On a factory-only layout these
 * calls are safe no-ops / return an error without touching flash.
 */

/*
 * Call once early in app_main. If the running image is booting for the first
 * time after an OTA (ESP_OTA_IMG_PENDING_VERIFY) it is marked valid so the
 * bootloader will not roll back. A production build should run self-tests
 * before confirming; here a successful boot to this point is the check.
 */
void ota_confirm_boot(void);

/*
 * Download and install a new image from `url` (HTTP/HTTPS) using
 * esp_https_ota, then reboot into it. Only returns on failure, with the
 * esp_err_t describing what went wrong; the current image keeps running.
 */
esp_err_t ota_run_from_url(const char *url);
