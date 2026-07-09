#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ai_inference_model_data.h"

/*
 * Active TinyML model: Google "Visual Wake Words" person detection
 * (MobileNet, 96x96 grayscale, int8). The weights live in
 * ai_inference_person_model.c (copied verbatim from the esp-tflite-micro
 * person_detection example, len = 300568 bytes).
 *
 * Output is a 2-class softmax {notperson=0, person=1}; the person score
 * (target_class_index = 1) drives presence detection. When TFLM cannot load
 * this model at runtime, ai_inference.c transparently falls back to the
 * frame-difference motion detector.
 */
extern const unsigned char g_person_detect_model_data[];

static const ai_inference_model_asset_t g_ai_inference_generated_model_asset = {
    .data = g_person_detect_model_data,
    .len = 300568U,
    .tensor_arena_size = 150U * 1024U,
    .input_width = 96U,
    .input_height = 96U,
    .detection_threshold = 150U, /* ~59% person score, matches example's 60% */
    .class_count = 2U,
    .background_index = 0U,       /* notperson */
    .target_class_index = 1U,     /* person */
    .output_type = AI_INFERENCE_OUTPUT_CLASSIFICATION,
};
