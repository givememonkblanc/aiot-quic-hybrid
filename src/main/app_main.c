#include <stdio.h>
#include <string.h>

#if __has_include("freertos/FreeRTOS.h")
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#include <stdint.h>
#define pdMS_TO_TICKS(ms) (ms)
static inline void vTaskDelay(uint32_t ticks)
{
    (void)ticks;
}
#endif

#include "app_config.h"
#include "../components/common/include/data_packet.h"
#include "../components/common/include/ai_result.h"
#include "../components/dl_qads/include/dl_qads.h"
#include "../components/receiver/include/receiver.h"
#include "../components/sources/include/source_manager.h"
#include "../components/transport/include/transport_router.h"
#include "../components/ai_inference/include/ai_inference.h"
#include "../components/wifi_link/include/wifi_link.h"

#include "core_mqtt.h"
#include "core_mqtt_serializer.h"
#include "../components/transport/mqtt_quic_transport.h"
#include "ngtcp2_sample.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"

static const char *TAG = "APP-MAIN";

static dl_qads_t g_scheduler;
static receiver_t g_receiver;
static source_manager_t g_source_manager;
static transport_router_t g_router;
static ai_inference_t g_ai_inference;

static MQTTContext_t g_mqttContext;
static NetworkContext_t g_networkContext;
static uint8_t g_networkBuffer[2048];
static bool g_mqtt_connected = false;
static bool g_mqtt_transport_ready = false;
static bool g_mqtt_connect_sent = false;
extern void quic_client_get_metrics(uint32_t *rtt_ms, uint32_t *cwnd, uint32_t *bytes_in_flight);
extern uint32_t quic_client_get_handshake_time_ms(void);
static const size_t g_stress_payload_size = APP_STRESS_DATAGRAM_PAYLOAD_SIZE < 64 ? 64 : APP_STRESS_DATAGRAM_PAYLOAD_SIZE;
static MQTTConnectInfo_t g_mqttConnectInfo = {
    .cleanSession = true,
    .pClientIdentifier = APP_DEVICE_ID,
    .clientIdentifierLength = sizeof(APP_DEVICE_ID) - 1,
    .keepAliveSeconds = 60
};

static void mqtt_event_callback(MQTTContext_t *pMqttContext, MQTTPacketInfo_t *pPacketInfo, MQTTDeserializedInfo_t *pDeserializedInfo) {
    if (pPacketInfo->type == MQTT_PACKET_TYPE_CONNACK) {
        ESP_LOGI(TAG, "MQTT CONNACK received");
        g_mqtt_connected = true;
    } else if (pPacketInfo->type == MQTT_PACKET_TYPE_PUBACK) {
        ESP_LOGI(TAG, "MQTT PUBACK received");
    }
}

static void app_log_decision(const data_packet_t *packet, const net_metrics_t *metrics, const schedule_decision_t *decision, const aiot_ai_result_t *ai_result)
{
    printf("[TINYML-DATA],%llu,%d,%u,%lu,%lu,%lu,%lu,%lu,%d,%d,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu",
        (unsigned long long)packet->timestamp_us,
        packet->type,
        packet->payload_len,
        (unsigned long)metrics->rtt_ms,
        (unsigned long)metrics->loss_percent,
        (unsigned long)metrics->tx_queue_depth,
        (unsigned long)metrics->datagram_budget,
        (unsigned long)metrics->handshake_time_ms,
        metrics->stream_window_available ? 1 : 0,
        metrics->quic_ready ? 1 : 0,
        (unsigned long)metrics->cpu_usage,
        (unsigned long)metrics->heap_free,
        (unsigned long)metrics->heap_caps,
        (unsigned long)metrics->main_stack_hwm,
        (unsigned long)metrics->net_tx_stack_hwm,
        (unsigned long)metrics->tx_queue_max,
        (unsigned long)metrics->route_queue_full_count,
        (unsigned long)metrics->stream_send_ok,
        (unsigned long)metrics->stream_send_fail,
        (unsigned long)metrics->datagrams_sent,
        (unsigned long)metrics->datagrams_lost,
        (unsigned long)metrics->inference_us,
        (unsigned long)metrics->inference_to_enqueue_us,
        (unsigned long)decision->mode,
        (unsigned long)decision->pacing_level);
    
    if (ai_result != NULL && ai_result->detection_count > 0) {
        printf(",ai=%u,backend=%u,det=%u", 
            (unsigned)ai_result->detection_count,
            (unsigned)ai_result->backend,
            (unsigned)ai_result->detection_count);
    }
    printf("\n");

    if (decision->mode == TX_MODE_DROP) {
        printf("[DL-QADS-DROP] type=%d seq=%lu pacing=%u drop=%u\n",
            packet->type,
            (unsigned long)packet->sequence,
            decision->pacing_level,
            decision->drop_allowed);
    }
}

