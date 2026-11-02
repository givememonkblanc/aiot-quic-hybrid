#include "ai_inference_model_data.h"

#if defined(__has_include)
#if __has_include("ai_inference_model_asset_generated.h")
#if AIOT_MODEL_COMPRESSED
#include "ai_model_compact.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "miniz.h"
extern const unsigned char g_aiot_model_zlib[];
extern const size_t g_aiot_model_zlib_len;
#else
#include "ai_inference_model_asset_generated.h"
#endif
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
    const ai_inference_model_asset_t *asset = ai_inference_model_asset_get();

    return asset->data != NULL &&
        asset->len > 0U &&
        asset->tensor_arena_size > 0U &&
        asset->input_width > 0U &&
        asset->input_height > 0U;
}

const ai_inference_model_asset_t *ai_inference_model_asset_get(void)
{
#if AIOT_MODEL_COMPRESSED
    static ai_inference_model_asset_t expanded;
    static bool attempted;
    if (!attempted) {
        attempted = true;
        expanded = *AIOT_ACTIVE_MODEL_ASSET;
        uint8_t *buffer = heap_caps_malloc(expanded.len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buffer) {
            size_t bytes = tinfl_decompress_mem_to_mem(buffer, expanded.len,
                g_aiot_model_zlib, g_aiot_model_zlib_len, TINFL_FLAG_PARSE_ZLIB_HEADER);
            if (bytes == expanded.len) expanded.data = buffer;
            else heap_caps_free(buffer);
        }
        if (!expanded.data) ESP_LOGE("ai_model", "Lossless model unpack failed");
    }
    return &expanded;
#else
    return AIOT_ACTIVE_MODEL_ASSET;
#endif
}
