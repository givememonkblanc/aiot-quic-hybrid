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

static schedule_decision_t make_datagram_decision(uint8_t pacing_level,
                                                  uint16_t delivery_timeout_ms)
{
    schedule_decision_t decision = {
        .mode = TX_MODE_DATAGRAM,
        .stream_id = 0,
        .pacing_level = pacing_level,
        .retransmit_allowed = 0,
        .drop_allowed = 1,
        .delivery_timeout_ms = delivery_timeout_ms,
    };

    return decision;
}

static uint16_t packet_deadline_ms(const dl_qads_t *scheduler, const data_packet_t *packet)
{
    return (packet->type == DATA_TYPE_IMAGE) ? scheduler->config.image_deadline_ms
                                             : scheduler->config.telemetry_deadline_ms;
}

static bool network_congested(const net_metrics_t *metrics)
{
    return !metrics->stream_window_available ||
           metrics->tx_queue_depth > 3U ||
           metrics->rtt_ms > 80U ||
           metrics->loss_percent > 5U;
}

/*
 * AoI-aware shedding: a loss-tolerant packet whose remaining lifetime is
 * shorter than the current RTT cannot arrive fresh under congestion, so
 * spending link budget on it only delays newer objects.
 */
static bool packet_past_useful_life(const dl_qads_t *scheduler,
                                    const data_packet_t *packet,
                                    const net_metrics_t *metrics,
                                    uint64_t now_us)
{
    uint16_t deadline_ms = packet_deadline_ms(scheduler, packet);

    if (!packet->loss_tolerant || deadline_ms == 0U || now_us <= packet->timestamp_us) {
        return false;
    }

    uint32_t age_ms = (uint32_t)((now_us - packet->timestamp_us) / 1000ULL);
    uint32_t remaining_ms = (age_ms < deadline_ms) ? (deadline_ms - age_ms) : 0U;

    return remaining_ms <= metrics->rtt_ms && network_congested(metrics);
}

void dl_qads_init(dl_qads_t *scheduler, const dl_qads_config_t *config)
{
    memset(scheduler, 0, sizeof(*scheduler));
    scheduler->config = *config;
}

static schedule_decision_t dl_qads_decide_core(const dl_qads_t *scheduler, const data_packet_t *packet,
                                               const net_metrics_t *metrics, uint64_t now_us)
{
    uint32_t datagram_budget = metrics->datagram_budget;

    // Never AoI-shed a CRITICAL payload (state change / high-confidence hit);
    // routine and elevated traffic still yields link budget under congestion.
    if (packet->importance < (uint8_t)AIOT_IMPORTANCE_CRITICAL &&
        packet_past_useful_life(scheduler, packet, metrics, now_us)) {
        return make_drop_decision(3);
    }

    if (packet->type == DATA_TYPE_AI_RESULT) {
        // Adaptive forwarding preference: routine telemetry rides the reliable
        // stream while healthy, but degrades to datagrams under congestion so it
        // never queues behind retransmissions. A salient detection (ELEVATED+)
        // keeps the reliable stream even under congestion — the perception layer
        // tells the transport this result is worth protecting.
        bool salient = packet->importance >= (uint8_t)AIOT_IMPORTANCE_ELEVATED;
        if (scheduler->config.policy_mode != QADS_POLICY_RELIABILITY_BIASED &&
            !salient &&
            metrics->quic_ready &&
            network_congested(metrics)) {
            return make_datagram_decision(2, scheduler->config.telemetry_deadline_ms);
        }
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
                return make_datagram_decision(severe_congestion ? 3U : 2U,
                                              scheduler->config.image_deadline_ms);
            }

            if (metrics->stream_window_available) {
                return make_stream_decision(scheduler->config.image_stream_id,
                                            severe_congestion ? 3U : 2U);
            }

            return make_defer_decision(scheduler->config.image_stream_id,
                                       severe_congestion ? 3U : 2U, 1);
        }
    }

    if (metrics->loss_percent > 20U && packet->importance < (uint8_t)AIOT_IMPORTANCE_CRITICAL) {
        return make_drop_decision(3);
    }

    if (packet->type != DATA_TYPE_IMAGE && datagram_budget < packet->payload_len) {
        return make_drop_decision(3);
    }

    if (packet->type == DATA_TYPE_IMAGE && datagram_budget <= AIOT_IMAGE_CHUNK_HEADER_SIZE) {
        return make_drop_decision(3);
    }

    return make_datagram_decision((metrics->rtt_ms > 80U) ? 3U : 2U,
                                  packet_deadline_ms(scheduler, packet));
}

schedule_decision_t dl_qads_decide(const dl_qads_t *scheduler, const data_packet_t *packet,
                                   const net_metrics_t *metrics, uint64_t now_us)
{
    schedule_decision_t decision = dl_qads_decide_core(scheduler, packet, metrics, now_us);
    decision.importance = packet->importance;  // echo for logging/observability
    return decision;
}
