#include "moq_audit.h"
/*
 * ngtcp2
 *
 * Copyright (c) 2021 ngtcp2 contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif /* defined(HAVE_CONFIG_H) */

#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_wolfssl.h>

#include <openssl/ssl.h>
#include <openssl/rand.h>
#include <openssl/err.h>

//#include <ev.h>
#include "esp_ev_compat.h"
#include "ngtcp2_sample.h"
#include "quic_stream_buffer.h"
#include "esp_heap_caps.h"
#include <esp_task_wdt.h>
#include <esp_timer.h>
#if AIOT_MOBILITY_CAMERA
#include <wolfssl/wolfcrypt/aes.h>
#endif

#include "esp_log.h"
static const char *TAG = "QUIC";
// @NOTE: hack to avoid using stderr
#undef stderr
#define stderr stdout
/*
#define REMOTE_HOST "127.0.0.1"
#define REMOTE_PORT "4433"
#define ALPN "\xahq-interop"
#define MESSAGE "GET /\r\n"
*/

/*
 * Example 1: Handshake with www.google.com
 *
 * #define REMOTE_HOST "www.google.com"
 * #define REMOTE_PORT "443"
 * #define ALPN "\x2h3"
 *
 * and undefine MESSAGE macro.
 */

#define REMOTE_HOST "127.0.0.1"
#define REMOTE_PORT "14567"
#define ALPN "\x4mqtt"

static int64_t g_quic_handshake_start_us;
static int64_t g_quic_handshake_done_us;
static uint32_t g_quic_handshake_time_ms;

// Real datagram delivery accounting fed by ngtcp2 ACK processing
// (distinct from send-attempt failures counted in the transport layer).
static volatile uint32_t g_dgram_acked;
static volatile uint32_t g_dgram_lost;

// Global configuration for dynamic hostname/port
static char g_host_copy[128], g_port_copy[16], g_alpn_copy[32];
static quic_client_config_t g_config = {
    .hostname = REMOTE_HOST,
    .port = REMOTE_PORT,
    .alpn = ALPN
};

// Global state for QUIC connection
static volatile bool g_quic_connected = false;
static volatile bool g_quic_handshake_completed = false;
static volatile uint64_t g_quic_n_local_streams = 0;
static uint32_t g_quic_reconnect_attempts = 0;
static SSL_CTX *g_shared_ssl_ctx = NULL;
static uint8_t *g_shared_dgram_tx_buf = NULL;
static size_t g_shared_dgram_tx_buf_len = 0;
static uint8_t *g_stream_storage;
static volatile bool g_network_available = true;
static volatile int g_migration_status;
static uint64_t g_migration_started_us;

// Mutex for thread-safe QUIC operations
SemaphoreHandle_t quic_mutex = NULL;
#include "quic_lock_metrics.inc"
#if AIOT_PIPELINE_PROFILE
#include "quic_pipeline_profile.inc"
#endif
static bool quic_processing = false;  // Flag to prevent reentrancy

#define APP_BUFFER_SIZE 4096
static uint8_t app_recv_buffer[APP_BUFFER_SIZE];
static size_t app_recv_buffer_len = 0;
static size_t app_recv_buffer_read_pos = 0;
static uint8_t g_pkt_rx_buf[16384] __attribute__((aligned(4)));

