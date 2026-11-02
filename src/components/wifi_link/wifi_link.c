#include "include/wifi_link.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

#define WIFI_LINK_CONNECTED_BIT BIT0
#define WIFI_LINK_FAILED_BIT BIT1
#define WIFI_LINK_MAX_RETRIES 5

static const char *TAG = "wifi_link";
static EventGroupHandle_t s_wifi_events;
static int s_retry_count;
static wifi_link_status_t s_status;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_switch_pending;
static wifi_config_t s_next_config;

void wifi_link_get_status(wifi_link_status_t *status)
{
    portENTER_CRITICAL(&s_status_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_status_lock);
}

static esp_err_t apply_handoff(void)
{
    wifi_config_t config;
    portENTER_CRITICAL(&s_status_lock);
    config = s_next_config;
    s_switch_pending = false;
    portEXIT_CRITICAL(&s_status_lock);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err == ESP_OK) err = esp_wifi_connect();
    return err;
}

esp_err_t wifi_link_handoff(const char *ssid, const char *password)
{
    if (!s_wifi_events || !ssid || !password || strlen(ssid) > 32 ||
        strlen(ssid) == 0 || strlen(password) > 63 || strlen(password) < 8)
        return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_status_lock);
    if (s_switch_pending) {
        portEXIT_CRITICAL(&s_status_lock);
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_next_config, 0, sizeof(s_next_config));
    memcpy(s_next_config.sta.ssid, ssid, strlen(ssid));
    memcpy(s_next_config.sta.password, password, strlen(password));
    s_next_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    s_next_config.sta.pmf_cfg.capable = true;
    s_switch_pending = true;
    portEXIT_CRITICAL(&s_status_lock);
    esp_err_t err = esp_wifi_disconnect();
    if (err == ESP_ERR_WIFI_NOT_CONNECT) return apply_handoff();
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_status_lock);
        s_switch_pending = false;
        portEXIT_CRITICAL(&s_status_lock);
    }
    return err;
}

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
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        printf("[wifi_evt] WIFI_EVENT_STA_DISCONNECTED reason=%d rssi=%d\n",
               disc != NULL ? (int)disc->reason : -1,
               disc != NULL ? (int)disc->rssi : 0);
        portENTER_CRITICAL(&s_status_lock);
        s_status.ready = false;
        s_status.disconnects++;
        s_status.disconnected_us = esp_timer_get_time();
        s_status.disconnect_reason = disc ? disc->reason : -1;
        bool switching = s_switch_pending;
        portEXIT_CRITICAL(&s_status_lock);
        xEventGroupClearBits(s_wifi_events, WIFI_LINK_CONNECTED_BIT);
        if (switching) {
            s_retry_count = 0;
            esp_err_t err = apply_handoff();
            ESP_LOGI(TAG, "handoff apply=%s", esp_err_to_name(err));
            return;
        }
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
        portENTER_CRITICAL(&s_status_lock);
        s_status.ready = true;
        s_status.ip_generation++;
        s_status.ipv4 = event->ip_info.ip.addr;
        s_status.got_ip_us = esp_timer_get_time();
        portEXIT_CRITICAL(&s_status_lock);
        xEventGroupClearBits(s_wifi_events, WIFI_LINK_FAILED_BIT);
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

    if (strlen(ssid) > sizeof(wifi_config.sta.ssid) ||
        strlen(password) >= sizeof(wifi_config.sta.password)) return false;

    if (s_wifi_events != NULL) {
        ESP_LOGE(TAG, "wifi_link_connect is initialization-only; use wifi_link_handoff");
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
    /* app_cfg owns persistence; repeated research handoffs must not write flash. */
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

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
    // Cap TX power to 10dBm: full-power TX bursts brown out the board on
    // current-limited USB ports (repeated POWERON resets during connect).
    esp_wifi_set_max_tx_power(40);
    printf("[wifi_link] Wi-Fi start, waiting for connection...\n");

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events,
        WIFI_LINK_CONNECTED_BIT | WIFI_LINK_FAILED_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms));
    printf("[wifi_link] Wait done, bits=0x%x\n", (unsigned)bits);

    /* Keep handlers and event group for the complete interface lifetime. */

    if ((bits & WIFI_LINK_CONNECTED_BIT) != 0) {
        ESP_LOGI(TAG, "Wi-Fi connected to SSID '%s'", ssid);
        printf("[wifi_link] SUCCESS: Connected to Wi-Fi\n");
        return true;
    }

    ESP_LOGW(TAG, "Wi-Fi connection failed or timed out for SSID '%s'", ssid);
    printf("[wifi_link] FAILED: Connection failed or timed out\n");
    return false;
}
