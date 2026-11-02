/* MQTT baseline of the mission; see moq_mqtt_shim.h. */
#if AIOT_MQTT_BASELINE
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
/* src/components/transport has a header of the same name; this one includes esp-mqtt's from its
 * own directory. */
#include "mqtt5_client.h"
#include "moq_mqtt_shim.h"
#ifndef AIOT_MOQT_MUS
#define AIOT_MOQT_MUS 0
#endif
#ifndef AIOT_MOQT_RATE_EVIDENCE_ONLY
#define AIOT_MOQT_RATE_EVIDENCE_ONLY 0
#endif

#define MQB_RESULTS "aiot/esp32/ai/events"
#define MQB_IMAGES "aiot/esp32/evidence/jpeg"
#define MQB_HELLO "aiot/esp32/ctl/hello"
#define MQB_DONE "aiot/esp32/ctl/done"
#define MQB_START "aiot/host/start"
#define MQB_ALIAS_RESULTS 2
#define MQB_ALIAS_IMAGES 3
/* The helper's frames (alias 1, offload/jpeg) on their own topic, QoS 1, so that the broker's
 * acknowledgement ends the frame's transmission (the Chen rule waits for it, as for an Object's
 * acknowledgement over MOQT); a helper subscribes to it. */
#define MQB_OFFLOAD "aiot/esp32/offload/jpeg"
#define MQB_ALIAS_OFFLOAD 1
/* The result slots of the MOQT publisher (AIOT_MOQT_RESULT_SLOTS of the relay campaign). */
#define MQB_RESULT_SLOTS 4
/* An image is under way while the outbox holds more than the results in flight. */
#define MQB_IMAGE_QUEUED_BYTES 1024
#define MQB_HEADER 7
#define MQB_CAP (256U * 1024U + 64U)

extern const char mqb_ca_start[] asm("_binary_mqtt_ca_pem_start");

static esp_mqtt_client_handle_t mqb;
static volatile bool mqb_connected, mqb_started, mqb_done_acked;
static volatile int mqb_inflight[MQB_RESULT_SLOTS];
static volatile int mqb_done_id = -1;
/* The evidence planner (AIOT_MOQT_MUS) over MQTT: the evidence goes with QoS 1 as well, so that the
 * broker's acknowledgement ends an object as the relay's does over MOQT; the service rate is the
 * acknowledged bytes over the time from the hand-over to the acknowledgement (EWMA of the objects,
 * the evidence only with AIOT_MOQT_RATE_EVIDENCE_ONLY). An object handed to the TCP stream cannot be
 * given up and no window bounds it: those are properties of the baseline. */
static volatile int mqb_image_id;            /* the evidence in flight to the broker, 0: none */
static volatile uint64_t mqb_image_group, mqb_image_t0, mqb_offload_t0;
static volatile size_t mqb_image_bytes;
#define MQB_RESULTS_DONE 8
static struct { uint64_t group, held_us; size_t bytes; } mqb_done_ring[MQB_RESULTS_DONE];
static volatile unsigned mqb_done_head, mqb_done_tail;
static volatile uint64_t mqb_rate_Bps;
static volatile unsigned mqb_rate_samples;
static volatile uint64_t mqb_result_source[MQB_RESULT_SLOTS], mqb_result_group[MQB_RESULT_SLOTS];
static volatile uint64_t mqb_ack_group, mqb_ack_source_to_ack;
static volatile bool mqb_ack_have;
static void mqb_rate_sample(size_t bytes, uint64_t us)
{
    if (!us || !bytes) return;
    uint64_t s = (uint64_t)bytes * 1000000ULL / us;
    mqb_rate_Bps = mqb_rate_samples ? (3 * mqb_rate_Bps + s) / 4 : s;
    mqb_rate_samples++;
}
static volatile int mqb_offload_id;          /* the frame in flight to the broker, 0: none */
static volatile uint64_t mqb_offload_group;
static volatile size_t mqb_offload_bytes;
static unsigned mqb_sessions;
static bool mqb_done_sent;
static uint8_t *mqb_buf;
static char mqb_uri[96];

