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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#if AIOT_CAMERA_INTEGRITY_TEST
#include "mbedtls/sha256.h"
static uint32_t camera_set_errors;
static unsigned camera_set_index;
static int camera_regs[10];
static char camera_diagnostic[640];
static size_t camera_diagnostic_len;
#define CAMERA_SET(expr) do { if ((expr) != 0) camera_set_errors |= 1U << camera_set_index; camera_set_index++; } while (0)
size_t source_manager_camera_diagnostic(char *out, size_t capacity)
{
    if (!camera_diagnostic_len || capacity <= camera_diagnostic_len) return 0;
    memcpy(out, camera_diagnostic, camera_diagnostic_len + 1);
    return camera_diagnostic_len;
}
#else
#define CAMERA_SET(expr) ((void)(expr))
#endif

static const char *TAG = "source_manager";

static void source_manager_log_heap_state(const char *stage)
{
#if AIOT_SOURCE_HAS_ESP_TIMER
    ESP_LOGI(TAG,
        "%s: internal_free=%u internal_largest=%u spiram_free=%u spiram_largest=%u",
        stage,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
#else
    (void)stage;
#endif
}

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
/* ESP32-S3-EYE camera PWDN is pulled low on the board, not GPIO-driven.
 * GPIO38 belongs to the microSD CMD signal. Other S3 boards may override. */
#ifndef CAMERA_PIN_PWDN
#define CAMERA_PIN_PWDN -1
#endif
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
#if defined(AIOT_CAMERA_XCLK_HZ)
    .xclk_freq_hz = AIOT_CAMERA_XCLK_HZ,
#else
    .xclk_freq_hz = 20000000,
#endif
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_JPEG,
#if AIOT_MOBILITY_CAMERA
#if AIOT_CAMERA_SVGA_TEST
    .frame_size = FRAMESIZE_SVGA,
#elif AIOT_CAMERA_UXGA
    .frame_size = FRAMESIZE_UXGA,
#else
    .frame_size = FRAMESIZE_SXGA,    /* Allocate room before OV2640 native 1024-square window. */
#endif
#else
    .frame_size = FRAMESIZE_QQVGA,
#endif
#if AIOT_MOBILITY_CAMERA
#if defined(AIOT_CAMERA_JPEG_QUALITY)
    .jpeg_quality = AIOT_CAMERA_JPEG_QUALITY,
#elif AIOT_CAMERA_UXGA
    .jpeg_quality = 6,  /* Preserve fine detail for remote screw inspection. */
#else
    .jpeg_quality = 10,
#endif
#else
    .jpeg_quality = 12,
#endif
    .fb_count = 1,
    .fb_location = CAMERA_FB_IN_DRAM,
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
};

static bool g_camera_initialized = false;
#if AIOT_CAMERA_DIAG
static unsigned diag_good, diag_bad, diag_max;
#endif
bool source_manager_camera_ready(void) { return g_camera_initialized; }
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
#if AIOT_MOBILITY_CAMERA
#define SOURCE_IMAGE_CAPACITY 262144U
static uint8_t *g_image_buffer;
#else
#define SOURCE_IMAGE_CAPACITY 8192U
static uint8_t g_image_buffer[SOURCE_IMAGE_CAPACITY];
#endif

#if AIOT_MOBILITY_CAMERA && defined(AIOT_CAMERA_BYTE_BUDGET)
/* Frame bytes held to a budget, as Vigil (MobiCom 2015) holds the uploads of an epoch to the estimated
 * capacity: in the dark the sensor gain raises noise, noise raises the JPEG bytes (10/10: 72 KB in the
 * day, 240-310 KB at night, over the 256 KB buffer, so nearly every frame was dropped). The OV2640
 * quantizer steps coarser while a frame is over the budget and back finer while it is well under, never
 * finer than the configured quality: in the day the camera is unchanged. A new quality applies a frame
 * or two later, so the step waits two frames. */
