#include "netlog.h"
#include "video_admission.h"
#include <stdio.h>
#include <stdlib.h>
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
#include "../components/dl_qads/include/mobility_scheduler.h"
#include "../components/sources/include/source_manager.h"
#include "../components/wifi_link/include/wifi_link.h"
#include "../components/store_forward/include/store_forward.h"

#include "core_mqtt.h"
#include "core_mqtt_serializer.h"
#include "../components/transport/mqtt_quic_transport.h"
#include "../components/transport/ngtcp2_sample.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"

#if APP_PRODUCT_MODE
#include "app_cfg.h"
#include "presence.h"
#include "provisioning.h"
#include "ota.h"
#include "status_led.h"
#include "display.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"
#include "driver/gpio.h"
#endif
#if AIOT_MOQT_OTA_REMOTE
#include "ota.h"
#endif

static const char *TAG = "APP-MAIN";

static dl_qads_t g_scheduler;
static source_manager_t g_source_manager;
#if APP_STORE_FORWARD_ENABLE
static store_forward_t g_sf;   /* offline buffer for the drone/intermittent case */
#endif

#if APP_PRODUCT_MODE
static app_cfg_t g_cfg;
static presence_t g_presence;
static char g_client_id[APP_CFG_DEVICE_ID_LEN];
/* Per-device auto-registration / discovery topics (built at startup). */
static char g_topic_cmd[96];
static char g_topic_status[96];
static char g_topic_state[96];
static char g_topic_register[96];
static char g_topic_ha[160];
/* MQTT Last Will (offline availability); pointer is NULL until the product
 * path arms it, so the shared CONNECT helper serializes no will otherwise. */
static MQTTPublishInfo_t g_will;
static const MQTTPublishInfo_t *g_will_ptr;
#endif

static MQTTContext_t g_mqttContext;
static NetworkContext_t g_networkContext;
static uint8_t g_networkBuffer[2048];
static bool g_mqtt_connected = false;
static bool g_mqtt_transport_ready = false;
static bool g_mqtt_connect_sent = false;
extern void quic_client_get_metrics(uint32_t *rtt_ms, uint32_t *cwnd, uint32_t *bytes_in_flight);
extern uint32_t quic_client_get_handshake_time_ms(void);
#if !APP_PRODUCT_MODE
static const size_t g_stress_payload_size = APP_STRESS_DATAGRAM_PAYLOAD_SIZE < 64 ? 64 : APP_STRESS_DATAGRAM_PAYLOAD_SIZE;
#endif
static MQTTConnectInfo_t g_mqttConnectInfo = {
    .cleanSession = true,
    .pClientIdentifier = APP_DEVICE_ID,
    .clientIdentifierLength = sizeof(APP_DEVICE_ID) - 1,
    .keepAliveSeconds = 60
};

#if APP_PRODUCT_MODE
/* Set by the MQTT command topic (aiot/<device>/cmd) when an "ota <url>"
 * command arrives; consumed by the product loop. */
static char g_ota_url[256];
static volatile bool g_ota_pending;

static void app_handle_cmd(const char *topic, size_t topic_len,
                           const char *payload, size_t payload_len)
{
    if (payload_len > 4 && strncmp(payload, "ota ", 4) == 0) {
        size_t url_len = payload_len - 4;
        if (url_len < sizeof(g_ota_url)) {
            memcpy(g_ota_url, payload + 4, url_len);
            g_ota_url[url_len] = '\0';
            g_ota_pending = true;
            ESP_LOGW(TAG, "OTA command received via cmd topic");
        }
    } else {
        ESP_LOGI(TAG, "cmd topic message (%.*s) len=%u ignored",
                 (int)topic_len, topic, (unsigned)payload_len);
    }
}
#endif

static void mqtt_event_callback(MQTTContext_t *pMqttContext, MQTTPacketInfo_t *pPacketInfo, MQTTDeserializedInfo_t *pDeserializedInfo) {
    if (pPacketInfo->type == MQTT_PACKET_TYPE_CONNACK) {
        ESP_LOGI(TAG, "MQTT CONNACK received");
        g_mqtt_connected = true;
    } else if (pPacketInfo->type == MQTT_PACKET_TYPE_PUBACK) {
        ESP_LOGI(TAG, "MQTT PUBACK received");
#if APP_PRODUCT_MODE
    } else if ((pPacketInfo->type & 0xF0U) == MQTT_PACKET_TYPE_PUBLISH) {
        MQTTPublishInfo_t *pub = pDeserializedInfo ? pDeserializedInfo->pPublishInfo : NULL;
        if (pub != NULL && pub->pPayload != NULL) {
            app_handle_cmd(pub->pTopicName, pub->topicNameLength,
                           (const char *)pub->pPayload, pub->payloadLength);
        }
#endif
    }
}

#if !APP_PRODUCT_MODE
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
    printf(",%lu,%lu",
        (unsigned long)metrics->datagram_expired,
        (unsigned long)metrics->datagram_evicted);
    
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

#endif /* !APP_PRODUCT_MODE (experiment CSV logging) */

static uint32_t app_get_time_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

#if !APP_PRODUCT_MODE
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
/*
 * System-wide CPU busy percentage since the previous call, derived from the
 * idle tasks' share of the run-time counters (core-count agnostic).
 * When log_tasks is set, also emits a [CPU-STATS] line with each task's
 * cumulative share for per-task resource accounting.
 */
