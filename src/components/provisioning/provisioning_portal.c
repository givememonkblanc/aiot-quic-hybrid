#include "include/provisioning.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char *TAG = "prov_portal";
#define PORTAL_DONE_BIT BIT0

static EventGroupHandle_t s_portal_events;
static app_cfg_t *s_cfg;

static const char *PORTAL_PAGE =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>AIoT Presence Setup</title><style>"
    "body{font-family:system-ui,sans-serif;max-width:420px;margin:24px auto;padding:0 16px;color:#111}"
    "h1{font-size:20px}label{display:block;margin:12px 0 4px;font-weight:600;font-size:14px}"
    "input{width:100%;padding:10px;border:1px solid #ccc;border-radius:8px;font-size:16px;box-sizing:border-box}"
    "button{margin-top:20px;width:100%;padding:12px;background:#0b74de;color:#fff;border:0;border-radius:8px;font-size:16px}"
    ".row{display:flex;gap:8px}.row>div{flex:1}small{color:#666}"
    "details{margin-top:16px}summary{color:#666;font-size:13px;cursor:pointer}"
    "</style></head><body><h1>존재감지장치 설정</h1>"
    "<p><small>Wi-Fi만 연결하면 됩니다. 서버는 자동으로 연결됩니다.</small></p>"
    "<form method=post action=/save>"
    "<label>Wi-Fi 이름 (SSID)</label><input name=ssid value='%s' required>"
    "<label>Wi-Fi 비밀번호</label><input name=pass type=password placeholder='(입력)'>"
    "<details><summary>고급 설정 (설치자용)</summary>"
    "<div class=row><div><label>Server host</label><input name=host value='%s'></div>"
    "<div><label>Port</label><input name=port type=number value='%u'></div></div>"
    "</details>"
    "<button type=submit>저장하고 연결</button>"
    "<p><small>저장하면 기기가 재부팅되어 네트워크에 연결됩니다.</small></p>"
    "</form></body></html>";

static const char *PORTAL_SAVED =
    "<!doctype html><meta charset=utf-8><title>Saved</title>"
    "<body style='font-family:system-ui;text-align:center;margin-top:60px'>"
    "<h2>&#10003; 저장 완료</h2><p>기기가 재부팅되어 네트워크에 연결됩니다.</p></body>";

/* URL-decode in place. */
static void url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') {
            *o++ = ' ';
        } else if (*p == '%' && p[1] && p[2]) {
            int hi = p[1], lo = p[2];
            hi = (hi <= '9') ? hi - '0' : (hi | 0x20) - 'a' + 10;
            lo = (lo <= '9') ? lo - '0' : (lo | 0x20) - 'a' + 10;
            *o++ = (char)((hi << 4) | lo);
            p += 2;
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
}

/* Extract a form field value into dst (URL-decoded). */
static bool form_field(const char *body, const char *key, char *dst, size_t cap)
{
    char needle[24];
    snprintf(needle, sizeof(needle), "%s=", key);
    const char *p = strstr(body, needle);
    if (p == NULL) {
        return false;
    }
    p += strlen(needle);
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= cap) {
        len = cap - 1;
    }
    memcpy(dst, p, len);
    dst[len] = '\0';
    url_decode(dst);
    return true;
}

static esp_err_t portal_get_handler(httpd_req_t *req)
{
    char page[2048];
    snprintf(page, sizeof(page), PORTAL_PAGE,
             s_cfg->wifi_ssid, s_cfg->server_host,
             (unsigned)s_cfg->server_port);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t portal_save_handler(httpd_req_t *req)
{
    char body[512];
    int total = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r <= 0) {
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    char field[128];
    if (form_field(body, "ssid", field, sizeof(field)) && field[0]) {
        app_cfg_set(s_cfg, "wifi.ssid", field);
    }
    if (form_field(body, "pass", field, sizeof(field)) && field[0]) {
        app_cfg_set(s_cfg, "wifi.pass", field);
    }
    if (form_field(body, "host", field, sizeof(field)) && field[0]) {
        app_cfg_set(s_cfg, "server.host", field);
    }
    if (form_field(body, "port", field, sizeof(field)) && field[0]) {
        app_cfg_set(s_cfg, "server.port", field);
    }
    if (form_field(body, "devid", field, sizeof(field)) && field[0]) {
        app_cfg_set(s_cfg, "device.id", field);
    }
    if (form_field(body, "hb", field, sizeof(field)) && field[0]) {
        app_cfg_set(s_cfg, "heartbeat.ms", field);
    }

    esp_err_t err = app_cfg_save(s_cfg);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PORTAL_SAVED, HTTPD_RESP_USE_STRLEN);

    if (err == ESP_OK) {
        xEventGroupSetBits(s_portal_events, PORTAL_DONE_BIT);
    }
    return ESP_OK;
}

/* Captive-portal catch-all: redirect everything to the setup page. */
static esp_err_t portal_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

static httpd_handle_t portal_start_httpd(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        return NULL;
    }
    httpd_uri_t get_root = {.uri = "/", .method = HTTP_GET, .handler = portal_get_handler};
    httpd_uri_t post_save = {.uri = "/save", .method = HTTP_POST, .handler = portal_save_handler};
    httpd_uri_t get_any = {.uri = "/*", .method = HTTP_GET, .handler = portal_redirect_handler};
    httpd_register_uri_handler(server, &get_root);
    httpd_register_uri_handler(server, &post_save);
    httpd_register_uri_handler(server, &get_any);
    return server;
}

static void portal_start_softap(const app_cfg_t *cfg)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&init);

    wifi_config_t ap = {0};
    int n = snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid),
                     "AIoT-Presence-%02X%02X", mac[4], mac[5]);
    ap.ap.ssid_len = (uint8_t)n;
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_OPEN;

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_wifi_start();
    /* Cap TX power to 10dBm. Full-power radio bursts brown out the board on
     * current-limited USB ports and cause a silent reset loop the moment the
     * AP starts beaconing (same fix as wifi_link.c for the STA path). */
    esp_wifi_set_max_tx_power(40);

    (void)cfg;
    ESP_LOGW(TAG, "SoftAP setup portal: connect to \"%s\" then open http://192.168.4.1/",
             (char *)ap.ap.ssid);
    printf("[SETUP] Connect phone to Wi-Fi \"%s\" and open http://192.168.4.1/\n",
           (char *)ap.ap.ssid);
    fflush(stdout);
}

bool provisioning_portal_run(app_cfg_t *cfg, unsigned timeout_ms)
{
    s_cfg = cfg;
    s_portal_events = xEventGroupCreate();

    portal_start_softap(cfg);
    httpd_handle_t server = portal_start_httpd();
    if (server == NULL) {
        ESP_LOGE(TAG, "failed to start setup HTTP server");
        return false;
    }

    TickType_t wait = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_portal_events, PORTAL_DONE_BIT,
                                           pdTRUE, pdFALSE, wait);
    /* let the browser receive the saved page before we tear down */
    vTaskDelay(pdMS_TO_TICKS(500));
    httpd_stop(server);
    return (bits & PORTAL_DONE_BIT) != 0;
}