static int camera_quality, camera_quality_min;
static void camera_budget(size_t len)
{
    static unsigned wait;
    const size_t b = AIOT_CAMERA_BYTE_BUDGET;
    if (wait) { wait--; return; }
    int q = camera_quality;
    if (len > b * 11 / 10) q += len > 2 * b ? 4 : len > b * 3 / 2 ? 2 : 1;
    else if (len < b * 8 / 10) q--;
    if (q < camera_quality_min) q = camera_quality_min;
    if (q > 63) q = 63;
    if (q == camera_quality) return;
    sensor_t *s = esp_camera_sensor_get();
    if (!s || s->set_quality(s, q) != 0) return;
    printf("[CAM_BUDGET] {\"bytes\":%u,\"quality\":%d,\"was\":%d}\n", (unsigned)len, q, camera_quality);
    camera_quality = q;
    wait = 2;
}
#endif

static void source_manager_store_ai_result(source_manager_t *manager,
    const aiot_ai_result_t *result)
{
    if (manager == NULL || result == NULL) {
        return;
    }

    manager->ai_payload.len = aiot_ai_result_serialize(result,
        manager->ai_payload.data,
        (uint16_t)sizeof(manager->ai_payload.data));
    manager->last_result = *result;
}

bool source_manager_get_last_ai_result(const source_manager_t *manager,
    aiot_ai_result_t *out, uint32_t *inference_us)
{
    if (manager == NULL || out == NULL) {
        return false;
    }
    *out = manager->last_result;
    if (inference_us != NULL) {
        *inference_us = manager->last_inference_us;
    }
    return manager->last_ran;
}

static void source_manager_set_empty_ai_result(source_manager_t *manager, uint32_t source_sequence)
{
    aiot_ai_result_t result;

    aiot_ai_result_init(&result, AIOT_AI_BACKEND_NONE, source_sequence);
    source_manager_store_ai_result(manager, &result);
}

static void source_manager_run_inference(source_manager_t *manager,
    const uint8_t *jpeg,
    uint32_t jpeg_len,
    uint32_t source_sequence)
{
    aiot_ai_result_t result;

    if (jpeg == NULL || jpeg_len < 100U || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
        ESP_LOGW(TAG, "Invalid JPEG for inference: len=%u first bytes=0x%02x 0x%02x",
                 (unsigned)jpeg_len,
                 (unsigned)(jpeg ? jpeg[0] : 0U),
                 (unsigned)(jpeg ? jpeg[1] : 0U));
        aiot_ai_result_init(&result, AIOT_AI_BACKEND_NONE, source_sequence);
        manager->last_ran = false;
        manager->last_inference_us = 0U;
        manager->last_importance = (uint8_t)AIOT_IMPORTANCE_ROUTINE;
        source_manager_store_ai_result(manager, &result);
        return;
    }

    uint64_t t0 = source_manager_now_us();
    bool has_detection = false;
    /* AIOT_AI_REPEAT > 1 stands for a slower model with the same accuracy: the result is that
     * of the last run, the time and the core are taken k times */
    for (unsigned run = 0; run < AIOT_AI_REPEAT; run++) {
        has_detection = ai_inference_run(&g_inference,
            jpeg,
            jpeg_len,
            source_sequence,
            &result);
    }
    manager->last_inference_us = (uint32_t)(source_manager_now_us() - t0);
    manager->last_ran = has_detection;

    if (!has_detection) {
        aiot_ai_result_init(&result, AIOT_AI_BACKEND_HEURISTIC, source_sequence);
    }

    /* Cross-layer hint: compare against the previous result (still in
     * last_result) before it is overwritten, then publish the importance. */
    manager->last_importance =
        (uint8_t)aiot_importance_from_result(&result, &manager->last_result);
    source_manager_store_ai_result(manager, &result);
}