static uint32_t app_cpu_busy_percent(bool log_tasks)
{
    static uint64_t prev_task_total;
    static uint64_t prev_idle_total;

    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    TaskStatus_t *status = malloc((size_t)task_count * sizeof(TaskStatus_t));
    if (status == NULL) {
        return 0;
    }

    configRUN_TIME_COUNTER_TYPE total_runtime = 0;
    task_count = uxTaskGetSystemState(status, task_count, &total_runtime);

    uint64_t task_total = 0;
    uint64_t idle_total = 0;
    for (UBaseType_t i = 0; i < task_count; ++i) {
        task_total += status[i].ulRunTimeCounter;
        if (strncmp(status[i].pcTaskName, "IDLE", 4) == 0) {
            idle_total += status[i].ulRunTimeCounter;
        }
    }

    uint32_t busy_percent = 0;
    uint64_t delta_total = task_total - prev_task_total;
    uint64_t delta_idle = idle_total - prev_idle_total;
    if (prev_task_total != 0 && delta_total > 0 && delta_idle <= delta_total) {
        busy_percent = (uint32_t)(100ULL - (delta_idle * 100ULL) / delta_total);
    }
    prev_task_total = task_total;
    prev_idle_total = idle_total;

    if (log_tasks && task_total > 0) {
        printf("[CPU-STATS] busy_pct=%lu", (unsigned long)busy_percent);
        for (UBaseType_t i = 0; i < task_count; ++i) {
            uint64_t share = ((uint64_t)status[i].ulRunTimeCounter * 1000ULL) / task_total;
            if (share >= 5ULL) {  // skip tasks below 0.5% to keep the line short
                printf(" %s=%lu.%lu", status[i].pcTaskName,
                       (unsigned long)(share / 10ULL),
                       (unsigned long)(share % 10ULL));
            }
        }
        printf("\n");
    }

    free(status);
    return busy_percent;
}
#else
static uint32_t app_cpu_busy_percent(bool log_tasks)
{
    (void)log_tasks;
    return 0;
}
#endif
#endif /* !APP_PRODUCT_MODE (experiment CPU stats) */

