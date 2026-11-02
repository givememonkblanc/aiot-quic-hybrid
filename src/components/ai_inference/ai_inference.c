#include "ai_inference.h"

#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include "esp_heap_caps.h"
#include "ai_inference_tflm.h"
#include "jpeg_decoder.h"
#ifndef AIOT_AI_ROI
#define AIOT_AI_ROI 0
#endif
#ifndef AIOT_AI_ROI_SCALE
#define AIOT_AI_ROI_SCALE 8
#endif
#define AIOT_AI_QUARTER (AIOT_AI_ROI && AIOT_AI_ROI_SCALE == 4)

static const char *TAG = "ai_inference";

/*
 * Decode the camera JPEG at full QQVGA (160x120) so the TinyML model gets real
 * detail; ai_inference_tflm's fill_input_tensor then downsamples to the model's
 * own input size (e.g. 96x96). Previously this decoded at 1/8 scale into a
 * 40x30 buffer, feeding the classifier a ~20x15 thumbnail. The buffer lives in
 * PSRAM (allocated once in ai_inference_init) to avoid internal DRAM pressure.
 * The 40x30 AIOT_INFERENCE_INPUT_* constants are only the motion detector's
 * subsample grid, independent of this decode resolution.
 */
#if AIOT_CAMERA_UXGA && AIOT_AI_QUARTER
/* the model region is cut from a quarter-size frame, 400x300 */
#define AIOT_DECODE_WIDTH 400U
#elif AIOT_CAMERA_UXGA
#define AIOT_DECODE_WIDTH 200U
#else
#define AIOT_DECODE_WIDTH 160U
#endif
#if AIOT_CAMERA_UXGA && AIOT_AI_QUARTER
#define AIOT_DECODE_HEIGHT 300U
#else
#define AIOT_DECODE_HEIGHT 160U
#endif
#define AIOT_DECODE_RGB565_BYTES (AIOT_DECODE_WIDTH * AIOT_DECODE_HEIGHT * 2U)

static uint8_t *g_decode_buffer;
static size_t g_decode_buffer_size;

