/* MQTT baseline of the mission (AIOT_MQTT_BASELINE): the publisher calls of the mission
 * application are served by an MQTT 3.1.1 client over TCP and TLS (esp-mqtt) instead of the
 * MOQT publisher over QUIC. Capture, inference, the passage counter, the result queue and the
 * representation of the images stay as they are; only the transport and its scheduling differ.
 *
 * Results go out with QoS 1 on one topic and images with QoS 0 on another, both over the one
 * connection, in the order in which the application hands them over. The session is persistent,
 * so that a broker keeps QoS 1 results for a subscriber whose connection was lost. A payload is
 * [alias u8][group u32 BE][properties length u16 BE][properties][object], the properties being
 * the Object Properties of the MOQT path, so that the subscribers read the same fields. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ngtcp2_sample.h"

int mqb_init_with_config(const quic_client_config_t *config);
int mqb_process(void);
bool mqb_is_connected(void);
void mqb_cleanup(void);
int mqb_moq_start(const char *authority);
bool mqb_moq_session_ready(void);
int mqb_moq_announce(void);
bool mqb_moq_going_away(void);
bool mqb_moq_track_ready(unsigned alias);
bool mqb_moq_ai_slot_available(void);
bool mqb_moq_image_slot_available(void);
bool mqb_moq_offload_flight(uint64_t *group, size_t *bytes, size_t *acked, uint64_t *deadline_us);
bool mqb_moq_offload_last(uint64_t *group, size_t *bytes, size_t *acked, bool *delivered);
uint64_t mqb_moq_service_deviation(void);
bool mqb_moq_image_flight(uint64_t *group, size_t *bytes, size_t *sent, size_t *acked, bool *abandoned);
bool mqb_moq_image_result(uint64_t *group, size_t *bytes, uint64_t *held_us, bool *delivered);
bool mqb_moq_image_abort(uint64_t group);
void mqb_moq_image_window(size_t bytes);
void mqb_moq_alert_clock(uint32_t (*safe_us)(void));
bool mqb_moq_last_ai_receipt(uint64_t *group, uint64_t *source_to_ack_us);
uint64_t mqb_moq_min_rtt_us(void);
uint64_t mqb_moq_srtt_us(void);
void mqb_moq_predict_stats(uint64_t *max, uint64_t *mean);
int mqb_moq_publish_ex(const quic_moq_object *object);
int mqb_moq_finish(void);
bool mqb_moq_finished(void);
int mqb_moq_tracks(const moqt_track_cfg *tracks, unsigned count);
int mqb_moq_configure(unsigned policy);
uint64_t mqb_moq_service_rate(void);
bool mqb_moq_service_initialized(void);
bool mqb_moq_new_group_requested(unsigned alias);
void mqb_moq_policy_stats(void);

#define quic_client_init_with_config mqb_init_with_config
#define quic_client_process mqb_process
#define quic_client_is_connected mqb_is_connected
#define quic_client_cleanup mqb_cleanup
#define quic_client_moq_start mqb_moq_start
#define quic_client_moq_session_ready mqb_moq_session_ready
#define quic_client_moq_announce mqb_moq_announce
#define quic_client_moq_going_away mqb_moq_going_away
#define quic_client_moq_track_ready mqb_moq_track_ready
#define quic_client_moq_ai_slot_available mqb_moq_ai_slot_available
#define quic_client_moq_image_slot_available mqb_moq_image_slot_available
#define quic_client_moq_offload_flight mqb_moq_offload_flight
#define quic_client_moq_offload_last mqb_moq_offload_last
#define quic_client_moq_service_deviation mqb_moq_service_deviation
#define quic_client_moq_image_flight mqb_moq_image_flight
#define quic_client_moq_image_result mqb_moq_image_result
#define quic_client_moq_image_abort mqb_moq_image_abort
#define quic_client_moq_image_window mqb_moq_image_window
#define quic_client_moq_alert_clock mqb_moq_alert_clock
#define quic_client_moq_last_ai_receipt mqb_moq_last_ai_receipt
#define quic_client_moq_min_rtt_us mqb_moq_min_rtt_us
#define quic_client_moq_srtt_us mqb_moq_srtt_us
#define quic_client_moq_predict_stats mqb_moq_predict_stats
#define quic_client_moq_publish_ex mqb_moq_publish_ex
#define quic_client_moq_finish mqb_moq_finish
#define quic_client_moq_finished mqb_moq_finished
#define quic_client_moq_tracks mqb_moq_tracks
#define quic_client_moq_configure mqb_moq_configure
#define quic_client_moq_service_rate mqb_moq_service_rate
#define quic_client_moq_service_initialized mqb_moq_service_initialized
#define quic_client_moq_new_group_requested mqb_moq_new_group_requested
#define quic_client_moq_policy_stats mqb_moq_policy_stats
/* Observations of the MOQT publisher that have no counterpart here. */
#define quic_client_moq_codec_observed(...) ((void)0)
#define quic_client_moq_pump_gap_observed(...) ((void)0)
#define quic_client_moq_source_observed(...) ((void)0)
