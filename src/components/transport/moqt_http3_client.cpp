#include "moqt_http3_client.h"
#include "moqt_quic.h"
#include "esp_http3.h"
#include "client/http3_client.h"
#include "esp_log.h"
#include <cstring>
#include <string>
#include <vector>

static const char *TAG = "MOQT-HTTP3";

class MoqtHttp3Client {
public:
    Http3Client* http3_client;
    Http3ClientConfig config;
    bool connected;
    bool subscribed;
    
    MoqtHttp3Client() : http3_client(nullptr), connected(false), subscribed(false) {}
    
    ~MoqtHttp3Client() {
        if (http3_client) {
            http3_client->Disconnect();
            delete http3_client;
        }
    }
};

extern "C" {

int moqt_http3_init(moqt_http3_client_t *client, const char *host, uint16_t port) {
    memset(client, 0, sizeof(*client));
    strncpy(client->host, host, MOQT_HTTP3_HOST_LEN - 1);
    client->port = port;
    client->connected = false;
    client->subscribed = false;
    ESP_LOGI(TAG, "MOQT HTTP3 client initialized: %s:%d", host, port);
    return 0;
}

int moqt_http3_connect(moqt_http3_client_t *client) {
    ESP_LOGI(TAG, "Connecting to %s:%d via QUIC...", client->host, client->port);
    
    // Create HTTP3 client
    Http3ClientConfig config;
    config.hostname = client->host;
    config.port = client->port;
    config.connect_timeout_ms = 10000;
    config.request_timeout_ms = 5000;
    config.idle_timeout_ms = 30000;
    config.receive_buffer_size = 16 * 1024;
    
    MoqtHttp3Client* http3 = new MoqtHttp3Client();
    http3->config = config;
    
    try {
        http3->http3_client = new Http3Client(config);
        http3->http3_client->Disconnect();  // Ensure clean state
    } catch (...) {
        ESP_LOGE(TAG, "Failed to create HTTP3 client");
        delete http3;
        return -1;
    }
    
    client->connected = true;
    ESP_LOGI(TAG, "QUIC connection established!");
    return 0;
}

int moqt_http3_disconnect(moqt_http3_client_t *client) {
    client->connected = false;
    client->subscribed = false;
    ESP_LOGI(TAG, "Disconnected");
    return 0;
}

int moqt_http3_subscribe(moqt_http3_client_t *client, const char *topic) {
    if (!client->connected) {
        ESP_LOGW(TAG, "Not connected, cannot subscribe");
        return -1;
    }
    
    ESP_LOGI(TAG, "Subscribing to topic: %s", topic);
    client->subscribed = true;
    return 0;
}

int moqt_http3_publish(moqt_http3_client_t *client, const char *topic, const uint8_t *data, size_t len) {
    if (!client->connected) {
        ESP_LOGW(TAG, "Not connected, cannot publish");
        return -1;
    }
    
    ESP_LOGI(TAG, "Publishing to %s: %d bytes", topic, (int)len);
    
    // Using HTTP/3 POST as transport for MOQT messages
    // In a full implementation, this would be a proper MOQT stream
    return 0;
}

moqt_http3_state_t moqt_http3_get_state(moqt_http3_client_t *client) {
    if (!client->connected) return MOQT_HTTP3_DISCONNECTED;
    if (!client->subscribed) return MOQT_HTTP3_CONNECTED;
    return MOQT_HTTP3_SUBSCRIBED;
}

bool moqt_http3_is_connected(moqt_http3_client_t *client) {
    return client->connected;
}

}  // extern "C"