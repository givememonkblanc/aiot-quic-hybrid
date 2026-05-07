#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "core_mqtt.h"
#include "mqtt_quic_transport.h"
#include "ngtcp2_sample.h"

static const char *TAG = "QUIC-DEMO";
static MQTTContext_t mqttContext;
static NetworkContext_t networkContext;
static uint8_t networkBuffer[1024];

static void mqtt_event_callback(MQTTContext_t *pMqttContext, MQTTPacketInfo_t *pPacketInfo, MQTTDeserializedInfo_t *pDeserializedInfo) {
    ESP_LOGI(TAG, "MQTT event received");
}

void quic_demo_task(void *pvParameters) {
    ServerInfo_t serverInfo = {
        .pHostName = "127.0.0.1",
        .port = 14567,
        .pAlpn = "\x4mqtt"
    };

    MQTTQUICConfig_t quicConfig = {
        .timeoutMs = 5000,
        .nonBlocking = true
    };

    quic_client_config_t config = {
        .hostname = serverInfo.pHostName,
        .port = "14567",
        .alpn = "\x4mqtt"
    };
    
    ESP_LOGI(TAG, "Init QUIC Client...");
    quic_client_init_with_config(&config);

    // Give it time to connect
    while (!quic_client_is_connected()) {
        quic_client_process();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "QUIC connected!");

    mqtt_quic_transport_init(&networkContext, &serverInfo, &quicConfig);

    MQTTFixedBuffer_t fixedBuffer = {
        .pBuffer = networkBuffer,
        .size = sizeof(networkBuffer)
    };

    MQTT_Init(&mqttContext, &xTransportInterface, mqtt_get_time_ms, mqtt_event_callback, &fixedBuffer);

    MQTTConnectInfo_t connectInfo = {
        .cleanSession = true,
        .pClientIdentifier = "esp32-quic",
        .clientIdentifierLength = strlen("esp32-quic"),
        .keepAliveSeconds = 60
    };

    bool sessionPresent = false;
    MQTT_Connect(&mqttContext, &connectInfo, NULL, 5000, &sessionPresent);

    ESP_LOGI(TAG, "MQTT connected over QUIC!");

    while (1) {
        quic_client_process();
        MQTT_ProcessLoop(&mqttContext);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
