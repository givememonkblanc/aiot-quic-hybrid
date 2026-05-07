#include "include/source_manager.h"

#include <stdio.h>
#include <string.h>

#if __has_include("esp_timer.h")
#include "esp_timer.h"
#define AIOT_SOURCE_HAS_ESP_TIMER 1
#else
#include <sys/time.h>
#endif

#if __has_include("esp_heap_caps.h")
#include "esp_heap_caps.h"
#endif

#include "sdkconfig.h"
#include "ai_inference.h"
#include "ai_result.h"
#include "esp_log.h"
#include "esp_camera.h"

static const char *TAG = "source_manager";

static uint64_t source_manager_now_us(void)
{
#if AIOT_SOURCE_HAS_ESP_TIMER
    return (uint64_t)esp_timer_get_time();
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((uint64_t)tv.tv_sec * 1000000ULL) + (uint64_t)tv.tv_usec;
#endif
}

#if CONFIG_IDF_TARGET_ESP32S3
#define CAMERA_PIN_PWDN 38
#define CAMERA_PIN_RESET -1
#define CAMERA_PIN_XCLK 15
#define CAMERA_PIN_SIOD 4
#define CAMERA_PIN_SIOC 5
#define CAMERA_PIN_D7 16
#define CAMERA_PIN_D6 17
#define CAMERA_PIN_D5 18
#define CAMERA_PIN_D4 12
#define CAMERA_PIN_D3 10
#define CAMERA_PIN_D2 8
#define CAMERA_PIN_D1 9
#define CAMERA_PIN_D0 11
#define CAMERA_PIN_VSYNC 6
#define CAMERA_PIN_HREF 7
#define CAMERA_PIN_PCLK 13
#else
#define CAMERA_PIN_PWDN 32
#define CAMERA_PIN_RESET -1
#define CAMERA_PIN_XCLK 0
#define CAMERA_PIN_SIOD 26
#define CAMERA_PIN_SIOC 27
#define CAMERA_PIN_D7 35
#define CAMERA_PIN_D6 21
#define CAMERA_PIN_D5 19
#define CAMERA_PIN_D4 18
#define CAMERA_PIN_D3 5
#define CAMERA_PIN_D2 4
#define CAMERA_PIN_D1 34
#define CAMERA_PIN_D0 39
#define CAMERA_PIN_VSYNC 25
#define CAMERA_PIN_HREF 23
#define CAMERA_PIN_PCLK 22
#endif

static camera_config_t camera_config = {
    .pin_pwdn = CAMERA_PIN_PWDN,
    .pin_reset = CAMERA_PIN_RESET,
    .pin_xclk = CAMERA_PIN_XCLK,
    .pin_sscb_sda = CAMERA_PIN_SIOD,
    .pin_sscb_scl = CAMERA_PIN_SIOC,
    .pin_d7 = CAMERA_PIN_D7,
    .pin_d6 = CAMERA_PIN_D6,
    .pin_d5 = CAMERA_PIN_D5,
    .pin_d4 = CAMERA_PIN_D4,
    .pin_d3 = CAMERA_PIN_D3,
    .pin_d2 = CAMERA_PIN_D2,
    .pin_d1 = CAMERA_PIN_D1,
    .pin_d0 = CAMERA_PIN_D0,
    .pin_vsync = CAMERA_PIN_VSYNC,
    .pin_href = CAMERA_PIN_HREF,
    .pin_pclk = CAMERA_PIN_PCLK,
    .xclk_freq_hz = 20000000,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_JPEG,
    .frame_size = FRAMESIZE_QVGA,
    .jpeg_quality = 12,
    .fb_count = 1,
    .fb_location = CAMERA_FB_IN_DRAM,
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
};

static bool g_camera_initialized = false;
static ai_inference_t g_inference;

