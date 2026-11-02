/* QUIC server endpoints for the MOQT relay: ngtcp2 1.13 with the OpenSSL 3.5+ backend.
 * IPv4 only. A listener bound to 0.0.0.0 learns the local address of each packet from
 * IP_PKTINFO, so one listener serves several network namespaces' veth addresses. */
#define _GNU_SOURCE
#include "quic.h"
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
int quic_cc_algo=NGTCP2_CC_ALGO_CUBIC;

#define SCIDLEN 18
#define MAX_LISTENERS 4
#define PKT 1500

typedef struct { int fd; const qhandler *h; void (*accepted)(qconn *); } listener;
static listener listeners[MAX_LISTENERS];
static unsigned nlisteners;
static SSL_CTX *ssl_ctx;
static uint8_t alpn_wire[64];
static size_t alpn_len;
static uint64_t idle_timeout_ms;
static uint8_t static_secret[32];
static qconn *conns;
static unsigned conn_serial;

uint64_t quic_now_us(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000ULL+(uint64_t)t.tv_nsec/1000;}
static ngtcp2_tstamp now_ts(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*NGTCP2_SECONDS+(uint64_t)t.tv_nsec;}
qconn *quic_conns(void){return conns;}

static int alpn_select(SSL *ssl,const unsigned char **out,unsigned char *outlen,const unsigned char *in,unsigned inlen,void *arg){
 (void)ssl;(void)arg;
 for(unsigned i=0;i<inlen;i+=1U+in[i])
  if(in[i]==alpn_len-1&&i+1U+in[i]<=inlen&&!memcmp(in+i+1,alpn_wire+1,alpn_len-1)){*out=in+i+1;*outlen=in[i];return SSL_TLSEXT_ERR_OK;}
 return SSL_TLSEXT_ERR_ALERT_FATAL;
}

int quic_init(const char *cert,const char *key,const char *alpn,uint64_t idle_ms){
 if(ngtcp2_crypto_ossl_init()!=0)return -1;
 ssl_ctx=SSL_CTX_new(TLS_server_method());
 if(!ssl_ctx)return -1;
 SSL_CTX_set_min_proto_version(ssl_ctx,TLS1_3_VERSION);
 SSL_CTX_set_max_proto_version(ssl_ctx,TLS1_3_VERSION);
 if(SSL_CTX_use_certificate_chain_file(ssl_ctx,cert)!=1||SSL_CTX_use_PrivateKey_file(ssl_ctx,key,SSL_FILETYPE_PEM)!=1){
  ERR_print_errors_fp(stderr);return -1;
 }
 SSL_CTX_set_alpn_select_cb(ssl_ctx,alpn_select,NULL);
 alpn_len=strlen(alpn)+1;
 if(alpn_len>sizeof(alpn_wire))return -1;
 alpn_wire[0]=(uint8_t)(alpn_len-1);memcpy(alpn_wire+1,alpn,alpn_len-1);
 idle_timeout_ms=idle_ms;
 if(getrandom(static_secret,sizeof(static_secret),0)!=sizeof(static_secret))return -1;
 return 0;
}

int quic_listen(const char *ip,int port,const qhandler *h,void (*accepted)(qconn *)){
 if(nlisteners>=MAX_LISTENERS)return -1;
 int fd=socket(AF_INET,SOCK_DGRAM|SOCK_NONBLOCK,0);
 if(fd<0)return -1;
 int on=1;
 setsockopt(fd,IPPROTO_IP,IP_PKTINFO,&on,sizeof(on));
 int buf=4<<20;setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&buf,sizeof(buf));setsockopt(fd,SOL_SOCKET,SO_SNDBUF,&buf,sizeof(buf));
 struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons((uint16_t)port)};
 if(inet_pton(AF_INET,ip,&a.sin_addr)!=1||bind(fd,(struct sockaddr *)&a,sizeof(a))!=0){close(fd);return -1;}
 listeners[nlisteners++]=(listener){fd,h,accepted};
 return 0;
}

