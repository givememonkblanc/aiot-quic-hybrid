#include "moqt_ngtcp2_client.h"
#include "moqt_quic.h"
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <esp_log.h>
#include <esp_err.h>

static const char *TAG = "MOQT-NGTCP2";

/* ngtcp2 memory limits for ESP32 */
#define MOQT_NGTCP2_MAX_CWIN (32 * 1024)    /* 32KB congestion window */
#define MOQT_NGTCP2_MAX_DATA (64 * 1024)     /* 64KB max data per connection */

int moqt_ngtcp2_init(moqt_ngtcp2_client_t *client, const char *host, uint16_t port)
{
    memset(client, 0, sizeof(*client));
    
    strncpy(client->host, host, MOQT_NGTCP2_HOST_LEN - 1);
    client->port = port;
    client->state = MOQT_NGTCP2_DISCONNECTED;
    client->sockfd = -1;
    
    client->max_cwin = MOQT_NGTCP2_MAX_CWIN;
    client->max_data = MOQT_NGTCP2_MAX_DATA;
    
    ESP_LOGI(TAG, "Client init: %s:%d (memory limits: cwin=%u, max_data=%u)",
            host, port, client->max_cwin, (unsigned)client->max_data);
    
    return 0;
}

static int set_socket_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int moqt_ngtcp2_connect(moqt_ngtcp2_client_t *client)
{
    if (client->state != MOQT_NGTCP2_DISCONNECTED) {
        ESP_LOGW(TAG, "Already connected");
        return -1;
    }
    
    client->state = MOQT_NGTCP2_HANDSHAKING;
    
    client->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (client->sockfd < 0) {
        ESP_LOGE(TAG, "socket failed");
        client->state = MOQT_NGTCP2_ERROR;
        return -1;
    }
    
    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(client->port);
    
    if (inet_pton(AF_INET, client->host, &server_addr.sin_addr) <= 0) {
        struct hostent *he = gethostbyname(client->host);
        if (he == NULL) {
            ESP_LOGE(TAG, "DNS lookup failed for %s", client->host);
            close(client->sockfd);
            client->sockfd = -1;
            client->state = MOQT_NGTCP2_ERROR;
            return -1;
        }
        memcpy(&server_addr.sin_addr, he->h_addr_list[0], he->h_length);
    }
    
    if (connect(client->sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "connect failed");
        close(client->sockfd);
        client->sockfd = -1;
        client->state = MOQT_NGTCP2_ERROR;
        return -1;
    }
    
    client->dcid_len = 16;
    for (int i = 0; i < 16; i++) {
        client->dcid[i] = rand() & 0xFF;
    }
    client->scid_len = 16;
    for (int i = 0; i < 16; i++) {
        client->scid[i] = rand() & 0xFF;
    }
    
    ESP_LOGI(TAG, "UDP connected, starting QUIC handshake...");
    
    client->state = MOQT_NGTCP2_CONNECTED;
    
    return 0;
}

int moqt_ngtcp2_disconnect(moqt_ngtcp2_client_t *client)
{
    if (client->sockfd >= 0) {
        close(client->sockfd);
        client->sockfd = -1;
    }
    
    client->state = MOQT_NGTCP2_DISCONNECTED;
    ESP_LOGI(TAG, "Disconnected");
    
    return 0;
}

int moqt_ngtcp2_send(moqt_ngtcp2_client_t *client, const uint8_t *data, size_t len)
{
    if (client->state != MOQT_NGTCP2_CONNECTED) {
        return -1;
    }
    
    if (client->sockfd < 0) {
        return -1;
    }
    
    ssize_t sent = send(client->sockfd, data, len, 0);
    if (sent < 0) {
        ESP_LOGE(TAG, "send failed: %d", (int)len);
        return -1;
    }
    
    ESP_LOGD(TAG, "Sent %d bytes", (int)sent);
    return 0;
}

int moqt_ngtcp2_process(moqt_ngtcp2_client_t *client)
{
    if (client->state == MOQT_NGTCP2_DISCONNECTED) {
        return 0;
    }
    
    if (client->sockfd < 0) {
        return -1;
    }
    
    uint8_t buf[2048];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    
    ssize_t received = recvfrom(client->sockfd, buf, sizeof(buf), 0,
            (struct sockaddr *)&from, &fromlen);
    
    if (received > 0) {
        ESP_LOGD(TAG, "Received %d bytes", (int)received);
        return 1;
    } else if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        ESP_LOGW(TAG, "recv error: %d", (int)errno);
        return -1;
    }
    
    return 0;
}

moqt_ngtcp2_state_t moqt_ngtcp2_get_state(moqt_ngtcp2_client_t *client)
{
    return client->state;
}

bool moqt_ngtcp2_is_connected(moqt_ngtcp2_client_t *client)
{
    return client->state == MOQT_NGTCP2_CONNECTED;
}