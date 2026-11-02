#ifndef NGTCP2_SAMPLE_H
#define NGTCP2_SAMPLE_H

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Configuration structure for QUIC client
typedef struct {
    const char *hostname;
    const char *port;
    const char *alpn;
} quic_client_config_t;

#if AIOT_MOQT_EXPERIMENT
#include "moqt_core.h"
/* One object offered to the MOQT publisher (draft-ietf-moq-transport-21). */
typedef struct {
    unsigned alias;               /* track alias, 1-based */
    const uint8_t *data; size_t len;
    uint64_t group, object, subgroup;
    uint8_t priority, status;     /* publisher priority when has_priority; MOQT_STATUS_* */
    bool has_priority, end_of_group, datagram;
    bool retain;                  /* keep for FETCH */
    bool never_drop;              /* mission data: no deadline; retained when it cannot be sent now */
    bool announce_gap;            /* add PRIOR_GROUP_ID_GAP so that subscribers can detect losses */
    bool retain_only;             /* keep the object for FETCH without sending it on a subscription */
    const uint8_t *props; size_t props_len; /* serialized Object Properties */
    uint64_t source_us, deadline_us;        /* application capture time and deadline */
    unsigned importance;          /* slot policies S0-S5 only */
} quic_moq_object;
int quic_client_moq_tracks(const moqt_track_cfg *tracks, unsigned count);
int quic_client_moq_publish_ex(const quic_moq_object *object);
int quic_client_moq_offer_track(unsigned alias, bool forward);
int quic_client_moq_announce(void);
int quic_client_moq_send_goaway(uint32_t timeout_ms);
bool quic_client_moq_session_ready(void);
bool quic_client_moq_going_away(void);
unsigned quic_client_moq_sessions(void);
bool quic_client_moq_new_group_requested(unsigned alias);
void quic_client_moq_policy_stats(void);
uint64_t quic_client_moq_service_rate(void);
uint64_t quic_client_moq_service_deviation(void);
bool quic_client_moq_service_initialized(void);
void quic_client_moq_codec_observed(uint64_t decode_us,uint64_t encode_us,uint32_t cpu_us,bool ok,size_t scratch);
void quic_client_moq_pump_gap_observed(uint64_t gap_us);
void quic_client_moq_source_observed(unsigned count,unsigned failed,uint64_t span_us,uint64_t max_gap_us);
int quic_client_moq_configure(unsigned policy);
bool quic_client_moq_ai_slot_available(void);
int quic_client_moq_publish_object(unsigned alias,const uint8_t *,size_t,uint64_t group,uint64_t source_us,uint64_t deadline_us,unsigned importance);
bool quic_client_moq_last_ai_receipt(uint64_t *group, uint64_t *source_to_ack_us);
int quic_client_moq_start(const char *authority);
bool quic_client_moq_ready(void);
bool quic_client_moq_image_slot_available(void);
bool quic_client_moq_image_result(uint64_t *group,size_t *bytes,uint64_t *held_us,bool *delivered);
bool quic_client_moq_image_flight(uint64_t *group,size_t *bytes,size_t *sent,size_t *acked,bool *abandoned);
bool quic_client_moq_image_abort(uint64_t group);
bool quic_client_moq_offload_abort(uint64_t group);
bool quic_client_moq_offload_flight(uint64_t *group,size_t *bytes,size_t *acked,uint64_t *deadline_us);
/* The last offload Object that ended: its bytes, acknowledged bytes, and whether it was delivered whole. */
bool quic_client_moq_offload_last(uint64_t *group,size_t *bytes,size_t *acked,bool *delivered);
void quic_client_moq_image_window(size_t bytes);
/* AIOT_MOQT_MUS_PREDICT: the time until a result can exist at the earliest, and the extra image
 * bytes admitted for it since the last call (largest, mean over packets) */
void quic_client_moq_alert_clock(uint32_t (*safe_us)(void));
/* AIOT_MOQT_TRACE_MS: the transport part of a [MOQ_TRACE] record, as JSON members */
void quic_client_moq_trace(char *out, size_t cap);
void quic_client_moq_predict_stats(uint64_t *max, uint64_t *mean);
uint64_t quic_client_moq_min_rtt_us(void);
uint64_t quic_client_moq_srtt_us(void);
uint64_t quic_client_moq_capacity_Bps(void);
bool quic_client_moq_track_ready(unsigned alias);
int quic_client_moq_publish_track(unsigned alias,const uint8_t *,size_t,uint64_t);
int quic_client_moq_finish(void);
bool quic_client_moq_finished(void);
int quic_client_moq_publish(const uint8_t *jpeg,size_t len,uint64_t group);
#if AIOT_MOQT_HELPER
/* SETUP Implementation of a helper; the relay subscribes to helper/events of such a peer. */
#define MOQT_HELPER_IMPLEMENTATION "aiot-helper/1"
/* Subscriber of one track at a relay (helper mode). The sink runs in the QUIC task with the
 * transport lock held, once per complete Object; `data` is valid only during the call. */
typedef void (*quic_moq_helper_sink)(const uint8_t *data,size_t len,uint64_t group,uint64_t object,
                                     const uint8_t *props,size_t props_len,uint64_t first_us,uint64_t done_us);
void quic_client_moq_helper_sink(quic_moq_helper_sink sink);
/* SUBSCRIBE on a new request stream once the session is ready; 0 sent. */
int quic_client_moq_subscribe(const char *const *ns,unsigned ns_count,const char *name,uint8_t priority);
/* 1 subscribed, 0 waiting for SUBSCRIBE_OK, -1 refused or ended */
int quic_client_moq_subscription_state(void);
void quic_client_moq_helper_stats(char *out,size_t cap);
#endif
#endif

// Mutex for QUIC connection protection
extern SemaphoreHandle_t quic_mutex;

// Non-blocking QUIC client functions
int quic_client_init_with_config(const quic_client_config_t *config);
int quic_client_process(void);  // Non-blocking process function
bool quic_client_is_connected(void);
int quic_client_local_stream_avail(void);
void quic_client_cleanup(void);

/* Break-before-make: preserve TLS/QUIC state, bind a new socket, validate path. */
void quic_client_set_network_available(bool available);
int quic_client_migrate(uint32_t ipv4);
int quic_client_migration_status(void); /* 0 idle, 1 validating, 2 success, -1 failure */
uint32_t quic_client_session_id(void);

// Thread-safe QUIC operations
int quic_client_write_safe(const uint8_t *data, size_t datalen);
int quic_client_read_safe(uint8_t *buffer, size_t buffer_size, size_t *bytes_read);
int quic_client_write_datagram_safe(const uint8_t *data, size_t datalen, uint64_t dgram_id);
uint32_t quic_client_get_handshake_time_ms(void);
void quic_client_get_datagram_stats(uint32_t *acked, uint32_t *lost);

void quic_client_get_metrics(uint32_t *rtt_ms, uint32_t *cwnd, uint32_t *bytes_in_flight);

#if AIOT_QUIC_BATCH_EXPERIMENT
#define QUIC_BATCH_MAX_FRAGMENTS 256
/* Each entry is one <=1180-byte DATAGRAM, never a large QUIC DATAGRAM. */
typedef struct { const uint8_t *data; size_t len; uint64_t id; } quic_batch_fragment_t;
bool quic_batch_set(unsigned kib);
unsigned quic_batch_get(void);
size_t quic_client_write_batch_safe(const quic_batch_fragment_t *fragments, size_t count,
                                  int64_t deadline_us, int *result);
void quic_batch_report(void);
#endif

#endif
