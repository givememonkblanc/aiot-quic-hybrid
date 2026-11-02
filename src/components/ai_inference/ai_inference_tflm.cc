#ifndef AIOT_AI_ROI
#define AIOT_AI_ROI 0
#endif
#include "ai_inference_tflm.h"

#include <new>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <vector>

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

#define AIOT_TENSOR_ARENA_INTERNAL_FALLBACK_LIMIT (256U * 1024U)

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

static void log_heap_state(const char *stage)
{
    ESP_LOGI(TAG,
        "%s: internal_free=%u internal_largest=%u spiram_free=%u spiram_largest=%u",
        stage,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

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

#if AIOT_MODEL_COMPRESSED
    // Generated from this exact model's operator table, not a guessed op list.
#include "ai_model_register_ops.inc"
    return true;
#else
    return add_resolver_op(ctx->resolver.AddAdd(), "ADD") &&
        add_resolver_op(ctx->resolver.AddPad(), "PAD") &&
        add_resolver_op(ctx->resolver.AddMean(), "MEAN") &&
        add_resolver_op(ctx->resolver.AddFullyConnected(), "FULLY_CONNECTED") &&
        add_resolver_op(ctx->resolver.AddMul(), "MUL") &&
        add_resolver_op(ctx->resolver.AddMinimum(), "MINIMUM") &&
        add_resolver_op(ctx->resolver.AddRelu(), "RELU") &&
        add_resolver_op(ctx->resolver.AddAveragePool2D(), "AVERAGE_POOL_2D") &&
        add_resolver_op(ctx->resolver.AddConv2D(), "CONV_2D") &&
        add_resolver_op(ctx->resolver.AddDepthwiseConv2D(), "DEPTHWISE_CONV_2D") &&
        add_resolver_op(ctx->resolver.AddReshape(), "RESHAPE") &&
        add_resolver_op(ctx->resolver.AddSoftmax(), "SOFTMAX") &&
        add_resolver_op(ctx->resolver.AddSub(), "SUB") &&
        add_resolver_op(ctx->resolver.AddConcatenation(), "CONCATENATION");
#endif
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
    /* Re-construct in place: copy-assigning a temporary would copy its
     * uninitialized registration slots (-Werror=uninitialized at -Os). */
    g_tflm.resolver.~MicroMutableOpResolver();
    new (&g_tflm.resolver) tflite::MicroMutableOpResolver<24>();
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
    log_heap_state("tflm:after reset_context");

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
    log_heap_state("tflm:after psram arena alloc");
    if (g_tflm.tensor_arena == NULL &&
        g_tflm.tensor_arena_size < AIOT_TENSOR_ARENA_INTERNAL_FALLBACK_LIMIT) {
        g_tflm.tensor_arena = static_cast<uint8_t *>(heap_caps_malloc(
            g_tflm.tensor_arena_size,
            MALLOC_CAP_8BIT));
        log_heap_state("tflm:after internal arena fallback");
    } else if (g_tflm.tensor_arena == NULL) {
        ESP_LOGW(TAG,
            "Refusing internal fallback for large tensor arena size=%lu; keeping internal DRAM for transport/TLS",
            static_cast<unsigned long>(g_tflm.tensor_arena_size));
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

    log_heap_state("tflm:after AllocateTensors");

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

#if AIOT_AI_LUMA_NORM
/* Night input (10/10): at the gain ceiling the OV2640 rows carry offsets (bands across the frame) and the
 * plate is half as bright as in the day, and the model, trained on day frames, finds nothing. Each row of
 * the region is moved by the difference of its median to the median of the frame, then the region is
 * scaled so that its median is the day level (153), never darker: in the day the input barely changes.
 * Integer arithmetic; replay_workers.night_luma is the same on the host. */
#define LUMA_NORM_DAY 153U
static uint8_t *g_luma;
static size_t g_luma_size;

static uint8_t hist_median(const uint32_t *hist, uint32_t n)
{
    uint32_t seen = 0U;
    for (unsigned v = 0; v < 256U; ++v) {
        seen += hist[v];
        if (2U * seen > n) {
            return static_cast<uint8_t>(v);
        }
    }
    return 255U;
}

/* the luma of the region rows [ry, ry+rh), normalized, into g_luma (width per row); false without memory */
static bool luma_norm(const uint8_t *rgb565, uint16_t width, uint16_t height, uint32_t rx, uint32_t ry,
    uint32_t rw, uint32_t rh)
{
    size_t need = static_cast<size_t>(width) * height;
    if (g_luma_size < need) {
        heap_caps_free(g_luma);
        g_luma = static_cast<uint8_t *>(heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        g_luma_size = g_luma ? need : 0U;
        if (!g_luma) {
            return false;
        }
    }
    static uint32_t hist[256], rhist[256];
    memset(hist, 0, sizeof(hist));
    for (size_t i = 0; i < need; ++i) {
        g_luma[i] = rgb565_luma(rgb565 + i * 2U);
        hist[g_luma[i]]++;
    }
    int32_t frame_median = hist_median(hist, need);
    memset(hist, 0, sizeof(hist));
    for (uint32_t y = ry; y < ry + rh; ++y) {
        uint8_t *row = g_luma + static_cast<size_t>(y) * width;
        memset(rhist, 0, sizeof(rhist));
        for (uint16_t x = 0; x < width; ++x) {
            rhist[row[x]]++;
        }
        int32_t shift = frame_median - hist_median(rhist, width);
        for (uint16_t x = 0; x < width; ++x) {
            int32_t v = row[x] + shift;
            row[x] = static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
        }
        for (uint32_t x = rx; x < rx + rw; ++x) {
            hist[row[x]]++;
        }
    }
    uint32_t median = hist_median(hist, rw * rh);
    uint32_t gain = median > 0U && median < LUMA_NORM_DAY ? (LUMA_NORM_DAY * 256U) / median : 256U;
    for (uint32_t y = ry; y < ry + rh; ++y) {
        uint8_t *row = g_luma + static_cast<size_t>(y) * width;
        for (uint32_t x = rx; x < rx + rw; ++x) {
            uint32_t v = (row[x] * gain) >> 8;
            row[x] = static_cast<uint8_t>(v > 255U ? 255U : v);
        }
    }
    return true;
}
#endif

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

    /* the part of the frame that the model sees */
    uint32_t rx = 0U, ry = 0U, rw = width, rh = height;
#if AIOT_AI_ROI
    rx = width * AIOT_AI_ROI_X0 / 1000U;
    ry = height * AIOT_AI_ROI_Y0 / 1000U;
    rw = width * AIOT_AI_ROI_X1 / 1000U - rx;
    rh = height * AIOT_AI_ROI_Y1 / 1000U - ry;
#endif
#if AIOT_AI_LUMA_NORM
    bool norm = luma_norm(rgb565, width, height, rx, ry, rw, rh);
#endif
    for (uint16_t y = 0; y < target_height; ++y) {
        uint16_t source_y = static_cast<uint16_t>(ry + (static_cast<uint32_t>(y) * rh) / target_height);
        for (uint16_t x = 0; x < target_width; ++x) {
            uint16_t source_x = static_cast<uint16_t>(rx + (static_cast<uint32_t>(x) * rw) / target_width);
            const uint8_t *pixel = rgb565 + (((static_cast<uint32_t>(source_y) * width) + source_x) * 2U);
            uint8_t luma = rgb565_luma(pixel);
#if AIOT_AI_LUMA_NORM
            if (norm) {
                luma = g_luma[static_cast<size_t>(source_y) * width + source_x];
            }
#endif
            size_t index = (static_cast<size_t>(y) * target_width + x) * static_cast<size_t>(channels);

            if (input->type == kTfLiteUInt8) {
                for (int c = 0; c < channels; ++c) {
                    input->data.uint8[index + static_cast<size_t>(c)] = luma;
                }
            } else if (input->type == kTfLiteInt8) {
                /* Affine-quantize the [0,1]-normalized luma to the tensor's own
                 * scale/zero_point: q = round((luma/255)/scale) + zero_point.
                 * For the person model (scale=1/255, zp=-128) this reduces to
                 * luma-128, matching the reference example's `pixel ^ 0x80`. */
                int32_t zero_point = input->params.zero_point;
                float scale = input->params.scale;
                int32_t quantized;
                if (scale > 0.0f) {
                    float real = static_cast<float>(luma) / 255.0f;
                    quantized = static_cast<int32_t>((real / scale) + 0.5f) + zero_point;
                } else {
                    quantized = static_cast<int32_t>(luma) - 128;
                }
                if (quantized < -128) {
                    quantized = -128;
                }
                if (quantized > 127) {
                    quantized = 127;
                }
                for (int c = 0; c < channels; ++c) {
                    input->data.int8[index + static_cast<size_t>(c)] = static_cast<int8_t>(quantized);
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

static uint8_t normalize_axis_extent(int span, int cells)
{
    if (span <= 0 || cells <= 0) {
        return 0U;
    }

    uint32_t value = (static_cast<uint32_t>(span) * 255U) / static_cast<uint32_t>(cells);
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

#if AIOT_AI_ROI
/* Objects at the local maxima of the object probability: two touching objects whose cells
 * adjoin stay two objects. A cell is a maximum when no neighbour is higher, and no neighbour
 * before it in reading order is as high, so a plateau gives one object. The position is the
 * centre of the cell, in the units of the whole frame; the extent is one cell.
 * A model with a count head (more than two channels: background, 1, 2, ... objects,
 * experiments/screw_mission/train_turntable_fomo2.py) gives one maximum for objects that touch;
 * its object probability is 1 - background, and a maximum is emitted as many times as its most
 * likely count, at the same place. */
static bool populate_fomo_peaks(const TfLiteTensor *output,
    const ai_inference_model_asset_t *model_asset,
    aiot_ai_result_t *result)
{
    if (output == NULL || output->dims == NULL || model_asset == NULL || result == NULL) {
        return false;
    }
    const int gh = tensor_dim_from_back(output, 2), gw = tensor_dim_from_back(output, 1);
    const int channels = tensor_dim_from_back(output, 0);
    if (gh <= 0 || gw <= 0 || channels < 2 || channels > 8 || gw * gh > 1024) {
        return false;
    }
    const uint8_t threshold = model_asset->detection_threshold == 0U ? 128U : model_asset->detection_threshold;
    const bool counted = channels > 2;
    const int object = model_asset->background_index == 0U ? 1 : 0;
    static uint8_t conf[1024];
    static uint8_t many[1024];
    result->detection_count = 0U;
    for (int i = 0; i < gw * gh; ++i) {
        many[i] = 1U;
        if (counted) {
            conf[i] = score_to_confidence(1.0f - tensor_value_as_score(output, i * channels));
            float best = tensor_value_as_score(output, i * channels + 1);
            for (int k = 2; k < channels; ++k) {
                const float v = tensor_value_as_score(output, i * channels + k);
                if (v > best) { best = v; many[i] = static_cast<uint8_t>(k); }
            }
        } else {
            conf[i] = score_to_confidence(tensor_value_as_score(output, i * channels + object));
        }
        if (conf[i] > result->raw_score) result->raw_score = conf[i];
    }
    const uint32_t x0 = 256U * AIOT_AI_ROI_X0 / 1000U, y0 = 256U * AIOT_AI_ROI_Y0 / 1000U;
    const uint32_t xw = 256U * AIOT_AI_ROI_X1 / 1000U - x0, yh = 256U * AIOT_AI_ROI_Y1 / 1000U - y0;
    for (int y = 0; y < gh; ++y) {
        for (int x = 0; x < gw; ++x) {
            const int i = y * gw + x;
            if (conf[i] < threshold) continue;
            bool peak = true;
            for (int dy = -1; dy <= 1 && peak; ++dy) {
                for (int dx = -1; dx <= 1 && peak; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= gw || ny >= gh) continue;
                    const int j = ny * gw + nx;
                    if (conf[j] > conf[i] || (j < i && conf[j] == conf[i])) peak = false;
                }
            }
            if (!peak) continue;
            for (uint8_t n = 0; n < many[i]; ++n) {
                insert_fomo_detection(result, 1U, conf[i],
                    static_cast<uint8_t>(x0 + ((2U * x + 1U) * xw) / (2U * gw)),
                    static_cast<uint8_t>(y0 + ((2U * y + 1U) * yh) / (2U * gh)),
                    static_cast<uint8_t>(xw / gw), static_cast<uint8_t>(yh / gh));
            }
        }
    }
    return true;
}
#endif

static bool populate_fomo_detections(const TfLiteTensor *output,
    const ai_inference_model_asset_t *model_asset,
    aiot_ai_result_t *result)
{
#if AIOT_AI_ROI
    return populate_fomo_peaks(output, model_asset, result);
#endif
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

    std::vector<uint8_t> best_class(static_cast<size_t>(grid_width * grid_height), 0U);
    std::vector<uint8_t> best_conf(static_cast<size_t>(grid_width * grid_height), 0U);
    std::vector<uint8_t> visited(static_cast<size_t>(grid_width * grid_height), 0U);

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

                if (model_asset->background_index != 0xFFU &&
                    channel == static_cast<int>(model_asset->background_index)) {
                    continue;
                }
                if (confidence > result->raw_score) {
                    result->raw_score = confidence;
                }
                if (confidence < threshold) {
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

            size_t index = static_cast<size_t>(y * grid_width + x);
            best_class[index] = static_cast<uint8_t>(best_channel);
            best_conf[index] = best_confidence;
        }
    }

    for (int y = 0; y < grid_height; ++y) {
        for (int x = 0; x < grid_width; ++x) {
            size_t start = static_cast<size_t>(y * grid_width + x);
            uint8_t class_id = best_class[start];
            uint8_t confidence = best_conf[start];
            if (class_id == 0U || confidence == 0U || visited[start] != 0U) {
                continue;
            }

            int min_x = x;
            int min_y = y;
            int max_x = x;
            int max_y = y;
            uint8_t blob_conf = confidence;
            std::vector<int> stack;
            stack.push_back(y * grid_width + x);
            visited[start] = 1U;

            while (!stack.empty()) {
                int cell = stack.back();
                stack.pop_back();
                int cy = cell / grid_width;
                int cx = cell % grid_width;
                if (cx < min_x) {
                    min_x = cx;
                }
                if (cy < min_y) {
                    min_y = cy;
                }
                if (cx > max_x) {
                    max_x = cx;
                }
                if (cy > max_y) {
                    max_y = cy;
                }

                uint8_t cell_conf = best_conf[static_cast<size_t>(cell)];
                if (cell_conf > blob_conf) {
                    blob_conf = cell_conf;
                }

                for (int ny = cy - 1; ny <= cy + 1; ++ny) {
                    if (ny < 0 || ny >= grid_height) {
                        continue;
                    }
                    for (int nx = cx - 1; nx <= cx + 1; ++nx) {
                        if (nx < 0 || nx >= grid_width) {
                            continue;
                        }
                        size_t next = static_cast<size_t>(ny * grid_width + nx);
                        if (visited[next] != 0U || best_class[next] != class_id || best_conf[next] == 0U) {
                            continue;
                        }
                        visited[next] = 1U;
                        stack.push_back(ny * grid_width + nx);
                    }
                }
            }

            insert_fomo_detection(result,
                class_id,
                blob_conf,
                normalize_axis_start(min_x, grid_width),
                normalize_axis_start(min_y, grid_height),
                normalize_axis_extent(max_x - min_x + 1, grid_width),
                normalize_axis_extent(max_y - min_y + 1, grid_height));
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
    result->raw_score = confidence;
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

/* Softmax classifier: emit a single detection when the score at
 * target_class_index (e.g. person) clears the threshold. */
static bool populate_classification_detection(const TfLiteTensor *output,
    const ai_inference_model_asset_t *model_asset,
    aiot_ai_result_t *result)
{
    int element_count = 1;
    int idx;
    uint8_t threshold;
    uint8_t confidence;

    if (output == NULL || output->dims == NULL || result == NULL || model_asset == NULL) {
        return false;
    }

    for (int i = 0; i < output->dims->size; ++i) {
        element_count *= output->dims->data[i];
    }
    idx = static_cast<int>(model_asset->target_class_index);
    if (element_count <= 0 || idx < 0 || idx >= element_count) {
        return false;
    }

    threshold = model_asset->detection_threshold == 0U ? 128U : model_asset->detection_threshold;
    confidence = score_to_confidence(tensor_value_as_score(output, idx));
    /* Surface the pre-threshold score: the presence state machine gates on
     * its own configurable thresholds, and the heartbeat reports it. Only
     * the detection list (used for scheduler importance) stays gated here. */
    result->raw_score = confidence;
    ESP_LOGI(TAG, "class score=%u/255 idx=%d (det_thr=%u)",
        (unsigned)confidence, idx, (unsigned)threshold);
    if (confidence < threshold) {
        result->detection_count = 0U;
        return true;
    }

    result->detection_count = 1U;
    result->detections[0].class_id = model_asset->target_class_index;
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
    } else if (model->output_type == AI_INFERENCE_OUTPUT_CLASSIFICATION) {
        parsed = populate_classification_detection(output, model, result);
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
