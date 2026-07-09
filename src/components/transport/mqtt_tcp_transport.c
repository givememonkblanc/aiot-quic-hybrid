/*
 * TCP+TLS transport for coreMQTT — non-QUIC baseline. See the header and
 * docs/tcp_baseline_design.md.
 *
 * STATUS: build-pending. This is written against the esp-tls API but has not
 * been compiled yet (the netem sweep is holding the build). The esp-tls
 * non-blocking / WANT_READ handling in particular must be verified on target.
 */

#include "mqtt_tcp_transport.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include "esp_tls.h"
#include "esp_log.h"

static const char *TAG = "MQTT_TCP";

/* Optional CA certificate for verifying the broker (PEM). When NULL we fall
 * back to no verification — acceptable only on a closed testbed; set this for
 * a fair, secure comparison against QUIC's authenticated TLS 1.3. */
static const char *g_ca_pem;
static size_t g_ca_pem_len;

void mqtt_tcp_transport_set_ca(const char *pem, size_t len)
{
    g_ca_pem = pem;
    g_ca_pem_len = len;
}

int mqtt_tcp_transport_connect(NetworkContext_t *ctx, const char *host,
                               uint16_t port, bool use_tls)
{
    if (ctx == NULL || host == NULL) {
        return -1;
    }
    ctx->host = host;
    ctx->port = port;
    ctx->use_tls = use_tls;
    ctx->connected = false;

    esp_tls_t *tls = esp_tls_init();
    if (tls == NULL) {
        ESP_LOGE(TAG, "esp_tls_init failed");
        return -1;
    }

    esp_tls_cfg_t cfg = {
        .timeout_ms = 10000,
        .non_block = false,       /* blocking connect; reads made non-blocking below */
    };
    if (use_tls) {
        if (g_ca_pem != NULL) {
            cfg.cacert_buf = (const unsigned char *)g_ca_pem;
            cfg.cacert_bytes = (unsigned int)g_ca_pem_len;
        } else {
            /* Testbed only: skip broker authentication. */
            cfg.skip_common_name = true;
            ESP_LOGW(TAG, "TLS without CA — broker not authenticated (testbed only)");
        }
    }

    int ret = esp_tls_conn_new_sync(host, (int)strlen(host), (int)port, &cfg, tls);
    if (ret != 1) {
        ESP_LOGE(TAG, "connect to %s:%u failed (ret=%d)", host, (unsigned)port, ret);
        esp_tls_conn_destroy(tls);
        return -1;
    }

    /* Make reads non-blocking so recv() can return 0 when no data is ready,
     * as coreMQTT's process loop expects. */
    int sockfd = -1;
    if (esp_tls_get_conn_sockfd(tls, &sockfd) == ESP_OK && sockfd >= 0) {
        int flags = fcntl(sockfd, F_GETFL, 0);
        fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
    }

    ctx->tls = tls;
    ctx->connected = true;
    ESP_LOGI(TAG, "connected to %s:%u (tls=%d)", host, (unsigned)port, (int)use_tls);
    return 0;
}

void mqtt_tcp_transport_disconnect(NetworkContext_t *ctx)
{
    if (ctx == NULL || ctx->tls == NULL) {
        return;
    }
    esp_tls_conn_destroy((esp_tls_t *)ctx->tls);
    ctx->tls = NULL;
    ctx->connected = false;
}

int32_t mqtt_tcp_transport_send(NetworkContext_t *ctx, const void *buf, size_t len)
{
    if (ctx == NULL || ctx->tls == NULL || buf == NULL) {
        return -1;
    }
    size_t written = 0;
    const uint8_t *p = (const uint8_t *)buf;
    while (written < len) {
        ssize_t n = esp_tls_conn_write((esp_tls_t *)ctx->tls, p + written, len - written);
        if (n > 0) {
            written += (size_t)n;
            continue;
        }
        if (n == ESP_TLS_ERR_SSL_WANT_WRITE || n == ESP_TLS_ERR_SSL_WANT_READ) {
            continue;  /* retry — coreMQTT sends are expected to complete */
        }
        ESP_LOGE(TAG, "write failed n=%d", (int)n);
        ctx->stream_send_fail++;
        return -1;
    }
    ctx->stream_send_ok++;
    return (int32_t)written;
}

int32_t mqtt_tcp_transport_recv(NetworkContext_t *ctx, void *buf, size_t len)
{
    if (ctx == NULL || ctx->tls == NULL || buf == NULL) {
        return -1;
    }
    ssize_t n = esp_tls_conn_read((esp_tls_t *)ctx->tls, buf, len);
    if (n >= 0) {
        return (int32_t)n;  /* bytes read (0 = none, on non-blocking) */
    }
    if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
        return 0;  /* no data available yet */
    }
    ESP_LOGE(TAG, "read failed n=%d", (int)n);
    return -1;
}