#if AIOT_MOBILITY_CAMERA
#include "source_async.inc"
#if AIOT_EMBED_REPLAY_JPEG
extern const uint8_t replay_jpeg_start[] asm("_binary_replay_jpeg_start");
extern const uint8_t replay_jpeg_end[] asm("_binary_replay_jpeg_end");
#endif
bool source_manager_replay_next(data_packet_t *packet)
{
#if AIOT_EMBED_REPLAY_JPEG
    static uint8_t *fixed;
    static unsigned epoch;
    static uint64_t origin;
    static uint32_t last_slot = UINT32_MAX;
    unsigned requested = mobility_replay_epoch();
    if (!requested) return false;
    size_t len = replay_jpeg_end-replay_jpeg_start;
    if (!fixed) {
        fixed = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!fixed) return false;
        memcpy(fixed,replay_jpeg_start,len);
    }
    uint64_t now = source_manager_now_us();
    if (epoch != requested) { epoch=requested;origin=now;last_slot=UINT32_MAX; }
    uint32_t slot=(uint32_t)((now-origin)/100000);
    if (slot == last_slot) return false;
    uint32_t missed=last_slot==UINT32_MAX ? slot : slot-last_slot-1;
    last_slot=slot;
    *packet=(data_packet_t){.type=DATA_TYPE_IMAGE,.sequence=(epoch<<20)|slot,
        .timestamp_us=origin+(uint64_t)slot*100000,.payload=fixed,.payload_len=len,.priority=1};
    moq_audit_printf("[REPLAY] {\"epoch\":%u,\"slot\":%lu,\"seq\":%lu,\"scheduled_us\":%llu,\"actual_us\":%llu,\"missed\":%lu}\n",
        epoch,(unsigned long)slot,(unsigned long)packet->sequence,(unsigned long long)packet->timestamp_us,
        (unsigned long long)now,(unsigned long)missed);
    return true;
#else
    (void)packet;
    return false;
#endif
}

#endif

void source_manager_init(source_manager_t *manager)
{
    ai_inference_capabilities_t capabilities;

    memset(manager, 0, sizeof(*manager));
#if AIOT_MOBILITY_CAMERA
    if (!g_image_buffer)
        g_image_buffer = heap_caps_malloc(SOURCE_IMAGE_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_image_buffer) {
        ESP_LOGE(TAG, "Camera JPEG buffer allocation failed");
        return;
    }
#endif
    ai_inference_init(&g_inference);
    ai_inference_get_capabilities(&capabilities);
    source_manager_set_empty_ai_result(manager, 0U);

    /* Keep the camera framebuffer in PSRAM when the chip has it, freeing
     * scarce internal DRAM for task stacks (the network TX task needs 16KB).
     * This used to sit behind an `#if defined(ESP_IDF_VERSION_MAJOR)` guard
     * that was always false (the version header wasn't included), so the fb
     * silently stayed in DRAM and starved the transport task on the larger
     * product build. heap_caps_get_total_size is available on IDF v4+. */
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0U) {
        camera_config.fb_location = CAMERA_FB_IN_PSRAM;
        camera_config.fb_count = 1;
#if AIOT_MOBILITY_CAMERA
        camera_config.fb_count = 2;
        camera_config.grab_mode = CAMERA_GRAB_LATEST;
#endif
    } else {
        camera_config.fb_location = CAMERA_FB_IN_DRAM;
        camera_config.fb_count = 1;
    }

    ESP_LOGI(TAG,
        "Inference backend=%u model_available=%u tensor_arena=%lu model_input=%ux%u fb_location=%s fb_count=%u",
        (unsigned)capabilities.backend,
        capabilities.model_available ? 1U : 0U,
        (unsigned long)capabilities.tensor_arena_size,
        (unsigned)capabilities.model_input_width,
        (unsigned)capabilities.model_input_height,
        camera_config.fb_location == CAMERA_FB_IN_PSRAM ? "psram" : "dram",
        (unsigned)camera_config.fb_count);

    source_manager_log_heap_state("camera:init before esp_camera_init");

    esp_err_t err = esp_camera_init(&camera_config);
#if AIOT_MOBILITY_CAMERA && defined(AIOT_CAMERA_BYTE_BUDGET)
    camera_quality = camera_quality_min = camera_config.jpeg_quality;
#endif
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed with error 0x%x", err);
        g_camera_initialized = false;
        return;
    }

    source_manager_log_heap_state("camera:init after esp_camera_init");

    g_camera_initialized = true;

    sensor_t *s = esp_camera_sensor_get();
#if AIOT_CAMERA_INTEGRITY_TEST
    camera_set_errors = 0;
    camera_set_index = 0;
