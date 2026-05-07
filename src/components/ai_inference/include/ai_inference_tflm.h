#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ai_inference_model_data.h"
#include "ai_result.h"

#ifdef __cplusplus
extern "C" {
#endif

bool ai_inference_tflm_runtime_available(void);
bool ai_inference_tflm_run(const ai_inference_model_asset_t *model,
    const uint8_t *rgb565,
    uint16_t width,
    uint16_t height,
    aiot_ai_result_t *result);

#ifdef __cplusplus
}
#endif
