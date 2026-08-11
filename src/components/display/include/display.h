#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Status screen for the ESP32-S3-EYE's 1.3" 240x240 ST7789 LCD.
 *
 * No LVGL: the OTA slot leaves ~70KB of headroom, so this renders text with
 * an embedded 8x16 font into a PSRAM framebuffer and flushes over SPI.
 *
 * Pages cycle on the BOOT button (GPIO0):
 *   0  presence state + score + link status
 *   1  network / firmware details
 *   2  screen off (backlight)
 */

typedef struct {
    const char *device_id;
    const char *server_host;
    uint16_t server_port;
    bool session_up;        /* QUIC+MQTT session established */
    int presence_state;     /* presence_state_t: 0 unknown, 1 absent, 2 present */
    uint8_t score;          /* last frame score */
    uint8_t enter_threshold;
    uint32_t heartbeats;
    uint32_t frames_total;
} display_status_t;

/* Bring up the panel and show a splash. Safe to call on boards without the
 * LCD: SPI errors are logged once and later calls become no-ops. */
esp_err_t display_init(void);

/* Poll the page button and re-render (throttled internally). Call from the
 * control loop at any cadence >= ~10Hz for responsive button handling. */
void display_update(const display_status_t *st);
