#include "include/wifi_link.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

#define WIFI_LINK_CONNECTED_BIT BIT0
#define WIFI_LINK_FAILED_BIT BIT1
#define WIFI_LINK_MAX_RETRIES 5

static const char *TAG = "wifi_link";
static EventGroupHandle_t s_wifi_events;
static int s_retry_count;

static bool wifi_link_has_placeholder(const char *value)
{
    return value == NULL || value[0] == '\0' || strcmp(value, "REPLACE_ME") == 0;
}

static void wifi_link_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        printf("[wifi_evt] WIFI_EVENT_STA_START\n");
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        printf("[wifi_evt] WIFI_EVENT_STA_DISCONNECTED\n");
        if (s_retry_count < WIFI_LINK_MAX_RETRIES) {
            s_retry_count++;
            esp_wifi_connect();
            printf("[wifi_evt] retrying Wi-Fi connection (%d/%d)\n", s_retry_count, WIFI_LINK_MAX_RETRIES);
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_LINK_FAILED_BIT);
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_retry_count = 0;
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        printf("[wifi_evt] IP_EVENT_STA_GOT_IP: IP=" IPSTR "\n", IP2STR(&event->ip_info.ip));
        printf("[wifi_evt] Gateway: " IPSTR "\n", IP2STR(&event->ip_info.gw));
        xEventGroupSetBits(s_wifi_events, WIFI_LINK_CONNECTED_BIT);
    }
}

bool wifi_link_connect(const char *ssid, const char *password, unsigned int timeout_ms)
{
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t wifi_config = {0};
    esp_event_handler_instance_t wifi_any_id;
    esp_event_handler_instance_t ip_got_ip;

    printf("[wifi_link] Attempting connect to SSID=%s timeout=%u\n", ssid, timeout_ms);
    
    if (wifi_link_has_placeholder(ssid) || wifi_link_has_placeholder(password)) {
        ESP_LOGW(TAG, "Wi-Fi credentials are placeholders; skipping station connect");
        return false;
    }

    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == NULL) {
        printf("[wifi_link] Failed to create event group\n");
        return false;
    }

    nvs_flash_init();
    printf("[wifi_link] Wi-Fi init complete, starting AP scan\n");
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    esp_wifi_init(&init_config);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_link_event_handler, NULL, &wifi_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_link_event_handler, NULL, &ip_got_ip);

    memcpy(wifi_config.sta.ssid, ssid, strlen(ssid));
    memcpy(wifi_config.sta.password, password, strlen(password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    // Disable WiFi power save to prevent "wifi:m f null" memory issue
    esp_wifi_set_ps(WIFI_PS_NONE);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    printf("[wifi_link] Wi-Fi start, waiting for connection...\n");

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events,
        WIFI_LINK_CONNECTED_BIT | WIFI_LINK_FAILED_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms));
    printf("[wifi_link] Wait done, bits=0x%x\n", (unsigned)bits);

    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_got_ip);
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_any_id);

    if ((bits & WIFI_LINK_CONNECTED_BIT) != 0) {
        ESP_LOGI(TAG, "Wi-Fi connected to SSID '%s'", ssid);
        printf("[wifi_link] SUCCESS: Connected to Wi-Fi\n");
        vEventGroupDelete(s_wifi_events);
        s_wifi_events = NULL;
        return true;
    }

    ESP_LOGW(TAG, "Wi-Fi connection failed or timed out for SSID '%s'", ssid);
    printf("[wifi_link] FAILED: Connection failed or timed out\n");
    vEventGroupDelete(s_wifi_events);
    s_wifi_events = NULL;
    return false;
}