static const uint8_t test_jpeg[] = {
    0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01,
    0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xFF, 0xDB, 0x00, 0x43,
    0x00, 0x08, 0x06, 0x06, 0x07, 0x06, 0x05, 0x08, 0x07, 0x07, 0x07, 0x09,
    0x09, 0x08, 0x0A, 0x0C, 0x14, 0x0D, 0x0C, 0x0B, 0x0B, 0x0C, 0x19, 0x12,
    0x13, 0x0F, 0x14, 0x1D, 0x1A, 0x1F, 0x1E, 0x1D, 0x1A, 0x1C, 0x1C, 0x20,
    0x24, 0x2E, 0x27, 0x20, 0x22, 0x2C, 0x23, 0x1C, 0x1C, 0x28, 0x37, 0x29,
    0x2C, 0x30, 0x31, 0x34, 0x34, 0x34, 0x1F, 0x27, 0x39, 0x3D, 0x38, 0x32,
    0x3C, 0x2E, 0x33, 0x34, 0x32, 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x11, 0x00, 0xFF, 0xC4, 0x00, 0x1F, 0x00, 0x00,
    0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
    0x0A, 0x0B, 0xFF, 0xC4, 0x00, 0xB5, 0x10, 0x00, 0x02, 0x01, 0x03, 0x03,
    0x02, 0x04, 0x03, 0x05, 0x05, 0x04, 0x04, 0x00, 0x00, 0x01, 0x7D, 0x01,
    0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13,
    0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08, 0x23,
    0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0, 0x24, 0x33, 0x62, 0x72, 0x82,
    0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28, 0x29,
    0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46,
    0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A,
    0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76,
    0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A,
    0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4,
    0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7,
    0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA,
    0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2, 0xE3,
    0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5,
    0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00,
    0x00, 0x3F, 0x00, 0xFB, 0xD5, 0xDB, 0x20, 0xA8, 0xA2, 0x80, 0x0A, 0xFF,
    0xD9
};

static int test_jpeg_len = sizeof(test_jpeg);
static uint8_t g_image_buffer[8192];

static void source_manager_store_ai_result(source_manager_t *manager,
    const aiot_ai_result_t *result)
{
    if (manager == NULL || result == NULL) {
        return;
    }

    manager->ai_payload.len = aiot_ai_result_serialize(result,
        manager->ai_payload.data,
        (uint16_t)sizeof(manager->ai_payload.data));
}

static void source_manager_set_empty_ai_result(source_manager_t *manager, uint32_t source_sequence)
{
    aiot_ai_result_t result;

    aiot_ai_result_init(&result, AIOT_AI_BACKEND_NONE, source_sequence);
    source_manager_store_ai_result(manager, &result);
}

static void source_manager_run_inference(source_manager_t *manager,
    const uint8_t *jpeg,
    uint16_t jpeg_len,
    uint32_t source_sequence)
{
    aiot_ai_result_t result;

    if (jpeg == NULL || jpeg_len < 100U || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
        ESP_LOGW(TAG, "Invalid JPEG for inference: len=%u first bytes=0x%02x 0x%02x",
                 (unsigned)jpeg_len,
                 (unsigned)(jpeg ? jpeg[0] : 0U),
                 (unsigned)(jpeg ? jpeg[1] : 0U));
        aiot_ai_result_init(&result, AIOT_AI_BACKEND_NONE, source_sequence);
        source_manager_store_ai_result(manager, &result);
        return;
    }

    bool has_detection = ai_inference_run(&g_inference,
        jpeg,
        jpeg_len,
        source_sequence,
        &result);

    if (!has_detection) {
        aiot_ai_result_init(&result, AIOT_AI_BACKEND_HEURISTIC, source_sequence);
    }

    source_manager_store_ai_result(manager, &result);
}

void source_manager_init(source_manager_t *manager)
{
    ai_inference_capabilities_t capabilities;

    memset(manager, 0, sizeof(*manager));
    ai_inference_init(&g_inference);
    ai_inference_get_capabilities(&capabilities);
    source_manager_set_empty_ai_result(manager, 0U);

#if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 4
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0U) {
        camera_config.fb_location = CAMERA_FB_IN_PSRAM;
        camera_config.fb_count = 1;
    } else {
        camera_config.fb_location = CAMERA_FB_IN_DRAM;
        camera_config.fb_count = 1;
    }
