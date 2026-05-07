#include "ai_inference_tflm.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#if defined(__has_include)
#if __has_include("tensorflow/lite/c/common.h") && \
    __has_include("tensorflow/lite/micro/micro_interpreter.h") && \
    __has_include("tensorflow/lite/micro/micro_mutable_op_resolver.h") && \
    __has_include("tensorflow/lite/schema/schema_generated.h")
#define AIOT_TFLM_AVAILABLE 1
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#else
#define AIOT_TFLM_AVAILABLE 0
#endif
#else
#define AIOT_TFLM_AVAILABLE 0
#endif

static const char *TAG = "ai_tflm";

#if AIOT_TFLM_AVAILABLE
namespace {

struct TflmContext {
    bool initialized;
    const ai_inference_model_asset_t *model_asset;
    uint8_t *tensor_arena;
    size_t tensor_arena_size;
    const tflite::Model *model;
    tflite::MicroMutableOpResolver<24> resolver;
    tflite::MicroInterpreter *interpreter;
};

TflmContext g_tflm;

static bool add_resolver_op(TfLiteStatus status, const char *name)
{
    if (status != kTfLiteOk) {
        ESP_LOGE(TAG, "Resolver registration failed for op=%s", name);
        return false;
    }
    return true;
}

static bool configure_resolver(TflmContext *ctx)
{
    if (ctx == NULL) {
        return false;
    }

    return add_resolver_op(ctx->resolver.AddMul(), "MUL") &&
        add_resolver_op(ctx->resolver.AddMinimum(), "MINIMUM") &&
        add_resolver_op(ctx->resolver.AddRelu(), "RELU") &&
        add_resolver_op(ctx->resolver.AddAveragePool2D(), "AVERAGE_POOL_2D") &&
        add_resolver_op(ctx->resolver.AddDepthwiseConv2D(), "DEPTHWISE_CONV_2D") &&
        add_resolver_op(ctx->resolver.AddSub(), "SUB") &&
        add_resolver_op(ctx->resolver.AddConcatenation(), "CONCATENATION");
}

static void reset_context(void)
{
    if (g_tflm.interpreter != NULL) {
        delete g_tflm.interpreter;
        g_tflm.interpreter = NULL;
    }
    if (g_tflm.tensor_arena != NULL) {
        heap_caps_free(g_tflm.tensor_arena);
        g_tflm.tensor_arena = NULL;
    }

    g_tflm.initialized = false;
    g_tflm.model_asset = NULL;
    g_tflm.tensor_arena_size = 0U;
    g_tflm.model = NULL;
    g_tflm.resolver = tflite::MicroMutableOpResolver<24>();
}

static uint8_t rgb565_luma(const uint8_t *pixel)
{
    uint16_t value = static_cast<uint16_t>(pixel[0]) |
        static_cast<uint16_t>(static_cast<uint16_t>(pixel[1]) << 8);
    uint8_t red = static_cast<uint8_t>((value >> 11) & 0x1FU);
    uint8_t green = static_cast<uint8_t>((value >> 5) & 0x3FU);
    uint8_t blue = static_cast<uint8_t>(value & 0x1FU);

    uint16_t red8 = static_cast<uint16_t>(red * 255U) / 31U;
    uint16_t green8 = static_cast<uint16_t>(green * 255U) / 63U;
    uint16_t blue8 = static_cast<uint16_t>(blue * 255U) / 31U;

    return static_cast<uint8_t>((red8 * 77U + green8 * 150U + blue8 * 29U) >> 8);
}

static bool setup_context(const ai_inference_model_asset_t *model_asset)
{
    if (model_asset == NULL ||
        model_asset->data == NULL ||
        model_asset->len == 0U ||
        model_asset->tensor_arena_size == 0U) {
        return false;
    }

    if (g_tflm.initialized &&
        g_tflm.model_asset == model_asset &&
        g_tflm.interpreter != NULL &&
        g_tflm.tensor_arena != NULL) {
        return true;
    }

    reset_context();

    if (!configure_resolver(&g_tflm)) {
        reset_context();
        return false;
    }

    g_tflm.model_asset = model_asset;
    g_tflm.tensor_arena_size = model_asset->tensor_arena_size;
    g_tflm.model = tflite::GetModel(model_asset->data);
    if (g_tflm.model == NULL) {
        ESP_LOGE(TAG, "GetModel failed");
        return false;
    }
    if (g_tflm.model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG,
            "Schema mismatch model=%u runtime=%u",
            (unsigned)g_tflm.model->version(),
            (unsigned)TFLITE_SCHEMA_VERSION);
        return false;
    }

