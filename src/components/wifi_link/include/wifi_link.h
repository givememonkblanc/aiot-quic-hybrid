#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool ready;
    uint32_t ip_generation;
    uint32_t disconnects;
    uint32_t ipv4; /* network byte order */
    int64_t disconnected_us;
    int64_t got_ip_us;
    int disconnect_reason;
} wifi_link_status_t;

bool wifi_link_connect(const char *ssid, const char *password, unsigned int timeout_ms);
void wifi_link_get_status(wifi_link_status_t *status);
/* Switch credentials at runtime without rebooting or destroying QUIC. */
esp_err_t wifi_link_handoff(const char *ssid, const char *password);
