/*
 * TCP-MQTT baseline entry point (the non-QUIC reference; see
 * docs/tcp_baseline_design.md). Selected by
 * APP_TRANSPORT_BACKEND == APP_TRANSPORT_BACKEND_TCP.
 *
 * Publishes every object (AI result, image) as an MQTT PUBLISH over one
 * TCP+TLS connection — no datagrams, no aux streams — so head-of-line blocking
 * under loss is exposed. Each payload is prefixed with the same
 * aiot_object_header_t (gen_ts_ms/object_seq) the QUIC datagram path uses, so a
 * subscriber measures freshness/delivery identically.
 *
 * The receiver-side measurement is done by the mosquitto logging subscriber;
 * this side just connects and publishes at the same cadence as the experiment.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "core_mqtt.h"

#include "../components/transport/mqtt_tcp_transport.h"
#include "../components/sources/include/source_manager.h"
#include "../components/wifi_link/include/wifi_link.h"
#include "../components/common/include/aiot_object.h"
#include "app_config.h"

static const char *TAG = "APP-TCP";

static MQTTContext_t   g_mqtt;
static NetworkContext_t g_net;              /* TCP NetworkContext */
static uint8_t         g_netbuf[2048];      /* coreMQTT fixed buffer */
static uint8_t         g_pubbuf[8192 + AIOT_OBJECT_HEADER_SIZE]; /* header + object */
static source_manager_t g_src;
static uint32_t        g_objseq[4];

static uint32_t app_tcp_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void app_tcp_event_cb(MQTTContext_t *ctx, MQTTPacketInfo_t *pkt,
                             MQTTDeserializedInfo_t *info)
{
    (void)ctx; (void)pkt; (void)info;   /* baseline: no inbound handling */
}

/* Frame [aiot_object_header_t | payload] and PUBLISH it on `topic`. */
static void app_tcp_publish(const char *topic, uint8_t track, uint8_t priority,
                            const uint8_t *payload, uint16_t len)
{
    if (payload == NULL || (size_t)len + AIOT_OBJECT_HEADER_SIZE > sizeof(g_pubbuf)) {
        return;
    }
    aiot_object_header_t hdr = {
        .magic_ver = AIOT_OBJECT_MAGIC_VER,
        .track_id = track,
        .priority = priority,
        .flags = 0,
        .object_seq = g_objseq[track & 3U]++,
        .gen_ts_ms = app_tcp_now_ms(),
    };
    memcpy(g_pubbuf, &hdr, AIOT_OBJECT_HEADER_SIZE);
    memcpy(g_pubbuf + AIOT_OBJECT_HEADER_SIZE, payload, len);

    static uint16_t pid = 1;
    MQTTPublishInfo_t pub = {
        .qos = MQTTQoS0,   /* best-effort parity; TCP ordering still causes HoL */
        .pTopicName = topic,
        .topicNameLength = (uint16_t)strlen(topic),
        .pPayload = g_pubbuf,
        .payloadLength = AIOT_OBJECT_HEADER_SIZE + (size_t)len,
    };
    if (MQTT_Publish(&g_mqtt, &pub, pid++) != MQTTSuccess) {
        g_net.stream_send_fail++;
    }
}

void app_tcp_baseline_main(void)
{
    printf("[APP]TCP-MQTT baseline starting\n");
    fflush(stdout);

    if (!wifi_link_connect(APP_WIFI_SSID, APP_WIFI_PASSWORD, APP_WIFI_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "Wi-Fi connect failed");
        return;
    }
    source_manager_init(&g_src);

    if (mqtt_tcp_transport_connect(&g_net, APP_TCP_BROKER_HOST, APP_TCP_BROKER_PORT,
                                   APP_TCP_USE_TLS) != 0) {
        ESP_LOGE(TAG, "TCP connect to broker failed");
        return;
    }

    TransportInterface_t ti = {
        .send = mqtt_tcp_transport_send,
        .recv = mqtt_tcp_transport_recv,
        .pNetworkContext = &g_net,
    };
    MQTTFixedBuffer_t fb = { .pBuffer = g_netbuf, .size = sizeof(g_netbuf) };
    if (MQTT_Init(&g_mqtt, &ti, app_tcp_now_ms, app_tcp_event_cb, &fb) != MQTTSuccess) {
        ESP_LOGE(TAG, "MQTT_Init failed");
        return;
    }

    MQTTConnectInfo_t ci = {
        .cleanSession = true,
        .pClientIdentifier = "aiot-tcp-bench",
        .clientIdentifierLength = (uint16_t)strlen("aiot-tcp-bench"),
        .keepAliveSeconds = 60,
    };
    bool session_present = false;
    if (MQTT_Connect(&g_mqtt, &ci, NULL, 5000U, &session_present) != MQTTSuccess) {
        ESP_LOGE(TAG, "MQTT_Connect failed");
        return;
    }
    ESP_LOGI(TAG, "TCP-MQTT connected to %s:%u", APP_TCP_BROKER_HOST, APP_TCP_BROKER_PORT);

    for (;;) {
        data_packet_t packet;
        if (source_manager_next_packet(&g_src, &packet) && packet.payload_len > 0) {
            if (packet.type == DATA_TYPE_IMAGE) {
                app_tcp_publish(APP_TCP_TOPIC_IMG, AIOT_TRACK_IMAGE, packet.priority,
                                packet.payload, packet.payload_len);
            } else {
                app_tcp_publish(APP_TCP_TOPIC_AI, AIOT_TRACK_AI_RESULT, packet.priority,
                                packet.payload, packet.payload_len);
            }
        }
        MQTT_ProcessLoop(&g_mqtt);
        vTaskDelay(pdMS_TO_TICKS(APP_CONTROL_PERIOD_MS));
    }
}
