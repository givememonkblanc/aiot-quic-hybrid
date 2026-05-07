#ifndef MOQT_NGTCP2_CLIENT_H
#define MOQT_NGTCP2_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define MOQT_NGTCP2_HOST_LEN 64

/* Forward declare ngtcp2 types */
typedef struct st_ngtcp2_conn ngtcp2_conn;
typedef struct st_ngtcp2_path ngtcp2_path;

typedef enum {
    MOQT_NGTCP2_DISCONNECTED,
    MOQT_NGTCP2_HANDSHAKING,
    MOQT_NGTCP2_CONNECTED,
    MOQT_NGTCP2_CLOSING,
    MOQT_NGTCP2_ERROR
} moqt_ngtcp2_state_t;

typedef struct {
    char host[MOQT_NGTCP2_HOST_LEN];
    uint16_t port;
    int sockfd;
    moqt_ngtcp2_state_t state;
    
    /* ngtcp2 connection */
    ngtcp2_conn *conn;
    
    /* Connection ID */
    uint8_t dcid[16];
    size_t dcid_len;
    uint8_t scid[16];
    size_t scid_len;
    
    /* TLS context */
    void *ssl;
    void *ctx;
    
    /* Memory limits for ESP32 */
    uint32_t max_cwin;
    uint64_t max_data;
} moqt_ngtcp2_client_t;

/* Initialize client */
int moqt_ngtcp2_init(moqt_ngtcp2_client_t *client, const char *host, uint16_t port);

/* Connect with TLS handshake */
int moqt_ngtcp2_connect(moqt_ngtcp2_client_t *client);

/* Disconnect */
int moqt_ngtcp2_disconnect(moqt_ngtcp2_client_t *client);

/* Send data */
int moqt_ngtcp2_send(moqt_ngtcp2_client_t *client, const uint8_t *data, size_t len);

/* Process incoming packets */
int moqt_ngtcp2_process(moqt_ngtcp2_client_t *client);

/* Get state */
moqt_ngtcp2_state_t moqt_ngtcp2_get_state(moqt_ngtcp2_client_t *client);
bool moqt_ngtcp2_is_connected(moqt_ngtcp2_client_t *client);

#endif