static void log_internal_heap_state(const char *stage) {
  ESP_LOGI(TAG,
           "%s: internal_free=%u internal_largest=%u default_free=%u",
           stage,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
}

/*
 * wolfSSL custom allocator: keep large TLS handshake allocations out of the
 * internal DRAM heap, which long uptimes + repeated reconnects fragment until
 * 3KB+ contiguous blocks fail and the handshake dies in cert/key parsing.
 *
 * Safe on ESP32-S3: wolfSSL HW crypto ports copy operands into accelerator
 * blocks word-by-word from the CPU (esp32_mp.c / esp32_sha.c DPORT_REG_WRITE
 * loops), they do NOT DMA from the source buffer; AES HW is disabled.
 *
 * Small allocations stay in internal DRAM (latency); >= threshold goes PSRAM
 * first, internal DRAM as fallback.
 */
#if AIOT_QUIC_PSRAM_POOL
#define WOLFSSL_PSRAM_THRESHOLD 256
#else
#define WOLFSSL_PSRAM_THRESHOLD 1024
#endif

static void *transport_wolfssl_malloc(size_t size) {
  void *p = NULL;
  if (size >= WOLFSSL_PSRAM_THRESHOLD) {
    p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (p == NULL) {
    p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
  }
  return p;
}

static void transport_wolfssl_free(void *ptr) {
  heap_caps_free(ptr);
}

static void *transport_wolfssl_realloc(void *ptr, size_t size) {
  void *p = NULL;
  if (size >= WOLFSSL_PSRAM_THRESHOLD) {
    p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (p == NULL) {
    p = heap_caps_realloc(ptr, size, MALLOC_CAP_8BIT);
  }
  return p;
}

#if AIOT_QUIC_PSRAM_POOL
void *aiot_wolfssl_malloc(size_t size, void *heap, int type) {
  (void)heap; (void)type;
  return transport_wolfssl_malloc(size);
}
void aiot_wolfssl_free(void *ptr, void *heap, int type) {
  (void)heap; (void)type;
  transport_wolfssl_free(ptr);
}
void *aiot_wolfssl_realloc(void *ptr, size_t size, void *heap, int type) {
  (void)heap; (void)type;
  return transport_wolfssl_realloc(ptr, size);
}
#endif

static void transport_wolfssl_allocator_init(void) {
#if !AIOT_QUIC_PSRAM_POOL
  wolfSSL_SetAllocators(transport_wolfssl_malloc,
                        transport_wolfssl_free,
                        transport_wolfssl_realloc);
#endif
  ESP_LOGI(TAG, "wolfSSL allocator: PSRAM-first above %d bytes",
           WOLFSSL_PSRAM_THRESHOLD);
}

static bool quic_callback_enter(TickType_t timeout_ticks) {
  if (quic_mutex == NULL) {
    return false;
  }

  if (quic_lock_take( timeout_ticks) != pdTRUE) {
    return false;
  }

  if (quic_processing) {
    quic_lock_give();
    return false;
  }

  quic_processing = true;
  return true;
}

static void quic_callback_leave(void) {
  quic_processing = false;
  quic_lock_give();
}

static uint64_t timestamp(void) {
  return esp_timer_get_time() * 1000;
}

static int create_sock(struct sockaddr *addr, socklen_t *paddrlen,
                       const char *host, const char *port) {
  struct addrinfo hints = {0};
  struct addrinfo *res, *rp;
  int rv;
  int fd = -1;

  hints.ai_flags = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;

  rv = getaddrinfo(host, port, &hints, &res);
  if (rv != 0) {
    // Replace gai_strerror with displaying the error code directly
    ESP_LOGE(TAG, "getaddrinfo failed with error code: %d", rv);
    return -1;
  }

  for (rp = res; rp; rp = rp->ai_next) {
    fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd == -1) {
      continue;
    }

    break;
  }

  if (fd == -1) {
    goto end;
  }

  *paddrlen = rp->ai_addrlen;
  memcpy(addr, rp->ai_addr, rp->ai_addrlen);

end:
  freeaddrinfo(res);

  return fd;
}

static int connect_sock(struct sockaddr *local_addr, socklen_t *plocal_addrlen,
                        int fd, const struct sockaddr *remote_addr,
                        size_t remote_addrlen) {
  socklen_t len;

  if (connect(fd, remote_addr, (socklen_t)remote_addrlen) != 0) {
    ESP_LOGE(TAG, "connect: %s", strerror(errno));
    return -1;
  }

  len = *plocal_addrlen;

  if (getsockname(fd, local_addr, &len) == -1) {
    ESP_LOGE(TAG, "getsockname: %s", strerror(errno));
    return -1;
  }

  *plocal_addrlen = len;

  return 0;
}

struct client {
  ngtcp2_crypto_conn_ref conn_ref;
  int fd;
  int old_fd;
  struct sockaddr_storage old_local_addr;
  socklen_t old_local_addrlen;
  quic_stream_buffer_t tx;
  struct sockaddr_storage local_addr;
  socklen_t local_addrlen;
  SSL_CTX *ssl_ctx;
  SSL *ssl;
  ngtcp2_conn *conn;

  struct {
    int64_t stream_id;
    const uint8_t *data;
    size_t datalen;
    size_t nwrite;
  } stream;

  ngtcp2_ccerr last_error;

  ev_io rev;
  ev_timer timer;

  uint8_t *dgram_tx_buf;
  size_t dgram_tx_buf_len;
};

struct client g_client = {.fd = -1, .old_fd = -1};
#if AIOT_MOQT_EXPERIMENT
#include "moqt_ngtcp2.inc"
#endif

static int numeric_host_family(const char *hostname, int family) {
  uint8_t dst[sizeof(struct in6_addr)];
  return inet_pton(family, hostname, dst) == 1;
}

static int numeric_host(const char *hostname) {
  return numeric_host_family(hostname, AF_INET) ||
         numeric_host_family(hostname, AF_INET6);
}

#if AIOT_MOBILITY_CAMERA
/* AES-GCM zero-key/zero-IV, one-block known-answer vector. Fail closed. */
static int transport_aes_selftest(void) {
  static bool passed;
  if (passed) return 0;
  static Aes aes;
  const byte zero[16] = {0};
  const byte expected_ct[16] = {0x03,0x88,0xda,0xce,0x60,0xb6,0xa3,0x92,0xf3,0x28,0xc2,0xb9,0x71,0xb2,0xfe,0x78};
  const byte expected_tag[16] = {0xab,0x6e,0x47,0xd4,0x2c,0xec,0x13,0xbd,0xf5,0x3a,0x67,0xb2,0x12,0x57,0xbd,0xdf};
  byte ct[16], tag[16], plain[16];
  if (wc_AesInit(&aes, NULL, INVALID_DEVID) != 0) return -1;
  int ret = wc_AesGcmSetKey(&aes, zero, 16);
  if (!ret) ret = wc_AesGcmEncrypt(&aes, ct, zero, 16, zero, 12, tag, 16, NULL, 0);
  if (!ret && (memcmp(ct, expected_ct, 16) || memcmp(tag, expected_tag, 16))) ret = -1;
  if (!ret) ret = wc_AesGcmDecrypt(&aes, plain, ct, 16, zero, 12, tag, 16, NULL, 0);
  if (!ret && memcmp(plain, zero, 16)) ret = -1;
  if (!ret) {
    tag[0] ^= 1;
    if (wc_AesGcmDecrypt(&aes, plain, ct, 16, zero, 12, tag, 16, NULL, 0) == 0) ret = -1;
  }
  wc_AesFree(&aes);
  printf("[AES_SELFTEST] %s\n", ret ? "FAIL" : "PASS");
  passed = ret == 0;
  return ret;
}
#endif

#if AIOT_QUIC_PSRAM_POOL
/* Connection pools need ordinary byte-addressable memory, not DMA memory.
 * Fail large allocations rather than consuming Wi-Fi's internal SRAM reserve. */
static unsigned quic_alloc_caps(size_t size) {
  return MALLOC_CAP_8BIT | (size >= 512 ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL);
}
static void *quic_pool_malloc(size_t size, void *user) {
  (void)user;
  return heap_caps_malloc(size, quic_alloc_caps(size));
}
static void quic_pool_free(void *ptr, void *user) {
  (void)user;
  heap_caps_free(ptr);
}
static void *quic_pool_calloc(size_t n, size_t size, void *user) {
  (void)user;
  if (size && n > SIZE_MAX / size) return NULL;
  return heap_caps_calloc(n, size, quic_alloc_caps(n * size));
}
static void *quic_pool_realloc(void *ptr, size_t size, void *user) {
  (void)user;
  return heap_caps_realloc(ptr, size, quic_alloc_caps(size));
}
static const ngtcp2_mem quic_pool = {
  .malloc = quic_pool_malloc, .free = quic_pool_free,
  .calloc = quic_pool_calloc, .realloc = quic_pool_realloc,
};
#endif

static int client_ssl_init(struct client *c) {
#if AIOT_MOBILITY_CAMERA
  if (transport_aes_selftest() != 0) return -1;
#endif
  static bool allocator_registered = false;
  if (!allocator_registered) {
    transport_wolfssl_allocator_init();
    allocator_registered = true;
  }
  if (g_shared_ssl_ctx == NULL) {
    g_shared_ssl_ctx = SSL_CTX_new(wolfTLSv1_3_client_method());
    log_internal_heap_state("ssl:init after SSL_CTX_new");
    if (!g_shared_ssl_ctx) {
      ESP_LOGE(TAG, "SSL_CTX_new: %s", ERR_error_string(ERR_get_error(), NULL));
      return -1;
    }

    if (ngtcp2_crypto_wolfssl_configure_client_context(g_shared_ssl_ctx) != 0) {
      ESP_LOGE(TAG, "ngtcp2_crypto_wolfssl_configure_client_context failed");
      return -1;
    }
    log_internal_heap_state("ssl:init after configure_context");
#ifdef AIOT_QUIC_CIPHER_LIST
    if (wolfSSL_CTX_set_cipher_list(g_shared_ssl_ctx, AIOT_QUIC_CIPHER_LIST) != WOLFSSL_SUCCESS) {
      ESP_LOGE(TAG, "Requested TLS cipher list is unavailable");
      return -1;
    }
#endif

#if defined(APP_QUIC_VERIFY_PEER) && APP_QUIC_VERIFY_PEER
    /* Verify the QUIC server against the embedded CA. Enabled automatically
     * when a server_ca.pem is dropped into components/transport/ (see the
     * component CMakeLists, which embeds it and defines APP_QUIC_VERIFY_PEER). */
    extern const uint8_t server_ca_pem_start[] asm("_binary_server_ca_pem_start");
    extern const uint8_t server_ca_pem_end[]   asm("_binary_server_ca_pem_end");
    if (wolfSSL_CTX_load_verify_buffer(g_shared_ssl_ctx, server_ca_pem_start,
                                       (long)(server_ca_pem_end - server_ca_pem_start),
                                       WOLFSSL_FILETYPE_PEM) != WOLFSSL_SUCCESS) {
      ESP_LOGE(TAG, "failed to load embedded server CA for QUIC verification");
      return -1;
    }
    wolfSSL_CTX_set_verify(g_shared_ssl_ctx, WOLFSSL_VERIFY_PEER, NULL);
    ESP_LOGI(TAG, "QUIC server certificate verification ENABLED");
#else
    /* Development / experiment default: the server is not authenticated. Drop a
     * server_ca.pem into components/transport/ for a production build. */
    wolfSSL_CTX_set_verify(g_shared_ssl_ctx, WOLFSSL_VERIFY_NONE, NULL);
    ESP_LOGW(TAG, "QUIC server certificate NOT verified (insecure). Add "
                  "components/transport/server_ca.pem for a production build.");
#endif
  } else {
    log_internal_heap_state("ssl:init reusing shared SSL_CTX");
  }

  c->ssl_ctx = g_shared_ssl_ctx;
  wolfSSL_CTX_UseSNI(c->ssl_ctx, WOLFSSL_SNI_HOST_NAME, g_config.hostname, strlen(g_config.hostname) + 1);

  c->ssl = SSL_new(c->ssl_ctx);
  log_internal_heap_state("ssl:init after SSL_new");
  if (!c->ssl) {
    ESP_LOGE(TAG, "SSL_new: %s", ERR_error_string(ERR_get_error(), NULL));
    return -1;
  }

  SSL_set_app_data(c->ssl, &c->conn_ref);
  SSL_set_connect_state(c->ssl);
  
  size_t alpn_len = strlen(g_config.alpn);
  if (alpn_len > 1 && (uint8_t)g_config.alpn[0] + 1 == alpn_len) {
    SSL_set_alpn_protos(c->ssl, (const unsigned char *)g_config.alpn,
                        (unsigned int)alpn_len);
    ESP_LOGI(TAG, "Set ALPN wire-format list (length: %zu)", alpn_len);
    log_internal_heap_state("ssl:init after set_alpn");
  } else {
    ESP_LOGE(TAG, "Invalid ALPN wire-format length: %zu", alpn_len);
    return -1;
  }
  
  if (!numeric_host(g_config.hostname)) {
    SSL_set_tlsext_host_name(c->ssl, g_config.hostname);
    log_internal_heap_state("ssl:init after set_sni");
  }

  return 0;
}

static int client_prepare_datagram_buffer(struct client *c) {
  if (c == NULL) {
    return -1;
  }
  if (g_shared_dgram_tx_buf != NULL && g_shared_dgram_tx_buf_len >= 1452U) {
    c->dgram_tx_buf = g_shared_dgram_tx_buf;
    c->dgram_tx_buf_len = g_shared_dgram_tx_buf_len;
    return 0;
  }

  g_shared_dgram_tx_buf = heap_caps_malloc(1452U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (g_shared_dgram_tx_buf == NULL) {
    g_shared_dgram_tx_buf = heap_caps_malloc(1452U, MALLOC_CAP_8BIT);
  }
  if (g_shared_dgram_tx_buf == NULL) {
    ESP_LOGE(TAG, "Failed to allocate reusable datagram output buffer");
    return -1;
  }
  g_shared_dgram_tx_buf_len = 1452U;
  c->dgram_tx_buf = g_shared_dgram_tx_buf;
  c->dgram_tx_buf_len = g_shared_dgram_tx_buf_len;
  log_internal_heap_state("quic:init after dgram buffer alloc");
  return 0;
}

static void rand_cb(uint8_t *dest, size_t destlen,
                    const ngtcp2_rand_ctx *rand_ctx) {
  int rv;
  (void)rand_ctx;

  rv = RAND_bytes(dest, (int)destlen);
  if (rv != 1) {
    assert(0);
    abort();
  }
}

static int get_new_connection_id_cb(ngtcp2_conn *conn, ngtcp2_cid *cid,
                                    uint8_t *token, size_t cidlen,
                                    void *user_data) {
  (void)conn;
  (void)user_data;

  if (RAND_bytes(cid->data, (int)cidlen) != 1) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  cid->datalen = cidlen;

  if (RAND_bytes(token, NGTCP2_STATELESS_RESET_TOKENLEN) != 1) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static void delete_crypto_aead_ctx_wdt_cb(ngtcp2_conn *conn,
                                          ngtcp2_crypto_aead_ctx *aead_ctx,
                                          void *user_data) {
  (void)conn;
  (void)user_data;

  ESP_LOGI(TAG, "delete_crypto_aead_ctx_wdt_cb");
  esp_task_wdt_reset();
  ngtcp2_crypto_delete_crypto_aead_ctx_cb(conn, aead_ctx, user_data);
  esp_task_wdt_reset();
}

static void delete_crypto_cipher_ctx_wdt_cb(ngtcp2_conn *conn,
                                            ngtcp2_crypto_cipher_ctx *cipher_ctx,
                                            void *user_data) {
  (void)conn;
  (void)user_data;

  ESP_LOGI(TAG, "delete_crypto_cipher_ctx_wdt_cb");
  esp_task_wdt_reset();
  ngtcp2_crypto_delete_crypto_cipher_ctx_cb(conn, cipher_ctx, user_data);
  esp_task_wdt_reset();
}

#if AIOT_MOQT_EXPERIMENT
#include AIOT_MOQT_PIN_HEADER
#include "wolfssl/wolfcrypt/sha256.h"
#endif
static int handshake_completed_cb(ngtcp2_conn *conn, void *user_data) {
#if AIOT_MOQT_EXPERIMENT
    struct client *moq_client = user_data;
    WOLFSSL_X509 *cert = wolfSSL_get_peer_certificate(moq_client->ssl);
    int cert_len = 0;
    const unsigned char *der = cert ? wolfSSL_X509_get_der(cert,&cert_len) : NULL;
    unsigned char digest[32];
    int valid = der && cert_len>0 && wc_Sha256Hash(der,(word32)cert_len,digest)==0 && !memcmp(digest,moqt_peer_pin,32);
    if(cert)wolfSSL_X509_free(cert);
    if(!valid){ESP_LOGE(TAG,"MOQT receiver certificate pin mismatch");return NGTCP2_ERR_CALLBACK_FAILURE;}
    char *selected=NULL;unsigned short selected_len=0;
    if(wolfSSL_ALPN_GetProtocol(moq_client->ssl,&selected,&selected_len)!=WOLFSSL_SUCCESS || selected_len!=7 || memcmp(selected,"moqt-21",7))return NGTCP2_ERR_CALLBACK_FAILURE;
    moq_audit_printf("[MOQ_TLS] peer_pin=verified alpn=moqt-21\n");
#endif

    (void)conn;
    (void)user_data;
    printf("[QUIC_CIPHER] %s\n", wolfSSL_get_cipher_name(((struct client *)user_data)->ssl));
    ESP_LOGI(TAG, "QUIC handshake completed callback triggered!");
    g_quic_handshake_done_us = esp_timer_get_time();
    if (g_quic_handshake_start_us > 0 && g_quic_handshake_done_us >= g_quic_handshake_start_us) {
        g_quic_handshake_time_ms = (uint32_t)((g_quic_handshake_done_us - g_quic_handshake_start_us) / 1000ULL);
    }
    g_quic_handshake_completed = true;
    g_quic_connected = true;
    g_quic_n_local_streams = ngtcp2_conn_get_streams_bidi_left(conn);
    ESP_LOGI(TAG, "[QUIC_METRIC] handshake_time_ms=%lu", (unsigned long)g_quic_handshake_time_ms);
    return 0;
}

// Forward declaration for recv_stream_data callback
static int recv_stream_data(ngtcp2_conn *conn, uint32_t flags,
                           int64_t stream_id, uint64_t offset,
                           const uint8_t *data, size_t datalen,
                           void *user_data, void *stream_user_data);

static int extend_max_local_streams_bidi(ngtcp2_conn *conn,
                                         uint64_t max_streams,
                                         void *user_data) {
  (void)user_data;
  g_quic_n_local_streams = ngtcp2_conn_get_streams_bidi_left(conn);
  ESP_LOGI(TAG, "Extending max local streams bidi to %" PRIu64 " (available=%" PRIu64 ")\n",
           max_streams, g_quic_n_local_streams);
  return 0;
}

static int ack_datagram_cb(ngtcp2_conn *conn, uint64_t dgram_id,
                           void *user_data) {
  (void)conn;
  (void)user_data;
  g_dgram_acked++;
  ESP_LOGD(TAG, "[DGRAM_ACK] seq=%llu acked=%lu",
           (unsigned long long)dgram_id, (unsigned long)g_dgram_acked);
  return 0;
}

static int lost_datagram_cb(ngtcp2_conn *conn, uint64_t dgram_id,
                            void *user_data) {
  (void)conn;
  (void)user_data;
  g_dgram_lost++;
  ESP_LOGW(TAG, "[DGRAM_LOST] seq=%llu lost=%lu",
           (unsigned long long)dgram_id, (unsigned long)g_dgram_lost);
  return 0;
}

#if !AIOT_MOBILITY_RESEARCH
static void log_printf(void *user_data, const char *fmt, ...) {
  va_list ap;
  (void)user_data;

  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);

  fprintf(stderr, "\n");
}

#endif

static int stream_ack_cb(ngtcp2_conn *conn, int64_t stream_id, uint64_t offset,
                         uint64_t datalen, void *user_data, void *stream_user_data)
{
    (void)conn; (void)stream_user_data;
    struct client *c = user_data;
#if AIOT_MOQT_EXPERIMENT
    if (moq_acked(stream_id, offset, datalen) != 0)
        return NGTCP2_ERR_CALLBACK_FAILURE;
#endif
    if (stream_id == c->stream.stream_id && !quic_stream_buffer_ack(&c->tx, offset, datalen))
        return NGTCP2_ERR_CALLBACK_FAILURE;
    return 0;
}

static int path_validation_cb(ngtcp2_conn *conn, uint32_t flags, const ngtcp2_path *path,
                             const ngtcp2_path *old_path, ngtcp2_path_validation_result res,
                             void *user_data)
{
    (void)conn; (void)flags; (void)old_path;
    struct client *c = user_data;
    if (g_migration_status != 1 || path->local.addrlen != c->local_addrlen ||
        memcmp(path->local.addr, &c->local_addr, c->local_addrlen) != 0) return 0;
    if (res == NGTCP2_PATH_VALIDATION_RESULT_ABORTED) return 0;
    g_migration_status = res == NGTCP2_PATH_VALIDATION_RESULT_SUCCESS ? 2 : -1;
    ESP_LOGI("MOBILITY", "event=path_validation session=%lu result=%d elapsed_ms=%llu t_us=%llu",
        (unsigned long)g_quic_reconnect_attempts, g_migration_status,
        (unsigned long long)((esp_timer_get_time()-g_migration_started_us)/1000),
        (unsigned long long)esp_timer_get_time());
    return 0;
}

static int client_quic_init(struct client *c,
                            const struct sockaddr *remote_addr,
                            socklen_t remote_addrlen,
                            const struct sockaddr *local_addr,
                            socklen_t local_addrlen) {
  ESP_LOGI(TAG, "In client_quic_init");
  ngtcp2_path path = {
    .local =
      {
        .addr = (struct sockaddr *)local_addr,
        .addrlen = local_addrlen,
      },
    .remote =
      {
        .addr = (struct sockaddr *)remote_addr,
        .addrlen = remote_addrlen,
      },
  };
  ngtcp2_callbacks callbacks = {
    .client_initial = ngtcp2_crypto_client_initial_cb,
    .recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb,
#if AIOT_PIPELINE_PROFILE
    .encrypt = pipe_encrypt,
#else
    .encrypt = ngtcp2_crypto_encrypt_cb,
#endif
    .decrypt = ngtcp2_crypto_decrypt_cb,
#if AIOT_PIPELINE_PROFILE
    .hp_mask = pipe_hp,
#else
    .hp_mask = ngtcp2_crypto_hp_mask_cb,
#endif
    .recv_retry = ngtcp2_crypto_recv_retry_cb,
    .recv_stream_data = recv_stream_data,  // Add this callback!
    .handshake_completed = handshake_completed_cb,  // Add handshake completion callback
    .extend_max_local_streams_bidi = extend_max_local_streams_bidi,
    .rand = rand_cb,
    .get_new_connection_id = get_new_connection_id_cb,
    .update_key = ngtcp2_crypto_update_key_cb,
    .delete_crypto_aead_ctx = delete_crypto_aead_ctx_wdt_cb,
    .delete_crypto_cipher_ctx = delete_crypto_cipher_ctx_wdt_cb,
    .get_path_challenge_data = ngtcp2_crypto_get_path_challenge_data_cb,
    .version_negotiation = ngtcp2_crypto_version_negotiation_cb,
    .ack_datagram = ack_datagram_cb,
    .lost_datagram = lost_datagram_cb,
    .acked_stream_data_offset = stream_ack_cb,
#if AIOT_MOQT_EXPERIMENT
    .stream_close = moq_stream_close,
    .stream_reset = moq_stream_reset_cb,
    .stream_stop_sending = moq_stream_stop_cb,
#endif
    .path_validation = path_validation_cb,
  };
  ngtcp2_cid dcid, scid;
  ngtcp2_settings settings;
  ngtcp2_transport_params params;
  int rv;

  dcid.datalen = NGTCP2_MIN_INITIAL_DCIDLEN;
  if (RAND_bytes(dcid.data, (int)dcid.datalen) != 1) {
    ESP_LOGE(TAG, "RAND_bytes failed");
    return -1;
  }

  scid.datalen = 8;
  if (RAND_bytes(scid.data, (int)scid.datalen) != 1) {
    fprintf(stderr, "RAND_bytes failed\n");
    return -1;
  }
  ngtcp2_settings_default(&settings);
  log_internal_heap_state("quic:init after rand_and_defaults");

  // Select Congestion Control Algorithm: BBR vs CUBIC
  // NGTCP2_CC_ALGO_CUBIC (default) - Loss-based, good for high-bandwidth networks
  // NGTCP2_CC_ALGO_BBR - Model-based, better for lossy/variable networks
  // NGTCP2_CC_ALGO_RENO - Classic Reno for comparison
#ifdef CONFIG_QUIC_CC_ALGO_BBR
  settings.cc_algo = NGTCP2_CC_ALGO_BBR;
  ESP_LOGI(TAG, "===>  CC ALGO: BBR (Model-based, optimal for lossy networks)");
#elif defined(CONFIG_QUIC_CC_ALGO_RENO)
  settings.cc_algo = NGTCP2_CC_ALGO_RENO;
  ESP_LOGI(TAG, "===>  CC ALGO: RENO (Classic, for baseline comparison)");
#else
  // Default to CUBIC
  settings.cc_algo = NGTCP2_CC_ALGO_CUBIC;
  ESP_LOGI(TAG, "===>  CC ALGO: CUBIC (Loss-based, default)");
#endif

  settings.initial_ts = timestamp();
  ESP_LOGI(TAG, "===>  INITIAL TS: %llu", (unsigned long long)settings.initial_ts);
#if AIOT_MOBILITY_RESEARCH
  settings.log_printf = NULL;
#else
  settings.log_printf = log_printf;
#endif

  ngtcp2_transport_params_default(&params);

  // Enable QUIC Datagram Extension (RFC 9221)
  // Advertise support for DATAGRAM frames with max payload of 1200 bytes
  params.max_datagram_frame_size = 1200;
  ESP_LOGI(TAG, "===>  DATAGRAM: Enabled (max_frame_size=%u)", (unsigned)params.max_datagram_frame_size);

  params.initial_max_streams_uni = 3;
#if AIOT_MOQT_EXPERIMENT
  /* Each SUBSCRIBE, FETCH and namespace request of the peer is a bidirectional stream;
   * credit returns when a stream closes (moq_stream_close). */
  /* Draft-21 section 15.5 bounds concurrent requests with the QUIC stream limit: one per
   * request slot of the adapter. */
  params.initial_max_streams_bidi = 8;
  params.initial_max_streams_uni = 4;
  params.initial_max_stream_data_bidi_remote = 4096;
  params.initial_max_stream_data_uni = 4096;
#if AIOT_MOQT_HELPER
  /* Helper: whole JPEG Objects arrive on the relay's streams; 4 KiB of credit per stream
   * would limit the frame to 4 KiB per round trip. */
  params.initial_max_stream_data_uni = 64 * 1024;
  /* The relay sends a PING every second on an idle subscriber session; a relay that is gone
   * or restarted is noticed after 5 s and the helper connects again. */
  params.max_idle_timeout = 5 * NGTCP2_SECONDS;
#endif
#endif
  params.initial_max_stream_data_bidi_local = 128 * 1024;
  params.initial_max_data = 1024 * 1024;

  ESP_LOGI("DEBUG", "EXP-F3: Settings/Params setup done. Checking heap...");
  // TEMP: if (!heap_caps_check_integrity_all(true)) {
  //   ESP_LOGE("DEBUG", "PANIC at F3! Heap corrupted BY ngtcp2 settings/params setup!");
  // }

  rv =
    ngtcp2_conn_client_new(&c->conn, &dcid, &scid, &path, NGTCP2_PROTO_VER_V1,
                           &callbacks, &settings, &params,
#if AIOT_QUIC_PSRAM_POOL
                           &quic_pool,
#else
                           NULL,
#endif
                           c);
  if (rv != 0) {
    ESP_LOGE(TAG, "ngtcp2_conn_client_new: %s", ngtcp2_strerror(rv));
    return -1;
  }

  ESP_LOGI("DEBUG", "EXP-F4: ngtcp2_conn_client_new done. Checking heap...");
  log_internal_heap_state("quic:init after conn_client_new");
  // TEMP: if (!heap_caps_check_integrity_all(true)) {
  //   ESP_LOGE("DEBUG", "PANIC at F4! Heap corrupted BY ngtcp2 connection allocation!");
  // }

  ngtcp2_conn_set_tls_native_handle(c->conn, c->ssl);
  log_internal_heap_state("quic:init after set_tls_native_handle");

  return 0;
}

static int client_read_socket(struct client *c, int fd, struct sockaddr_storage *local, socklen_t local_len) {
  // @NOTE: adjust the buffer size as needed
  uint8_t *buf = g_pkt_rx_buf;
  struct sockaddr_storage addr;
  struct iovec iov = {
    .iov_base = buf,
    .iov_len = sizeof(g_pkt_rx_buf),
  };
  struct msghdr msg = {0};
  ssize_t nread;
  ngtcp2_path path = {0};
  ngtcp2_pkt_info pi = {0};
  int rv;
  bool saw_packet = false;
  uint32_t packet_index = 0;
#if AIOT_MOQT_BOUNDED_PUMP
  uint64_t pump_start=esp_timer_get_time();
#endif

  msg.msg_name = &addr;
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;

  for (;;) {
#if AIOT_MOQT_BOUNDED_PUMP
    if(packet_index>=16 || esp_timer_get_time()-pump_start>=20000)break;
#endif
    esp_task_wdt_reset();
    msg.msg_namelen = sizeof(addr);

    ESP_LOGI("DEBUG", "EXP-D0_A: BEFORE recvmsg. Checking heap...");
    // TEMPORARY: Disable heap integrity check to isolate LoadProhibited cause
    // if (!heap_caps_check_integrity_all(true)) {
    //   ESP_LOGE("DEBUG", "PANIC at D0_A! Heap was ALREADY corrupted before network rx! Check Camera/TinyML tasks.");
    // }

    nread = recvmsg(fd, &msg, MSG_DONTWAIT);

    ESP_LOGI("DEBUG", "EXP-D0_B: AFTER recvmsg. Checking heap...");
    // if (!heap_caps_check_integrity_all(true)) {
    //   ESP_LOGE("DEBUG", "PANIC at D0_B! Heap corrupted BY recvmsg buffer handling!");
    // }

    if (nread == -1) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        ESP_LOGE(TAG, "recvmsg: %s", strerror(errno));
      } else if (!saw_packet) {
        ESP_LOGI(TAG, "recvmsg: no inbound UDP packet available yet");
      }

      break;
    }

    saw_packet = true;
    ++packet_index;

    char remote_host[64] = {0};
    char remote_serv[16] = {0};
    if (((struct sockaddr *)&addr)->sa_family == AF_INET) {
      struct sockaddr_in *sin = (struct sockaddr_in *)&addr;
      inet_ntop(AF_INET, &sin->sin_addr, remote_host, sizeof(remote_host));
      snprintf(remote_serv, sizeof(remote_serv), "%u", ntohs(sin->sin_port));
    } else if (((struct sockaddr *)&addr)->sa_family == AF_INET6) {
      struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&addr;
      inet_ntop(AF_INET6, &sin6->sin6_addr, remote_host, sizeof(remote_host));
      snprintf(remote_serv, sizeof(remote_serv), "%u", ntohs(sin6->sin6_port));
    } else {
      snprintf(remote_host, sizeof(remote_host), "af:%d", ((struct sockaddr *)&addr)->sa_family);
      snprintf(remote_serv, sizeof(remote_serv), "?");
    }

    ESP_LOGI(TAG, "recvmsg: got %d bytes from %s:%s", (int)nread, remote_host, remote_serv);

    ESP_LOGI("DEBUG", "EXP-D1: UDP Recv done. Checking heap...");
    // TEMP: if (!heap_caps_check_integrity_all(true)) {
    //   ESP_LOGE("DEBUG", "PANIC at D1! Heap corrupted by UDP recv buffer handling!");
    // }

    path.local.addrlen = local_len;
    path.local.addr = (struct sockaddr *)local;
    path.remote.addrlen = msg.msg_namelen;
    path.remote.addr = msg.msg_name;

    ESP_LOGI("DEBUG", "EXP-D0_C: AFTER path/address setup. Checking heap...");
    // TEMP: if (!heap_caps_check_integrity_all(true)) {
    //   ESP_LOGE("DEBUG", "PANIC at D0_C! Heap corrupted during sockaddr / path struct assignments!");
    // }

    uint64_t read_start_us = esp_timer_get_time();
    ESP_LOGI(TAG, "client_read: packet[%lu] before ngtcp2_conn_read_pkt nread=%d ts=%llu",
             (unsigned long)packet_index, (int)nread, (unsigned long long)read_start_us);
    esp_task_wdt_reset();
    rv = ngtcp2_conn_read_pkt(c->conn, &path, &pi, buf, (size_t)nread,
                              timestamp());
    uint64_t read_end_us = esp_timer_get_time();
    ESP_LOGI(TAG, "client_read: packet[%lu] after ngtcp2_conn_read_pkt rv=%d elapsed_us=%llu",
             (unsigned long)packet_index, rv,
             (unsigned long long)(read_end_us - read_start_us));
    esp_task_wdt_reset();
    if (rv != 0) {
      ESP_LOGE(TAG, "ngtcp2_conn_read_pkt: %s", ngtcp2_strerror(rv));
      if (rv == NGTCP2_ERR_DRAINING) {
        const ngtcp2_ccerr *close = ngtcp2_conn_get_ccerr(c->conn);
        ESP_LOGW("MOBILITY", "event=peer_close code=%llu frame=%llu reason=%.*s",
                 (unsigned long long)close->error_code, (unsigned long long)close->frame_type,
                 (int)close->reasonlen, close->reason ? (const char *)close->reason : "");
      }
      if (!c->last_error.error_code) {
        if (rv == NGTCP2_ERR_CRYPTO) {
          ngtcp2_ccerr_set_tls_alert(
            &c->last_error, ngtcp2_conn_get_tls_alert(c->conn), NULL, 0);
        } else {
          ngtcp2_ccerr_set_liberr(&c->last_error, rv, NULL, 0);
        }
      }
      return -1;
    }

    ESP_LOGI(TAG, "ngtcp2_conn_read_pkt: accepted %d-byte packet", (int)nread);
    esp_task_wdt_reset();
  }

  return 0;
}

static int client_read(struct client *c)
{
    if (!g_network_available) return 0;
    int ret = client_read_socket(c, c->fd, &c->local_addr, c->local_addrlen);
    if (ret == 0 && c->old_fd >= 0)
        ret = client_read_socket(c, c->old_fd, &c->old_local_addr, c->old_local_addrlen);
    return ret;
}

static int client_send_packet(struct client *c, const uint8_t *data,
                              size_t datalen, const ngtcp2_path *path) {
  struct iovec iov = {
    .iov_base = (uint8_t *)data,
    .iov_len = datalen,
  };
  struct msghdr msg = {0};
  ssize_t nwrite;

  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;

  if (!g_network_available) return 0; /* recovery timer retransmits after link returns */
  int fd = c->fd;
  if (path && path->local.addrlen &&
      (path->local.addrlen != c->local_addrlen ||
       memcmp(path->local.addr, &c->local_addr, c->local_addrlen) != 0)) {
    if (c->old_fd < 0 || path->local.addrlen != c->old_local_addrlen ||
        memcmp(path->local.addr, &c->old_local_addr, c->old_local_addrlen) != 0) return 0;
    fd = c->old_fd;
  }
#if AIOT_PIPELINE_PROFILE
  uint32_t socket_cpu_start=aiot_task_runtime_now();
  int64_t socket_wall_start=esp_timer_get_time();
  if(s_pipe_scope) pipe_wifi_track(data,datalen);
#endif
  do {
    nwrite = sendmsg(fd, &msg, 0);
  } while (nwrite == -1 && errno == EINTR);

#if AIOT_PIPELINE_PROFILE
  if(s_pipe_scope){s_pipe.socket_cpu+=(uint32_t)(aiot_task_runtime_now()-socket_cpu_start);
      s_pipe.socket_wall+=esp_timer_get_time()-socket_wall_start;++s_pipe.socket_calls;}
#endif
  if (nwrite == -1) {
    ESP_LOGE(TAG, "sendmsg: %s", strerror(errno));
    return -1;
  }

  return 0;
}

static size_t client_get_message(struct client *c, int64_t *pstream_id,
                                 int *pfin, ngtcp2_vec *datav,
                                 size_t datavcnt) {
  if (datavcnt == 0) {
    return 0;
  }

  if (c->stream.stream_id != -1 && c->tx.submitted < c->tx.queued) {
    *pstream_id = c->stream.stream_id;
    *pfin = 0;
    datav->len = quic_stream_buffer_pending(&c->tx, &datav->base);
    return 1;
  }

  *pstream_id = -1;
  *pfin = 0;
  datav->base = NULL;
  datav->len = 0;

  return 0;
}

static int client_write_streams(struct client *c) {
  ngtcp2_tstamp ts = timestamp();
  ngtcp2_pkt_info pi;
  ngtcp2_ssize nwrite;
  uint8_t buf[1452] __attribute__((aligned(4)));
  ngtcp2_path_storage ps;
  ngtcp2_vec datav;
  size_t datavcnt;
  int64_t stream_id;
  ngtcp2_ssize wdatalen;
  uint32_t flags;
  int fin;

  ngtcp2_path_storage_zero(&ps);
#if AIOT_MOQT_EXPERIMENT
  if (moq_begin_pump(c->conn) != 0) {
    ESP_LOGE(TAG, "failed to expire MOQT stream");
    return -1;
  }
#endif
#if AIOT_MOQT_BOUNDED_PUMP
  uint64_t pump_start=esp_timer_get_time();unsigned pump_packets=0;
#endif
  for (;;) {
#if AIOT_MOQT_BOUNDED_PUMP
    if(pump_packets>=8 || esp_timer_get_time()-pump_start>=20000)return 0;
    ++pump_packets;
    ts=timestamp();
#endif
#if AIOT_MOQT_EXPERIMENT
    datavcnt = moq_pending(&stream_id, &fin, &datav);
#else
    datavcnt = client_get_message(c, &stream_id, &fin, &datav, 1);
#endif

    flags = NGTCP2_WRITE_STREAM_FLAG_NONE;
    if (fin) {
      flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
    }

    nwrite = ngtcp2_conn_writev_stream(c->conn, &ps.path, &pi, buf, sizeof(buf),
                                       &wdatalen, flags, stream_id, &datav,
                                       datavcnt, ts);
    if (nwrite < 0) {
      switch (nwrite) {
      case NGTCP2_ERR_STREAM_DATA_BLOCKED:
#if AIOT_MOQT_EXPERIMENT
        moq_mark_blocked(stream_id);continue;
#endif
        return 0; /* preserve pending bytes until flow control opens */
      case NGTCP2_ERR_WRITE_MORE:
#if AIOT_MOQT_EXPERIMENT
        moq_submitted(stream_id,wdatalen);
#else
        c->tx.submitted += (size_t)wdatalen;
#endif
        continue;
      default:
        ESP_LOGE(TAG, "ngtcp2_conn_writev_stream: %s", ngtcp2_strerror((int)nwrite));
        ngtcp2_ccerr_set_liberr(&c->last_error, (int)nwrite, NULL, 0);
        return -1;
      }
    }

    if (nwrite == 0) {
      return 0;
    }

#if AIOT_MOQT_EXPERIMENT
    moq_submitted(stream_id,wdatalen);
#else
    if (wdatalen > 0) {
      c->tx.submitted += (size_t)wdatalen;
    }
#endif

#if AIOT_MOQT_EXPERIMENT && AIOT_MOQT_WMM
    {
      static int tos_fd = -1, tos_now = -1;
      /* a packet without new stream data carries acknowledgements, PING or control, or
       * retransmitted data of any stream: voice only when it is small */
      int tos = (wdatalen > 0 ? moq_stream_precedence(stream_id) : nwrite <= 128 ? 6 : 0) << 5;
      static unsigned tos_log;
      if (c->fd != tos_fd || tos != tos_now) {
        int rc = setsockopt(c->fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)), back = -1;
        socklen_t blen = sizeof(back);
        getsockopt(c->fd, IPPROTO_IP, IP_TOS, &back, &blen);
        if (tos_log < 8) {
          tos_log++;
          printf("[MOQ_WMM] {\"fd\":%d,\"tos\":%d,\"rc\":%d,\"errno\":%d,\"readback\":%d}\n", c->fd, tos, rc,
                 rc ? errno : 0, back);
        }
        if (rc == 0) {
          tos_fd = c->fd;
          tos_now = tos;
        }
      }
    }
#endif
    if (client_send_packet(c, buf, (size_t)nwrite, &ps.path) != 0) {
      break;
    }

    ngtcp2_conn_update_pkt_tx_time(c->conn, ts);
  }

  return 0;
}

