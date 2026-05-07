#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AIOT_AI_RESULT_MAGIC 0xD1U
#define AIOT_AI_RESULT_VERSION 0x01U
#define AIOT_AI_RESULT_MAX_DETECTIONS 3U
#define AIOT_AI_RESULT_HEADER_SIZE 16U
#define AIOT_AI_RESULT_DETECTION_SIZE 6U
#define AIOT_AI_RESULT_MAX_PAYLOAD_LEN \
    (AIOT_AI_RESULT_HEADER_SIZE + (AIOT_AI_RESULT_MAX_DETECTIONS * AIOT_AI_RESULT_DETECTION_SIZE))

typedef enum {
    AIOT_AI_BACKEND_NONE = 0,
    AIOT_AI_BACKEND_HEURISTIC = 1,
    AIOT_AI_BACKEND_TFLM = 2,
} aiot_ai_backend_t;

typedef enum {
    AIOT_AI_CLASS_NONE = 0,
    AIOT_AI_CLASS_GENERIC_OBJECT = 1,
} aiot_ai_class_t;

typedef struct {
    uint8_t class_id;
    uint8_t confidence;
    uint8_t x;
    uint8_t y;
    uint8_t width;
    uint8_t height;
} aiot_ai_detection_t;

typedef struct {
    aiot_ai_backend_t backend;
    uint8_t detection_count;
    uint16_t source_width;
    uint16_t source_height;
    uint16_t input_width;
    uint16_t input_height;
    uint32_t source_sequence;
    aiot_ai_detection_t detections[AIOT_AI_RESULT_MAX_DETECTIONS];
} aiot_ai_result_t;

void aiot_ai_result_init(aiot_ai_result_t *result, aiot_ai_backend_t backend, uint32_t source_sequence);
uint16_t aiot_ai_result_serialize(const aiot_ai_result_t *result, uint8_t *dst, uint16_t capacity);
bool aiot_ai_result_parse(const uint8_t *payload, uint16_t payload_len, aiot_ai_result_t *result);