static void mqb_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    esp_mqtt_event_handle_t e = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        mqb_connected = true;
        mqb_sessions++;
        esp_mqtt_client_subscribe(mqb, MQB_START, 1);
        esp_mqtt_client_enqueue(mqb, MQB_HELLO, "1", 1, 1, 0, true);
        printf("[MQB_CONNECTED] {\"session\":%u,\"present\":%d}\n", mqb_sessions, e->session_present);
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (mqb_connected) printf("[MQB_DISCONNECTED] {\"session\":%u}\n", mqb_sessions);
        mqb_connected = false;
        break;
    case MQTT_EVENT_DATA:
        if (e->topic_len == (int)strlen(MQB_START) && !memcmp(e->topic, MQB_START, (size_t)e->topic_len)) {
            if (!mqb_started) printf("[MQB_START]\n");
            mqb_started = true;
        }
        break;
    case MQTT_EVENT_PUBLISHED:
        for (unsigned i = 0; i < MQB_RESULT_SLOTS; i++)
            if (mqb_inflight[i] == e->msg_id) {
                mqb_inflight[i] = 0;
                uint64_t now = (uint64_t)esp_timer_get_time();
                mqb_ack_group = mqb_result_group[i];
                mqb_ack_source_to_ack = now > mqb_result_source[i] ? now - mqb_result_source[i] : 0;
                mqb_ack_have = true;
            }
        if (mqb_offload_id && mqb_offload_id == e->msg_id) {
            if (!AIOT_MOQT_RATE_EVIDENCE_ONLY) mqb_rate_sample(mqb_offload_bytes, (uint64_t)esp_timer_get_time() - mqb_offload_t0);
            mqb_offload_id = 0;
        }
        if (mqb_image_id && mqb_image_id == e->msg_id) {
            uint64_t held = (uint64_t)esp_timer_get_time() - mqb_image_t0;
            mqb_rate_sample(mqb_image_bytes, held);
            if (mqb_done_head - mqb_done_tail < MQB_RESULTS_DONE) {
                unsigned k = mqb_done_head % MQB_RESULTS_DONE;
                mqb_done_ring[k].group = mqb_image_group; mqb_done_ring[k].bytes = mqb_image_bytes; mqb_done_ring[k].held_us = held;
                mqb_done_head++;
            }
            mqb_image_id = 0;
        }
        if (e->msg_id == mqb_done_id) mqb_done_acked = true;
        break;
    case MQTT_EVENT_ERROR:
        if (e->error_handle)
            printf("[MQB_ERROR] {\"type\":%d,\"tls\":%d,\"sock_errno\":%d}\n", (int)e->error_handle->error_type,
                   (int)e->error_handle->esp_tls_last_esp_err, e->error_handle->esp_transport_sock_errno);
        break;
    default:
        break;
    }
}

int mqb_init_with_config(const quic_client_config_t *config)
{
    if (mqb) return 0;
    mqb_buf = heap_caps_malloc(MQB_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mqb_buf) return -1;
    snprintf(mqb_uri, sizeof(mqb_uri), "mqtts://%s:%s", config->hostname, config->port);
    /* The settings of the MOQT path where MQTT has them: an idle connection is given up after
     * about 5 s, and a new attempt follows 1 s after a failure. */
    esp_mqtt_client_config_t c = {
        .broker.address.uri = mqb_uri,
        .broker.verification.certificate = mqb_ca_start,
        .broker.verification.skip_cert_common_name_check = true,
        .credentials.client_id = "aiot-esp32",
        .session.keepalive = 5,
        .session.disable_clean_session = true,
        .network.reconnect_timeout_ms = 1000,
        .network.timeout_ms = 10000,
        .buffer.size = 2048,
        .buffer.out_size = 2048,
        .task.stack_size = 8192,
    };
    mqb = esp_mqtt_client_init(&c);
    if (!mqb) return -1;
    esp_mqtt_client_register_event(mqb, ESP_EVENT_ANY_ID, mqb_event, NULL);
    printf("[MOQ_PROFILE] transport=mqtt311-tls results_qos=1 images_qos=0 keepalive_s=5\n");
    return esp_mqtt_client_start(mqb) == ESP_OK ? 0 : -1;
}

