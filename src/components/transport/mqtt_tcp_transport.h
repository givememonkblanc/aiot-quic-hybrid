#pragma once

/*
 * TCP+TLS transport for coreMQTT — the non-QUIC baseline (see
 * docs/tcp_baseline_design.md). Implements the same TransportInterface_t
 * send/recv as the QUIC transport, over one esp-tls (mbedTLS) TCP connection.
 * All application objects (control/telemetry/AI/image) ride MQTT PUBLISH on
 * this single ordered, reliable byte stream — no datagrams, no aux streams —
 * which is exactly what exposes head-of-line blocking under loss.
 *
 * Selected at build time by APP_TRANSPORT_BACKEND == APP_TRANSPORT_BACKEND_TCP.
 * This header defines the coreMQTT `struct NetworkContext`; only ONE transport
 * backend (QUIC or TCP) is compiled into a given image.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__has_include)
#  if __has_include("freertos/FreeRTOS.h")
#    include "freertos/FreeRTOS.h"
#  endif
#endif

/* coreMQTT's transport context. TCP-backend fields only. */
typedef struct NetworkContext {
    void    *tls;                 /* esp_tls_t* handle (opaque here) */
    const char *host;             /* broker hostname / IP */
    uint16_t port;                /* broker port (8883 for MQTTS) */
    bool     use_tls;             /* TLS (fair vs QUIC) or plain TCP (debug) */
    bool     connected;

    /* Stats mirrored from the QUIC context so the same telemetry columns work.
     * On TCP everything is a reliable stream send, so datagram_* stay 0. */
    uint32_t stream_send_ok;
    uint32_t stream_send_fail;
    uint32_t reconnects;
    uint32_t main_stack_hwm;
    uint32_t net_tx_stack_hwm;
} NetworkContext_t;

/* Provide a CA certificate (PEM) to authenticate the broker (fair TLS parity
 * with QUIC). If unset, TLS runs without broker verification (testbed only). */
void mqtt_tcp_transport_set_ca(const char *pem, size_t len);

/* Connect the TCP(+TLS) socket to the broker. Returns 0 on success. */
int mqtt_tcp_transport_connect(NetworkContext_t *ctx, const char *host,
                               uint16_t port, bool use_tls);

/* Tear the connection down. */
void mqtt_tcp_transport_disconnect(NetworkContext_t *ctx);

/* coreMQTT TransportInterface_t.send / .recv.
 * send: bytes written, or < 0 on error.
 * recv: bytes read (may be < requested), 0 if none available yet, < 0 on error. */
int32_t mqtt_tcp_transport_send(NetworkContext_t *ctx, const void *buf, size_t len);
int32_t mqtt_tcp_transport_recv(NetworkContext_t *ctx, void *buf, size_t len);
