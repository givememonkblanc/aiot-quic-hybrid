#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "../../common/include/ai_result.h"
#include "../../common/include/data_packet.h"

typedef struct {
    uint8_t data[AIOT_AI_RESULT_MAX_PAYLOAD_LEN];
    uint16_t len;
} source_manager_ai_payload_t;

typedef struct {
    uint32_t next_sequence;
    uint8_t slot;
    source_manager_ai_payload_t ai_payload;
    /* Result of the most recent on-capture inference. Consumers (app_main)
     * read this instead of re-running inference on the reassembled frame,
     * so the model runs exactly once per frame. */
    aiot_ai_result_t last_result;
    bool last_ran;              /* inference actually executed this frame */
    uint32_t last_inference_us; /* wall-clock of the last inference run */
    uint8_t last_importance;    /* aiot_importance_t of the last frame */
} source_manager_t;

void source_manager_init(source_manager_t *manager);
bool source_manager_next_packet(source_manager_t *manager, data_packet_t *packet);

/* Copy the most recent inference result into *out (and its duration into
 * *inference_us when non-NULL). Returns true if inference ran for that frame,
 * mirroring ai_inference_run()'s return. */
bool source_manager_get_last_ai_result(const source_manager_t *manager,
    aiot_ai_result_t *out, uint32_t *inference_us);
