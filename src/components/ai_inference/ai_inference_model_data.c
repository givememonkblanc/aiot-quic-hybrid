#include "ai_inference_model_data.h"

#if defined(__has_include)
#if __has_include("ai_inference_model_asset_generated.h")
#include "ai_inference_model_asset_generated.h"
#define AIOT_GENERATED_MODEL_ASSET_PRESENT 1
#else
#define AIOT_GENERATED_MODEL_ASSET_PRESENT 0
#endif
#else
#define AIOT_GENERATED_MODEL_ASSET_PRESENT 0
#endif

#if AIOT_GENERATED_MODEL_ASSET_PRESENT
#define AIOT_ACTIVE_MODEL_ASSET (&g_ai_inference_generated_model_asset)
#else
static const ai_inference_model_asset_t g_model_asset = {
    .data = NULL,
    .len = 0U,
    .tensor_arena_size = 0U,
    .input_width = 0U,
    .input_height = 0U,
    .detection_threshold = 0U,
    .class_count = 0U,
    .background_index = 0xFFU,
    .output_type = AI_INFERENCE_OUTPUT_GENERIC,
};
#define AIOT_ACTIVE_MODEL_ASSET (&g_model_asset)
#endif

bool ai_inference_model_asset_available(void)
{
    const ai_inference_model_asset_t *asset = AIOT_ACTIVE_MODEL_ASSET;

    return asset->data != NULL &&
        asset->len > 0U &&
        asset->tensor_arena_size > 0U &&
        asset->input_width > 0U &&
        asset->input_height > 0U;
}

const ai_inference_model_asset_t *ai_inference_model_asset_get(void)
{
    return AIOT_ACTIVE_MODEL_ASSET;
}