static int client_handle_expiry(struct client *c);
static int client_write(struct client *c) {
  if (client_write_streams(c) != 0) {
    return -1;
  }

  return 0;
}

static int client_handle_expiry(struct client *c) {
  int rv = ngtcp2_conn_handle_expiry(c->conn, timestamp());
  if (rv != 0) {
    ESP_LOGE(TAG, "ngtcp2_conn_handle_expiry: %s", ngtcp2_strerror(rv));
    return -1;
  }

  return 0;
}

static void client_close(struct client *c) {
  ngtcp2_ssize nwrite;
  ngtcp2_pkt_info pi;
  ngtcp2_path_storage ps;
  uint8_t buf[1280] __attribute__((aligned(4)));

  if (ngtcp2_conn_in_closing_period(c->conn) ||
      ngtcp2_conn_in_draining_period(c->conn)) {
    goto fin;
  }

  ngtcp2_path_storage_zero(&ps);

  nwrite = ngtcp2_conn_write_connection_close(
    c->conn, &ps.path, &pi, buf, sizeof(buf), &c->last_error, timestamp());
  if (nwrite < 0) {
    ESP_LOGE(TAG, "ngtcp2_conn_write_connection_close: %s", ngtcp2_strerror((int)nwrite));
    goto fin;
  }

  client_send_packet(c, buf, (size_t)nwrite, &ps.path);

fin:
  ev_break(EV_DEFAULT, EVBREAK_ALL);
}

