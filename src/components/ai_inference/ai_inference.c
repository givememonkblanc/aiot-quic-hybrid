#include "ai_inference.h"

#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "ai_inference_tflm.h"
#include "jpeg_decoder.h"

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
#define AIOT_DECODE_WIDTH 160U
#define AIOT_DECODE_HEIGHT 120U
#define AIOT_DECODE_RGB565_BYTES (AIOT_DECODE_WIDTH * AIOT_DECODE_HEIGHT * 2U)

static uint8_t *g_decode_buffer;
static size_t g_decode_buffer_size;

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
    uint16_t jpeg_len,
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
    uint16_t jpeg_len,
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

static bool ai_inference_run_tflm(ai_inference_t *inference,
    const uint8_t *jpeg,
    uint16_t jpeg_len,
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

    if (!ai_inference_decode_jpeg(jpeg, jpeg_len, inference, result, &decode_info)) {
        return false;
    }

    if (!ai_inference_tflm_run(model, g_decode_buffer, decode_info.width, decode_info.height, result)) {
        inference->backend_fallbacks++;
        ESP_LOGW(TAG,
            "TFLM invoke/postprocess failed; falling back seq=%lu arena=%lu model_len=%lu",
            (unsigned long)source_sequence,
            (unsigned long)model->tensor_arena_size,
            (unsigned long)model->len);
        return false;
    }

    inference->inference_count++;
    return true;
}

void ai_inference_init(ai_inference_t *inference)
{
    if (inference == NULL) {
        return;
    }

    /* Allocate the shared decode buffer once, preferring PSRAM. */
    if (g_decode_buffer == NULL) {
        g_decode_buffer_size = AIOT_DECODE_RGB565_BYTES;
        g_decode_buffer = heap_caps_malloc(g_decode_buffer_size,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (g_decode_buffer == NULL) {
            g_decode_buffer = heap_caps_malloc(g_decode_buffer_size, MALLOC_CAP_8BIT);
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
    uint16_t jpeg_len,
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