/* -- ngtcp2 callbacks --------------------------------------------------------------- */
static ngtcp2_conn *get_conn(ngtcp2_crypto_conn_ref *ref){return ((qconn *)ref->user_data)->conn;}
static void rand_cb(uint8_t *dest,size_t len,const ngtcp2_rand_ctx *ctx){(void)ctx;if(getrandom(dest,len,0)!=(ssize_t)len)memset(dest,0,len);}
static int new_cid_cb(ngtcp2_conn *conn,ngtcp2_cid *cid,uint8_t *token,size_t cidlen,void *user){
 (void)conn;(void)user;
 if(getrandom(cid->data,cidlen,0)!=(ssize_t)cidlen)return NGTCP2_ERR_CALLBACK_FAILURE;
 cid->datalen=cidlen;
 return ngtcp2_crypto_generate_stateless_reset_token(token,static_secret,sizeof(static_secret),cid)?NGTCP2_ERR_CALLBACK_FAILURE:0;
}
static int handshake_cb(ngtcp2_conn *conn,void *user){(void)conn;qconn *c=user;if(c->h->opened)c->h->opened(c);return 0;}
static int recv_stream_cb(ngtcp2_conn *conn,uint32_t flags,int64_t id,uint64_t offset,const uint8_t *data,size_t len,void *user,void *sud){
 (void)offset;(void)sud;qconn *c=user;
 if(c->h->data&&c->h->data(c,id,data,len,(flags&NGTCP2_STREAM_DATA_FLAG_FIN)!=0))return NGTCP2_ERR_CALLBACK_FAILURE;
 /* The data is consumed at once: return the flow control credit. */
 if(len){ngtcp2_conn_extend_max_stream_offset(conn,id,len);ngtcp2_conn_extend_max_offset(conn,len);}
 return 0;
}
static int acked_cb(ngtcp2_conn *conn,int64_t id,uint64_t offset,uint64_t len,void *user,void *sud){
 (void)conn;(void)sud;qconn *c=user;if(c->h->acked)c->h->acked(c,id,offset,len);return 0;
}
static int stream_close_cb(ngtcp2_conn *conn,uint32_t flags,int64_t id,uint64_t code,void *user,void *sud){
 (void)sud;qconn *c=user;
 if(c->h->closed)c->h->closed(c,id,(flags&NGTCP2_STREAM_CLOSE_FLAG_APP_ERROR_CODE_SET)!=0,code);
 /* Streams opened by the peer return their credit (RFC 9000, section 4.6). */
 if(!ngtcp2_conn_is_local_stream(conn,id)){
  if(ngtcp2_is_bidi_stream(id))ngtcp2_conn_extend_max_streams_bidi(conn,1);else ngtcp2_conn_extend_max_streams_uni(conn,1);
 }
 return 0;
}
static int stream_reset_cb(ngtcp2_conn *conn,int64_t id,uint64_t final_size,uint64_t code,void *user,void *sud){
 (void)conn;(void)final_size;(void)sud;qconn *c=user;if(c->h->reset)c->h->reset(c,id,code,false);return 0;
}
static int stop_sending_cb(ngtcp2_conn *conn,int64_t id,uint64_t code,void *user,void *sud){
 (void)conn;(void)sud;qconn *c=user;if(c->h->reset)c->h->reset(c,id,code,true);return 0;
}

static void conn_free(qconn *c){
 if(c->h->gone)c->h->gone(c);
 for(qconn **p=&conns;*p;p=&(*p)->next)if(*p==c){*p=c->next;break;}
 if(c->ssl)SSL_set_app_data(c->ssl,NULL);
 if(c->conn)ngtcp2_conn_del(c->conn);
 if(c->octx)ngtcp2_crypto_ossl_ctx_del(c->octx);
 if(c->ssl)SSL_free(c->ssl);
 free(c);
}

static int send_pkt(qconn *c,const uint8_t *data,size_t len){
 struct iovec iov={(void *)data,len};
 char cbuf[CMSG_SPACE(sizeof(struct in_pktinfo))]={0};
 struct msghdr m={.msg_name=&c->remote,.msg_namelen=c->remote_len,.msg_iov=&iov,.msg_iovlen=1,.msg_control=cbuf,.msg_controllen=sizeof(cbuf)};
 struct cmsghdr *cm=CMSG_FIRSTHDR(&m);
 cm->cmsg_level=IPPROTO_IP;cm->cmsg_type=IP_PKTINFO;cm->cmsg_len=CMSG_LEN(sizeof(struct in_pktinfo));
 struct in_pktinfo pi={0};pi.ipi_spec_dst=((struct sockaddr_in *)&c->local)->sin_addr;
 memcpy(CMSG_DATA(cm),&pi,sizeof(pi));
 for(;;){
  ssize_t n=sendmsg(c->fd,&m,0);
  if(n>=0)return 0;
  if(errno==EINTR)continue;
  /* A full socket buffer drops the packet; QUIC recovers it. */
  return errno==EAGAIN||errno==EWOULDBLOCK||errno==ENOBUFS?0:-1;
 }
}

