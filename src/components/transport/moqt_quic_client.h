#ifndef MOQT_QUIC_CLIENT_H
#define MOQT_QUIC_CLIENT_H

#include "moqt_quic.h"
#include <stdint.h>
#include <stdbool.h>

#define MOQT_CLIENT_MAX_TOPICS 8

typedef enum {
    MOQT_STATE_DISCONNECTED,
    MOQT_STATE_CONNECTING,
    MOQT_STATE_CONNECTED,
    MOQT_STATE_SUBSCRIBED,
    MOQT_STATE_ERROR
} moqt_client_state_t;

typedef struct {
    void *http3_client;
    moqt_client_state_t state;
    uint64_t next_request_id;
    char topic[MOQT_CLIENT_MAX_TOPICS][MOQT_MAX_TOPIC_LEN];
    uint8_t topic_count;
    void (*publish_callback)(const char *topic, const uint8_t *data, size_t len);
    
    char host[64];
    uint16_t port;
} moqt_client_t;

int moqt_client_init(moqt_client_t *client, const char *host, uint16_t port);
int moqt_client_connect(moqt_client_t *client);
int moqt_client_disconnect(moqt_client_t *client);

int moqt_client_subscribe(moqt_client_t *client, const char *topic, uint8_t qos);
int moqt_client_publish(moqt_client_t *client, const char *topic, const uint8_t *data, size_t len);

void moqt_client_set_publish_callback(moqt_client_t *client, 
    void (*callback)(const char *topic, const uint8_t *data, size_t len));

int moqt_client_process(moqt_client_t *client);

const char* moqt_client_state_str(moqt_client_state_t state);

#endif