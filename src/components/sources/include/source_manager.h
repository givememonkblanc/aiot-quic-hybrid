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
} source_manager_t;

void source_manager_init(source_manager_t *manager);
bool source_manager_next_packet(source_manager_t *manager, data_packet_t *packet);
