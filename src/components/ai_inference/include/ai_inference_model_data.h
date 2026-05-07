#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AIOT_TFLM_MODEL_SCHEMA_VERSION 0U

typedef enum {
    AI_INFERENCE_OUTPUT_GENERIC = 0,
    AI_INFERENCE_OUTPUT_FOMO = 1,
} ai_inference_output_type_t;

typedef struct {
    const uint8_t *data;
    size_t len;
    uint32_t tensor_arena_size;
    uint16_t input_width;
    uint16_t input_height;
    uint8_t detection_threshold;
    uint8_t class_count;
    uint8_t background_index;
    ai_inference_output_type_t output_type;
} ai_inference_model_asset_t;

bool ai_inference_model_asset_available(void);
const ai_inference_model_asset_t *ai_inference_model_asset_get(void);