#endif
#if !AIOT_CAMERA_DEFAULTS_TEST
    if (s != NULL) {
        CAMERA_SET(s->set_brightness(s, 0));
        CAMERA_SET(s->set_contrast(s, 0));
        CAMERA_SET(s->set_saturation(s, 0));
        CAMERA_SET(s->set_whitebal(s, 1));
        CAMERA_SET(s->set_awb_gain(s, 1));
        CAMERA_SET(s->set_wb_mode(s, 0));
        CAMERA_SET(s->set_exposure_ctrl(s, 1));
        /* AEC2 controls DSP automatic exposure, not the sensor night-mode bit.
         * Keep automatic gain capped at 16x for the indoor scene. */
        CAMERA_SET(s->set_aec2(s, 1));
        CAMERA_SET(s->set_ae_level(s, 0));
        CAMERA_SET(s->set_aec_value(s, 300));
        CAMERA_SET(s->set_gain_ctrl(s, 1));
        CAMERA_SET(s->set_agc_gain(s, 0));
        CAMERA_SET(s->set_gainceiling(s, (gainceiling_t)3));
        CAMERA_SET(s->set_bpc(s, 0));
        CAMERA_SET(s->set_wpc(s, 1));
        CAMERA_SET(s->set_raw_gma(s, 1));
        CAMERA_SET(s->set_lenc(s, 1));
        CAMERA_SET(s->set_hmirror(s, 0));
        CAMERA_SET(s->set_vflip(s, 0));
        CAMERA_SET(s->set_dcw(s, 1));
        CAMERA_SET(s->set_colorbar(s, 0));
#if AIOT_CAMERA_EXPOSURE_TEST
        /* Register units, not milliseconds. Disable both automatic exposure
         * controllers; leave automatic gain enabled for this diagnostic. */
        if (s->set_aec2(s, 0) != 0 || s->set_exposure_ctrl(s, 0) != 0 ||
            s->set_aec_value(s, 120) != 0) {
            ESP_LOGE(TAG, "Fixed-exposure diagnostic setup failed");
            g_camera_initialized = false;
            return;
        }
#endif
    }

#endif /* application sensor tuning */

#if AIOT_MOBILITY_CAMERA
    if (!s || s->id.PID != OV2640_PID ||
#if AIOT_CAMERA_SVGA_TEST
        s->set_framesize(s, FRAMESIZE_SVGA) != 0 ||
#elif AIOT_CAMERA_QVGA
        s->set_framesize(s, FRAMESIZE_QVGA) != 0 ||   /* the full field of view at 320x240 */
#elif AIOT_CAMERA_UXGA
        s->set_framesize(s, FRAMESIZE_UXGA) != 0 ||
#else
        s->set_res_raw(s, 0, 0, 0, 0, 200, 0, 1200, 1200, 1024, 1024, true, false) != 0 ||
#endif
        !source_ai_start(manager)) {
        ESP_LOGE(TAG, "Camera resolution / asynchronous AI initialization failed");
        g_camera_initialized = false;
        return;
    }
#endif
#if AIOT_CAMERA_COLORBAR_TEST
    /* Apply after the final frame-size call because OV2640 set_window writes
     * COM7, which also contains the color-bar enable bit. */
    if (s->set_colorbar(s, 1) != 0) {
        ESP_LOGE(TAG, "OV2640 color-bar diagnostic setup failed");
        g_camera_initialized = false;
        return;
    }
#endif
#if AIOT_CAMERA_INTEGRITY_TEST
    /* Driver get_reg uses bit 8 for sensor bank. Read once after configuration,
     * before the application starts consuming frames; no per-frame SCCB I/O. */
    const int addresses[] = {0x112, 0x05a, 0x05b, 0x05c, 0x0c0,
                             0x0c1, 0x044, 0x0da, 0x050, 0x08c};
    for (unsigned i = 0; i < 10; i++) camera_regs[i] = s->get_reg(s, addresses[i], 0xff);
#endif
#if AIOT_CAMERA_DIAG
    /* Identification of the sensor and a small frame size, to separate the sensor from the
     * transfer of large frames. */
    printf("[CAM_ID] {\"pid\":%u,\"ver\":%u,\"midh\":%u,\"midl\":%u,\"reg_0a\":%d,\"reg_0b\":%d,\"reg_1c\":%d,\"reg_1d\":%d}\n",
           (unsigned)s->id.PID, (unsigned)s->id.VER, (unsigned)s->id.MIDH, (unsigned)s->id.MIDL,
           s->get_reg(s, 0x10a, 0xff), s->get_reg(s, 0x10b, 0xff), s->get_reg(s, 0x11c, 0xff), s->get_reg(s, 0x11d, 0xff));
    printf("[CAM_DIAG] framesize=QVGA result=%d\n", s->set_framesize(s, FRAMESIZE_QVGA));
