#pragma once

#include <stdint.h>

#include "../../common/include/data_packet.h"

typedef enum {
    TX_MODE_STREAM = 0,
    TX_MODE_DATAGRAM = 1,
    TX_MODE_DEFER = 2,
    TX_MODE_DROP = 3,
} tx_mode_t;

typedef enum {
    QADS_POLICY_HYBRID_BASELINE = 0,
    QADS_POLICY_RELIABILITY_BIASED = 1,
    QADS_POLICY_THROUGHPUT_BIASED = 2,
    QADS_POLICY_STREAM_FIRST_HYBRID = 3,
} qads_policy_mode_t;

typedef struct {
    tx_mode_t mode;
    uint8_t stream_id;
    uint8_t pacing_level;
    uint8_t retransmit_allowed;
    uint8_t drop_allowed;
} schedule_decision_t;

typedef struct {
    uint8_t ai_stream_id;
    uint8_t sensor_stream_id;
    uint8_t image_stream_id;
    uint16_t image_datagram_budget;
    uint16_t control_period_ms;
    qads_policy_mode_t policy_mode;
} dl_qads_config_t;

typedef struct {
    dl_qads_config_t config;
} dl_qads_t;

void dl_qads_init(dl_qads_t *scheduler, const dl_qads_config_t *config);
schedule_decision_t dl_qads_decide(const dl_qads_t *scheduler, const data_packet_t *packet, const net_metrics_t *metrics);
