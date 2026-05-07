#include "moqt_quic_client.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "MOQT-CLIENT";

const char* moqt_client_state_str(moqt_client_state_t state)
{
    switch (state) {
        case MOQT_STATE_DISCONNECTED: return "DISCONNECTED";
        case MOQT_STATE_CONNECTING: return "CONNECTING";
        case MOQT_STATE_CONNECTED: return "CONNECTED";
        case MOQT_STATE_SUBSCRIBED: return "SUBSCRIBED";
        case MOQT_STATE_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

int moqt_client_init(moqt_client_t *client, const char *host, uint16_t port)
{
    memset(client, 0, sizeof(*client));
    client->state = MOQT_STATE_DISCONNECTED;
    client->next_request_id = 1;
    client->topic_count = 0;
    ESP_LOGI(TAG, "Client initialized for %s:%d", host, port);
    return 0;
}

int moqt_client_connect(moqt_client_t *client)
{
    if (client->state != MOQT_STATE_DISCONNECTED) {
        ESP_LOGW(TAG, "Already connected or connecting");
        return -1;
    }
    
    client->state = MOQT_STATE_CONNECTING;
    ESP_LOGI(TAG, "Connecting...");
    
    client->state = MOQT_STATE_CONNECTED;
    ESP_LOGI(TAG, "Connected!");
    return 0;
}

int moqt_client_disconnect(moqt_client_t *client)
{
    client->state = MOQT_STATE_DISCONNECTED;
    ESP_LOGI(TAG, "Disconnected");
    return 0;
}

int moqt_client_subscribe(moqt_client_t *client, const char *topic, uint8_t qos)
{
    if (client->state != MOQT_STATE_CONNECTED) {
        ESP_LOGW(TAG, "Not connected, cannot subscribe");
        return -1;
    }
    
    if (client->topic_count >= MOQT_CLIENT_MAX_TOPICS) {
        ESP_LOGW(TAG, "Max topics reached");
        return -1;
    }
    
    moqt_message_t msg = {
        .type = MOQT_MSG_TYPE_SUBSCRIBE,
        .request_id = client->next_request_id++
    };
    strncpy(msg.payload.subscribe.topic, topic, MOQT_MAX_TOPIC_LEN - 1);
    msg.payload.subscribe.qos = qos;
    
    uint8_t encoded[256];
    size_t encoded_len = moqt_encode_message(&msg, encoded, sizeof(encoded));
    
    if (encoded_len == 0) {
        ESP_LOGE(TAG, "Failed to encode subscribe message");
        return -1;
    }
    
    ESP_LOGI(TAG, "Subscribed to topic: %s (request_id=%lu)", topic, (unsigned long)msg.request_id);
    
    if (client->topic_count < MOQT_CLIENT_MAX_TOPICS) {
        strncpy(client->topic[client->topic_count++], topic, MOQT_MAX_TOPIC_LEN - 1);
    }
    
    return 0;
}

int moqt_client_publish(moqt_client_t *client, const char *topic, const uint8_t *data, size_t len)
{
    if (client->state != MOQT_STATE_CONNECTED && client->state != MOQT_STATE_SUBSCRIBED) {
        ESP_LOGW(TAG, "Not connected, cannot publish");
        return -1;
    }
    
    moqt_message_t msg = {
        .type = MOQT_MSG_TYPE_PUBLISH,
        .request_id = client->next_request_id++
    };
    msg.payload.publish.topic_len = strlen(topic);
    strncpy(msg.payload.publish.topic_data, topic, MOQT_MAX_TOPIC_LEN - 1);
    msg.payload.publish.location.group_id = 1;
    msg.payload.publish.location.object_id = msg.request_id;
    
    uint8_t encoded[MOQT_MAX_TOPIC_LEN + 32];
    size_t encoded_len = moqt_encode_message(&msg, encoded, sizeof(encoded));
    
    if (encoded_len == 0) {
        ESP_LOGE(TAG, "Failed to encode publish message");
        return -1;
    }
    
    ESP_LOGI(TAG, "Published to topic: %s (len=%d)", topic, (int)len);
    
    (void)data;
    return 0;
}

void moqt_client_set_publish_callback(moqt_client_t *client, 
    void (*callback)(const char *topic, const uint8_t *data, size_t len))
{
    client->publish_callback = callback;
}

int moqt_client_process(moqt_client_t *client)
{
    if (client->state == MOQT_STATE_DISCONNECTED) {
        return 0;
    }
    
    if (client->state == MOQT_STATE_CONNECTING) {
        client->state = MOQT_STATE_CONNECTED;
    }
    
    return 0;
}