static void ai_inference_log_heap_state(const char *stage)
{
    ESP_LOGI(TAG,
        "%s: internal_free=%u internal_largest=%u spiram_free=%u spiram_largest=%u",
        stage,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

static uint8_t ai_inference_rgb565_luma(const uint8_t *pixel)
{
    uint16_t value = (uint16_t)pixel[0] | (uint16_t)((uint16_t)pixel[1] << 8);
    uint8_t red = (uint8_t)((value >> 11) & 0x1FU);
    uint8_t green = (uint8_t)((value >> 5) & 0x3FU);
    uint8_t blue = (uint8_t)(value & 0x1FU);

    uint16_t red8 = (uint16_t)(red * 255U) / 31U;
    uint16_t green8 = (uint16_t)(green * 255U) / 63U;
    uint16_t blue8 = (uint16_t)(blue * 255U) / 31U;

    return (uint8_t)((red8 * 77U + green8 * 150U + blue8 * 29U) >> 8);
}

static uint8_t ai_inference_normalize_coord(uint16_t value, uint16_t limit)
{
    if (limit == 0U) {
        return 0U;
    }

    if (value >= limit) {
        value = (uint16_t)(limit - 1U);
    }

    return (uint8_t)((value * 255U) / limit);
}

static uint8_t ai_inference_normalize_extent(uint16_t value, uint16_t limit)
{
    if (limit == 0U) {
        return 0U;
    }

    if (value > limit) {
        value = limit;
    }

    return (uint8_t)((value * 255U) / limit);
}

/*
 * Presence-oriented detector: frame-to-frame motion. A person entering the
 * view changes many pixels; a static scene changes almost none. Confidence
 * scales with the fraction of pixels whose luma changed since the previous
 * frame, so waving/walking in front reads high and an empty room reads ~0.
 * This is what makes the sensor react like a real occupancy detector (the
 * old bright-blob heuristic never fired on a person against a plain wall).
 */
#define AIOT_MOTION_MAX_PIXELS (AIOT_INFERENCE_INPUT_WIDTH * AIOT_INFERENCE_INPUT_HEIGHT)

static bool ai_inference_detect_motion(ai_inference_t *inference,
    const uint8_t *rgb565,
    uint16_t width,
    uint16_t height,
    aiot_ai_result_t *result)
{
    if (inference == NULL || rgb565 == NULL || result == NULL || width == 0U || height == 0U) {
        return false;
    }
    /* Sample the decoded frame onto a fixed motion grid so the reference buffer
     * stays small and the detector is independent of the decode resolution. */
    const uint16_t gw = AIOT_INFERENCE_INPUT_WIDTH;
    const uint16_t gh = AIOT_INFERENCE_INPUT_HEIGHT;
    const uint32_t n = (uint32_t)gw * gh;

    /* Per-instance reference frame — see the note in ai_inference_t. */
    uint8_t *prev = inference->prev_luma;
    bool have_prev = (inference->prev_luma_count == n);
    uint32_t changed = 0U;
    uint16_t min_x = gw, min_y = gh, max_x = 0U, max_y = 0U;

    for (uint16_t gy = 0; gy < gh; ++gy) {
        uint16_t sy = (uint16_t)((uint32_t)gy * height / gh);
        for (uint16_t gx = 0; gx < gw; ++gx) {
            uint16_t sx = (uint16_t)((uint32_t)gx * width / gw);
            uint32_t si = (uint32_t)sy * width + sx;
            uint8_t luma = ai_inference_rgb565_luma(rgb565 + (si * 2U));
            uint32_t gi = (uint32_t)gy * gw + gx;
            if (have_prev) {
                int diff = (int)luma - (int)prev[gi];
                if (diff < 0) {
                    diff = -diff;
                }
                if (diff >= 18) {       /* per-pixel change above sensor noise */
                    changed++;
                    if (gx < min_x) min_x = gx;
                    if (gx > max_x) max_x = gx;
                    if (gy < min_y) min_y = gy;
                    if (gy > max_y) max_y = gy;
                }
            }
            prev[gi] = luma;
        }
    }
    inference->prev_luma_count = n;

    if (!have_prev) {
        return false;               /* first frame only seeds the reference */
    }

    /* ~12.75% of pixels changing saturates confidence at 255. */
    uint32_t confidence = (changed * 2000U) / n;
    if (confidence > 255U) {
        confidence = 255U;
    }
    if (confidence < 8U) {          /* essentially static: no presence signal */
        return false;
    }

    result->detection_count = 1U;
    result->raw_score = (uint8_t)confidence;
    result->detections[0].class_id = AIOT_AI_CLASS_GENERIC_OBJECT;
    result->detections[0].confidence = (uint8_t)confidence;
    result->detections[0].x = ai_inference_normalize_coord(min_x, gw);
    result->detections[0].y = ai_inference_normalize_coord(min_y, gh);
    result->detections[0].width = ai_inference_normalize_extent((uint16_t)(max_x - min_x + 1U), gw);
    result->detections[0].height = ai_inference_normalize_extent((uint16_t)(max_y - min_y + 1U), gh);
    return true;
}

static bool ai_inference_decode_jpeg(const uint8_t *jpeg,
    uint32_t jpeg_len,
    ai_inference_t *inference,
    aiot_ai_result_t *result,
    esp_jpeg_image_output_t *decode_info)
{
    if (jpeg_len < 100U) {
        ESP_LOGW(TAG, "JPEG too small to decode: len=%u (minimum 100)", (unsigned)jpeg_len);
        return false;
    }

    if (g_decode_buffer == NULL || jpeg == NULL || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
        ESP_LOGE(TAG, "Invalid JPEG SOI marker: len=%u first bytes=0x%02x 0x%02x",
                 (unsigned)jpeg_len,
                 (unsigned)(jpeg ? jpeg[0] : 0U),
                 (unsigned)(jpeg ? jpeg[1] : 0U));
        return false;
    }

    esp_jpeg_image_cfg_t info_cfg = {
        .indata = (uint8_t *)jpeg,
        .indata_size = jpeg_len,
        .outbuf = NULL,
        .outbuf_size = 0U,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
    };
    esp_jpeg_image_output_t source_info = {0};
    if (esp_jpeg_get_image_info(&info_cfg, &source_info) == ESP_OK) {
        result->source_width = source_info.width;
        result->source_height = source_info.height;
    }

    esp_jpeg_image_cfg_t decode_cfg = {
        .indata = (uint8_t *)jpeg,
        .indata_size = jpeg_len,
        .outbuf = g_decode_buffer,
        .outbuf_size = g_decode_buffer_size,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
    };

    /* Preserve the 160x120 AI decode buffer when the preview is QVGA. */
    if (source_info.width == 320U && source_info.height == 240U)
        decode_cfg.out_scale = JPEG_IMAGE_SCALE_1_2;
    if (source_info.width == 640U && source_info.height == 640U)
        decode_cfg.out_scale = JPEG_IMAGE_SCALE_1_4;
    /* Higher-resolution preview keeps the bounded 160-square AI buffer. */
    if (source_info.width == 1024U && source_info.height == 1024U)
        decode_cfg.out_scale = JPEG_IMAGE_SCALE_1_8;
    /* Native UXGA preview decodes to 200x150, never full-resolution RGB. */
    if (source_info.width == 1600U && source_info.height == 1200U)
        decode_cfg.out_scale = AIOT_AI_QUARTER ? JPEG_IMAGE_SCALE_1_4 : JPEG_IMAGE_SCALE_1_8;
#if AIOT_CAMERA_SVGA_TEST
    /* Match the UXGA test's 200x150 decoded AI raster and 96x96 model input. */
    if (source_info.width == 800U && source_info.height == 600U)
        decode_cfg.out_scale = JPEG_IMAGE_SCALE_1_4;
#endif

    esp_err_t err = esp_jpeg_decode(&decode_cfg, decode_info);
    if (err != ESP_OK) {
        inference->decode_failures++;
        ESP_LOGW(TAG, "JPEG decode failed err=0x%x len=%u",
            (unsigned)err,
            (unsigned)jpeg_len);
        return false;
    }

    result->input_width = decode_info->width;
    result->input_height = decode_info->height;
    return true;
}

static bool ai_inference_run_heuristic(ai_inference_t *inference,
    const uint8_t *jpeg,
    uint32_t jpeg_len,
    uint32_t source_sequence,
    aiot_ai_result_t *result)
{
    esp_jpeg_image_output_t decode_info = {0};

    aiot_ai_result_init(result, AIOT_AI_BACKEND_HEURISTIC, source_sequence);
    if (!ai_inference_decode_jpeg(jpeg, jpeg_len, inference, result, &decode_info)) {
        return false;
    }

    inference->inference_count++;
    return ai_inference_detect_motion(inference, g_decode_buffer, decode_info.width, decode_info.height, result);
}

#if AIOT_AI_DARK_BLOBS
/*
 * Dark objects on a bright plate (screws on a turntable). A pixel is dark when its luma is
 * below AIOT_AI_DARK_FRACTION percent of the median luma of the frame. Connected dark
 * regions between the area limits are objects; larger regions are background outside the
 * plate or a fixture, smaller ones noise. The same detector exists for the host in
 * experiments/moq/dark_blobs.py.
 */
#ifndef AIOT_AI_DARK_FRACTION
#define AIOT_AI_DARK_FRACTION 55
#endif
#define AIOT_BLOB_AREA_MIN 3U
#define AIOT_BLOB_AREA_MAX 80U
static uint8_t *g_blob_mask;     /* 0 bright, 1 dark, 2 visited */
static uint16_t *g_blob_stack;

static bool ai_inference_detect_dark_blobs(const uint8_t *rgb565, uint16_t width, uint16_t height,
                                           aiot_ai_result_t *result)
{
    const uint32_t n = (uint32_t)width * height;
    if (n == 0U || n > 65535U) return false;
    if (g_blob_mask == NULL) {
        g_blob_mask = heap_caps_malloc(65536U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        g_blob_stack = heap_caps_malloc(65536U * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (g_blob_mask == NULL || g_blob_stack == NULL) return false;
    }
    uint32_t histogram[256] = {0};
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t l = ai_inference_rgb565_luma(rgb565 + i * 2U);
        g_blob_mask[i] = l;
        histogram[l]++;
    }
    uint32_t below = 0U, median = 0U;
    while (median < 255U && (below += histogram[median]) < n / 2U) median++;
    const uint32_t threshold = median * AIOT_AI_DARK_FRACTION / 100U;
    for (uint32_t i = 0; i < n; ++i) g_blob_mask[i] = g_blob_mask[i] < threshold ? 1U : 0U;

    uint8_t count = 0U;
    for (uint32_t start = 0; start < n && count < AIOT_AI_RESULT_MAX_DETECTIONS; ++start) {
        if (g_blob_mask[start] != 1U) continue;
        uint32_t top = 0U, area = 0U;
        uint16_t min_x = width, min_y = height, max_x = 0U, max_y = 0U;
        g_blob_stack[top++] = (uint16_t)start;
        g_blob_mask[start] = 2U;
        while (top) {
            uint32_t p = g_blob_stack[--top];
            uint16_t x = (uint16_t)(p % width), y = (uint16_t)(p / width);
            area++;
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    int nx = (int)x + dx, ny = (int)y + dy;
                    if (nx < 0 || ny < 0 || nx >= (int)width || ny >= (int)height) continue;
                    uint32_t q = (uint32_t)ny * width + (uint32_t)nx;
                    if (g_blob_mask[q] != 1U || top >= 65535U) continue;
                    g_blob_mask[q] = 2U;
                    g_blob_stack[top++] = (uint16_t)q;
                }
            }
        }
        if (area < AIOT_BLOB_AREA_MIN || area > AIOT_BLOB_AREA_MAX) continue;
        aiot_ai_detection_t *d = &result->detections[count++];
        d->class_id = AIOT_AI_CLASS_GENERIC_OBJECT;
        d->confidence = 255U;
        d->x = ai_inference_normalize_coord(min_x, width);
        d->y = ai_inference_normalize_coord(min_y, height);
        d->width = ai_inference_normalize_extent((uint16_t)(max_x - min_x + 1U), width);
        d->height = ai_inference_normalize_extent((uint16_t)(max_y - min_y + 1U), height);
    }
    result->detection_count = count;
    result->raw_score = count ? 255U : 0U;
    return true;
}
#endif

static bool ai_inference_run_tflm(ai_inference_t *inference,
    const uint8_t *jpeg,
    uint32_t jpeg_len,
    uint32_t source_sequence,
    aiot_ai_result_t *result)
{
    const ai_inference_model_asset_t *model = ai_inference_model_asset_get();
    esp_jpeg_image_output_t decode_info = {0};

    aiot_ai_result_init(result, AIOT_AI_BACKEND_TFLM, source_sequence);
    if (!ai_inference_model_asset_available() || model == NULL) {
        inference->model_unavailable_count++;
        return false;
    }

    if (!ai_inference_tflm_runtime_available()) {
        inference->backend_fallbacks++;
        ESP_LOGW(TAG, "TFLM dependency/runtime unavailable; falling back seq=%lu",
            (unsigned long)source_sequence);
        return false;
    }

#if AIOT_MOBILITY_CAMERA
    int64_t decode_start = esp_timer_get_time();
#endif
    if (!ai_inference_decode_jpeg(jpeg, jpeg_len, inference, result, &decode_info)) {
        return false;
    }
#if AIOT_MOBILITY_CAMERA
    int64_t model_start = esp_timer_get_time();
#endif
    if (!ai_inference_tflm_run(model, g_decode_buffer, decode_info.width, decode_info.height, result)) {
        inference->backend_fallbacks++;
        ESP_LOGW(TAG,
            "TFLM invoke/postprocess failed; falling back seq=%lu arena=%lu model_len=%lu",
            (unsigned long)source_sequence,
            (unsigned long)model->tensor_arena_size,
            (unsigned long)model->len);
        return false;
    }

#if AIOT_MOBILITY_CAMERA
    printf("[AI_PHASE] {\"seq\":%lu,\"decode_us\":%lld,\"model_us\":%lld}\n", (unsigned long)source_sequence,
        (long long)(model_start-decode_start), (long long)(esp_timer_get_time()-model_start));
#endif
    inference->inference_count++;
    return true;
}

size_t ai_inference_copy_raster(uint8_t *dst, size_t capacity, uint16_t width, uint16_t height)
{
    size_t bytes = (size_t)width * height * 2U;
    if (dst == NULL || g_decode_buffer == NULL || bytes == 0U || bytes > g_decode_buffer_size || bytes > capacity) {
        return 0U;
    }
    for (size_t i = 0; i < bytes; i += 2U) {
        dst[i] = g_decode_buffer[i + 1U];
        dst[i + 1U] = g_decode_buffer[i];
    }
    return bytes;
}

/* Decodes a JPEG into the caller's buffer at the detector's raster (UXGA at 1/8: 200x150), byte-swapped as
 * ai_inference_copy_raster returns it; reentrant (its own output buffer), for a decoder on another core. */
size_t ai_inference_decode_raster(const uint8_t *jpeg, uint32_t jpeg_len, uint8_t *dst, size_t capacity,
    uint16_t *width, uint16_t *height)
{
    if (jpeg == NULL || dst == NULL || jpeg_len < 100U || jpeg[0] != 0xFF || jpeg[1] != 0xD8) return 0U;
    esp_jpeg_image_cfg_t info_cfg = {.indata = (uint8_t *)jpeg, .indata_size = jpeg_len,
        .out_format = JPEG_IMAGE_FORMAT_RGB565, .out_scale = JPEG_IMAGE_SCALE_0};
    esp_jpeg_image_output_t info = {0};
    if (esp_jpeg_get_image_info(&info_cfg, &info) != ESP_OK) return 0U;
    esp_jpeg_image_cfg_t cfg = {.indata = (uint8_t *)jpeg, .indata_size = jpeg_len, .outbuf = dst,
        .outbuf_size = (uint32_t)capacity, .out_format = JPEG_IMAGE_FORMAT_RGB565, .out_scale = JPEG_IMAGE_SCALE_0};
    if (info.width == 320U && info.height == 240U) cfg.out_scale = JPEG_IMAGE_SCALE_1_2;
    if (info.width == 640U && info.height == 640U) cfg.out_scale = JPEG_IMAGE_SCALE_1_4;
    if (info.width == 1024U && info.height == 1024U) cfg.out_scale = JPEG_IMAGE_SCALE_1_8;
    if (info.width == 1600U && info.height == 1200U) cfg.out_scale = AIOT_AI_QUARTER ? JPEG_IMAGE_SCALE_1_4 : JPEG_IMAGE_SCALE_1_8;
    esp_jpeg_image_output_t out = {0};
    if (esp_jpeg_decode(&cfg, &out) != ESP_OK) return 0U;
    size_t bytes = (size_t)out.width * out.height * 2U;
    if (bytes > capacity) return 0U;
    for (size_t i = 0; i < bytes; i += 2U) { uint8_t t = dst[i]; dst[i] = dst[i + 1U]; dst[i + 1U] = t; }
    *width = out.width; *height = out.height;
    return bytes;
}

bool ai_inference_decode_only(ai_inference_t *inference, const uint8_t *jpeg, uint32_t jpeg_len,
    uint32_t source_sequence, aiot_ai_result_t *result)
{
    esp_jpeg_image_output_t decode_info = {0};
    if (inference == NULL || result == NULL) {
        return false;
    }
    aiot_ai_result_init(result, AIOT_AI_BACKEND_NONE, source_sequence);
    return ai_inference_decode_jpeg(jpeg, jpeg_len, inference, result, &decode_info);
}

void ai_inference_init(ai_inference_t *inference)
{
    if (inference == NULL) {
        return;
    }

    /* Allocate the shared decode buffer once, preferring PSRAM. */
    if (g_decode_buffer == NULL) {
        ai_inference_log_heap_state("ai_init:before decode buffer alloc");
        g_decode_buffer_size = AIOT_DECODE_RGB565_BYTES;
        g_decode_buffer = heap_caps_malloc(g_decode_buffer_size,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ai_inference_log_heap_state("ai_init:after psram decode buffer alloc");
        if (g_decode_buffer == NULL) {
            g_decode_buffer = heap_caps_malloc(g_decode_buffer_size, MALLOC_CAP_8BIT);
            ai_inference_log_heap_state("ai_init:after internal decode buffer fallback");
        }
        if (g_decode_buffer == NULL) {
            g_decode_buffer_size = 0;
            ESP_LOGE(TAG, "decode buffer alloc failed (%u bytes)",
                (unsigned)AIOT_DECODE_RGB565_BYTES);
        }
    }

    memset(inference, 0, sizeof(*inference));
    inference->initialized = true;
    inference->model_ready = ai_inference_model_asset_available();
    inference->selected_backend =
        (inference->model_ready && ai_inference_tflm_runtime_available())
            ? AI_INFERENCE_BACKEND_TFLM
            : AI_INFERENCE_BACKEND_HEURISTIC;
    ai_inference_log_heap_state("ai_init:complete");
}

void ai_inference_get_capabilities(ai_inference_capabilities_t *capabilities)
{
    const ai_inference_model_asset_t *model = ai_inference_model_asset_get();

    if (capabilities == NULL) {
        return;
    }

    memset(capabilities, 0, sizeof(*capabilities));
    capabilities->model_available = ai_inference_model_asset_available();
    capabilities->backend = (capabilities->model_available && ai_inference_tflm_runtime_available())
        ? AI_INFERENCE_BACKEND_TFLM
        : AI_INFERENCE_BACKEND_HEURISTIC;

    if (model != NULL) {
        capabilities->tensor_arena_size = model->tensor_arena_size;
        capabilities->model_input_width = model->input_width;
        capabilities->model_input_height = model->input_height;
    }
}

bool ai_inference_run(ai_inference_t *inference,
    const uint8_t *jpeg,
    uint32_t jpeg_len,
    uint32_t source_sequence,
    aiot_ai_result_t *result)
{
    bool ok = false;
    bool tflm_available = false;

    if (inference == NULL || jpeg == NULL || jpeg_len == 0U || result == NULL) {
        return false;
    }

    if (!inference->initialized) {
        ai_inference_init(inference);
    }
#if AIOT_AI_DARK_BLOBS
    {
        esp_jpeg_image_output_t decode_info = {0};
        aiot_ai_result_init(result, AIOT_AI_BACKEND_DARK_BLOBS, source_sequence);
        inference->selected_backend = AI_INFERENCE_BACKEND_HEURISTIC;
        if (!ai_inference_decode_jpeg(jpeg, jpeg_len, inference, result, &decode_info)) return false;
        inference->inference_count++;
        return ai_inference_detect_dark_blobs(g_decode_buffer, decode_info.width, decode_info.height, result);
    }
#endif

    inference->model_ready = ai_inference_model_asset_available();
    tflm_available = inference->model_ready && ai_inference_tflm_runtime_available();
    inference->selected_backend = tflm_available
        ? AI_INFERENCE_BACKEND_TFLM
        : AI_INFERENCE_BACKEND_HEURISTIC;

    if (tflm_available) {
        ok = ai_inference_run_tflm(inference, jpeg, jpeg_len, source_sequence, result);
        if (ok) {
            return true;
        }
    } else if (!inference->model_ready) {
        inference->model_unavailable_count++;
    }

    inference->selected_backend = AI_INFERENCE_BACKEND_HEURISTIC;
    return ai_inference_run_heuristic(inference, jpeg, jpeg_len, source_sequence, result);
}
