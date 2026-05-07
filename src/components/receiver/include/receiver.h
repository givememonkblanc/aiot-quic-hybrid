#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    QUIC_EVENT_CONNECTED,
    QUIC_EVENT_DATA,
    QUIC_EVENT_CLOSED,
} quic_port_event_t;

typedef struct {
    uint32_t datagrams_received;
    uint32_t datagrams_acked;
    uint32_t datagrams_lost;
    uint32_t datagrams_spurious;
    uint32_t stream_payloads_received;
    uint32_t ai_results_received;
    uint32_t ai_results_invalid;
    uint32_t jpeg_frames_received;
    uint32_t jpeg_frames_acked;
    uint32_t invalid_jpeg_frames;
    uint16_t last_payload_len;
    uint32_t last_ai_source_sequence;
    uint8_t last_first_byte;
    uint8_t last_last_byte;
    uint8_t last_ai_detection_count;
    uint8_t last_ai_backend;
} receiver_stats_t;

typedef struct {
    receiver_stats_t stats;
    uint8_t stream_buf[1024];
    uint16_t stream_buf_len;
    uint8_t reassembly_buf[8192];
    uint16_t reassembly_len;
    uint16_t reassembly_total;
    bool reassembly_active;
} receiver_t;

void receiver_init(receiver_t *receiver);
void receiver_handle_event(receiver_t *receiver,
    quic_port_event_t event,
    const uint8_t *payload,
    uint16_t payload_len);