#endif
    ESP_LOGI(TAG, "Camera initialized successfully");
}

#if AIOT_PIPELINE_PROFILE
extern uint32_t aiot_task_runtime_now(void);
extern uint64_t camera_profile_ready_us(const camera_fb_t *fb);
#endif
bool source_manager_next_packet(source_manager_t *manager, data_packet_t *packet)
{
#if AIOT_MOBILITY_CAMERA
    if (!g_image_buffer) return false;
#endif
#if AIOT_CAMERA_INTEGRITY_TEST
    camera_diagnostic_len = 0;
#endif
    memset(packet, 0, sizeof(*packet));
    packet->sequence = manager->next_sequence++;
    packet->timestamp_us = source_manager_now_us();

    switch (manager->slot++ % 2U) {
        case 0:
            packet->type = DATA_TYPE_AI_RESULT;
            packet->priority = 0;
            packet->deadline_class = 0;
            packet->loss_tolerant = false;
            packet->importance = manager->last_importance;
            packet->payload = manager->ai_payload.data;
            packet->payload_len = manager->ai_payload.len;
            return true;

        default: {
            manager->last_frame_real = false;
            size_t jpeg_len = 0;

            if (g_camera_initialized) {
#if AIOT_PIPELINE_PROFILE
                int64_t get_start=esp_timer_get_time();uint32_t get_cpu=aiot_task_runtime_now();
#endif
#if AIOT_CAMERA_DIAG
                {
                    /* every 8 s the next frame size; a summary of the stage that ended */
                    static const framesize_t sizes[] = {FRAMESIZE_QVGA, FRAMESIZE_VGA, FRAMESIZE_SVGA, FRAMESIZE_XGA,
                                                        FRAMESIZE_HD, FRAMESIZE_SXGA, FRAMESIZE_UXGA};
                    static const char *names[] = {"QVGA", "VGA", "SVGA", "XGA", "HD", "SXGA", "UXGA"};
                    static int stage = -1;
                    static int64_t since;
                    int64_t now = esp_timer_get_time();
                    if (stage < 6 && (stage < 0 || now - since > 8000000)) {
                        if (stage >= 0)
                            printf("[CAM_STAGE] {\"size\":\"%s\",\"good\":%u,\"bad\":%u,\"bytes_max\":%u}\n",
                                   names[stage], diag_good, diag_bad, diag_max);
                        stage++;
                        diag_good = diag_bad = diag_max = 0;
                        sensor_t *ds = esp_camera_sensor_get();
                        int rc = ds ? ds->set_framesize(ds, sizes[stage]) : -1;
                        printf("[CAM_DIAG] framesize=%s result=%d\n", names[stage], rc);
                        for (int i = 0; i < 2; i++) { camera_fb_t *old = esp_camera_fb_get(); if (old) esp_camera_fb_return(old); }
                        since = esp_timer_get_time();
                    }
                }
#endif
                camera_fb_t *fb = esp_camera_fb_get();
#if AIOT_PIPELINE_PROFILE
                int64_t get_done=esp_timer_get_time();get_cpu=aiot_task_runtime_now()-get_cpu;
                uint64_t sensor_start=fb ? (uint64_t)fb->timestamp.tv_sec*1000000+fb->timestamp.tv_usec : 0;
                uint64_t ready=fb ? camera_profile_ready_us(fb) : 0;
                uint32_t copy_cpu=aiot_task_runtime_now();int64_t copy_start=esp_timer_get_time();
#endif
                if (fb != NULL) {
                    jpeg_len = fb->len;
#if AIOT_MOBILITY_CAMERA && defined(AIOT_CAMERA_BYTE_BUDGET)
                    camera_budget(jpeg_len);
#endif
                    ESP_LOGD(TAG, "Camera frame: len=%zu first=0x%02x 0x%02x",
                              jpeg_len,
                              (unsigned)(jpeg_len > 0 ? fb->buf[0] : 0),
                              (unsigned)(jpeg_len > 1 ? fb->buf[1] : 0));
                    if (jpeg_len >= 4 && jpeg_len <= SOURCE_IMAGE_CAPACITY &&
                        fb->buf[0] == 0xFF && fb->buf[1] == 0xD8) {
#if AIOT_CAMERA_INTEGRITY_TEST
                        static unsigned diagnostic_frame;
                        bool sample = (++diagnostic_frame % 10U) == 1U;
                        uint8_t source_sha[32], copy_sha[32];
                        int hash_rc = 0;
                        int64_t hash_start = esp_timer_get_time();
                        if (sample) hash_rc = mbedtls_sha256(fb->buf, jpeg_len, source_sha, 0);
#endif
                        memcpy(g_image_buffer, fb->buf, jpeg_len);
#if AIOT_CAMERA_INTEGRITY_TEST
                        if (sample) {
                            hash_rc |= mbedtls_sha256(g_image_buffer, jpeg_len, copy_sha, 0);
                            if (!hash_rc) {
                                char hex[65];
                                for (unsigned i=0;i<32;i++) snprintf(hex+2*i,3,"%02x",source_sha[i]);
                                int n = snprintf(camera_diagnostic, sizeof(camera_diagnostic),
                                    "{\"kind\":\"camera_integrity\",\"ts\":%lu,\"bytes\":%u,\"w\":%u,\"h\":%u,\"sha256\":\"%s\",\"copy_equal\":%s,\"probe_us\":%lld,\"set_errors\":%lu,\"regs\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d]}",
                                    (unsigned long)(packet->timestamp_us/1000),(unsigned)jpeg_len,
                                    (unsigned)fb->width,(unsigned)fb->height,hex,
                                    memcmp(source_sha,copy_sha,32)==0?"true":"false",
                                    (long long)(esp_timer_get_time()-hash_start),(unsigned long)camera_set_errors,
                                    camera_regs[0],camera_regs[1],camera_regs[2],camera_regs[3],camera_regs[4],
                                    camera_regs[5],camera_regs[6],camera_regs[7],camera_regs[8],camera_regs[9]);
                                if (n>0 && n<sizeof(camera_diagnostic)) camera_diagnostic_len=n;
                            }
                        }
#endif
                        manager->last_frame_real = true;
#if AIOT_CAMERA_DIAG
                        diag_good++;
                        if (jpeg_len > diag_max) diag_max = (unsigned)jpeg_len;
#endif
                    } else {
#if AIOT_CAMERA_DIAG
                        diag_bad++;
#endif
                        ESP_LOGW(TAG, "Invalid camera frame: len=%zu, using test JPEG", jpeg_len);
                        jpeg_len = 0;
                    }
#if AIOT_PIPELINE_PROFILE
                    copy_cpu=aiot_task_runtime_now()-copy_cpu;
                    int64_t copy_done=esp_timer_get_time();
                    printf("[CAPTURE_PROFILE] {\"seq\":%lu,\"start_us\":%llu,\"ready_us\":%llu,\"get_start_us\":%lld,\"get_done_us\":%lld,\"copy_done_us\":%lld,\"get_cpu_us\":%lu,\"copy_cpu_us\":%lu,\"copy_wall_us\":%lld,\"bytes\":%u,\"valid\":%d}\n",
                        (unsigned long)packet->sequence,(unsigned long long)sensor_start,(unsigned long long)ready,
                        (long long)get_start,(long long)get_done,(long long)copy_done,(unsigned long)get_cpu,
                        (unsigned long)copy_cpu,(long long)(copy_done-copy_start),(unsigned)jpeg_len,manager->last_frame_real);
#endif
                    esp_camera_fb_return(fb);
                } else {
#if AIOT_CAMERA_DIAG
                    diag_bad++;
#endif
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

#if AIOT_MOBILITY_CAMERA
            packet->timestamp_us = source_manager_now_us();
#else
            source_manager_run_inference(manager, g_image_buffer, (uint32_t)jpeg_len, packet->sequence);
#endif

            packet->type = DATA_TYPE_IMAGE;
            packet->priority = 1;
            packet->deadline_class = 1;
            packet->loss_tolerant = true;
            packet->importance = manager->last_importance;
            packet->payload = g_image_buffer;
            packet->payload_len = jpeg_len;
            ESP_LOGD(TAG, "Image: %zu bytes", jpeg_len);
            return true;
        }
    }
}