static void app_log_periodic_summary(uint32_t loop_count, const net_metrics_t *metrics)
{
    printf("[APP-SUMMARY] loop=%lu rtt_ms=%lu handshake_ms=%lu heap_free=%lu min_heap_8bit=%lu txq=%lu txq_max=%lu dgram_ok=%lu dgram_fail=%lu stream_ok=%lu stream_fail=%lu\n",
           (unsigned long)loop_count,
           (unsigned long)metrics->rtt_ms,
           (unsigned long)metrics->handshake_time_ms,
           (unsigned long)metrics->heap_free,
           (unsigned long)metrics->heap_caps,
           (unsigned long)metrics->tx_queue_depth,
           (unsigned long)metrics->tx_queue_max,
           (unsigned long)metrics->datagrams_sent,
           (unsigned long)metrics->datagrams_lost,
           (unsigned long)metrics->stream_send_ok,
           (unsigned long)metrics->stream_send_fail);
}

static uint32_t app_get_time_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static int app_mqtt_send_connect(void)
{
    uint8_t connect_buf[128];
    MQTTFixedBuffer_t connect_packet = {
        .pBuffer = connect_buf,
        .size = sizeof(connect_buf)
    };
    size_t remaining_length = 0;
    size_t packet_size = 0;

    MQTTStatus_t mqtt_status = MQTT_GetConnectPacketSize(&g_mqttConnectInfo,
                                                         NULL,
                                                         &remaining_length,
                                                         &packet_size);
    if (mqtt_status != MQTTSuccess) {
        ESP_LOGE(TAG, "MQTT_GetConnectPacketSize failed: %d", mqtt_status);
        return -1;
    }

    mqtt_status = MQTT_SerializeConnect(&g_mqttConnectInfo,
                                        NULL,
                                        remaining_length,
                                        &connect_packet);
    if (mqtt_status != MQTTSuccess) {
        ESP_LOGE(TAG, "MQTT_SerializeConnect failed: %d", mqtt_status);
        return -1;
    }

    if (mqtt_quic_transport_send(&g_networkContext, connect_buf, packet_size) != (int32_t)packet_size) {
        ESP_LOGE(TAG, "Failed to send serialized MQTT CONNECT packet");
        return -1;
    }

    g_mqtt_connect_sent = true;
    ESP_LOGI(TAG, "MQTT CONNECT packet sent");
    return 0;
}

static void app_mqtt_mark_connected(bool session_present)
{
    MQTTPacketInfo_t packet_info = {
        .type = MQTT_PACKET_TYPE_CONNACK,
        .pRemainingData = NULL,
        .remainingLength = 2,
        .headerLength = 2
    };
    MQTTDeserializedInfo_t deserialized_info = {
        .packetIdentifier = 0,
        .pPublishInfo = NULL,
        .deserializationResult = MQTTSuccess
    };

    g_mqttContext.connectStatus = MQTTConnected;
    g_mqttContext.keepAliveIntervalSec = g_mqttConnectInfo.keepAliveSeconds;
    g_mqttContext.waitingForPingResp = false;
    g_mqttContext.pingReqSendTimeMs = 0U;
    g_mqttContext.lastPacketRxTime = app_get_time_ms();
    g_mqtt_connected = true;
    g_mqtt_connect_sent = false;

    mqtt_event_callback(&g_mqttContext, &packet_info, &deserialized_info);
    ESP_LOGI(TAG, "MQTT session established (session_present=%d)", (int)session_present);
}

