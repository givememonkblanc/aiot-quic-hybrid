#ifndef MOQT_HTTP3_CLIENT_H
#define MOQT_HTTP3_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOQT_HTTP3_MAX_TOPICS 8
#define MOQT_HTTP3_HOST_LEN 64

typedef enum {
    MOQT_HTTP3_DISCONNECTED,
    MOQT_HTTP3_CONNECTING,
    MOQT_HTTP3_CONNECTED,
    MOQT_HTTP3_SUBSCRIBED,
    MOQT_HTTP3_ERROR
} moqt_http3_state_t;

typedef struct {
    char host[MOQT_HTTP3_HOST_LEN];
    uint16_t port;
    bool connected;
    bool subscribed;
} moqt_http3_client_t;

int moqt_http3_init(moqt_http3_client_t *client, const char *host, uint16_t port);
int moqt_http3_connect(moqt_http3_client_t *client);
int moqt_http3_disconnect(moqt_http3_client_t *client);

int moqt_http3_subscribe(moqt_http3_client_t *client, const char *topic);
int moqt_http3_publish(moqt_http3_client_t *client, const char *topic, const uint8_t *data, size_t len);

moqt_http3_state_t moqt_http3_get_state(moqt_http3_client_t *client);
bool moqt_http3_is_connected(moqt_http3_client_t *client);

#ifdef __cplusplus
}
#endif

#endif