static ngtcp2_path conn_path(qconn *c){
 return (ngtcp2_path){.local={(ngtcp2_sockaddr *)&c->local,c->local_len},.remote={(ngtcp2_sockaddr *)&c->remote,c->remote_len}};
}

static void start_close(qconn *c){
 if(c->closing||c->draining)return;
 c->closing=true;
 uint8_t buf[PKT];ngtcp2_path_storage ps;ngtcp2_path_storage_zero(&ps);ngtcp2_pkt_info pi;
 ngtcp2_ssize n=ngtcp2_conn_write_connection_close(c->conn,&ps.path,&pi,buf,sizeof(buf),&c->err,now_ts());
 if(n>0)send_pkt(c,buf,(size_t)n);
 c->close_at=quic_now_us()+3*ngtcp2_conn_get_pto(c->conn)/1000;
}

void quic_close(qconn *c,uint64_t app_code){
 ngtcp2_ccerr_set_application_error(&c->err,app_code,NULL,0);
 start_close(c);
}

static int write_conn(qconn *c){
 if(c->closing||c->draining)return 0;
 if(c->h->tick)c->h->tick(c,quic_now_us());
 uint8_t buf[PKT];ngtcp2_path_storage ps;ngtcp2_path_storage_zero(&ps);ngtcp2_pkt_info pi;
 size_t cap=ngtcp2_conn_get_max_tx_udp_payload_size(c->conn);if(cap>sizeof(buf))cap=sizeof(buf);
 for(unsigned guard=0;guard<4096;guard++){
  int64_t id=-1;ngtcp2_vec v={0};bool fin=false;
  int has=ngtcp2_conn_get_handshake_completed(c->conn)&&c->h->pull?c->h->pull(c,&id,&v,&fin):0;
  ngtcp2_ssize consumed=-1;ngtcp2_tstamp ts=now_ts();
  ngtcp2_ssize n=ngtcp2_conn_writev_stream(c->conn,&ps.path,&pi,buf,cap,&consumed,
    fin?NGTCP2_WRITE_STREAM_FLAG_FIN:NGTCP2_WRITE_STREAM_FLAG_NONE,has?id:-1,has?&v:NULL,has?1:0,ts);
  if(n<0){
   if(has&&(n==NGTCP2_ERR_STREAM_DATA_BLOCKED||n==NGTCP2_ERR_STREAM_SHUT_WR||n==NGTCP2_ERR_STREAM_NOT_FOUND)){
    if(c->h->blocked)c->h->blocked(c,id);
    continue;
   }
   fprintf(stderr,"conn %u: write %s\n",c->id,ngtcp2_strerror((int)n));
   ngtcp2_ccerr_set_liberr(&c->err,(int)n,NULL,0);start_close(c);return -1;
  }
  if(has&&consumed>=0&&c->h->pushed)c->h->pushed(c,id,(size_t)consumed,fin&&(size_t)consumed==v.len);
  if(n==0)break;
  if(send_pkt(c,buf,(size_t)n)){ngtcp2_ccerr_set_liberr(&c->err,NGTCP2_ERR_INTERNAL,NULL,0);start_close(c);return -1;}
  ngtcp2_conn_update_pkt_tx_time(c->conn,ts);
 }
 return 0;
}

static qconn *find_conn(const ngtcp2_version_cid *vc){
 for(qconn *c=conns;c;c=c->next){
  ngtcp2_cid ids[16];size_t n=ngtcp2_conn_get_scid(c->conn,NULL);
  if(n>16)n=16;
  ngtcp2_conn_get_scid(c->conn,ids);
  for(size_t i=0;i<n;i++)if(ids[i].datalen==vc->dcidlen&&!memcmp(ids[i].data,vc->dcid,vc->dcidlen))return c;
  /* Retransmitted Initial packets still carry the original destination ID. */
  const ngtcp2_cid *o=ngtcp2_conn_get_client_initial_dcid(c->conn);
  if(o&&o->datalen==vc->dcidlen&&!memcmp(o->data,vc->dcid,vc->dcidlen))return c;
 }
 return NULL;
}