int mqb_process(void) { return 0; }
bool mqb_is_connected(void) { return mqb_connected; }
int mqb_moq_start(const char *authority) { (void)authority; return 0; }
bool mqb_moq_session_ready(void) { return mqb_connected; }
int mqb_moq_announce(void) { return 0; }
bool mqb_moq_going_away(void) { return false; }
bool mqb_moq_track_ready(unsigned alias) { (void)alias; return mqb_started; }
int mqb_moq_tracks(const moqt_track_cfg *tracks, unsigned count) { (void)tracks; (void)count; return 0; }
int mqb_moq_configure(unsigned policy) { (void)policy; return 0; }
uint64_t mqb_moq_service_rate(void) { return mqb_rate_samples ? mqb_rate_Bps : 64000; }
bool mqb_moq_service_initialized(void) { return mqb_rate_samples != 0; }
uint64_t mqb_moq_service_deviation(void) { return 0; }
bool mqb_moq_image_flight(uint64_t *group, size_t *bytes, size_t *sent, size_t *acked, bool *abandoned)
{
    if (!mqb_image_id) return false;
    if (group) *group = mqb_image_group;
    if (bytes) *bytes = mqb_image_bytes;
    if (sent) *sent = mqb_image_bytes;
    if (acked) *acked = 0;
    if (abandoned) *abandoned = false;
    return true;
}
bool mqb_moq_image_result(uint64_t *group, size_t *bytes, uint64_t *held_us, bool *delivered)
{
    if (mqb_done_tail == mqb_done_head) return false;
    unsigned k = mqb_done_tail % MQB_RESULTS_DONE;
    if (group) *group = mqb_done_ring[k].group;
    if (bytes) *bytes = mqb_done_ring[k].bytes;
    if (held_us) *held_us = mqb_done_ring[k].held_us;
    if (delivered) *delivered = true;
    mqb_done_tail++;
    return true;
}
bool mqb_moq_image_abort(uint64_t group) { (void)group; return false; }
void mqb_moq_image_window(size_t bytes) { (void)bytes; }
void mqb_moq_alert_clock(uint32_t (*safe_us)(void)) { (void)safe_us; }
bool mqb_moq_last_ai_receipt(uint64_t *group, uint64_t *source_to_ack_us)
{
    if (!mqb_ack_have) return false;
    if (group) *group = mqb_ack_group;
    if (source_to_ack_us) *source_to_ack_us = mqb_ack_source_to_ack;
    return true;
}
uint64_t mqb_moq_min_rtt_us(void) { return 0; }
uint64_t mqb_moq_srtt_us(void) { return 0; }
void mqb_moq_predict_stats(uint64_t *max, uint64_t *mean) { if (max) *max = 0; if (mean) *mean = 0; }
bool mqb_moq_new_group_requested(unsigned alias) { (void)alias; return false; }
void mqb_moq_policy_stats(void) {}

bool mqb_moq_ai_slot_available(void)
{
    for (unsigned i = 0; i < MQB_RESULT_SLOTS; i++)
        if (!mqb_inflight[i]) return true;
    return false;
}

bool mqb_moq_offload_flight(uint64_t *group, size_t *bytes, size_t *acked, uint64_t *deadline_us)
{
    if (!mqb_offload_id) return false;
    if (group) *group = mqb_offload_group;
    if (bytes) *bytes = mqb_offload_bytes;
    if (acked) *acked = 0;
    if (deadline_us) *deadline_us = 0;
    return true;
}

