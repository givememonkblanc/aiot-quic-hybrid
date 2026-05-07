#include "moqt_udp_client.h"
#include "moqt_quic.h"
#include "esp_log.h"
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

static const char *TAG = "MOQT-UDP";

int moqt_udp_init(moqt_udp_client_t *client, const char *host, uint16_t port) {
    memset(client, 0, sizeof(*client));
    strncpy(client->host, host, MOQT_UDP_HOST_LEN - 1);
    client->port = port;
    client->sockfd = -1;
    client->state = MOQT_UDP_DISCONNECTED;
    ESP_LOGI(TAG, "MOQT UDP client init: %s:%d", host, port);
    return 0;
}

int moqt_udp_connect(moqt_udp_client_t *client) {
    client->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (client->sockfd < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        return -1;
    }
    
    struct sockaddr_in dest_addr;
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(client->port);
    inet_pton(AF_INET, client->host, &dest_addr.sin_addr);
    
    if (connect(client->sockfd, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to connect");
        close(client->sockfd);
        client->sockfd = -1;
        return -1;
    }
    
    client->state = MOQT_UDP_CONNECTED;
    ESP_LOGI(TAG, "UDP connected to %s:%d", client->host, client->port);
    return 0;
}

int moqt_udp_disconnect(moqt_udp_client_t *client) {
    if (client->sockfd >= 0) {
        close(client->sockfd);
        client->sockfd = -1;
    }
    client->state = MOQT_UDP_DISCONNECTED;
    ESP_LOGI(TAG, "UDP disconnected");
    return 0;
}

int moqt_udp_send(moqt_udp_client_t *client, const uint8_t *data, size_t len) {
    if (client->sockfd < 0 || client->state != MOQT_UDP_CONNECTED) {
        return -1;
    }
    
    ssize_t sent = send(client->sockfd, data, len, 0);
    if (sent < 0) {
        ESP_LOGE(TAG, "Send failed");
        return -1;
    }
    
    ESP_LOGD(TAG, "Sent %d bytes via UDP", (int)sent);
    return (int)sent;
}

moqt_udp_state_t moqt_udp_get_state(moqt_udp_client_t *client) {
    return client->state;
}

bool moqt_udp_is_connected(moqt_udp_client_t *client) {
    return client->state == MOQT_UDP_CONNECTED && client->sockfd >= 0;
}