static int app_mqtt_send_connect(void)
{
    uint8_t connect_buf[256];
    MQTTFixedBuffer_t connect_packet = {
        .pBuffer = connect_buf,
        .size = sizeof(connect_buf)
    };
    size_t remaining_length = 0;
    size_t packet_size = 0;

    /* Product path arms an MQTT Last Will (retained "offline") so the broker
     * announces the device dropping off; experiment path passes no will. */
    const MQTTPublishInfo_t *will = NULL;
#if APP_PRODUCT_MODE
    will = g_will_ptr;
#endif

    MQTTStatus_t mqtt_status = MQTT_GetConnectPacketSize(&g_mqttConnectInfo,
                                                         will,
                                                         &remaining_length,
                                                         &packet_size);
    if (mqtt_status != MQTTSuccess) {
        ESP_LOGE(TAG, "MQTT_GetConnectPacketSize failed: %d", mqtt_status);
        return -1;
    }
    if (packet_size > sizeof(connect_buf)) {
        ESP_LOGE(TAG, "CONNECT packet too large: %u", (unsigned)packet_size);
        return -1;
    }

    mqtt_status = MQTT_SerializeConnect(&g_mqttConnectInfo,
                                        will,
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

#if APP_SCENARIO_MODE_ENABLE
/*
 * Scenario timeline: phase 0 = normal, 1 = image burst, 2 = recovery.
 * Anchored at the first call after the MQTT session is up so every rep
 * measures the same offsets from data-plane start.
 */
static int64_t g_scenario_t0_us;
static int g_scenario_phase = -1;

static int scenario_phase_now(void)
{
    if (g_scenario_t0_us == 0) {
        g_scenario_t0_us = esp_timer_get_time();
    }
    int64_t elapsed_s = (esp_timer_get_time() - g_scenario_t0_us) / 1000000;
    int phase = (int)(elapsed_s / APP_SCENARIO_PHASE_SECS);
    if (phase > 2) {
        phase = 2;
    }
    if (phase != g_scenario_phase) {
        g_scenario_phase = phase;
        printf("[SCENARIO] phase=%d elapsed_s=%lld t_ms=%lld\n",
               phase, (long long)elapsed_s, (long long)(esp_timer_get_time() / 1000));
        fflush(stdout);
    }
    return phase;
}

static void scenario_maybe_send_image(const uint8_t *jpeg, size_t len, uint8_t priority)
{
    static int64_t last_tx_us;
#if APP_TRANSPORT_MODE == APP_TRANSPORT_MODE_STREAM_ONLY
    static uint16_t image_pid = 1;
#endif

    if (scenario_phase_now() != 1 || len == 0) {
        return;
    }
    int64_t now = esp_timer_get_time();
    if (last_tx_us != 0 && (now - last_tx_us) < (int64_t)APP_IMAGE_TX_PERIOD_MS * 1000) {
        return;
    }
    last_tx_us = now;

    size_t chunks = 0;
    for (size_t off = 0; off < len; off += APP_IMAGE_CHUNK_SIZE) {
        size_t chunk = (len - off < APP_IMAGE_CHUNK_SIZE) ? (len - off) : APP_IMAGE_CHUNK_SIZE;
#if APP_TRANSPORT_MODE == APP_TRANSPORT_MODE_STREAM_ONLY
        if (g_mqtt_connected) {
            uint8_t topic[] = "aiot/image";
            MQTTPublishInfo_t pub = {
                .qos = MQTTQoS0,
                .pTopicName = (const char *)topic,
                .topicNameLength = (uint16_t)sizeof(topic) - 1,
                .pPayload = (const void *)(jpeg + off),
                .payloadLength = chunk,
            };
            if (MQTT_Publish(&g_mqttContext, &pub, image_pid++) != MQTTSuccess) {
                ESP_LOGW(TAG, "Image chunk publish failed off=%zu", off);
            }
        }
#else
        mqtt_quic_transport_send_hybrid(&g_networkContext,
                                        DELIVERY_UNRELIABLE,
                                        jpeg + off, chunk,
                                        AIOT_TRACK_IMAGE,
                                        priority,
                                        APP_DEADLINE_IMAGE_MS);
#endif
        chunks++;
    }
    printf("[SCENARIO] image_tx len=%u chunks=%u t_ms=%lld\n",
           (unsigned)len, (unsigned)chunks, (long long)(esp_timer_get_time() / 1000));
}
#endif /* APP_SCENARIO_MODE_ENABLE */

/*
 * Establish the QUIC connection and initialize MQTT-over-QUIC on top of it.
 * The MQTT CONNECT/CONNACK handshake itself is driven by the main loop; this
 * only stands the transport up and leaves g_mqtt_transport_ready set on
 * success. Reusable by the product session supervisor for re-establishment.
 * serverInfo/mqttQuicConfig are static because the transport stores pointers
 * to them; `host` must likewise outlive the session (global cfg or literal).
 */
/*
 * Accumulate JPEG image chunks into buf. Returns COMPLETE once a full SOI..EOI
 * frame is assembled (caller consumes the buffer then resets active and len),
 * ACCUMULATING while collecting, or SKIP on a bad start-of-image / overflow
 * (state already reset). Shared by the experiment and product image loops.
 */
typedef enum {
    JPEG_ACC_ACCUMULATING = 0,
    JPEG_ACC_COMPLETE,
    JPEG_ACC_SKIP,
} jpeg_acc_t;

static jpeg_acc_t app_jpeg_reassemble(const uint8_t *payload, uint16_t payload_len,
    uint8_t *buf, size_t buf_size, uint16_t *len, bool *active)
{
    if (!*active) {
        if (payload_len < 2 || payload[0] != 0xFF || payload[1] != 0xD8) {
            return JPEG_ACC_SKIP;
        }
        *active = true;
        *len = 0;
    }
    if ((size_t)*len + payload_len > buf_size) {
        ESP_LOGW(TAG, "JPEG reassembly overflow, dropping frame (%u+%u>%u)",
            *len, payload_len, (unsigned)buf_size);
        *active = false;
        *len = 0;
        return JPEG_ACC_SKIP;
    }
    memcpy(buf + *len, payload, payload_len);
    *len = (uint16_t)(*len + payload_len);
    if (*len >= 2 && buf[*len - 2] == 0xFF && buf[*len - 1] == 0xD9) {
        return JPEG_ACC_COMPLETE;
    }
    return JPEG_ACC_ACCUMULATING;
}

static bool app_quic_mqtt_bringup(const char *host, uint16_t port)
{
    static ServerInfo_t serverInfo;
    static MQTTQUICConfig_t mqttQuicConfig;

    g_mqtt_connected = false;
    g_mqtt_connect_sent = false;
    g_mqtt_transport_ready = false;

    bool quic_runtime_failed = false;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    quic_client_config_t quic_cfg = {
        .hostname = host,
        .port = port_str,
        .alpn = "\x04mqtt"
    };

    ESP_LOGI(TAG, "Initializing QUIC connection to %s:%u", host, (unsigned)port);
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

            serverInfo.pHostName = host;
            serverInfo.port = port;
            serverInfo.pAlpn = "\x04mqtt";
            mqttQuicConfig.timeoutMs = 5000;
            mqttQuicConfig.nonBlocking = true;

            /* The transport (route queue + network TX task) is created once for
             * the process lifetime; on a supervisor-driven re-establish we only
             * re-init QUIC and MQTT and reuse the existing queue/task, otherwise
             * every reconnect would leak a queue and spawn a duplicate task. */
            static bool transport_inited = false;
            bool transport_ok = transport_inited;
            if (!transport_inited) {
                if (mqtt_quic_transport_init(&g_networkContext, &serverInfo, &mqttQuicConfig) == pdPASS) {
                    transport_inited = true;
                    transport_ok = true;
                }
            }
            if (!transport_ok) {
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

    return g_mqtt_transport_ready;
}

#if !APP_PRODUCT_MODE
static void app_experiment_main(void)
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
        .telemetry_deadline_ms = APP_DEADLINE_CLASS1_MS,
        .image_deadline_ms = APP_DEADLINE_IMAGE_MS,
        .policy_mode = (qads_policy_mode_t)APP_QADS_POLICY_MODE,
    };

    printf("[APP]Starting aiot_quic firmware (MQTT over QUIC)\n");
    printf("[APP]reset_reason=%d\n", (int)esp_reset_reason());
    fflush(stdout);
    printf("[APP]Wi-Fi connecting to SSID=%s host=%s\n", APP_WIFI_SSID, APP_PEER_HOST);
    fflush(stdout);
    
    bool wifi_ready = wifi_link_connect(APP_WIFI_SSID, APP_WIFI_PASSWORD, APP_WIFI_TIMEOUT_MS);
    printf("[APP]Wi-Fi connect result: %d\n", (int)wifi_ready);
    fflush(stdout);

    printf("[APP]AI inference initialized\n");
    fflush(stdout);
    ESP_LOGI("DEBUG", "EXP-E2: AI/TinyML Initialized. Checking heap...");
    if (!heap_caps_check_integrity_all(true)) {
        ESP_LOGE("DEBUG", "PANIC at E2! Heap corrupted BY AI/TinyML Initialization!");
    }

    source_manager_init(&g_source_manager);
#if APP_STORE_FORWARD_ENABLE
    store_forward_init(&g_sf);
#endif
    printf("[APP]Camera initialized\n");
    fflush(stdout);
    ESP_LOGI("DEBUG", "EXP-E1: Camera Initialized. Checking heap...");
    if (!heap_caps_check_integrity_all(true)) {
        ESP_LOGE("DEBUG", "PANIC at E1! Heap corrupted BY Camera Initialization!");
    }

    dl_qads_init(&g_scheduler, &scheduler_config);

    if (wifi_ready) {
        app_quic_mqtt_bringup(APP_PEER_HOST, APP_MQTT_PORT);
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

        // All transport modes establish the MQTT session over a stream before
        // the data plane starts (control channel is stream-only by design).
        if (g_mqtt_transport_ready && !g_mqtt_connected) {
            vTaskDelay(pdMS_TO_TICKS(APP_CONTROL_PERIOD_MS));
            continue;
        }

#if APP_SCENARIO_MODE_ENABLE
        scenario_phase_now();
#endif

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
        metrics.stream_window_available = quic_client_local_stream_avail() != 0;
        metrics.cpu_usage = app_cpu_busy_percent((loop_count % 100) == 0);
        metrics.heap_free = esp_get_free_heap_size();
        metrics.heap_caps = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
        metrics.main_stack_hwm = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
        metrics.net_tx_stack_hwm = g_networkContext.net_tx_stack_hwm;
        metrics.tx_queue_max = g_networkContext.route_queue_max_depth;
        metrics.route_queue_full_count = g_networkContext.route_queue_full_count;
        metrics.stream_send_ok = g_networkContext.stream_send_ok;
        metrics.stream_send_fail = g_networkContext.stream_send_fail;
        metrics.datagrams_sent = g_networkContext.datagram_send_ok;
        uint32_t dgram_acked = 0;
        quic_client_get_datagram_stats(&dgram_acked, &metrics.datagrams_lost);
        metrics.datagram_expired = g_networkContext.datagram_expired;
        metrics.datagram_evicted = g_networkContext.datagram_evicted;
        g_networkContext.main_stack_hwm = metrics.main_stack_hwm;

        if (packet.type == DATA_TYPE_IMAGE) {
            const uint8_t *payload = packet.payload;
            uint16_t payload_len = packet.payload_len;

            jpeg_acc_t st = app_jpeg_reassemble(payload, payload_len,
                reassembly_buf, sizeof(reassembly_buf),
                &reassembly_len, &reassembly_active);

            if (st == JPEG_ACC_COMPLETE) {
                ESP_LOGI(TAG, "JPEG frame reassembled: %u bytes", reassembly_len);
                /* Inference already ran on this frame inside source_manager;
                 * consume that result instead of running the model again. */
                if (source_manager_get_last_ai_result(&g_source_manager,
                                                       &g_ai_result,
                                                       &metrics.inference_us)) {
                    int64_t inference_done_us = esp_timer_get_time();
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

#if APP_STORE_FORWARD_ENABLE
                        if (!quic_client_is_connected()) {
                            /* Link down (coverage gap): buffer with current gen_ts. */
                            store_forward_push(&g_sf, AIOT_TRACK_AI_RESULT, packet.priority,
                                (uint32_t)(esp_timer_get_time() / 1000), 0U,
                                tmp, (uint16_t)datagram_len);
                        } else {
                            /* Link up: flush buffered detections (original gen_ts) first. */
                            sf_object_t bo;
                            for (int d = 0; d < APP_STORE_FORWARD_DRAIN_PER_LOOP &&
                                            store_forward_pop(&g_sf, &bo); ++d) {
                                mqtt_quic_transport_send_hybrid_ts(&g_networkContext,
                                    DELIVERY_UNRELIABLE, bo.data, bo.len, bo.track,
                                    bo.priority, APP_DEADLINE_CLASS1_MS, bo.gen_ts_ms);
                            }
#endif
                        for (uint32_t burst = 0; burst < burst_count; ++burst) {
                            mqtt_quic_transport_send_hybrid(&g_networkContext,
                                                            tinyml_mode,
                                                            tmp, datagram_len,
                                                            AIOT_TRACK_AI_RESULT,
                                                            packet.priority,
                                                            APP_DEADLINE_CLASS1_MS);
                            if (APP_STRESS_INTER_BURST_DELAY_MS > 0 && burst + 1U < burst_count) {
                                vTaskDelay(pdMS_TO_TICKS(APP_STRESS_INTER_BURST_DELAY_MS));
                            }
                        }
#if APP_STORE_FORWARD_ENABLE
                        }
#endif
                        metrics.inference_to_enqueue_us = (uint32_t)(esp_timer_get_time() - inference_done_us);
                    }
                    app_log_decision(&packet, &metrics,
                                     &(schedule_decision_t){.mode = 0},
                                     &g_ai_result);
                }
#if APP_SCENARIO_MODE_ENABLE
                scenario_maybe_send_image(reassembly_buf, reassembly_len, packet.priority);
#endif
                esp_task_wdt_reset();
                reassembly_active = false;
                reassembly_len = 0;
            }

            vTaskDelay(pdMS_TO_TICKS(APP_CONTROL_PERIOD_MS));
            continue;
        }

        schedule_decision_t decision = dl_qads_decide(&g_scheduler, &packet, &metrics,
                                                      (uint64_t)esp_timer_get_time());

        if (decision.mode == TX_MODE_DROP) {
            app_log_decision(&packet, &metrics, &decision, NULL);
            vTaskDelay(pdMS_TO_TICKS(APP_CONTROL_PERIOD_MS));
            continue;
        }

        if (packet.type != DATA_TYPE_IMAGE) {
            bool use_datagram;
#if APP_TRANSPORT_MODE == APP_TRANSPORT_MODE_DATAGRAM_ONLY
            use_datagram = true;
#elif APP_TRANSPORT_MODE == APP_TRANSPORT_MODE_STREAM_ONLY
            use_datagram = false;
#else
            // Hybrid: the scheduler's forwarding preference is authoritative.
            use_datagram = (decision.mode == TX_MODE_DATAGRAM);
#endif
            if (decision.mode == TX_MODE_DEFER && APP_TRANSPORT_MODE == APP_TRANSPORT_MODE_HYBRID) {
                app_log_decision(&packet, &metrics, &decision, NULL);
                vTaskDelay(pdMS_TO_TICKS(APP_CONTROL_PERIOD_MS));
                continue;
            }

            if (use_datagram) {
                mqtt_quic_transport_send_hybrid(&g_networkContext,
                                                DELIVERY_UNRELIABLE,
                                                packet.payload,
                                                packet.payload_len,
                                                AIOT_TRACK_TELEMETRY,
                                                packet.priority,
                                                decision.delivery_timeout_ms != 0U
                                                    ? decision.delivery_timeout_ms
                                                    : APP_DEADLINE_CLASS1_MS);
            } else if (g_mqtt_connected) {
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
#endif /* !APP_PRODUCT_MODE */

#if APP_PRODUCT_MODE
/* -------------------------------------------------------------------------
 * Product application: presence-sensor device.
 * ---------------------------------------------------------------------- */

static int app_wifi_rssi_dbm(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return 0;
}

/* Publish a presence state-change event on the reliable, ordered QUIC stream
 * (MQTT PUBLISH). Semantics: events must never be lost or reordered. */
static void app_publish_presence_event(const presence_event_t *ev)
{
    if (!g_mqtt_connected) {
        return;
    }
    char json[256];
    int n = presence_event_json(ev, g_cfg.device_id, esp_timer_get_time(), json, sizeof(json));
    if (n <= 0) {
        return;
    }
    static uint16_t pid = 1;
    MQTTPublishInfo_t pub = {
        .qos = MQTTQoS0,
        .pTopicName = APP_TOPIC_PRESENCE_EVENT,
        .topicNameLength = (uint16_t)(sizeof(APP_TOPIC_PRESENCE_EVENT) - 1),
        .pPayload = (const void *)json,
        .payloadLength = (size_t)n,
    };
    if (MQTT_Publish(&g_mqttContext, &pub, pid++) != MQTTSuccess) {
        ESP_LOGW(TAG, "presence event publish failed");
    }
    printf("[PRESENCE] %s\n", json);
    fflush(stdout);
}

/* Publish a retained value on the reliable stream (used for availability,
 * current state, registration, and discovery config so a backend that
 * connects later still sees the device's latest snapshot). */
static void app_publish_retained(const char *topic, const char *payload, size_t len)
{
    if (!g_mqtt_connected) {
        return;
    }
    static uint16_t pid = 200;
    MQTTPublishInfo_t pub = {
        .qos = MQTTQoS0,
        .retain = true,
        .pTopicName = topic,
        .topicNameLength = (uint16_t)strlen(topic),
        .pPayload = (const void *)payload,
        .payloadLength = len,
    };
    if (MQTT_Publish(&g_mqttContext, &pub, pid++) != MQTTSuccess) {
        ESP_LOGW(TAG, "retained publish to %s failed", topic);
    }
}

/*
 * Auto-registration & discovery, published once per session after CONNACK:
 * the device announces its identity + topic map (retained), marks itself
 * online (paired with the offline Last Will), publishes a Home Assistant
 * MQTT Discovery config so a compatible backend auto-adds the sensor, and
 * seeds the retained current state.
 */
static void app_publish_registration(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    const char *fw = (desc != NULL) ? desc->version : "unknown";

    app_publish_retained(g_topic_status, APP_STATUS_ONLINE, strlen(APP_STATUS_ONLINE));

    char reg[360];
    int n = snprintf(reg, sizeof(reg),
        "{\"device\":\"%s\",\"type\":\"%s\",\"model\":\"%s\",\"mf\":\"%s\","
        "\"fw\":\"%s\",\"hb_ms\":%lu,\"topics\":{\"event\":\"%s\",\"state\":\"%s\","
        "\"status\":\"%s\",\"cmd\":\"%s\"}}",
        g_cfg.device_id, APP_DEVICE_TYPE, APP_DEVICE_MODEL, APP_DEVICE_MANUFACTURER,
        fw, (unsigned long)g_cfg.heartbeat_ms, APP_TOPIC_PRESENCE_EVENT,
        g_topic_state, g_topic_status, g_topic_cmd);
    if (n > 0 && (size_t)n < sizeof(reg)) {
        app_publish_retained(g_topic_register, reg, (size_t)n);
    }

#if APP_HA_DISCOVERY_ENABLE
    char ha[512];
    n = snprintf(ha, sizeof(ha),
        "{\"name\":\"Presence %s\",\"uniq_id\":\"%s_presence\",\"dev_cla\":\"occupancy\","
        "\"stat_t\":\"%s\",\"pl_on\":\"present\",\"pl_off\":\"absent\","
        "\"avty_t\":\"%s\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
        "\"dev\":{\"ids\":[\"%s\"],\"name\":\"AIoT Presence %s\",\"mdl\":\"%s\",\"mf\":\"%s\"}}",
        g_cfg.device_id, g_cfg.device_id, g_topic_state, g_topic_status,
        g_cfg.device_id, g_cfg.device_id, APP_DEVICE_MODEL, APP_DEVICE_MANUFACTURER);
    if (n > 0 && (size_t)n < sizeof(ha)) {
        app_publish_retained(g_topic_ha, ha, (size_t)n);
    }
#endif

    const char *st = presence_state_name(g_presence.state);
    app_publish_retained(g_topic_state, st, strlen(st));

    printf("[APP]registered device=%s (announce + discovery published)\n", g_cfg.device_id);
    fflush(stdout);
}

/* Snapshot on enter: loss-tolerant JPEG delivered as QUIC datagram chunks. */
static void app_send_snapshot(const uint8_t *jpeg, size_t len, uint8_t priority)
{
    if (len == 0) {
        return;
    }
    size_t chunks = 0;
    for (size_t off = 0; off < len; off += APP_SNAPSHOT_CHUNK_SIZE) {
        size_t chunk = (len - off < APP_SNAPSHOT_CHUNK_SIZE) ? (len - off) : APP_SNAPSHOT_CHUNK_SIZE;
        mqtt_quic_transport_send_hybrid(&g_networkContext, DELIVERY_UNRELIABLE,
                                        jpeg + off, chunk, AIOT_TRACK_IMAGE,
                                        priority, APP_DEADLINE_IMAGE_MS);
        chunks++;
    }
    printf("[PRESENCE] snapshot tx len=%u chunks=%u\n", (unsigned)len, (unsigned)chunks);
    fflush(stdout);
}

#if APP_STREAM_ENABLE
/* Live view: send the captured JPEG frame on the image track (loss-tolerant
 * datagram chunks), rate-limited. The receiver reassembles it into an MJPEG
 * stream. Independent of presence state.
 *
 * Returns the gen_ts stamped on every chunk of this frame, or 0 when the
 * frame was rate-limited away. All chunks share one gen_ts so the receiver
 * can key the reassembled JPEG by it and join it with the AI result produced
 * from the same capture. */
static uint32_t app_image_reject(const char *reason, size_t len) {
#if AIOT_MOBILITY_CAMERA
    if (mobility_profile_get() >= 7) printf("[IMG_GATE] {\"us\":%llu,\"reason\":\"%s\",\"bytes\":%u}\n",
        (unsigned long long)esp_timer_get_time(),reason,(unsigned)len);
#endif
    return 0;
}

static uint32_t app_stream_frame_at(const uint8_t *jpeg, size_t len, uint8_t priority, uint32_t frame_ts)
{
    static int64_t last_us;
#if AIOT_MOBILITY_CAMERA
    static video_admission_t admission;
#endif
    if (len < 4) {
        return app_image_reject("invalid_jpeg",len);
    }
    int64_t now = esp_timer_get_time();
    if (last_us != 0 && (now - last_us) < (int64_t)APP_STREAM_INTERVAL_MS * 1000) {
        return app_image_reject("interval",len);
    }
    last_us = now;
    uint32_t gen_ts_ms = frame_ts ? frame_ts : (uint32_t)(now / 1000);
#if AIOT_MOBILITY_CAMERA
    size_t chunk_count = (len + APP_SNAPSHOT_CHUNK_SIZE - 1) / APP_SNAPSHOT_CHUNK_SIZE;
    size_t required = len + chunk_count * (AIOT_OBJECT_HEADER_SIZE + 16);
    uint32_t used = __atomic_load_n(&g_networkContext.route_payload_bytes, __ATOMIC_RELAXED);
    if (required > mobility_video_budget() || used > mobility_video_budget() - required ||
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) < required + 131072U)
        return app_image_reject("memory_budget",len);
    uint32_t video_rtt = 0, video_cwnd = 0, video_inflight = 0;
    quic_client_get_metrics(&video_rtt, &video_cwnd, &video_inflight);
    wifi_ap_record_t ap;
    int video_rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
    bool video_queue_busy=g_networkContext.route_queue && uxQueueMessagesWaiting(g_networkContext.route_queue);
#if AIOT_QUIC_BATCH_EXPERIMENT
    video_queue_busy = video_queue_busy || mqtt_quic_image_inflight();
#endif
    if (mobility_mode_get() == 2 && mobility_profile_get() != 6 && mobility_profile_get() != 8 && !video_admission_allow(&admission,
        video_policy_get(), now, video_rssi, video_rtt, video_cwnd, video_inflight,
        len, video_queue_busy))
        return app_image_reject("admission",len);

#endif
    if (g_networkContext.route_queue && uxQueueSpacesAvailable(g_networkContext.route_queue) <
        (len + APP_SNAPSHOT_CHUNK_SIZE - 1) / APP_SNAPSHOT_CHUNK_SIZE) return app_image_reject("queue_slots",len);
#if AIOT_PIPELINE_PROFILE
    extern uint32_t aiot_task_runtime_now(void);
    uint32_t fragment_cpu=aiot_task_runtime_now();int64_t fragment_start=esp_timer_get_time();
#endif
    for (size_t off = 0; off < len; off += APP_SNAPSHOT_CHUNK_SIZE) {
        size_t chunk = (len - off < APP_SNAPSHOT_CHUNK_SIZE) ? (len - off) : APP_SNAPSHOT_CHUNK_SIZE;
        /* Generous deadline so a frame's chunks aren't shed while queued;
         * dropping a chunk corrupts the whole frame at the receiver. */
        int accepted = mqtt_quic_transport_send_hybrid_ts(&g_networkContext, DELIVERY_UNRELIABLE,
                                           jpeg + off, chunk, AIOT_TRACK_IMAGE,
                                           priority,
#if AIOT_MOBILITY_CAMERA
                                           mobility_mode_get() == 2 ? 2000U : 0U,
#else
                                           1500U,
#endif
                                           gen_ts_ms);
        if (accepted != (int)chunk) return app_image_reject("enqueue_partial",len);
    }
#if AIOT_PIPELINE_PROFILE
    printf("[FRAGMENT_PROFILE] {\"gen_ms\":%lu,\"bytes\":%u,\"fragments\":%u,\"cpu_us\":%lu,\"wall_us\":%lld}\n",
        (unsigned long)gen_ts_ms,(unsigned)len,(unsigned)chunk_count,
        (unsigned long)(aiot_task_runtime_now()-fragment_cpu),(long long)(esp_timer_get_time()-fragment_start));
#endif
    return gen_ts_ms;
}

static uint32_t app_stream_frame(const uint8_t *jpeg, size_t len, uint8_t priority)
{
    return app_stream_frame_at(jpeg, len, priority, 0);
}

/* The inference result for a streamed frame, on the AI track, stamped with
 * that frame's gen_ts. The product path otherwise publishes only presence
 * transitions, which leaves the host with no per-frame score to compare
 * against a reference detector. */
static void app_send_ai_result(const aiot_ai_result_t *result,
                               uint32_t gen_ts_ms, uint8_t priority)
{
    uint8_t buf[AIOT_AI_RESULT_MAX_PAYLOAD_LEN];
    uint16_t n = aiot_ai_result_serialize(result, buf, (uint16_t)sizeof(buf));

    if (n == 0U) {
        return;
    }
    mqtt_quic_transport_send_hybrid_ts(&g_networkContext, DELIVERY_UNRELIABLE,
                                       buf, n, AIOT_TRACK_AI_RESULT,
                                       priority, APP_DEADLINE_CLASS1_MS, gen_ts_ms);
}
#endif

static uint32_t g_hb_count;

static void app_send_heartbeat(int64_t now_us)
{
    g_hb_count++;
    /* Re-evaluate the exit timeout even when no frame arrived. */
    presence_event_t tick_ev;
    if (presence_tick(&g_presence, now_us, &tick_ev)) {
        app_publish_presence_event(&tick_ev);
        const char *st = presence_state_name(tick_ev.state);
        app_publish_retained(g_topic_state, st, strlen(st));
        status_led_set(tick_ev.state == PRESENCE_STATE_PRESENT
                           ? STATUS_LED_PRESENT : STATUS_LED_ONLINE);
    }
    char hb[256];
    int n = presence_heartbeat_json(&g_presence, g_cfg.device_id, now_us,
                                    app_wifi_rssi_dbm(), esp_get_free_heap_size(),
                                    hb, sizeof(hb));
    if (n > 0) {
        mqtt_quic_transport_send_hybrid(&g_networkContext, DELIVERY_UNRELIABLE,
                                        (const uint8_t *)hb, (size_t)n,
                                        AIOT_TRACK_TELEMETRY, 1, APP_DEADLINE_CLASS1_MS);
    }
}

static void app_product_main(void)
{
    esp_err_t wdt_ret = esp_task_wdt_add(NULL);
    if (wdt_ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_task_wdt_add(main_task) returned %s", esp_err_to_name(wdt_ret));
    }

    putchar('A');
    putchar('\n');
    for (volatile int i = 0; i < 100000; i++);

    status_led_init(APP_STATUS_LED_GPIO);
    status_led_set(STATUS_LED_BOOT);

    /* Persistent config: NVS values over compile-time defaults. */
    app_cfg_load(&g_cfg);
    printf("[APP]Starting AIoT Presence Sensor (product build)\n");
    printf("[APP]reset_reason=%d\n", (int)esp_reset_reason());
    app_cfg_print(&g_cfg);
    fflush(stdout);

    /* Confirm this OTA image is healthy (cancels the pending rollback). */
    ota_confirm_boot();

    /* On-board 240x240 status screen (S3-EYE); no-op if the panel is absent. */
    display_init();

    /* Always-on field-service console over USB/UART. */
    provisioning_console_start(&g_cfg);

    /* Forced-setup button held LOW at boot, or missing config -> setup portal. */
    bool force_setup = false;
#if APP_SETUP_BUTTON_GPIO >= 0
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << APP_SETUP_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);
    vTaskDelay(pdMS_TO_TICKS(50));
    if (gpio_get_level(APP_SETUP_BUTTON_GPIO) == 0) {
        force_setup = true;
        printf("[SETUP] setup button held; forcing provisioning portal\n");
    }
#endif

    if (force_setup || !app_cfg_provisioned(&g_cfg)) {
        printf("[SETUP] launching SoftAP setup portal (device not provisioned)\n");
        fflush(stdout);
        status_led_set(STATUS_LED_SETUP);
        /* The portal blocks this task until the user submits config; stop
         * watching it with the task WDT so we don't spam warnings meanwhile. */
        esp_task_wdt_delete(NULL);
        provisioning_portal_run(&g_cfg, 0 /* wait forever */);
        printf("[SETUP] configuration saved; rebooting\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }

    /* Sensing + transport pipeline. Inference is owned by source_manager
     * (runs once per captured frame); app_main consumes its result. */
    source_manager_init(&g_source_manager);

    dl_qads_config_t scheduler_config = {
        .ai_stream_id = APP_AI_STREAM_ID,
        .sensor_stream_id = APP_SENSOR_STREAM_ID,
        .image_stream_id = APP_IMAGE_STREAM_ID,
        .image_datagram_budget = APP_IMAGE_DATAGRAM_BUDGET,
        .control_period_ms = APP_CONTROL_PERIOD_MS,
        .telemetry_deadline_ms = APP_DEADLINE_CLASS1_MS,
        .image_deadline_ms = APP_DEADLINE_IMAGE_MS,
        .policy_mode = (qads_policy_mode_t)APP_QADS_POLICY_MODE,
    };
    dl_qads_init(&g_scheduler, &scheduler_config);

    presence_config_t pcfg = {
        .enter_confidence = g_cfg.presence_enter_conf,
        .exit_confidence = g_cfg.presence_exit_conf,
        .enter_frames = g_cfg.presence_enter_frames,
        .exit_timeout_ms = g_cfg.presence_exit_timeout_ms,
    };
    presence_init(&g_presence, &pcfg);

    /* MQTT client id = provisioned device id. */
    strlcpy(g_client_id, g_cfg.device_id, sizeof(g_client_id));
    g_mqttConnectInfo.pClientIdentifier = g_client_id;
    g_mqttConnectInfo.clientIdentifierLength = (uint16_t)strlen(g_client_id);

    /* Per-device topics for auto-registration / discovery. */
    snprintf(g_topic_cmd, sizeof(g_topic_cmd), "aiot/%s%s", g_cfg.device_id, APP_TOPIC_CMD_SUFFIX);
    snprintf(g_topic_status, sizeof(g_topic_status), "aiot/%s%s", g_cfg.device_id, APP_TOPIC_STATUS_SUFFIX);
    snprintf(g_topic_state, sizeof(g_topic_state), "aiot/%s%s", g_cfg.device_id, APP_TOPIC_STATE_SUFFIX);
    snprintf(g_topic_register, sizeof(g_topic_register), "aiot/%s%s", g_cfg.device_id, APP_TOPIC_REGISTER_SUFFIX);
    snprintf(g_topic_ha, sizeof(g_topic_ha), "%s/binary_sensor/%s/config",
             APP_HA_DISCOVERY_PREFIX, g_cfg.device_id);

    /* Arm the Last Will: a retained "offline" the broker publishes if we drop. */
    g_will.qos = MQTTQoS0;
    g_will.retain = true;
    g_will.pTopicName = g_topic_status;
    g_will.topicNameLength = (uint16_t)strlen(g_topic_status);
    g_will.pPayload = APP_STATUS_OFFLINE;
    g_will.payloadLength = strlen(APP_STATUS_OFFLINE);
    g_will_ptr = &g_will;

    status_led_set(STATUS_LED_CONNECTING);
    bool wifi_ready = wifi_link_connect(g_cfg.wifi_ssid, g_cfg.wifi_password, APP_WIFI_TIMEOUT_MS);
    printf("[APP]Wi-Fi connect result: %d\n", (int)wifi_ready);
    fflush(stdout);
    if (wifi_ready) {
        app_quic_mqtt_bringup(g_cfg.server_host, g_cfg.server_port);
    } else {
        status_led_set(STATUS_LED_ERROR);   /* wrong password / AP not found */
    }

    uint32_t backoff_ms = APP_SESSION_RETRY_MIN_MS;
    int64_t next_retry_us = 0;
    bool subscribed = false;
    bool registered = false;
    int64_t last_hb_us = 0;

    static uint8_t reassembly_buf[8192];
    static uint16_t reassembly_len = 0;
    static bool reassembly_active = false;
    static aiot_ai_result_t ai_result = {0};

    printf("[APP-MAIN] Entering product loop\n");
    fflush(stdout);

    while (1) {
        esp_task_wdt_reset();
        int64_t now_us = esp_timer_get_time();

        {
            display_status_t ds = {
                .device_id = g_cfg.device_id,
                .server_host = g_cfg.server_host,
                .server_port = g_cfg.server_port,
                .session_up = g_mqtt_connected,
                .presence_state = (int)g_presence.state,
                .score = g_presence.last_score,
                .enter_threshold = g_presence.config.enter_confidence,
                .heartbeats = g_hb_count,
                .frames_total = g_presence.frames_total,
            };
            display_update(&ds);
        }

        /* OTA request delivered via the command topic. */
        if (g_ota_pending) {
            g_ota_pending = false;
            printf("[OTA] starting update from %s\n", g_ota_url);
            fflush(stdout);
            esp_err_t oerr = ota_run_from_url(g_ota_url);  /* reboots on success */
            ESP_LOGE(TAG, "OTA failed: %s (continuing on current image)",
                     esp_err_to_name(oerr));
        }

        /* Drive / supervise the session. */
        if (quic_client_is_connected()) {
            if (quic_client_process() != 0) {
                ESP_LOGW(TAG, "QUIC processing failed; tearing down session");
                quic_client_cleanup();
            }
        }

        if (!quic_client_is_connected()) {
            g_mqtt_connected = false;
            g_mqtt_transport_ready = false;
            subscribed = false;
            registered = false;
            status_led_set(STATUS_LED_CONNECTING);
            if (now_us >= next_retry_us) {
                ESP_LOGW(TAG, "re-establishing session (backoff=%lums)",
                         (unsigned long)backoff_ms);
                esp_wifi_connect();  /* nudge Wi-Fi in case it also dropped */
                if (app_quic_mqtt_bringup(g_cfg.server_host, g_cfg.server_port)) {
                    backoff_ms = APP_SESSION_RETRY_MIN_MS;
                    next_retry_us = 0;
                } else {
                    next_retry_us = now_us + (int64_t)backoff_ms * 1000;
                    backoff_ms = (backoff_ms * 2U > APP_SESSION_RETRY_MAX_MS)
                                     ? APP_SESSION_RETRY_MAX_MS : backoff_ms * 2U;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(APP_CONTROL_PERIOD_MS));
            continue;
        }

        /* MQTT session establishment (control channel is stream-only). */
        if (g_mqtt_connected) {
            MQTT_ProcessLoop(&g_mqttContext);
        } else if (g_mqtt_transport_ready) {
            if (!g_mqtt_connect_sent) {
                app_mqtt_send_connect();
            } else {
                app_mqtt_poll_connect();
            }
            vTaskDelay(pdMS_TO_TICKS(APP_CONTROL_PERIOD_MS));
            continue;
        } else {
            vTaskDelay(pdMS_TO_TICKS(APP_CONTROL_PERIOD_MS));
            continue;
        }

        /* Session fully up: subscribe to the command topic once. */
        if (!subscribed) {
            MQTTSubscribeInfo_t sub = {
                .qos = MQTTQoS0,
                .pTopicFilter = g_topic_cmd,
                .topicFilterLength = (uint16_t)strlen(g_topic_cmd),
            };
            if (MQTT_Subscribe(&g_mqttContext, &sub, 1,
                               MQTT_GetPacketId(&g_mqttContext)) == MQTTSuccess) {
                subscribed = true;
                printf("[APP]subscribed to cmd topic %s\n", g_topic_cmd);
                fflush(stdout);
            }
        }

        /* Auto-registration + discovery announce, once the session is up. */
        if (subscribed && !registered) {
            app_publish_registration();
            registered = true;
            status_led_set(g_presence.state == PRESENCE_STATE_PRESENT
                               ? STATUS_LED_PRESENT : STATUS_LED_ONLINE);
        }

        /* Heartbeat + exit-timeout re-evaluation. */
        if (last_hb_us == 0 ||
            (now_us - last_hb_us) >= (int64_t)g_cfg.heartbeat_ms * 1000) {
            last_hb_us = now_us;
            app_send_heartbeat(now_us);
        }

        /* Sensing: only image frames feed the detector. */
        data_packet_t packet;
        if (!source_manager_next_packet(&g_source_manager, &packet)) {
            vTaskDelay(pdMS_TO_TICKS(APP_CONTROL_PERIOD_MS));
            continue;
        }
        if (packet.type != DATA_TYPE_IMAGE) {
            vTaskDelay(pdMS_TO_TICKS(APP_CONTROL_PERIOD_MS));
            continue;
        }

        const uint8_t *payload = packet.payload;
        uint16_t payload_len = packet.payload_len;
        jpeg_acc_t st = app_jpeg_reassemble(payload, payload_len,
            reassembly_buf, sizeof(reassembly_buf),
            &reassembly_len, &reassembly_active);
        if (st != JPEG_ACC_COMPLETE) {
            vTaskDelay(pdMS_TO_TICKS(APP_CONTROL_PERIOD_MS));
            continue;
        }

        /* Inference already ran on this frame inside source_manager; consume
         * that result instead of running the model a second time. */
        bool have_ai = source_manager_get_last_ai_result(&g_source_manager, &ai_result, NULL);
        if (have_ai) {
            int64_t t = esp_timer_get_time();
            presence_event_t ev;
            if (presence_update(&g_presence, &ai_result, t, &ev)) {
                app_publish_presence_event(&ev);
                /* Update the retained per-device state (discovery/backends). */
                const char *st = presence_state_name(ev.state);
                app_publish_retained(g_topic_state, st, strlen(st));
                status_led_set(ev.state == PRESENCE_STATE_PRESENT
                                   ? STATUS_LED_PRESENT : STATUS_LED_ONLINE);
                if (ev.state == PRESENCE_STATE_PRESENT && g_cfg.snapshot_on_enter) {
                    app_send_snapshot(reassembly_buf, reassembly_len, packet.priority);
                }
            }
        }

        /* Periodic frame streaming for training-data collection. */
        if (g_cfg.snapshot_periodic_ms != 0U) {
            static int64_t last_periodic_us;
            int64_t t = esp_timer_get_time();
            if (t - last_periodic_us >= (int64_t)g_cfg.snapshot_periodic_ms * 1000) {
                last_periodic_us = t;
                app_send_snapshot(reassembly_buf, reassembly_len, packet.priority);
            }
        }
#if APP_STREAM_ENABLE
        uint32_t frame_ts = app_stream_frame(reassembly_buf, reassembly_len, packet.priority);
        if (frame_ts != 0U && have_ai) {
            app_send_ai_result(&ai_result, frame_ts, packet.priority);
        }
#endif
        esp_task_wdt_reset();
        reassembly_active = false;
        reassembly_len = 0;
        vTaskDelay(pdMS_TO_TICKS(APP_CONTROL_PERIOD_MS));
    }
}
#endif /* APP_PRODUCT_MODE */

#if APP_TRANSPORT_BACKEND == APP_TRANSPORT_BACKEND_TCP
extern void app_tcp_baseline_main(void);   /* src/main/app_tcp_baseline.c */
#endif

#if AIOT_MOBILITY_RESEARCH
#include "app_mobility.inc"
#endif

#if AIOT_MOQT_EXPERIMENT
#include "app_moq.inc"
#endif

void app_main(void)
{
#ifdef AIOT_NETLOG_HOST
    if (!netlog_start(AIOT_NETLOG_HOST, AIOT_NETLOG_PORT)) printf("[NETLOG] start failed\n");
#endif
#if AIOT_MOQT_EXPERIMENT
    app_moq_main();
    return;
#endif
#if AIOT_MOBILITY_RESEARCH
    app_mobility_main();
    return;
#endif
#if APP_TRANSPORT_BACKEND == APP_TRANSPORT_BACKEND_TCP
    app_tcp_baseline_main();   /* non-QUIC baseline */
#elif APP_PRODUCT_MODE
    app_product_main();
#else
    app_experiment_main();
#endif
}
