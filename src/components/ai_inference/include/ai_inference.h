#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ai_result.h"
#include "ai_inference_model_data.h"

#define AIOT_INFERENCE_INPUT_WIDTH 40U
#define AIOT_INFERENCE_INPUT_HEIGHT 30U

typedef enum {
    AI_INFERENCE_BACKEND_HEURISTIC = 0,
    AI_INFERENCE_BACKEND_TFLM = 1,
    AI_INFERENCE_BACKEND_ESPDL = 2,
} ai_inference_backend_t;

typedef struct {
    bool initialized;
    bool model_ready;
    ai_inference_backend_t selected_backend;
    uint32_t inference_count;
    uint32_t decode_failures;
    uint32_t backend_fallbacks;
    uint32_t model_unavailable_count;
    /* Per-instance previous frame for the motion detector. Must NOT be shared
     * across instances: source_manager and app_main each run inference on the
     * same frame, so a shared buffer makes every diff "frame vs itself" = 0. */
    uint8_t prev_luma[AIOT_INFERENCE_INPUT_WIDTH * AIOT_INFERENCE_INPUT_HEIGHT];
    uint32_t prev_luma_count;
} ai_inference_t;

typedef struct {
    ai_inference_backend_t backend;
    bool model_available;
    uint32_t tensor_arena_size;
    uint16_t model_input_width;
    uint16_t model_input_height;
} ai_inference_capabilities_t;

void ai_inference_init(ai_inference_t *inference);
void ai_inference_get_capabilities(ai_inference_capabilities_t *capabilities);
bool ai_inference_run(ai_inference_t *inference,
    const uint8_t *jpeg,
    uint32_t jpeg_len,
    uint32_t source_sequence,
    aiot_ai_result_t *result);

#ifdef AIOT_ESPDET_RUNTIME
/* Single worker only: copy the most recent encoded ROI before another inference. */
size_t ai_inference_copy_roi(uint8_t *dst, size_t capacity);
#endif
/* The RGB565 raster of the last decode (result->input_width x input_height), copied with its bytes
 * swapped to the big-endian order of the JPEG encoder; valid in the task of the inference until its
 * next run. Returns the bytes copied, 0 when it does not fit. */
size_t ai_inference_copy_raster(uint8_t *dst, size_t capacity, uint16_t width, uint16_t height);
size_t ai_inference_decode_raster(const uint8_t *jpeg, uint32_t jpeg_len, uint8_t *dst, size_t capacity,
    uint16_t *width, uint16_t *height);
/* The first stage of the inference alone: the JPEG decoded into the detector's raster (as
 * ai_inference_run decodes it), result->input_width x input_height set; the model is not run, so the
 * rest of the inference can run elsewhere on ai_inference_copy_raster's copy. */
bool ai_inference_decode_only(ai_inference_t *inference, const uint8_t *jpeg, uint32_t jpeg_len,
    uint32_t source_sequence, aiot_ai_result_t *result);