#endif

    ESP_LOGI(TAG,
        "Inference backend=%u model_available=%u tensor_arena=%lu model_input=%ux%u fb_location=%s fb_count=%u",
        (unsigned)capabilities.backend,
        capabilities.model_available ? 1U : 0U,
        (unsigned long)capabilities.tensor_arena_size,
        (unsigned)capabilities.model_input_width,
        (unsigned)capabilities.model_input_height,
        camera_config.fb_location == CAMERA_FB_IN_PSRAM ? "psram" : "dram",
        (unsigned)camera_config.fb_count);

    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed with error 0x%x", err);
        g_camera_initialized = false;
        return;
    }

    g_camera_initialized = true;

    sensor_t *s = esp_camera_sensor_get();
    if (s != NULL) {
        s->set_brightness(s, 0);
        s->set_contrast(s, 0);
        s->set_saturation(s, 0);
        s->set_whitebal(s, 1);
        s->set_awb_gain(s, 1);
        s->set_wb_mode(s, 0);
        s->set_exposure_ctrl(s, 1);
        s->set_aec2(s, 0);
        s->set_ae_level(s, 0);
        s->set_aec_value(s, 300);
        s->set_gain_ctrl(s, 1);
        s->set_agc_gain(s, 0);
        s->set_gainceiling(s, (gainceiling_t)0);
        s->set_bpc(s, 0);
        s->set_wpc(s, 1);
        s->set_raw_gma(s, 1);
        s->set_lenc(s, 1);
        s->set_hmirror(s, 0);
        s->set_vflip(s, 0);
        s->set_dcw(s, 1);
        s->set_colorbar(s, 0);
    }

    ESP_LOGI(TAG, "Camera initialized successfully");
}

bool source_manager_next_packet(source_manager_t *manager, data_packet_t *packet)
{
    memset(packet, 0, sizeof(*packet));
    packet->sequence = manager->next_sequence++;
    packet->timestamp_us = source_manager_now_us();

    switch (manager->slot++ % 2U) {
        case 0:
            packet->type = DATA_TYPE_AI_RESULT;
            packet->priority = 0;
            packet->deadline_class = 0;
            packet->loss_tolerant = false;
            packet->payload = manager->ai_payload.data;
            packet->payload_len = manager->ai_payload.len;
            return true;

        default: {
            size_t jpeg_len = g_camera_initialized ? 0 : test_jpeg_len;

            if (g_camera_initialized) {
                camera_fb_t *fb = esp_camera_fb_get();
                if (fb != NULL) {
                    jpeg_len = fb->len;
                    ESP_LOGD(TAG, "Camera frame: len=%zu first=0x%02x 0x%02x",
                              jpeg_len,
                              (unsigned)(jpeg_len > 0 ? fb->buf[0] : 0),
                              (unsigned)(jpeg_len > 1 ? fb->buf[1] : 0));
                    if (jpeg_len > 0 && jpeg_len <= sizeof(g_image_buffer) &&
                        fb->buf[0] == 0xFF && fb->buf[1] == 0xD8) {
                        memcpy(g_image_buffer, fb->buf, jpeg_len);
                    } else {
                        ESP_LOGW(TAG, "Invalid camera frame: len=%zu, using test JPEG", jpeg_len);
                        jpeg_len = 0;
                    }
                    esp_camera_fb_return(fb);
                } else {
                    ESP_LOGW(TAG, "Camera fb is NULL, using test JPEG");
                    jpeg_len = 0;
                }
            } else {
                ESP_LOGW(TAG, "Camera not initialized, using test JPEG");
            }

            if (jpeg_len == 0) {
                memcpy(g_image_buffer, test_jpeg, test_jpeg_len);
                jpeg_len = test_jpeg_len;
                ESP_LOGD(TAG, "Using test JPEG: len=%d", jpeg_len);
            }

            source_manager_run_inference(manager, g_image_buffer, (uint16_t)jpeg_len, packet->sequence);

            packet->type = DATA_TYPE_IMAGE;
            packet->priority = 1;
            packet->deadline_class = 1;
            packet->loss_tolerant = true;
            packet->payload = g_image_buffer;
            packet->payload_len = jpeg_len;
            ESP_LOGD(TAG, "Image: %zu bytes", jpeg_len);
            return true;
        }
    }
}
