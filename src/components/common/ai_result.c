#include "ai_result.h"

static void aiot_ai_result_write_u16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void aiot_ai_result_write_u32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);
    dst[2] = (uint8_t)((value >> 16) & 0xFFU);
    dst[3] = (uint8_t)((value >> 24) & 0xFFU);
}

static uint16_t aiot_ai_result_read_u16(const uint8_t *src)
{
    return (uint16_t)src[0] | (uint16_t)((uint16_t)src[1] << 8);
}

static uint32_t aiot_ai_result_read_u32(const uint8_t *src)
{
    return (uint32_t)src[0] |
        ((uint32_t)src[1] << 8) |
        ((uint32_t)src[2] << 16) |
        ((uint32_t)src[3] << 24);
}

void aiot_ai_result_init(aiot_ai_result_t *result, aiot_ai_backend_t backend, uint32_t source_sequence)
{
    if (result == NULL) {
        return;
    }

    result->backend = backend;
    result->detection_count = 0;
    result->source_width = 0U;
    result->source_height = 0U;
    result->input_width = 0U;
    result->input_height = 0U;
    result->source_sequence = source_sequence;

    for (uint8_t i = 0; i < AIOT_AI_RESULT_MAX_DETECTIONS; ++i) {
        result->detections[i].class_id = AIOT_AI_CLASS_NONE;
        result->detections[i].confidence = 0U;
        result->detections[i].x = 0U;
        result->detections[i].y = 0U;
        result->detections[i].width = 0U;
        result->detections[i].height = 0U;
    }
}

uint16_t aiot_ai_result_serialize(const aiot_ai_result_t *result, uint8_t *dst, uint16_t capacity)
{
    if (result == NULL || dst == NULL || capacity < AIOT_AI_RESULT_HEADER_SIZE) {
        return 0U;
    }

    uint8_t detection_count = result->detection_count;
    if (detection_count > AIOT_AI_RESULT_MAX_DETECTIONS) {
        detection_count = AIOT_AI_RESULT_MAX_DETECTIONS;
    }

    uint16_t required = (uint16_t)(AIOT_AI_RESULT_HEADER_SIZE +
        (uint16_t)detection_count * AIOT_AI_RESULT_DETECTION_SIZE);
    if (capacity < required) {
        return 0U;
    }

    dst[0] = AIOT_AI_RESULT_MAGIC;
    dst[1] = AIOT_AI_RESULT_VERSION;
    dst[2] = (uint8_t)result->backend;
    dst[3] = detection_count;
    aiot_ai_result_write_u16(dst + 4, result->source_width);
    aiot_ai_result_write_u16(dst + 6, result->source_height);
    aiot_ai_result_write_u16(dst + 8, result->input_width);
    aiot_ai_result_write_u16(dst + 10, result->input_height);
    aiot_ai_result_write_u32(dst + 12, result->source_sequence);

    for (uint8_t i = 0; i < detection_count; ++i) {
        const aiot_ai_detection_t *detection = &result->detections[i];
        uint16_t offset = (uint16_t)(AIOT_AI_RESULT_HEADER_SIZE +
            (uint16_t)i * AIOT_AI_RESULT_DETECTION_SIZE);
        dst[offset + 0] = detection->class_id;
        dst[offset + 1] = detection->confidence;
        dst[offset + 2] = detection->x;
        dst[offset + 3] = detection->y;
        dst[offset + 4] = detection->width;
        dst[offset + 5] = detection->height;
    }

    return required;
}

bool aiot_ai_result_parse(const uint8_t *payload, uint16_t payload_len, aiot_ai_result_t *result)
{
    if (payload == NULL || result == NULL || payload_len < AIOT_AI_RESULT_HEADER_SIZE) {
        return false;
    }

    if (payload[0] != AIOT_AI_RESULT_MAGIC || payload[1] != AIOT_AI_RESULT_VERSION) {
        return false;
    }

    uint8_t detection_count = payload[3];
    if (detection_count > AIOT_AI_RESULT_MAX_DETECTIONS) {
        return false;
    }

    uint16_t required = (uint16_t)(AIOT_AI_RESULT_HEADER_SIZE +
        (uint16_t)detection_count * AIOT_AI_RESULT_DETECTION_SIZE);
    if (payload_len < required) {
        return false;
    }

    aiot_ai_result_init(result, (aiot_ai_backend_t)payload[2], aiot_ai_result_read_u32(payload + 12));
    result->detection_count = detection_count;
    result->source_width = aiot_ai_result_read_u16(payload + 4);
    result->source_height = aiot_ai_result_read_u16(payload + 6);
    result->input_width = aiot_ai_result_read_u16(payload + 8);
    result->input_height = aiot_ai_result_read_u16(payload + 10);

    for (uint8_t i = 0; i < detection_count; ++i) {
        uint16_t offset = (uint16_t)(AIOT_AI_RESULT_HEADER_SIZE +
            (uint16_t)i * AIOT_AI_RESULT_DETECTION_SIZE);
        result->detections[i].class_id = payload[offset + 0];
        result->detections[i].confidence = payload[offset + 1];
        result->detections[i].x = payload[offset + 2];
        result->detections[i].y = payload[offset + 3];
        result->detections[i].width = payload[offset + 4];
        result->detections[i].height = payload[offset + 5];
    }

    return true;
}
