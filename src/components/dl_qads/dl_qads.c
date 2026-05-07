#include "include/dl_qads.h"

#include <string.h>

#include "../common/include/image_chunk.h"

static schedule_decision_t make_stream_decision(uint8_t stream_id, uint8_t pacing_level)
{
    schedule_decision_t decision = {
        .mode = TX_MODE_STREAM,
        .stream_id = stream_id,
        .pacing_level = pacing_level,
        .retransmit_allowed = 1,
        .drop_allowed = 0,
    };

    return decision;
}

static schedule_decision_t make_defer_decision(uint8_t stream_id, uint8_t pacing_level,
                                               uint8_t retransmit_allowed)
{
    schedule_decision_t decision = {
        .mode = TX_MODE_DEFER,
        .stream_id = stream_id,
        .pacing_level = pacing_level,
        .retransmit_allowed = retransmit_allowed,
        .drop_allowed = 0,
    };

    return decision;
}

static schedule_decision_t make_drop_decision(uint8_t pacing_level)
{
    schedule_decision_t decision = {
        .mode = TX_MODE_DROP,
        .stream_id = 0,
        .pacing_level = pacing_level,
        .retransmit_allowed = 0,
        .drop_allowed = 1,
    };

    return decision;
}

static schedule_decision_t make_datagram_decision(uint8_t pacing_level)
{
    schedule_decision_t decision = {
        .mode = TX_MODE_DATAGRAM,
        .stream_id = 0,
        .pacing_level = pacing_level,
        .retransmit_allowed = 0,
        .drop_allowed = 1,
    };

    return decision;
}

void dl_qads_init(dl_qads_t *scheduler, const dl_qads_config_t *config)
{
    memset(scheduler, 0, sizeof(*scheduler));
    scheduler->config = *config;
}

schedule_decision_t dl_qads_decide(const dl_qads_t *scheduler, const data_packet_t *packet, const net_metrics_t *metrics)
{
    uint32_t datagram_budget = metrics->datagram_budget;

    if (packet->type == DATA_TYPE_AI_RESULT) {
        return make_stream_decision(scheduler->config.ai_stream_id, 0);
    }

    if (packet->type == DATA_TYPE_SENSOR) {
        if (!metrics->stream_window_available && metrics->tx_queue_depth > 3U) {
            return make_defer_decision(scheduler->config.sensor_stream_id, 2, 1);
        }

        return make_stream_decision(scheduler->config.sensor_stream_id, 1);
    }

    if (packet->type == DATA_TYPE_IMAGE) {
        bool datagram_available;
        bool moderate_congestion;
        bool severe_congestion;

        if (!metrics->quic_ready) {
            return make_defer_decision(0, 2, 0);
        }

        if (scheduler->config.policy_mode == QADS_POLICY_RELIABILITY_BIASED) {
            return make_stream_decision(scheduler->config.image_stream_id, 2);
        }

        datagram_budget = scheduler->config.image_datagram_budget;
        if (scheduler->config.policy_mode == QADS_POLICY_THROUGHPUT_BIASED &&
            metrics->quic_ready &&
            metrics->tx_queue_depth <= 8U) {
            datagram_budget += scheduler->config.image_datagram_budget / 4U;
        }

        datagram_available = datagram_budget > AIOT_IMAGE_CHUNK_HEADER_SIZE;

        if (scheduler->config.policy_mode == QADS_POLICY_STREAM_FIRST_HYBRID) {
            moderate_congestion = !metrics->stream_window_available ||
                                  metrics->tx_queue_depth > 3U ||
                                  metrics->rtt_ms > 80U ||
                                  metrics->loss_percent > 5U;
            severe_congestion = !metrics->stream_window_available ||
                                metrics->tx_queue_depth > 8U ||
                                metrics->rtt_ms > 140U ||
                                metrics->loss_percent > 12U;

            if (!moderate_congestion) {
                return make_stream_decision(scheduler->config.image_stream_id, 1);
            }

            if (datagram_available) {
                return make_datagram_decision(severe_congestion ? 3U : 2U);
            }

            if (metrics->stream_window_available) {
                return make_stream_decision(scheduler->config.image_stream_id,
                                            severe_congestion ? 3U : 2U);
            }

            return make_defer_decision(scheduler->config.image_stream_id,
                                       severe_congestion ? 3U : 2U, 1);
        }
    }

    if (metrics->loss_percent > 20U) {
        return make_drop_decision(3);
    }

    if (packet->type != DATA_TYPE_IMAGE && datagram_budget < packet->payload_len) {
        return make_drop_decision(3);
    }

    if (packet->type == DATA_TYPE_IMAGE && datagram_budget <= AIOT_IMAGE_CHUNK_HEADER_SIZE) {
        return make_drop_decision(3);
    }

    return make_datagram_decision((metrics->rtt_ms > 80U) ? 3U : 2U);
}
