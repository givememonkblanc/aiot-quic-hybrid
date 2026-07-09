#ifndef MQTT_QUIC_TRANSPORT_H
#define MQTT_QUIC_TRANSPORT_H

#include "core_mqtt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_timer.h"

#include "../common/include/aiot_object.h"

/**
 * @brief Delivery mode for hybrid QUIC transport (Stream vs Datagram).
 */
typedef enum {
    DELIVERY_RELIABLE = 0,    // QUIC Stream (reliable, ordered) - MQTT control, telemetry
    DELIVERY_UNRELIABLE = 1    // QUIC Datagram (unreliable, low-latency) - video frames, TinyML results
} delivery_mode_t;

/**
 * @brief Message packet for the IoT routing queue (between producer tasks and network task).
 */
typedef struct {
    delivery_mode_t mode;
    uint8_t *payload;
    size_t payload_len;
    uint32_t topic_id;
    uint8_t track_id;
    uint8_t priority;
    uint32_t object_seq;
    int64_t deadline_us;      // absolute expiry (esp_timer domain), 0 = no expiry
    int64_t enqueue_ts_us;
    int64_t dequeue_ts_us;
    int64_t send_start_ts_us;
    int64_t send_done_ts_us;
    uint32_t queue_depth_at_enqueue;
    int send_result;
} iot_route_msg_t;

/**
 * @brief Information about the server to connect to.
 */
typedef struct ServerInfo
{
    const char *pHostName;
    uint16_t port;
    const char *pAlpn;  // Application Layer Protocol Negotiation
} ServerInfo_t;

/**
 * @brief MQTT over QUIC connection configuration.
 */
typedef struct MQTTQUICConfig
{
    uint32_t timeoutMs;
    bool nonBlocking;
} MQTTQUICConfig_t;

/**
 * @brief Network context for the transport implementation.
 */
typedef struct NetworkContext
{
    const ServerInfo_t *pServerInfo;
    const MQTTQUICConfig_t *pMqttQuicConfig;
    
    /* Must hold one full MQTT packet: fixed header + topic + the largest
     * publish payload (APP_IMAGE_CHUNK_SIZE image chunks in stream mode). */
    uint8_t send_buffer[2048];
    size_t send_buffer_len;
    uint32_t expected_packet_length;
    bool packet_length_determined;
    bool is_mqtt_connect_packet;
    QueueHandle_t route_queue;
    uint32_t datagram_budget;
    uint32_t route_queue_max_depth;
    uint32_t route_queue_full_count;
    uint32_t stream_send_ok;
    uint32_t stream_send_fail;
    uint32_t datagram_send_ok;
    uint32_t datagram_send_fail;
    uint32_t datagram_expired;
    uint32_t datagram_evicted;
    uint32_t object_seq_next[4];   // per-track object sequence (index = track_id)
    uint32_t main_stack_hwm;
    uint32_t net_tx_stack_hwm;
} NetworkContext_t;

BaseType_t mqtt_quic_transport_init(NetworkContext_t *pNetworkContext,
                                  const ServerInfo_t *pServerInfo,
                                  const MQTTQUICConfig_t *pMqttQuicConfig);

int32_t mqtt_quic_transport_recv(NetworkContext_t *pNetworkContext,
                              void *pBuffer,
                              size_t bytesToRecv);

int32_t mqtt_quic_transport_send(NetworkContext_t *pNetworkContext,
                               const void *pBuffer,
                               size_t bytesToSend);

int mqtt_quic_transport_send_hybrid(NetworkContext_t *ctx,
                                    delivery_mode_t mode,
                                    const uint8_t *payload,
                                    size_t payload_len,
                                    uint8_t track_id,
                                    uint8_t priority,
                                    uint32_t deadline_ms);

// TransportInterface declaration
extern TransportInterface_t xTransportInterface;

// Add time function declaration
uint32_t mqtt_get_time_ms(void);

#endif /* MQTT_QUIC_TRANSPORT_H */
