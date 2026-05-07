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
} ai_inference_backend_t;

typedef struct {
    bool initialized;
    bool model_ready;
    ai_inference_backend_t selected_backend;
    uint32_t inference_count;
    uint32_t decode_failures;
    uint32_t backend_fallbacks;
    uint32_t model_unavailable_count;
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
    uint16_t jpeg_len,
    uint32_t source_sequence,
    aiot_ai_result_t *result);