static void app_mqtt_poll_connect(void)
{
    uint8_t rx_buf[32];
    int32_t bytes_received = mqtt_quic_transport_recv(&g_networkContext, rx_buf, sizeof(rx_buf));

    if (bytes_received < 4) {
        return;
    }

    MQTTPacketInfo_t incoming_packet = {
        .type = rx_buf[0],
        .pRemainingData = &rx_buf[2],
        .remainingLength = rx_buf[1],
        .headerLength = 2
    };
    uint16_t packet_id = 0;
    bool session_present = false;
    MQTTStatus_t mqtt_status = MQTT_DeserializeAck(&incoming_packet, &packet_id, &session_present);

    if (incoming_packet.type == MQTT_PACKET_TYPE_CONNACK && mqtt_status == MQTTSuccess) {
        app_mqtt_mark_connected(session_present);
    }
}

void app_main(void)
{
    esp_err_t wdt_ret = esp_task_wdt_add(NULL);
    if (wdt_ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_task_wdt_add(main_task) returned %s", esp_err_to_name(wdt_ret));
    }

    putchar('A');
    putchar('\n');
    for (volatile int i = 0; i < 100000; i++);

    dl_qads_config_t scheduler_config = {
        .ai_stream_id = APP_AI_STREAM_ID,
        .sensor_stream_id = APP_SENSOR_STREAM_ID,
        .image_stream_id = APP_IMAGE_STREAM_ID,
        .image_datagram_budget = APP_IMAGE_DATAGRAM_BUDGET,
        .control_period_ms = APP_CONTROL_PERIOD_MS,
        .policy_mode = (qads_policy_mode_t)APP_QADS_POLICY_MODE,
    };

    printf("[APP]Starting aiot_quic firmware (MQTT over QUIC)\n");
    fflush(stdout);
    printf("[APP]Wi-Fi connecting to SSID=%s host=%s\n", APP_WIFI_SSID, APP_PEER_HOST);
    fflush(stdout);
    
    bool wifi_ready = wifi_link_connect(APP_WIFI_SSID, APP_WIFI_PASSWORD, APP_WIFI_TIMEOUT_MS);
    printf("[APP]Wi-Fi connect result: %d\n", (int)wifi_ready);
    fflush(stdout);

    ai_inference_init(&g_ai_inference);
    printf("[APP]AI inference initialized\n");
    fflush(stdout);
    ESP_LOGI("DEBUG", "EXP-E2: AI/TinyML Initialized. Checking heap...");
    if (!heap_caps_check_integrity_all(true)) {
        ESP_LOGE("DEBUG", "PANIC at E2! Heap corrupted BY AI/TinyML Initialization!");
    }

    source_manager_init(&g_source_manager);
    printf("[APP]Camera initialized\n");
    fflush(stdout);
    ESP_LOGI("DEBUG", "EXP-E1: Camera Initialized. Checking heap...");
    if (!heap_caps_check_integrity_all(true)) {
        ESP_LOGE("DEBUG", "PANIC at E1! Heap corrupted BY Camera Initialization!");
    }

    receiver_init(&g_receiver);
    dl_qads_init(&g_scheduler, &scheduler_config);
    transport_router_init(&g_router, NULL);

    if (wifi_ready) {
        bool quic_runtime_failed = false;
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", APP_MQTT_PORT);
        
        quic_client_config_t quic_cfg = {
            .hostname = APP_PEER_HOST,
            .port = port_str,
            .alpn = "\x04mqtt"
        };
        
        ESP_LOGI(TAG, "Initializing QUIC connection to %s:%d", APP_PEER_HOST, APP_MQTT_PORT);
        if (quic_client_init_with_config(&quic_cfg) != 0) {
            ESP_LOGE(TAG, "Failed to initialize QUIC client");
            quic_runtime_failed = true;
        }

        int wait_ms = 0;
        while (!quic_runtime_failed && !quic_client_is_connected() && wait_ms < 10000) {
            if (quic_client_process() != 0) {
                ESP_LOGE(TAG, "QUIC processing failed while waiting for connection");
                quic_client_cleanup();
                quic_runtime_failed = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            wait_ms += 100;
        }

        if (!quic_runtime_failed && quic_client_is_connected()) {
            int stream_wait_ms = 0;
            while (!quic_runtime_failed && !quic_client_local_stream_avail() && stream_wait_ms < 10000) {
                if (quic_client_process() != 0) {
                    ESP_LOGE(TAG, "QUIC processing failed while waiting for stream credit");
                    quic_client_cleanup();
                    quic_runtime_failed = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
                stream_wait_ms += 100;
            }

            if (quic_runtime_failed) {
                ESP_LOGE(TAG, "QUIC runtime failed before MQTT initialization");
            } else if (!quic_client_local_stream_avail()) {
                ESP_LOGE(TAG, "QUIC connected but no local bidirectional stream became available");
            } else {
                ESP_LOGI(TAG, "QUIC Connected and stream credit available. Initializing MQTT...");
            
                ServerInfo_t serverInfo = {
                    .pHostName = APP_PEER_HOST,
                    .port = APP_MQTT_PORT,
                    .pAlpn = "\x04mqtt"
                };

                MQTTQUICConfig_t mqttQuicConfig = {
                    .timeoutMs = 5000,
                    .nonBlocking = true
                };

                if (mqtt_quic_transport_init(&g_networkContext, &serverInfo, &mqttQuicConfig) != pdPASS) {
                    ESP_LOGE(TAG, "Failed to initialize MQTT-over-QUIC transport");
                } else {
                    xTransportInterface.pNetworkContext = &g_networkContext;
                    xTransportInterface.recv = mqtt_quic_transport_recv;
                    xTransportInterface.send = mqtt_quic_transport_send;

                    MQTTFixedBuffer_t fixedBuffer = {
                        .pBuffer = g_networkBuffer,
                        .size = sizeof(g_networkBuffer)
                    };

                    MQTTStatus_t mqttStatus = MQTT_Init(&g_mqttContext,
                                                       &xTransportInterface,
                                                       app_get_time_ms,
                                                       mqtt_event_callback,
                                                       &fixedBuffer);
                    if (mqttStatus != MQTTSuccess) {
                        ESP_LOGE(TAG, "MQTT_Init failed: %d", mqttStatus);
                    } else {

                        g_mqtt_transport_ready = true;
                    }
                }
            }
        } else if (!quic_runtime_failed) {
            ESP_LOGE(TAG, "Failed to establish QUIC connection");
        }
    }

    printf("[APP-MAIN] Entering main loop\n");
    fflush(stdout);

    while (1) {
        esp_task_wdt_reset();

        data_packet_t packet;
        net_metrics_t metrics = {0};
        static uint32_t loop_count = 0;
        static aiot_ai_result_t g_ai_result = {0};

        static uint8_t reassembly_buf[8192];
        static uint16_t reassembly_len = 0;
        static bool reassembly_active = false;

        loop_count++;

        if (quic_client_is_connected() && (!g_mqtt_transport_ready || g_mqtt_connected)) {
            if (quic_client_process() != 0) {
                ESP_LOGE(TAG, "QUIC processing failed in main loop; cleaning up connection");
                quic_client_cleanup();
            }
        }

        if (g_mqtt_connected) {
            esp_task_wdt_reset();
            MQTT_ProcessLoop(&g_mqttContext);
        } else if (g_mqtt_transport_ready && quic_client_is_connected()) {
            if (!g_mqtt_connect_sent) {
                if (app_mqtt_send_connect() != 0) {
                    ESP_LOGE(TAG, "Failed to start MQTT CONNECT sequence");
                }
            } else {
                app_mqtt_poll_connect();
            }
            esp_task_wdt_reset();
        }

        if (g_mqtt_transport_ready && !g_mqtt_connected && APP_TRANSPORT_MODE != APP_TRANSPORT_MODE_DATAGRAM_ONLY) {
            vTaskDelay(pdMS_TO_TICKS(APP_CONTROL_PERIOD_MS));
            continue;
        }

        if (!source_manager_next_packet(&g_source_manager, &packet)) {
            vTaskDelay(pdMS_TO_TICKS(APP_CONTROL_PERIOD_MS));
            continue;
        }

        esp_task_wdt_reset();

        uint32_t quic_cwnd = 0;
        uint32_t quic_bytes_in_flight = 0;
        quic_client_get_metrics(&metrics.rtt_ms, &quic_cwnd, &quic_bytes_in_flight);
        metrics.datagram_budget = quic_cwnd;
        metrics.tx_queue_depth = g_networkContext.route_queue ? (uint32_t)uxQueueMessagesWaiting(g_networkContext.route_queue) : 0;
        metrics.handshake_time_ms = quic_client_get_handshake_time_ms();
        metrics.quic_ready = quic_client_is_connected();
        metrics.heap_free = esp_get_free_heap_size();
        metrics.heap_caps = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
        metrics.main_stack_hwm = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
        metrics.net_tx_stack_hwm = g_networkContext.net_tx_stack_hwm;
        metrics.tx_queue_max = g_networkContext.route_queue_max_depth;
        metrics.route_queue_full_count = g_networkContext.route_queue_full_count;
        metrics.stream_send_ok = g_networkContext.stream_send_ok;
        metrics.stream_send_fail = g_networkContext.stream_send_fail;
        metrics.datagrams_sent = g_networkContext.datagram_send_ok;
        metrics.datagrams_lost = g_networkContext.datagram_send_fail;
        g_networkContext.main_stack_hwm = metrics.main_stack_hwm;

        if (packet.type == DATA_TYPE_IMAGE) {
            const uint8_t *payload = packet.payload;
            uint16_t payload_len = packet.payload_len;

            if (!reassembly_active) {
                if (payload_len < 2 || payload[0] != 0xFF || payload[1] != 0xD8) {
                    ESP_LOGW(TAG, "Invalid SOI, skipping len=%u", payload_len);
                    vTaskDelay(pdMS_TO_TICKS(APP_CONTROL_PERIOD_MS));
                    continue;
                }
                reassembly_active = true;
                reassembly_len = 0;
            }

            if (reassembly_len + payload_len > sizeof(reassembly_buf)) {
                ESP_LOGW(TAG, "Reassembly overflow: %u + %u > %u", reassembly_len, payload_len, (unsigned)sizeof(reassembly_buf));
                reassembly_active = false;
                reassembly_len = 0;
                vTaskDelay(pdMS_TO_TICKS(APP_CONTROL_PERIOD_MS));
                continue;
            }

            memcpy(reassembly_buf + reassembly_len, payload, payload_len);
            reassembly_len += payload_len;

            bool frame_complete = false;
            if (reassembly_len >= 2 &&
                reassembly_buf[reassembly_len - 2] == 0xFF &&
                reassembly_buf[reassembly_len - 1] == 0xD9) {
                frame_complete = true;
            }

            if (frame_complete) {
                ESP_LOGI(TAG, "JPEG frame reassembled: %u bytes", reassembly_len);
                int64_t inference_start_us = esp_timer_get_time();
                if (ai_inference_run(&g_ai_inference,
                                      reassembly_buf,
                                      reassembly_len,
                                      packet.sequence,
                                      &g_ai_result)) {
                    int64_t inference_done_us = esp_timer_get_time();
                    metrics.inference_us = (uint32_t)(inference_done_us - inference_start_us);
                    uint8_t tmp[APP_STRESS_DATAGRAM_PAYLOAD_SIZE < 64 ? 64 : APP_STRESS_DATAGRAM_PAYLOAD_SIZE];
                    memset(tmp, 0, sizeof(tmp));
                    int ai_len = snprintf((char *)tmp, sizeof(tmp),
                                               "ai=%u,backend=%u,det=%u",
                                               (unsigned)g_ai_result.detection_count,
                                               (unsigned)g_ai_result.backend,
                                               (unsigned)g_ai_result.detection_count);
                    if (ai_len > 0) {
                        size_t datagram_len = (size_t)ai_len;
                        uint32_t burst_count = 1;
                        if (APP_STRESS_MODE_ENABLE) {
                            datagram_len = g_stress_payload_size;
                            burst_count = APP_STRESS_DATAGRAM_BURST_COUNT;
                            for (size_t fill = (size_t)ai_len; fill < datagram_len; ++fill) {
                                tmp[fill] = (uint8_t)('A' + (fill % 26U));
                            }
                        }
                        delivery_mode_t tinyml_mode = DELIVERY_UNRELIABLE;
#if APP_TRANSPORT_MODE == APP_TRANSPORT_MODE_STREAM_ONLY
                        tinyml_mode = DELIVERY_RELIABLE;
#elif APP_TRANSPORT_MODE == APP_TRANSPORT_MODE_DATAGRAM_ONLY
                        tinyml_mode = DELIVERY_UNRELIABLE;
#else
                        tinyml_mode = DELIVERY_UNRELIABLE;
#endif

                        for (uint32_t burst = 0; burst < burst_count; ++burst) {
                            mqtt_quic_transport_send_hybrid(&g_networkContext,
                                                            tinyml_mode,
                                                            tmp, datagram_len, 0);
                            if (APP_STRESS_INTER_BURST_DELAY_MS > 0 && burst + 1U < burst_count) {
                                vTaskDelay(pdMS_TO_TICKS(APP_STRESS_INTER_BURST_DELAY_MS));
                            }
                        }
                        metrics.inference_to_enqueue_us = (uint32_t)(esp_timer_get_time() - inference_done_us);
                    }
                    app_log_decision(&packet, &metrics,
                                     &(schedule_decision_t){.mode = 0},
                                     &g_ai_result);
                }
                esp_task_wdt_reset();
                reassembly_active = false;
                reassembly_len = 0;
                vTaskDelay(pdMS_TO_TICKS(APP_CONTROL_PERIOD_MS));
                continue;
            }

            vTaskDelay(pdMS_TO_TICKS(APP_CONTROL_PERIOD_MS));
            continue;
        }

        schedule_decision_t decision = dl_qads_decide(&g_scheduler, &packet, &metrics);

        if (decision.mode == TX_MODE_DROP) {
            app_log_decision(&packet, &metrics, &decision, NULL);
            vTaskDelay(pdMS_TO_TICKS(APP_CONTROL_PERIOD_MS));
            continue;
        }

        if (packet.type != DATA_TYPE_IMAGE && APP_TRANSPORT_MODE == APP_TRANSPORT_MODE_DATAGRAM_ONLY) {
            mqtt_quic_transport_send_hybrid(&g_networkContext,
                                            DELIVERY_UNRELIABLE,
                                            packet.payload,
                                            packet.payload_len,
                                            0);
        } else if (packet.type != DATA_TYPE_IMAGE && g_mqtt_connected) {
            uint8_t topic[] = "aiot/telemetry";
            static uint16_t pid = 1;
            MQTTPublishInfo_t pub = {
                .qos = MQTTQoS0,
                .pTopicName = (const char *)topic,
                .topicNameLength = (uint16_t)sizeof(topic) - 1,
                .pPayload = (const void *)packet.payload,
                .payloadLength = packet.payload_len,
            };
            if (MQTT_Publish(&g_mqttContext, &pub, pid++) != MQTTSuccess) {
                ESP_LOGW(TAG, "MQTT publish failed type=%d seq=%lu",
                         packet.type, (unsigned long)packet.sequence);
            }
        }

        if (packet.type != DATA_TYPE_IMAGE) {
            app_log_decision(&packet, &metrics, &decision, NULL);
        }

        if ((loop_count % 100) == 0) {
            app_log_periodic_summary(loop_count, &metrics);
        }

        vTaskDelay(pdMS_TO_TICKS(APP_CONTROL_PERIOD_MS));
    }
}
