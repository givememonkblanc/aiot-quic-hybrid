/* QUIC server endpoints on ngtcp2 with the OpenSSL backend, for the MOQT relay.
 * One poll loop serves every listener and connection. The application sees a connection
 * through a handler: received stream data, acknowledgements, stream ends, and a pull
 * function that names the stream to write next. */
#ifndef RELAY_QUIC_H
#define RELAY_QUIC_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <netinet/in.h>
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>

typedef struct qconn qconn;
typedef struct {
 void (*opened)(qconn *);                   /* handshake completed */
 int (*data)(qconn *,int64_t id,const uint8_t *,size_t,bool fin);
 void (*acked)(qconn *,int64_t id,uint64_t offset,uint64_t len);
 void (*closed)(qconn *,int64_t id,bool app_error,uint64_t code);
 void (*reset)(qconn *,int64_t id,uint64_t code,bool stop_sending);
 /* Next data to write: 1 with id, vec and fin; 0 when nothing is ready. A stream reported
  * blocked is not named again until the next write round. */
 int (*pull)(qconn *,int64_t *id,ngtcp2_vec *,bool *fin);
 void (*pushed)(qconn *,int64_t id,size_t n,bool fin);
 void (*blocked)(qconn *,int64_t id);
 void (*tick)(qconn *,uint64_t now_us);     /* before every write round */
 void (*gone)(qconn *);                     /* the connection is freed after this */
} qhandler;

struct qconn {
 ngtcp2_conn *conn; void *ssl,*octx;
 ngtcp2_crypto_conn_ref ref;
 ngtcp2_cid scid;
 int fd; struct sockaddr_storage local,remote; socklen_t local_len,remote_len;
 const qhandler *h; void *app;
 bool closing,draining; uint64_t close_at;
 ngtcp2_ccerr err;
 unsigned id;                                /* serial number, for the log */
 struct qconn *next;
};

/* Listener on ip:port; `accepted` is called for every new connection before its handshake. */
int quic_listen(const char *ip,int port,const qhandler *h,void (*accepted)(qconn *));
int quic_init(const char *cert,const char *key,const char *alpn,uint64_t idle_ms);
/* congestion control of every connection: NGTCP2_CC_ALGO_CUBIC (default) or NGTCP2_CC_ALGO_BBR */
extern int quic_cc_algo;
/* Runs until `*stop` becomes true. `idle` runs once per loop iteration. */
void quic_run(volatile bool *stop,void (*idle)(uint64_t now_us));
void quic_close(qconn *,uint64_t app_code);
uint64_t quic_now_us(void);                 /* CLOCK_MONOTONIC */
qconn *quic_conns(void);
#endif
