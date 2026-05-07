#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include "mqtt_client.h"

#define MQTT_CLIENT_MAX_TOPICS 8

typedef enum {
    MQTT_STATE_DISCONNECTED,
    MQTT_STATE_CONNECTING,
    MQTT_STATE_CONNECTED,
    MQTT_STATE_SUBSCRIBED,
    MQTT_STATE_ERROR
} mqtt_client_state_t;

typedef struct {
    esp_mqtt_client_handle_t mqtt_client;
    mqtt_client_state_t state;
    uint64_t next_request_id;
    char topic[MQTT_CLIENT_MAX_TOPICS][128];
    uint8_t topic_count;
    char host[64];
    uint16_t port;
    char client_id[32];
} mqtt_client_t;

int mqtt_client_init(mqtt_client_t *client, const char *host, uint16_t port, const char *client_id);
int mqtt_client_connect(mqtt_client_t *client);
int mqtt_client_disconnect(mqtt_client_t *client);

int mqtt_client_subscribe(mqtt_client_t *client, const char *topic, int qos);
int mqtt_client_publish(mqtt_client_t *client, const char *topic, const uint8_t *data, size_t len, int qos);

mqqtt_client_state_t mqtt_client_get_state(mqtt_client_t *client);

#endif