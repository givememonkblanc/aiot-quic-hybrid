#include "ai_inference.h"

#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "ai_inference_tflm.h"
#include "jpeg_decoder.h"

static const char *TAG = "ai_inference";

#define AIOT_INFERENCE_RGB565_BYTES (AIOT_INFERENCE_INPUT_WIDTH * AIOT_INFERENCE_INPUT_HEIGHT * 2U)

static uint8_t g_decode_buffer[AIOT_INFERENCE_RGB565_BYTES];

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

static bool ai_inference_detect_blob(const uint8_t *rgb565,
    uint16_t width,
    uint16_t height,
    aiot_ai_result_t *result)
{
    if (rgb565 == NULL || result == NULL || width == 0U || height == 0U) {
        return false;
    }

    uint32_t total_luma = 0U;
    uint8_t max_luma = 0U;
    uint32_t pixel_count = (uint32_t)width * height;

    for (uint32_t i = 0; i < pixel_count; ++i) {
        uint8_t luma = ai_inference_rgb565_luma(rgb565 + (i * 2U));
        total_luma += luma;
        if (luma > max_luma) {
            max_luma = luma;
        }
    }

    uint8_t mean_luma = (uint8_t)(total_luma / pixel_count);
    uint8_t threshold = (uint8_t)(mean_luma + 24U);
    if (threshold < mean_luma || threshold > max_luma) {
        threshold = max_luma;
    }

    uint16_t min_x = width;
    uint16_t min_y = height;
    uint16_t max_x = 0U;
    uint16_t max_y = 0U;
    uint32_t active_pixels = 0U;
    uint32_t active_luma = 0U;

    for (uint16_t y = 0; y < height; ++y) {
        for (uint16_t x = 0; x < width; ++x) {
            uint32_t index = ((uint32_t)y * width + x) * 2U;
            uint8_t luma = ai_inference_rgb565_luma(rgb565 + index);
            if (luma < threshold) {
                continue;
            }

            if (x < min_x) {
                min_x = x;
            }
            if (y < min_y) {
                min_y = y;
            }
            if (x > max_x) {
                max_x = x;
            }
            if (y > max_y) {
                max_y = y;
            }

            active_pixels++;
            active_luma += luma;
        }
    }

    if (active_pixels < (pixel_count / 25U) || min_x >= width || min_y >= height) {
        return false;
    }

    uint16_t box_width = (uint16_t)(max_x - min_x + 1U);
    uint16_t box_height = (uint16_t)(max_y - min_y + 1U);
    if (box_width < 3U || box_height < 3U) {
        return false;
    }

    uint8_t contrast = (uint8_t)((active_luma / active_pixels) - mean_luma);
    uint32_t occupancy = (active_pixels * 255U) / pixel_count;
    uint16_t confidence = (uint16_t)contrast * 3U + (uint16_t)occupancy;
    if (confidence > 255U) {
        confidence = 255U;
    }

    result->detection_count = 1U;
    result->detections[0].class_id = AIOT_AI_CLASS_GENERIC_OBJECT;
    result->detections[0].confidence = (uint8_t)confidence;
    result->detections[0].x = ai_inference_normalize_coord(min_x, width);
    result->detections[0].y = ai_inference_normalize_coord(min_y, height);
    result->detections[0].width = ai_inference_normalize_extent(box_width, width);
    result->detections[0].height = ai_inference_normalize_extent(box_height, height);
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

    if (jpeg == NULL || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
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
        .outbuf_size = sizeof(g_decode_buffer),
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_1_8,
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
    return ai_inference_detect_blob(g_decode_buffer, decode_info.width, decode_info.height, result);
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