static void read_cb(struct ev_loop *loop, ev_io *w, int revents) {
  struct client *c = w->data;
  (void)loop;
  (void)revents;

  if (!quic_callback_enter(pdMS_TO_TICKS(10))) {
    return;
  }

  if (client_read(c) != 0) {
    /* Closing mutates the connection too; never race teardown/packet TX. */
    if (c->conn != NULL) client_close(c);
    quic_callback_leave();
    return;
  }

  quic_callback_leave();

  // Brief delay to avoid collision and allow other tasks to run
  vTaskDelay(pdMS_TO_TICKS(2));

  /* To make it simple, just have one writer thread in timer_cb
  if (client_write(c) != 0) {
    client_close(c);
  }
  */
}

static void timer_cb(struct ev_loop *loop, ev_timer *w, int revents) {
  (void)loop;
  (void)w;
  (void)revents;
}

static ngtcp2_conn *get_conn(ngtcp2_crypto_conn_ref *conn_ref) {
  struct client *c = conn_ref->user_data;
  return c->conn;
}

static int client_init(struct client *c) {
  struct sockaddr_storage remote_addr, local_addr;
  socklen_t remote_addrlen = sizeof(remote_addr), local_addrlen = sizeof(local_addr);

  memset(c, 0, sizeof(*c));
  c->fd = c->old_fd = -1;
  if (g_stream_storage == NULL)
      g_stream_storage = heap_caps_malloc(65536, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (g_stream_storage == NULL) return -1;
  c->tx.data = g_stream_storage;
  c->tx.capacity = 65536;

  ngtcp2_ccerr_default(&c->last_error);
  c->conn_ref.get_conn = get_conn;
  c->conn_ref.user_data = c;

  c->fd = create_sock((struct sockaddr *)&remote_addr, &remote_addrlen,
                      g_config.hostname, g_config.port);
  if (c->fd == -1) {
    return -1;
  }

  if (connect_sock((struct sockaddr *)&local_addr, &local_addrlen, c->fd,
                   (struct sockaddr *)&remote_addr, remote_addrlen) != 0) {
    return -1;
  }

  memcpy(&c->local_addr, &local_addr, sizeof(c->local_addr));
  c->local_addrlen = local_addrlen;

  ESP_LOGI("DEBUG", "EXP-F1: Socket/Path setup done. Checking heap...");
  // TEMP: if (!heap_caps_check_integrity_all(true)) {
  //   ESP_LOGE("DEBUG", "PANIC at F1! Heap corrupted BY socket or basic struct setup!");
  // }

  if (client_ssl_init(c) != 0) {
    return -1;
  }

  ESP_LOGI("DEBUG", "EXP-F2: TLS/WolfSSL Context setup done. Checking heap...");
  // TEMP: if (!heap_caps_check_integrity_all(true)) {
  //   ESP_LOGE("DEBUG", "PANIC at F2! Heap corrupted BY WolfSSL init!");
  // }

  if (client_quic_init(c, (struct sockaddr *)&remote_addr, remote_addrlen,
                       (struct sockaddr *)&local_addr, local_addrlen) != 0) {
    return -1;
  }

  c->stream.stream_id = -1;

  ev_io_init(&c->rev, read_cb, c->fd, EV_READ);
  c->rev.data = c;

  ev_timer_init(&c->timer, timer_cb, 0., 0.);
  c->timer.data = c;

  if (client_prepare_datagram_buffer(c) != 0) {
    return -1;
  }

  return 0;
}

/* Release everything client_init() acquired. NULL/zero-safe, so it is also
 * correct for a client that failed part-way through initialization. */
static void client_free(struct client *c) {
  if (!c) {
    return;
  }

  ev_timer_stop(EV_DEFAULT, &c->timer);
  ev_timer_destroy(&c->timer);
  ev_io_stop(EV_DEFAULT, &c->rev);

  ngtcp2_conn_del(c->conn);
  c->conn = NULL;
  SSL_free(c->ssl);
  c->ssl = NULL;
  c->ssl_ctx = NULL;
  c->dgram_tx_buf = NULL;
  c->dgram_tx_buf_len = 0;

  /* The UDP socket outlived every previous teardown: each reconnect burned
   * one lwip fd until the pool was exhausted. */
  if (c->fd >= 0) {
    close(c->fd);
  }
  if (c->old_fd >= 0) close(c->old_fd);
  c->fd = c->old_fd = -1;
}

/* Tear the client down and return it to a pristine state for a re-init. */
static void client_release(struct client *c) {
  ESP_LOGI(TAG, "reconnect attempt=%lu releasing client resources",
           (unsigned long)g_quic_reconnect_attempts);
  client_free(c);
  log_internal_heap_state("after client_free before memset");
  memset(c, 0, sizeof(*c));
  c->fd = c->old_fd = -1;
}

static ssize_t client_write_application_data(struct client *c, const uint8_t *data, size_t datalen)
{
    if (!c || !c->conn || !data || !datalen) return -1;
    if (c->stream.stream_id < 0) {
        int64_t id;
        int rv = ngtcp2_conn_open_bidi_stream(c->conn, &id, NULL);
        if (rv != 0) return -2;
        c->stream.stream_id = id;
    }
    if (!quic_stream_buffer_push(&c->tx, data, datalen)) return -2;
    /* Success means accepted into the owned buffer, not peer delivery.
     * Sending occurs in process(); never report failure after accepting bytes. */
    return 0;
}

int client_read_application_data(struct client *c, uint8_t *buffer, size_t buffer_size, size_t *bytes_read) {
    *bytes_read = 0;
    ESP_LOGI(TAG, "client_read_application_data: buf_len=%zu read_pos=%zu req=%zu",
             app_recv_buffer_len, app_recv_buffer_read_pos, buffer_size);
    
    // First check if we have data in our buffer
    if (app_recv_buffer_len > app_recv_buffer_read_pos) {
        // We have unread data in the buffer
        size_t available = app_recv_buffer_len - app_recv_buffer_read_pos;
        size_t to_copy = (available < buffer_size) ? available : buffer_size;
        ESP_LOGI(TAG, "client_read_application_data: draining buffered app data available=%zu copy=%zu",
                 available, to_copy);
        
        memcpy(buffer, app_recv_buffer + app_recv_buffer_read_pos, to_copy);
        app_recv_buffer_read_pos += to_copy;
        
        // Reset buffer if all data has been read
        if (app_recv_buffer_read_pos >= app_recv_buffer_len) {
            app_recv_buffer_len = 0;
            app_recv_buffer_read_pos = 0;
        }
        
        *bytes_read = to_copy;
        ESP_LOGI(TAG, "client_read_application_data: returned buffered %zu bytes", to_copy);
        return 0;
    }
    
    if (client_read(c) != 0) {
        ESP_LOGE(TAG, "client_read failed");
        return -1;
    }

    ESP_LOGI(TAG, "client_read_application_data: after client_read buf_len=%zu read_pos=%zu",
             app_recv_buffer_len, app_recv_buffer_read_pos);
    
    if (app_recv_buffer_len > 0) {
        return client_read_application_data(c, buffer, buffer_size, bytes_read);
    }

    ESP_LOGI(TAG, "client_read_application_data: no application data available");
    // No data available at this time
    return -2;  // Special code for no data
}

int recv_stream_data(ngtcp2_conn *conn, uint32_t flags,
                    int64_t stream_id, uint64_t offset,
                    const uint8_t *data, size_t datalen,
                    void *user_data, void *stream_user_data) {
    (void)user_data;
    (void)stream_user_data;
#if AIOT_MOQT_EXPERIMENT
    return moq_recv(conn,flags,stream_id,offset,data,datalen);
#endif
    ESP_LOGI(TAG, "recv_stream_data: stream=%lld flags=0x%lx offset=%llu datalen=%zu buf_len=%zu",
             (long long)stream_id, (unsigned long)flags,
             (unsigned long long)offset, datalen, app_recv_buffer_len);
    if (datalen > 0 && data != NULL) {
        char hex_str[65];
        size_t hex_len = datalen > 32 ? 32 : datalen;
        for (size_t i = 0; i < hex_len; ++i) {
            snprintf(&hex_str[i * 2], 3, "%02x", data[i]);
        }
        hex_str[hex_len * 2] = '\0';
        ESP_LOGI(TAG, "recv_stream_data: first bytes (%zu): %s%s",
                 hex_len, hex_str, datalen > hex_len ? "..." : "");
    }
    
    // Store the received data in our buffer
    if (datalen > 0 && app_recv_buffer_len + datalen <= APP_BUFFER_SIZE) {
        memcpy(app_recv_buffer + app_recv_buffer_len, data, datalen);
        app_recv_buffer_len += datalen;
        ESP_LOGI(TAG, "recv_stream_data: buffered %zu bytes, new buf_len=%zu",
                 datalen, app_recv_buffer_len);
    } else if (datalen > 0) {
        ESP_LOGE(TAG, "recv_stream_data: buffer overflow risk buf_len=%zu datalen=%zu cap=%d",
                 app_recv_buffer_len, datalen, APP_BUFFER_SIZE);
    }
    
    // Acknowledge the data was received by using ngtcp2_conn_extend_max_stream_offset
    int rv = ngtcp2_conn_extend_max_stream_offset(conn, stream_id, datalen);
    if (rv != 0) {
        ESP_LOGE(TAG, "ngtcp2_conn_extend_max_stream_offset: %s", ngtcp2_strerror(rv));
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    
    return 0;
}

// Non-blocking QUIC client functions
static int quic_client_init_locked(const quic_client_config_t *config) {
    // Initialize mutex for thread safety
    if (quic_mutex == NULL) {
        quic_mutex = xSemaphoreCreateMutex();
        if (quic_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create QUIC mutex");
            return -1;
        }
        ESP_LOGI(TAG, "QUIC mutex created successfully");
    }
    
    /* Release whatever a previous attempt left behind before reusing the
     * client; a bare memset here would strand its socket and timer. */
    client_release(&g_client);
#if AIOT_MOQT_EXPERIMENT
    moq_reset();
#endif
    log_internal_heap_state("after client_release");
    quic_processing = false;
    g_quic_handshake_start_us = esp_timer_get_time();
    g_quic_handshake_done_us = 0;
    g_quic_handshake_time_ms = 0;
    g_migration_status = 0;
    app_recv_buffer_len = app_recv_buffer_read_pos = 0;
    g_dgram_acked = 0;
    g_dgram_lost = 0;
    
    if (config) {
        if (strlen(config->hostname) >= sizeof(g_host_copy) || strlen(config->port) >= sizeof(g_port_copy) ||
            strlen(config->alpn) >= sizeof(g_alpn_copy)) return -1;
        strcpy(g_host_copy, config->hostname); strcpy(g_port_copy, config->port); strcpy(g_alpn_copy, config->alpn);
        g_config.hostname = g_host_copy; g_config.port = g_port_copy; g_config.alpn = g_alpn_copy;
        ESP_LOGI(TAG, "QUIC client config: %s:%s with ALPN %s", 
               g_config.hostname, g_config.port, g_config.alpn);
    }

    g_quic_reconnect_attempts++;
    ESP_LOGI(TAG, "reconnect attempt=%lu starting init",
             (unsigned long)g_quic_reconnect_attempts);

    ESP_LOGI(TAG, "init random number generator");
    srandom((unsigned int)timestamp());

    log_internal_heap_state("before ev_default_loop_init");

    // Initialize the event loop (non-blocking)
    esp_err_t ev_ret = ev_default_loop_init();
    if (ev_ret != ESP_OK) {
        ESP_LOGE(TAG, "ev_default_loop_init failed: %s", esp_err_to_name(ev_ret));
        return -1;
    }

    log_internal_heap_state("after ev_default_loop_init");
    ESP_LOGI("DEBUG", "EXP-E3: Event/Network Base Initialized. Checking heap...");
  // TEMP: if (!heap_caps_check_integrity_all(true)) {
  //   ESP_LOGE("DEBUG", "PANIC at E3! Heap corrupted BY Event Loop or Wi-Fi setup!");
  // }

    ESP_LOGI(TAG, "init client ...");

    if (client_init(&g_client) != 0) {
        log_internal_heap_state("client_init failed");
        ESP_LOGE(TAG, "client_init failed");
        client_release(&g_client);
        return -1;
    }

    g_quic_connected = false;
    g_quic_handshake_completed = false;
    log_internal_heap_state("after client_init");
    
    ESP_LOGI(TAG, "QUIC client initialization completed");
    ESP_LOGI("DEBUG", "EXP-E4: QUIC Init Done. Checking heap before main loop...");
  // TEMP: if (!heap_caps_check_integrity_all(true)) {
  //   ESP_LOGE("DEBUG", "PANIC at E4! Heap corrupted BY QUIC setup/allocations!");
  // }
    return 0;
}

int quic_client_init_with_config(const quic_client_config_t *config)
{
    if (quic_mutex == NULL) quic_mutex = xSemaphoreCreateMutex();
    if (quic_mutex == NULL) return -1;
    quic_lock_take( portMAX_DELAY);
    int ret = quic_client_init_locked(config);
    quic_lock_give();
    return ret;
}

#if AIOT_QUIC_BATCH_EXPERIMENT
#define quic_client_process quic_client_process_impl
#endif
int quic_client_process(void) {
    if (!g_network_available) return 0;
    // The application loop yields; retain a one-tick bound for other callers.
    vTaskDelay(1);
    
    // Check if mutex is available
    if (quic_mutex == NULL) {
        ESP_LOGE(TAG, "QUIC mutex not initialized");
        return -1;
    }
    
    // Try to acquire mutex with timeout (50ms)
    if (quic_lock_take( pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire QUIC mutex, skipping this cycle");
        return 0; // Don't consider this an error, just skip
    }
    
    int result = 0;
    
    // Check if already processing to prevent reentrancy
    if (quic_processing) {
        ESP_LOGI(TAG, "QUIC processing already in progress, skipping");
        quic_lock_give();
        return 0;
    }
    
    // Set processing flag
    quic_processing = true;
    
    // Check if we have a valid connection
    if (!g_client.conn) {
        result = -1;
        goto cleanup;
    }
    
    // Check connection state before processing
    if (ngtcp2_conn_in_closing_period(g_client.conn) || 
        ngtcp2_conn_in_draining_period(g_client.conn)) {
        ESP_LOGI(TAG, "Connection is closing/draining, skipping processing");
        result = -1;
        goto cleanup;
    }

    if (ngtcp2_conn_get_expiry(g_client.conn) <= timestamp()) {
        result = client_handle_expiry(&g_client);
        if (result != 0) {
            ESP_LOGE(TAG, "client_handle_expiry failed: %d", result);
            goto cleanup;
        }
    }


    // Read from the socket and handle packets
    result = client_read(&g_client);
    if (result != 0) {
        ESP_LOGE(TAG, "client_read failed: %d", result);
        goto cleanup;
    }
    
    // Write any pending data
    result = client_write(&g_client);
    if (result != 0) {
        ESP_LOGE(TAG, "client_write failed: %d", result);
        goto cleanup;
    }

cleanup:
    quic_processing = false;
    quic_lock_give();
    return result;
}

bool quic_client_is_connected(void) {
    return g_quic_connected && g_quic_handshake_completed && g_client.conn != NULL;
}

void quic_client_get_metrics(uint32_t *rtt_ms, uint32_t *cwnd, uint32_t *bytes_in_flight) {
    if (rtt_ms) *rtt_ms = 0;
    if (cwnd) *cwnd = 0;
    if (bytes_in_flight) *bytes_in_flight = 0;

    if (!g_quic_connected || g_client.conn == NULL) {
        return;
    }

    if (quic_mutex != NULL && quic_lock_take( pdMS_TO_TICKS(10)) == pdTRUE) {
        ngtcp2_conn_info cinfo;
        ngtcp2_conn_get_conn_info(g_client.conn, &cinfo);
        
        if (rtt_ms) *rtt_ms = (uint32_t)(cinfo.smoothed_rtt / NGTCP2_MILLISECONDS);
        if (cwnd) *cwnd = (uint32_t)cinfo.cwnd;
        if (bytes_in_flight) *bytes_in_flight = (uint32_t)cinfo.bytes_in_flight;
        
        quic_lock_give();
    }
}

uint32_t quic_client_get_handshake_time_ms(void) {
    return g_quic_handshake_time_ms;
}

void quic_client_get_datagram_stats(uint32_t *acked, uint32_t *lost) {
    if (acked) {
        *acked = g_dgram_acked;
    }
    if (lost) {
        *lost = g_dgram_lost;
    }
}

int quic_client_local_stream_avail(void) {
    if (g_client.conn == NULL) {
        return 0;
    }

    g_quic_n_local_streams = ngtcp2_conn_get_streams_bidi_left(g_client.conn);
    return g_quic_n_local_streams > 0;
}

void quic_client_cleanup(void) {
    ESP_LOGI(TAG, "Cleaning up QUIC client...");
    ESP_LOGI(TAG, "reconnect attempt=%lu cleanup begin",
             (unsigned long)g_quic_reconnect_attempts);
    
    // Acquire mutex before cleanup
    if (quic_mutex != NULL) {
        quic_lock_take( portMAX_DELAY);
    }
    
    if (g_client.conn) {
        ESP_LOGI(TAG, "Freeing QUIC connection...");
    }
    /* Unconditional: a connection that never reached ngtcp2 still holds a
     * socket, a TLS context and a timer. */
#if AIOT_MOQT_EXPERIMENT
    if(g_client.conn && moq_finished){
        ngtcp2_ccerr_set_application_error(&g_client.last_error,0,NULL,0);
        client_close(&g_client);
    }
#endif
    client_release(&g_client);
#if AIOT_MOQT_EXPERIMENT
    moq_reset();
#endif


    g_quic_connected = false;
    g_quic_handshake_completed = false;
    g_migration_status = 0;
    quic_processing = false;
    
    // Keep the mutex across reconnects to avoid alloc/free churn in long-lived sessions.
    if (quic_mutex != NULL) {
        quic_lock_give();
        ESP_LOGI(TAG, "QUIC mutex preserved for reconnect reuse");
    }
    log_internal_heap_state("after quic_client_cleanup");
    
    ESP_LOGI(TAG, "QUIC client cleanup completed. Free heap: %lu bytes", esp_get_free_heap_size());
}

// Thread-safe wrapper for QUIC write operations
#if AIOT_QUIC_BATCH_EXPERIMENT
#define quic_client_write_safe quic_client_write_safe_impl
#endif
int quic_client_write_safe(const uint8_t *data, size_t datalen) {
    if (quic_mutex == NULL) {
        ESP_LOGE(TAG, "QUIC mutex not initialized");
        return -1;
    }
    
    if (data == NULL || datalen == 0) {
        ESP_LOGE(TAG, "Invalid write parameters");
        return -1;
    }
    
    // Acquire mutex with timeout
    if (quic_lock_take( pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire QUIC mutex for write");
        return -1;
    }
    
    int result = -1;
    
    // Check if connection is valid
    if (!quic_client_is_connected()) {
        ESP_LOGE(TAG, "QUIC connection not ready for write");
        goto cleanup;
    }
    
    // Check if processing is ongoing
    if (quic_processing) {
        ESP_LOGE(TAG, "QUIC processing in progress, cannot write");
        goto cleanup;
    }
    
    // Perform the write operation
    result = client_write_application_data(&g_client, data, datalen);
    if (result == 0) {
        ESP_LOGI(TAG, "Successfully wrote %zu bytes to QUIC stream", datalen);
    } else {
        ESP_LOGE(TAG, "Failed to write data to QUIC stream: %d", result);
    }

cleanup:
    quic_lock_give();
    return result;
}

// Thread-safe wrapper for QUIC read operations
#if AIOT_QUIC_BATCH_EXPERIMENT
#define quic_client_read_safe quic_client_read_safe_impl
#endif
int quic_client_read_safe(uint8_t *buffer, size_t buffer_size, size_t *bytes_read) {
    if (quic_mutex == NULL) {
        ESP_LOGE(TAG, "QUIC mutex not initialized");
        return -1;
    }
    
    if (buffer == NULL || buffer_size == 0 || bytes_read == NULL) {
        ESP_LOGE(TAG, "Invalid read parameters");
        return -1;
    }
    
    *bytes_read = 0;
    ESP_LOGI(TAG, "quic_client_read_safe: request buffer_size=%zu", buffer_size);
    
    // Acquire mutex with timeout
    if (quic_lock_take( pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire QUIC mutex for read");
        return -1;
    }
    
    int result = client_read_application_data(&g_client, buffer, buffer_size, bytes_read);
    ESP_LOGI(TAG, "quic_client_read_safe: result=%d bytes_read=%zu", result, *bytes_read);

    // Drive loss detection even when the app only polls reads (e.g. while
    // waiting for CONNACK): without this a lost packet is never
    // retransmitted because nothing runs the ngtcp2 expiry timer.
    if (g_client.conn != NULL &&
        !ngtcp2_conn_in_closing_period(g_client.conn) &&
        !ngtcp2_conn_in_draining_period(g_client.conn) &&
        ngtcp2_conn_get_expiry(g_client.conn) <= timestamp()) {
        if (client_handle_expiry(&g_client) == 0) {
            (void)client_write(&g_client);
        } else {
            ESP_LOGE(TAG, "client_handle_expiry (read path) failed");
        }
    }

    // Release mutex
    quic_lock_give();

    return result;
}

#if AIOT_QUIC_BATCH_EXPERIMENT
#define quic_client_write_datagram_safe quic_client_write_datagram_safe_impl
#endif
int quic_client_write_datagram_safe(const uint8_t *data, size_t datalen,
                                    uint64_t dgram_id) {
    if (quic_mutex == NULL) {
        ESP_LOGE(TAG, "QUIC mutex not initialized");
        return -1;
    }
    if (!g_network_available) return -2;
    if (data == NULL || datalen == 0) {
        ESP_LOGE(TAG, "Invalid datagram write parameters");
        return -1;
    }
    int64_t perf_start = esp_timer_get_time();
    if (quic_lock_take( pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire QUIC mutex for datagram write");
        return -1;
    }
    int64_t perf_locked = esp_timer_get_time();
    uint64_t perf_encode_us = 0, perf_udp_us = 0;
    int result = -1;
    if (!quic_client_is_connected()) {
        ESP_LOGE(TAG, "QUIC connection not ready for datagram write");
        goto cleanup;
    }
    if (quic_processing) {
        ESP_LOGE(TAG, "QUIC processing in progress, cannot write datagram");
        goto cleanup;
    }
    ngtcp2_conn *conn = g_client.conn;
    if (conn == NULL) {
        ESP_LOGE(TAG, "QUIC connection is NULL");
        goto cleanup;
    }
    
    /* MQTT keepalive/diagnostics must not starve behind a continuous video burst.
     * Both paths own quic_mutex here; drain accepted STREAM bytes first. */
    if (g_client.tx.submitted < g_client.tx.queued) {
        if (client_write(&g_client) != 0) goto cleanup;
        if (g_client.tx.submitted < g_client.tx.queued) {
            result = -2;
            goto cleanup;
        }
    }

    // Reuse a per-client QUIC output buffer to avoid heap churn on every datagram.
    size_t dest_len = g_client.dgram_tx_buf_len;
    uint8_t *dest = g_client.dgram_tx_buf;
    if (!dest || dest_len < 1452U) {
        ESP_LOGE(TAG, "Datagram output buffer not ready");
        result = -1;
        goto cleanup;
    }

    ngtcp2_path_storage ps;
    ngtcp2_pkt_info pi;
    ngtcp2_path_storage_zero(&ps);
    int accepted = 0;
    ngtcp2_tstamp ts = timestamp();
    int64_t perf_encode_start = esp_timer_get_time();
    ngtcp2_ssize nwrite = ngtcp2_conn_write_datagram(
        conn, &ps.path, &pi, dest, dest_len,
        &accepted, NGTCP2_WRITE_DATAGRAM_FLAG_NONE,
        dgram_id, data, datalen, ts);
    perf_encode_us = esp_timer_get_time() - perf_encode_start;
    if (nwrite < 0) {
        ESP_LOGE(TAG, "ngtcp2_conn_write_datagram: %s", ngtcp2_strerror((int)nwrite));
        result = -1;
        goto cleanup;
    }
    if (nwrite == 0) {
        ESP_LOGD(TAG, "No datagram data written (budget exhausted or connection not ready)");
        result = -2;
        goto cleanup;
    }
    int64_t perf_udp_start = esp_timer_get_time();
    int perf_udp_result = client_send_packet(&g_client, dest, (size_t)nwrite, &ps.path);
    perf_udp_us = esp_timer_get_time() - perf_udp_start;
    if (perf_udp_result != 0) {
        ESP_LOGE(TAG, "Failed to send datagram UDP packet");
        result = -1;
        goto cleanup;
    }
    ngtcp2_conn_update_pkt_tx_time(conn, ts);
    ESP_LOGI(TAG, "Sent QUIC datagram: %zu bytes (dgram_id=%llu)", (size_t)nwrite, (unsigned long long)dgram_id);
    result = accepted ? 0 : -2;
cleanup:
#if AIOT_MOBILITY_CAMERA
    /* Aggregate under the connection lock; no per-packet serial output. */
    static uint64_t calls, ok, refused, payload_bytes, lock_us, encode_us, udp_us, total_us;
    static int64_t perf_last;
    int64_t perf_end = esp_timer_get_time();
    calls++; ok += result == 0; refused += result == -2;
    if (result == 0) payload_bytes += datalen;
    lock_us += perf_locked - perf_start; encode_us += perf_encode_us;
    udp_us += perf_udp_us; total_us += perf_end - perf_start;
    bool report = perf_end - perf_last >= 1000000;
    uint64_t report_values[] = {calls, ok, refused, payload_bytes, lock_us, encode_us, udp_us, total_us};
    if (report) {
        calls = ok = refused = payload_bytes = lock_us = encode_us = udp_us = total_us = 0;
        perf_last = perf_end;
    }
#endif
    quic_lock_give();
#if AIOT_MOBILITY_CAMERA
    if (report) printf("[DGRAM_PERF] {\"us\":%lld,\"calls\":%llu,\"ok\":%llu,\"refused\":%llu,\"bytes\":%llu,\"lock_us\":%llu,\"encode_us\":%llu,\"udp_us\":%llu,\"total_us\":%llu}\n",
        (long long)perf_end, (unsigned long long)report_values[0], (unsigned long long)report_values[1],
        (unsigned long long)report_values[2], (unsigned long long)report_values[3],
        (unsigned long long)report_values[4], (unsigned long long)report_values[5],
        (unsigned long long)report_values[6], (unsigned long long)report_values[7]);
#endif
    return result;
}

#if AIOT_QUIC_BATCH_EXPERIMENT
#include "quic_batch.inc"
#endif

void quic_client_set_network_available(bool available) { g_network_available = available; }
int quic_client_migration_status(void) { return g_migration_status; }
uint32_t quic_client_session_id(void) { return g_quic_reconnect_attempts; }

int quic_client_migrate(uint32_t ipv4)
{
    if (!g_network_available || !quic_mutex ||
        quic_lock_take( pdMS_TO_TICKS(100)) != pdTRUE) return -1;
    int result = -1, fd = -1;
    struct client *c = &g_client;
    if (!quic_client_is_connected() || g_migration_status == 1) goto done;
    const ngtcp2_path *current = ngtcp2_conn_get_path(c->conn);
    if (current->remote.addr->sa_family != AF_INET) goto done;
    struct sockaddr_storage remote = {0}, local = {0};
    socklen_t remote_len = current->remote.addrlen, local_len = sizeof(local);
    memcpy(&remote, current->remote.addr, remote_len);
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) goto done;
    struct sockaddr_in bind_addr = {.sin_family = AF_INET, .sin_addr.s_addr = ipv4};
    if (bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0 ||
        connect_sock((struct sockaddr *)&local, &local_len, fd,
                     (struct sockaddr *)&remote, remote_len) != 0) goto done;
    ngtcp2_path path = {.local = {(struct sockaddr *)&local, local_len},
                       .remote = {(struct sockaddr *)&remote, remote_len}};
    result = ngtcp2_conn_initiate_immediate_migration(c->conn, &path, timestamp());
    if (result != 0) goto done;
    if (c->old_fd >= 0) close(c->old_fd);
    c->old_fd = c->fd; c->old_local_addr = c->local_addr; c->old_local_addrlen = c->local_addrlen;
    c->fd = fd; fd = -1; c->local_addr = local; c->local_addrlen = local_len;
    g_migration_status = 1;
    g_migration_started_us = esp_timer_get_time();
    ESP_LOGI("MOBILITY", "event=migration_start session=%lu t_us=%llu",
        (unsigned long)g_quic_reconnect_attempts, (unsigned long long)g_migration_started_us);
done:
    if (fd >= 0) close(fd);
    quic_lock_give();
    return result;
}

#if AIOT_QUIC_BATCH_EXPERIMENT
#undef quic_client_process
int quic_client_process(void) {
    uint32_t start=aiot_task_runtime_now();
    int result=quic_client_process_impl();
    uint32_t cpu=aiot_task_runtime_now()-start;
    portENTER_CRITICAL(&s_lock_stats_mux);
    s_other_quic_cpu_us+=cpu;
    portEXIT_CRITICAL(&s_lock_stats_mux);
    return result;
}
#undef quic_client_read_safe
int quic_client_read_safe(uint8_t *buffer, size_t buffer_size, size_t *bytes_read) {
    uint32_t start=aiot_task_runtime_now();
    int result=quic_client_read_safe_impl(buffer,buffer_size,bytes_read);
    uint32_t cpu=aiot_task_runtime_now()-start;
    portENTER_CRITICAL(&s_lock_stats_mux);
    s_other_quic_cpu_us+=cpu;
    portEXIT_CRITICAL(&s_lock_stats_mux);
    return result;
}
#undef quic_client_write_safe
int quic_client_write_safe(const uint8_t *data, size_t datalen) {
    uint32_t start=aiot_task_runtime_now();
    int result=quic_client_write_safe_impl(data,datalen);
    uint32_t cpu=aiot_task_runtime_now()-start;
    portENTER_CRITICAL(&s_lock_stats_mux);
    s_other_quic_cpu_us+=cpu;
    portEXIT_CRITICAL(&s_lock_stats_mux);
    return result;
}
#undef quic_client_write_datagram_safe
int quic_client_write_datagram_safe(const uint8_t *data, size_t datalen, uint64_t dgram_id) {
    uint32_t start=aiot_task_runtime_now();
    int result=quic_client_write_datagram_safe_impl(data,datalen,dgram_id);
    uint32_t cpu=aiot_task_runtime_now()-start;
    portENTER_CRITICAL(&s_lock_stats_mux);
    s_other_quic_cpu_us+=cpu;
    portEXIT_CRITICAL(&s_lock_stats_mux);
    return result;
}
#endif