bool mqb_moq_offload_last(uint64_t *group, size_t *bytes, size_t *acked, bool *delivered)
{
    /* an MQTT message cannot be given up: the frame that left the outbox was delivered whole */
    if (group) *group = mqb_offload_group;
    if (bytes) *bytes = mqb_offload_bytes;
    if (acked) *acked = mqb_offload_bytes;
    if (delivered) *delivered = true;
    return mqb_offload_bytes != 0;
}

bool mqb_moq_image_slot_available(void)
{
    return mqb_connected && esp_mqtt_client_get_outbox_size(mqb) < MQB_IMAGE_QUEUED_BYTES;
}

int mqb_moq_publish_ex(const quic_moq_object *q)
{
    if (q->alias != MQB_ALIAS_RESULTS && q->alias != MQB_ALIAS_IMAGES && q->alias != MQB_ALIAS_OFFLOAD) return 0;   /* status */
    size_t n = MQB_HEADER + q->props_len + q->len;
    if (!mqb || n > MQB_CAP || q->props_len > 0xFFFF) return -1;
    uint32_t g = (uint32_t)q->group;
    uint8_t h[MQB_HEADER] = {(uint8_t)q->alias, g >> 24, g >> 16, g >> 8, g, q->props_len >> 8, q->props_len};
    memcpy(mqb_buf, h, MQB_HEADER);
    if (q->props_len) memcpy(mqb_buf + MQB_HEADER, q->props, q->props_len);
    memcpy(mqb_buf + MQB_HEADER + q->props_len, q->data, q->len);
    bool result = q->alias == MQB_ALIAS_RESULTS;
    unsigned slot = 0;
    if (result) {
        while (slot < MQB_RESULT_SLOTS && mqb_inflight[slot]) slot++;
        if (slot == MQB_RESULT_SLOTS) return -1;
    }
    bool offload = q->alias == MQB_ALIAS_OFFLOAD, image = q->alias == MQB_ALIAS_IMAGES;
    if (offload && mqb_offload_id) return -1;   /* one frame at a time, as the image slot */
    if (image && AIOT_MOQT_MUS && mqb_image_id) return -1;
    int id = esp_mqtt_client_enqueue(mqb, result ? MQB_RESULTS : offload ? MQB_OFFLOAD : MQB_IMAGES, (const char *)mqb_buf, (int)n,
                                     result || offload || (image && AIOT_MOQT_MUS) ? 1 : 0, 0, true);
    if (id < 0) return -1;
    uint64_t now = (uint64_t)esp_timer_get_time();
    if (result) { mqb_result_group[slot] = q->group; mqb_result_source[slot] = q->source_us; mqb_inflight[slot] = id; }
    if (offload) { mqb_offload_group = q->group; mqb_offload_bytes = n; mqb_offload_t0 = now; mqb_offload_id = id; }
    if (image && AIOT_MOQT_MUS) { mqb_image_group = q->group; mqb_image_bytes = n; mqb_image_t0 = now; mqb_image_id = id; }
    return 0;
}

/* The end of the publication: once everything has been written, a QoS 1 message tells the
 * subscribers that nothing more follows. */
int mqb_moq_finish(void)
{
    if (!mqb_done_sent && mqb_connected && esp_mqtt_client_get_outbox_size(mqb) == 0) {
        mqb_done_id = esp_mqtt_client_enqueue(mqb, MQB_DONE, "1", 1, 1, 0, true);
        mqb_done_sent = mqb_done_id > 0;
    }
    return 0;
}

bool mqb_moq_finished(void) { return mqb_done_acked; }

void mqb_cleanup(void)
{
    if (mqb) {
        esp_mqtt_client_stop(mqb);
        esp_mqtt_client_destroy(mqb);
        mqb = NULL;
    }
    mqb_connected = false;
}
#endif