static qconn *accept_conn(listener *l,const uint8_t *data,size_t len,const struct sockaddr_storage *local,socklen_t ll,
                          const struct sockaddr_storage *remote,socklen_t rl){
 ngtcp2_pkt_hd hd;
 if(ngtcp2_accept(&hd,data,len)!=0)return NULL;
 qconn *c=calloc(1,sizeof(*c));
 if(!c)return NULL;
 c->fd=l->fd;c->h=l->h;c->local=*local;c->local_len=ll;c->remote=*remote;c->remote_len=rl;c->id=++conn_serial;
 c->ref.get_conn=get_conn;c->ref.user_data=c;
 ngtcp2_ccerr_default(&c->err);
 c->scid.datalen=SCIDLEN;
 if(getrandom(c->scid.data,SCIDLEN,0)!=SCIDLEN){free(c);return NULL;}
 ngtcp2_callbacks cb={
  .recv_client_initial=ngtcp2_crypto_recv_client_initial_cb,.recv_crypto_data=ngtcp2_crypto_recv_crypto_data_cb,
  .handshake_completed=handshake_cb,.encrypt=ngtcp2_crypto_encrypt_cb,.decrypt=ngtcp2_crypto_decrypt_cb,
  .hp_mask=ngtcp2_crypto_hp_mask_cb,.recv_stream_data=recv_stream_cb,.acked_stream_data_offset=acked_cb,
  .stream_close=stream_close_cb,.rand=rand_cb,.get_new_connection_id=new_cid_cb,.update_key=ngtcp2_crypto_update_key_cb,
  .stream_reset=stream_reset_cb,.delete_crypto_aead_ctx=ngtcp2_crypto_delete_crypto_aead_ctx_cb,
  .delete_crypto_cipher_ctx=ngtcp2_crypto_delete_crypto_cipher_ctx_cb,
  .get_path_challenge_data=ngtcp2_crypto_get_path_challenge_data_cb,.stream_stop_sending=stop_sending_cb,
  .version_negotiation=ngtcp2_crypto_version_negotiation_cb,
 };
 ngtcp2_settings s;ngtcp2_settings_default(&s);
 s.initial_ts=now_ts();
 s.cc_algo=(ngtcp2_cc_algo)quic_cc_algo;
 ngtcp2_transport_params p;ngtcp2_transport_params_default(&p);
 p.initial_max_stream_data_bidi_local=1<<20;p.initial_max_stream_data_bidi_remote=1<<20;
 p.initial_max_stream_data_uni=1<<20;p.initial_max_data=16<<20;
 p.initial_max_streams_bidi=100;p.initial_max_streams_uni=1000;
 p.max_idle_timeout=idle_timeout_ms*NGTCP2_MILLISECONDS;
 p.max_datagram_frame_size=1200;
 p.active_connection_id_limit=7;
 p.stateless_reset_token_present=1;
 p.original_dcid=hd.dcid;p.original_dcid_present=1;
 ngtcp2_crypto_generate_stateless_reset_token(p.stateless_reset_token,static_secret,sizeof(static_secret),&c->scid);
 ngtcp2_path path=conn_path(c);
 if(ngtcp2_conn_server_new(&c->conn,&hd.scid,&c->scid,&path,hd.version,&cb,&s,&p,NULL,c)!=0){free(c);return NULL;}
 SSL *ssl=SSL_new(ssl_ctx);
 ngtcp2_crypto_ossl_ctx *octx=NULL;
 if(!ssl||ngtcp2_crypto_ossl_ctx_new(&octx,ssl)!=0||ngtcp2_crypto_ossl_configure_server_session(ssl)!=0){
  if(ssl)SSL_free(ssl);
  ngtcp2_conn_del(c->conn);free(c);return NULL;
 }
 SSL_set_app_data(ssl,&c->ref);SSL_set_accept_state(ssl);
 c->ssl=ssl;c->octx=octx;
 ngtcp2_conn_set_tls_native_handle(c->conn,octx);
 c->next=conns;conns=c;
 if(l->accepted)l->accepted(c);
 return c;
}

