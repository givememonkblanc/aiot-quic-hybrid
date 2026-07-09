#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "data_packet.h"   /* aiot_importance_t */

/* Confidence at/above which a detection alone is CRITICAL (~70% of 255). */
#define AIOT_IMPORTANCE_HIGH_CONF 180U

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

/*
 * Map an inference result (and the previous one, may be NULL) to a semantic
 * importance for the transport scheduler. A change in detection state
 * (enter/exit) or a high-confidence hit is CRITICAL; any detection is
 * ELEVATED; otherwise ROUTINE.
 */
aiot_importance_t aiot_importance_from_result(const aiot_ai_result_t *cur,
                                              const aiot_ai_result_t *prev);

void aiot_ai_result_init(aiot_ai_result_t *result, aiot_ai_backend_t backend, uint32_t source_sequence);
uint16_t aiot_ai_result_serialize(const aiot_ai_result_t *result, uint8_t *dst, uint16_t capacity);
bool aiot_ai_result_parse(const uint8_t *payload, uint16_t payload_len, aiot_ai_result_t *result);
