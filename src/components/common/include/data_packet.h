#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    DATA_TYPE_AI_RESULT = 0,
    DATA_TYPE_SENSOR = 1,
    DATA_TYPE_IMAGE = 2,
} data_type_t;

/*
 * Semantic importance of a payload, derived from the AI result (cross-layer
 * hint from the perception layer to the transport scheduler). ROUTINE traffic
 * is shed/degraded first under pressure; CRITICAL traffic (a state change or a
 * high-confidence detection) is kept reliable and never AoI-shed.
 */
typedef enum {
    AIOT_IMPORTANCE_ROUTINE = 0,   /* steady state, no salient detection */
    AIOT_IMPORTANCE_ELEVATED = 1,  /* detection present, moderate confidence */
    AIOT_IMPORTANCE_CRITICAL = 2,  /* enter/exit transition or high-confidence hit */
} aiot_importance_t;

typedef struct {
    data_type_t type;
    uint32_t sequence;
    uint64_t timestamp_us;
    uint16_t payload_len;
    uint16_t total_len;
    uint8_t priority;
    uint8_t deadline_class;
    bool loss_tolerant;
    uint8_t importance;            /* aiot_importance_t; perception -> transport hint */
    const uint8_t *payload;
} data_packet_t;

typedef struct {
    uint32_t rtt_ms;
    uint32_t loss_percent;
    uint32_t tx_queue_depth;
    uint32_t datagram_budget;
    uint32_t handshake_time_ms;
    bool stream_window_available;
    bool quic_ready;
    uint32_t heap_free;
    uint32_t heap_caps;
    uint32_t cpu_usage;
    uint32_t main_stack_hwm;
    uint32_t net_tx_stack_hwm;
    uint32_t tx_queue_max;
    uint32_t route_queue_full_count;
    uint32_t stream_send_ok;
    uint32_t stream_send_fail;
    uint32_t datagrams_sent;
    uint32_t datagrams_lost;
    uint32_t datagram_expired;
    uint32_t datagram_evicted;
    uint32_t packets_reordered;
    uint32_t inference_us;
    uint32_t inference_to_enqueue_us;
} net_metrics_t;