static void read_listener(listener *l){
 for(;;){
  uint8_t buf[65536];struct sockaddr_storage remote;
  char cbuf[CMSG_SPACE(sizeof(struct in_pktinfo))];
  struct iovec iov={buf,sizeof(buf)};
  struct msghdr m={.msg_name=&remote,.msg_namelen=sizeof(remote),.msg_iov=&iov,.msg_iovlen=1,.msg_control=cbuf,.msg_controllen=sizeof(cbuf)};
  ssize_t n=recvmsg(l->fd,&m,0);
  if(n<0)return;
  struct sockaddr_storage local={0};struct sockaddr_in *la=(struct sockaddr_in *)&local;
  socklen_t ll=sizeof(local);getsockname(l->fd,(struct sockaddr *)&local,&ll);
  for(struct cmsghdr *cm=CMSG_FIRSTHDR(&m);cm;cm=CMSG_NXTHDR(&m,cm))
   if(cm->cmsg_level==IPPROTO_IP&&cm->cmsg_type==IP_PKTINFO){struct in_pktinfo pi;memcpy(&pi,CMSG_DATA(cm),sizeof(pi));la->sin_addr=pi.ipi_addr;}
  ngtcp2_version_cid vc;
  if(ngtcp2_pkt_decode_version_cid(&vc,buf,(size_t)n,SCIDLEN)!=0)continue;
  qconn *c=find_conn(&vc);
  if(!c){c=accept_conn(l,buf,(size_t)n,&local,ll,&remote,m.msg_namelen);if(!c)continue;}
  if(c->closing||c->draining)continue;
  /* The client may move (NAT rebinding); ngtcp2 validates the new path. */
  struct sockaddr_storage old_remote=c->remote;socklen_t old_len=c->remote_len;
  c->remote=remote;c->remote_len=m.msg_namelen;c->local=local;c->local_len=ll;
  ngtcp2_path path=conn_path(c);ngtcp2_pkt_info pi={0};
  int rv=ngtcp2_conn_read_pkt(c->conn,&path,&pi,buf,(size_t)n,now_ts());
  if(rv==0)continue;
  c->remote=old_remote;c->remote_len=old_len;
  if(rv==NGTCP2_ERR_DRAINING){c->draining=true;c->close_at=quic_now_us()+3*ngtcp2_conn_get_pto(c->conn)/1000;continue;}
  if(rv==NGTCP2_ERR_DROP_CONN){conn_free(c);continue;}
  fprintf(stderr,"conn %u: read %s\n",c->id,ngtcp2_strerror(rv));
  if(rv==NGTCP2_ERR_CRYPTO)ngtcp2_ccerr_set_tls_alert(&c->err,ngtcp2_conn_get_tls_alert(c->conn),NULL,0);
  else ngtcp2_ccerr_set_liberr(&c->err,rv,NULL,0);
  start_close(c);
 }
}

void quic_run(volatile bool *stop,void (*idle)(uint64_t)){
 while(!*stop){
  uint64_t now=quic_now_us(),wake=now+100000;
  for(qconn *c=conns;c;c=c->next){
   uint64_t e=(c->closing||c->draining)?c->close_at:ngtcp2_conn_get_expiry(c->conn)/1000;
   if(e<wake)wake=e;
  }
  struct pollfd p[MAX_LISTENERS];
  for(unsigned i=0;i<nlisteners;i++)p[i]=(struct pollfd){listeners[i].fd,POLLIN,0};
  int64_t wait_us=(int64_t)wake-(int64_t)now;if(wait_us<0)wait_us=0;
  struct timespec ts={wait_us/1000000,(wait_us%1000000)*1000};
  if(ppoll(p,nlisteners,&ts,NULL)>0)
   for(unsigned i=0;i<nlisteners;i++)if(p[i].revents&POLLIN)read_listener(&listeners[i]);
  now=quic_now_us();
  for(qconn *c=conns,*next;c;c=next){
   next=c->next;
   if(c->closing||c->draining){if(now>=c->close_at)conn_free(c);continue;}
   if(ngtcp2_conn_get_expiry(c->conn)/1000<=now){
    int rv=ngtcp2_conn_handle_expiry(c->conn,now_ts());
    if(rv){
     if(rv==NGTCP2_ERR_IDLE_CLOSE)fprintf(stderr,"conn %u: idle timeout\n",c->id);
     else fprintf(stderr,"conn %u: expiry %s\n",c->id,ngtcp2_strerror(rv));
     conn_free(c);continue;
    }
   }
  }
  if(idle)idle(now);
  for(qconn *c=conns;c;c=c->next)write_conn(c);
 }
}
