#ifndef MOQT_UDP_CLIENT_H
#define MOQT_UDP_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define MOQT_UDP_HOST_LEN 64

typedef enum {
    MOQT_UDP_DISCONNECTED,
    MOQT_UDP_CONNECTING,
    MOQT_UDP_CONNECTED,
    MOQT_UDP_ERROR
} moqt_udp_state_t;

typedef struct {
    char host[MOQT_UDP_HOST_LEN];
    uint16_t port;
    int sockfd;
    moqt_udp_state_t state;
} moqt_udp_client_t;

int moqt_udp_init(moqt_udp_client_t *client, const char *host, uint16_t port);
int moqt_udp_connect(moqt_udp_client_t *client);
int moqt_udp_disconnect(moqt_udp_client_t *client);
int moqt_udp_send(moqt_udp_client_t *client, const uint8_t *data, size_t len);

moqt_udp_state_t moqt_udp_get_state(moqt_udp_client_t *client);
bool moqt_udp_is_connected(moqt_udp_client_t *client);

#endif