    g_tflm.tensor_arena = static_cast<uint8_t *>(heap_caps_malloc(
        g_tflm.tensor_arena_size,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (g_tflm.tensor_arena == NULL) {
        g_tflm.tensor_arena = static_cast<uint8_t *>(heap_caps_malloc(
            g_tflm.tensor_arena_size,
            MALLOC_CAP_8BIT));
    }
    if (g_tflm.tensor_arena == NULL) {
        ESP_LOGE(TAG, "Tensor arena allocation failed size=%lu",
            static_cast<unsigned long>(g_tflm.tensor_arena_size));
        return false;
    }

    g_tflm.interpreter = new tflite::MicroInterpreter(
        g_tflm.model,
        g_tflm.resolver,
        g_tflm.tensor_arena,
        g_tflm.tensor_arena_size);
    if (g_tflm.interpreter == NULL) {
        ESP_LOGE(TAG, "Interpreter allocation failed");
        heap_caps_free(g_tflm.tensor_arena);
        g_tflm.tensor_arena = NULL;
        return false;
    }

    if (g_tflm.interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors failed");
        delete g_tflm.interpreter;
        g_tflm.interpreter = NULL;
        heap_caps_free(g_tflm.tensor_arena);
        g_tflm.tensor_arena = NULL;
        return false;
    }

    g_tflm.initialized = true;
    return true;
}

static int input_channels(const TfLiteTensor *tensor)
{
    if (tensor == NULL || tensor->dims == NULL) {
        return 0;
    }
    if (tensor->dims->size >= 4) {
        return tensor->dims->data[tensor->dims->size - 1];
    }
    return 1;
}

static uint16_t input_width(const TfLiteTensor *tensor, const ai_inference_model_asset_t *model_asset)
{
    if (tensor != NULL && tensor->dims != NULL && tensor->dims->size >= 3) {
        return static_cast<uint16_t>(tensor->dims->data[tensor->dims->size - 2]);
    }
    return model_asset->input_width;
}

static uint16_t input_height(const TfLiteTensor *tensor, const ai_inference_model_asset_t *model_asset)
{
    if (tensor != NULL && tensor->dims != NULL && tensor->dims->size >= 3) {
        return static_cast<uint16_t>(tensor->dims->data[tensor->dims->size - 3]);
    }
    return model_asset->input_height;
}

static bool fill_input_tensor(TfLiteTensor *input,
    const uint8_t *rgb565,
    uint16_t width,
    uint16_t height,
    const ai_inference_model_asset_t *model_asset)
{
    uint16_t target_width = input_width(input, model_asset);
    uint16_t target_height = input_height(input, model_asset);
    int channels = input_channels(input);

    if (input == NULL || rgb565 == NULL || width == 0U || height == 0U ||
        target_width == 0U || target_height == 0U ||
        (channels != 1 && channels != 3)) {
        return false;
    }

    for (uint16_t y = 0; y < target_height; ++y) {
        uint16_t source_y = static_cast<uint16_t>((static_cast<uint32_t>(y) * height) / target_height);
        for (uint16_t x = 0; x < target_width; ++x) {
            uint16_t source_x = static_cast<uint16_t>((static_cast<uint32_t>(x) * width) / target_width);
            const uint8_t *pixel = rgb565 + (((static_cast<uint32_t>(source_y) * width) + source_x) * 2U);
            uint8_t luma = rgb565_luma(pixel);
            size_t index = (static_cast<size_t>(y) * target_width + x) * static_cast<size_t>(channels);

            if (input->type == kTfLiteUInt8) {
                for (int c = 0; c < channels; ++c) {
                    input->data.uint8[index + static_cast<size_t>(c)] = luma;
                }
            } else if (input->type == kTfLiteInt8) {
                int32_t zero_point = input->params.zero_point;
                int32_t centered = static_cast<int32_t>(luma) - 128 + zero_point;
                if (centered < -128) {
                    centered = -128;
                }
                if (centered > 127) {
                    centered = 127;
                }
                for (int c = 0; c < channels; ++c) {
                    input->data.int8[index + static_cast<size_t>(c)] = static_cast<int8_t>(centered);
                }
            } else if (input->type == kTfLiteFloat32) {
                float normalized = static_cast<float>(luma) / 255.0f;
                for (int c = 0; c < channels; ++c) {
                    input->data.f[index + static_cast<size_t>(c)] = normalized;
                }
            } else {
                ESP_LOGW(TAG, "Unsupported input tensor type=%d", input->type);
                return false;
            }
        }
    }

    return true;
}

static float tensor_value_as_score(const TfLiteTensor *tensor, int index)
{
    if (tensor->type == kTfLiteFloat32) {
        return tensor->data.f[index];
    }
    if (tensor->type == kTfLiteUInt8) {
        return static_cast<float>(tensor->data.uint8[index]) / 255.0f;
    }
    if (tensor->type == kTfLiteInt8) {
        float scale = tensor->params.scale == 0.0f ? (1.0f / 128.0f) : tensor->params.scale;
        int32_t centered = static_cast<int32_t>(tensor->data.int8[index]) - tensor->params.zero_point;
        return static_cast<float>(centered) * scale;
    }
    return 0.0f;
}

static uint8_t score_to_confidence(float score)
{
    if (score < 0.0f) {
        score = 0.0f;
    }
    if (score > 1.0f) {
        score = 1.0f;
    }
    return static_cast<uint8_t>(score * 255.0f);
}

static int tensor_dim_from_back(const TfLiteTensor *tensor, int offset_from_back)
{
    if (tensor == NULL || tensor->dims == NULL) {
        return 0;
    }

    int index = tensor->dims->size - 1 - offset_from_back;
    if (index < 0 || index >= tensor->dims->size) {
        return 0;
    }

    return tensor->dims->data[index];
}

static uint8_t normalize_axis_start(int position, int cells)
{
    if (cells <= 0) {
        return 0U;
    }

    if (position < 0) {
        position = 0;
    }
    if (position >= cells) {
        position = cells - 1;
    }

    return static_cast<uint8_t>((static_cast<uint32_t>(position) * 255U) / static_cast<uint32_t>(cells));
}

static uint8_t normalize_axis_extent(int cells)
{
    if (cells <= 0) {
        return 0U;
    }

    uint32_t value = 255U / static_cast<uint32_t>(cells);
    if (value == 0U) {
        value = 1U;
    }
    if (value > 255U) {
        value = 255U;
    }
    return static_cast<uint8_t>(value);
}

static bool insert_fomo_detection(aiot_ai_result_t *result,
    uint8_t class_id,
    uint8_t confidence,
    uint8_t x,
    uint8_t y,
    uint8_t width,
    uint8_t height)
{
    uint8_t count;
    uint8_t insert_at;

    if (result == NULL || confidence == 0U) {
        return false;
    }

    count = result->detection_count;
    if (count > AIOT_AI_RESULT_MAX_DETECTIONS) {
        count = AIOT_AI_RESULT_MAX_DETECTIONS;
        result->detection_count = count;
    }

    insert_at = count;
    for (uint8_t i = 0; i < count; ++i) {
        if (confidence > result->detections[i].confidence) {
            insert_at = i;
            break;
        }
    }

    if (insert_at >= AIOT_AI_RESULT_MAX_DETECTIONS) {
        return false;
    }

    if (count < AIOT_AI_RESULT_MAX_DETECTIONS) {
        result->detection_count = (uint8_t)(count + 1U);
        count++;
    }

    for (uint8_t i = count; i > insert_at; --i) {
        if (i < AIOT_AI_RESULT_MAX_DETECTIONS) {
            result->detections[i] = result->detections[i - 1U];
        }
    }

    result->detections[insert_at].class_id = class_id;
    result->detections[insert_at].confidence = confidence;
    result->detections[insert_at].x = x;
    result->detections[insert_at].y = y;
    result->detections[insert_at].width = width;
    result->detections[insert_at].height = height;
    return true;
}

static bool populate_fomo_detections(const TfLiteTensor *output,
    const ai_inference_model_asset_t *model_asset,
    aiot_ai_result_t *result)
{
    int grid_height;
    int grid_width;
    int channel_count;
    uint8_t threshold;

    if (output == NULL || output->dims == NULL || model_asset == NULL || result == NULL) {
        return false;
    }

    grid_height = tensor_dim_from_back(output, 2);
    grid_width = tensor_dim_from_back(output, 1);
    channel_count = tensor_dim_from_back(output, 0);
    if (grid_height <= 0 || grid_width <= 0 || channel_count <= 0) {
        return false;
    }

    threshold = model_asset->detection_threshold == 0U ? 128U : model_asset->detection_threshold;
    result->detection_count = 0U;

    for (int y = 0; y < grid_height; ++y) {
        for (int x = 0; x < grid_width; ++x) {
            int best_channel = -1;
            uint8_t best_confidence = 0U;

            for (int channel = 0; channel < channel_count; ++channel) {
                size_t flat_index = ((static_cast<size_t>(y) * static_cast<size_t>(grid_width) +
                    static_cast<size_t>(x)) * static_cast<size_t>(channel_count)) +
                    static_cast<size_t>(channel);
                uint8_t confidence = score_to_confidence(tensor_value_as_score(output, static_cast<int>(flat_index)));
                uint8_t class_id;

                if (confidence < threshold) {
                    continue;
                }
                if (model_asset->background_index != 0xFFU &&
                    channel == static_cast<int>(model_asset->background_index)) {
                    continue;
                }

                class_id = static_cast<uint8_t>(channel + 1);
                if (model_asset->background_index != 0xFFU &&
                    channel > static_cast<int>(model_asset->background_index) &&
                    class_id > 0U) {
                    class_id--;
                }

                if (model_asset->class_count > 0U && class_id > model_asset->class_count) {
                    continue;
                }

                if (confidence > best_confidence) {
                    best_confidence = confidence;
                    best_channel = class_id;
                }
            }

            if (best_channel < 0) {
                continue;
            }

            insert_fomo_detection(result,
                static_cast<uint8_t>(best_channel),
                best_confidence,
                normalize_axis_start(x, grid_width),
                normalize_axis_start(y, grid_height),
                normalize_axis_extent(grid_width),
                normalize_axis_extent(grid_height));
        }
    }

    return true;
}

static bool populate_generic_detection(const TfLiteTensor *output,
    const ai_inference_model_asset_t *model_asset,
    aiot_ai_result_t *result)
{
    int element_count = 1;
    float best_score = -1000.0f;

    if (output == NULL || output->dims == NULL || result == NULL || model_asset == NULL) {
        return false;
    }

    for (int i = 0; i < output->dims->size; ++i) {
        element_count *= output->dims->data[i];
    }
    if (element_count <= 0) {
        return false;
    }

    for (int i = 0; i < element_count; ++i) {
        float score = tensor_value_as_score(output, i);
        if (score > best_score) {
            best_score = score;
        }
    }

    if (best_score < 0.0f) {
        best_score = 0.0f;
    }
    if (best_score > 1.0f) {
        best_score = 1.0f;
    }

    uint8_t threshold = model_asset->detection_threshold == 0U ? 128U : model_asset->detection_threshold;
    uint8_t confidence = score_to_confidence(best_score);
    if (confidence < threshold) {
        result->detection_count = 0U;
        return true;
    }

    result->detection_count = 1U;
    result->detections[0].class_id = AIOT_AI_CLASS_GENERIC_OBJECT;
    result->detections[0].confidence = confidence;
    result->detections[0].x = 32U;
    result->detections[0].y = 32U;
    result->detections[0].width = 192U;
    result->detections[0].height = 192U;
    return true;
}

}
#endif

extern "C" bool ai_inference_tflm_runtime_available(void)
{
#if AIOT_TFLM_AVAILABLE
    return true;
#else
    return false;
#endif
}

extern "C" bool ai_inference_tflm_run(const ai_inference_model_asset_t *model,
    const uint8_t *rgb565,
    uint16_t width,
    uint16_t height,
    aiot_ai_result_t *result)
{
#if AIOT_TFLM_AVAILABLE
    if (!setup_context(model) || result == NULL) {
        return false;
    }

    TfLiteTensor *input = g_tflm.interpreter->input(0);
    if (input == NULL) {
        ESP_LOGE(TAG, "Interpreter input tensor missing");
        return false;
    }

    result->input_width = input_width(input, model);
    result->input_height = input_height(input, model);
    if (!fill_input_tensor(input, rgb565, width, height, model)) {
        return false;
    }

    if (g_tflm.interpreter->Invoke() != kTfLiteOk) {
        ESP_LOGE(TAG, "Invoke failed");
        return false;
    }

    const TfLiteTensor *output = g_tflm.interpreter->output(0);
    bool parsed = false;

    if (model->output_type == AI_INFERENCE_OUTPUT_FOMO) {
        parsed = populate_fomo_detections(output, model, result);
    } else {
        parsed = populate_generic_detection(output, model, result);
    }

    if (!parsed) {
        ESP_LOGW(TAG, "Output postprocess unavailable for this model");
        result->detection_count = 0U;
    }
    return true;
#else
    (void)model;
    (void)rgb565;
    (void)width;
    (void)height;
    (void)result;
    return false;
#endif
}
