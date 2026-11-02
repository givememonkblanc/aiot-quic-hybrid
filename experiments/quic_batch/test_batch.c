/* Host test executes the production batching loop with a deterministic QUIC/
 * RTOS fake: fragmentation, pacing, deadlines and unlock ownership are real. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef int BaseType_t; typedef unsigned TickType_t;
typedef int64_t ngtcp2_tstamp; typedef long ngtcp2_ssize;
typedef struct { int path; } ngtcp2_path_storage;
typedef struct { int flags; } ngtcp2_pkt_info;
typedef struct { size_t max_datagram_frame_size; } ngtcp2_transport_params;
typedef struct {const uint8_t *data;size_t len;uint64_t id;} quic_batch_fragment_t;
#define QUIC_BATCH_MAX_FRAGMENTS 256
#define NGTCP2_WRITE_DATAGRAM_FLAG_NONE 0
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
static bool locked, g_network_available=true, quic_processing;
static int quic_mutex=1, s_lock_stats_mux;
static uint64_t s_lock_attempts,s_lock_acquired,s_lock_wait_us,s_lock_hold_us,s_lock_hold_max,s_other_quic_cpu_us;
static int64_t now;static uint32_t runtime,session;
static int pacing, refuse_forever, wire_packets, packet_failure;
static uint8_t output[1452];
static struct {void *conn; struct{size_t submitted,queued;}tx;uint8_t*dgram_tx_buf;size_t dgram_tx_buf_len;}g_client={(void*)1,{0,0},output,sizeof(output)};
static ngtcp2_transport_params tp={1200};
static int64_t esp_timer_get_time(void){return now;}
static uint32_t aiot_task_runtime_now(void){return runtime;}
static uint32_t quic_client_session_id(void){return session;}
static bool quic_client_is_connected(void){return true;}
static ngtcp2_tstamp timestamp(void){return now*1000;}
static BaseType_t quic_lock_take(TickType_t ticks){(void)ticks;assert(!locked);locked=true;return 1;}
static void quic_lock_give(void){assert(locked);locked=false;}
static void vTaskDelay(unsigned ticks){assert(!locked);now+=ticks*1000;}
static int client_write(void*c){(void)c;return 0;}
static void ngtcp2_path_storage_zero(ngtcp2_path_storage*p){memset(p,0,sizeof(*p));}
static const ngtcp2_transport_params *ngtcp2_conn_get_remote_transport_params(void*c){(void)c;return &tp;}
static void ngtcp2_conn_update_pkt_tx_time(void*c,ngtcp2_tstamp t){(void)c;(void)t;assert(locked);}
static int client_send_packet(void*c,const uint8_t*d,size_t n,const void*p){(void)c;(void)d;(void)n;(void)p;assert(locked);++wire_packets;return packet_failure?-1:0;}
static ngtcp2_ssize ngtcp2_conn_write_datagram(void*c,void*p,ngtcp2_pkt_info*pi,uint8_t*d,size_t cap,int*a,int flags,uint64_t id,const uint8_t*data,size_t len,ngtcp2_tstamp t){
 (void)c;(void)p;(void)pi;(void)d;(void)cap;(void)flags;(void)id;(void)data;(void)t;
 assert(locked);assert(len<=1180);now+=1000;runtime+=1000;
 if(refuse_forever || pacing-->0){*a=0;return 0;} *a=1;return len+30;
}
#include "../../src/components/transport/quic_batch.inc"
static void reset(void){memset(&s_batch,0,sizeof(s_batch));now=runtime=session=0;locked=false;pacing=refuse_forever=wire_packets=packet_failure=0;tp.max_datagram_frame_size=1200;}
int main(void){
 uint8_t data[1036]={0};quic_batch_fragment_t f[121];for(int i=0;i<121;i++)f[i]=(quic_batch_fragment_t){data,1036,(uint64_t)i};f[120].len=311;
 unsigned sizes[]={1,4,8,16,0};
 for(unsigned j=0;j<5;j++){
  reset();assert(quic_batch_set(sizes[j]));size_t limit=sizes[j]?sizes[j]:121;
  for(size_t off=0;off<121;off+=limit){size_t n=121-off<limit?121-off:limit;int ret;assert(quic_client_write_batch_safe(f+off,n,1000000,&ret)==n);assert(ret==0);assert(!locked);}
  assert(s_batch.fragments==121 && wire_packets==121);assert(s_batch.calls==(121+limit-1)/limit);
  if(limit<=16)assert(s_batch.locks==s_batch.calls);else assert(s_batch.locks==7);
  assert(s_batch.cpu_us==121000); /* waits/yields excluded */
 }
 int ret;reset();pacing=3;assert(quic_client_write_batch_safe(f,4,1000000,&ret)==4);assert(s_batch.refused==3 && wire_packets==4 && !locked);
 reset();refuse_forever=1;assert(quic_client_write_batch_safe(f,4,1000000,&ret)==0);assert(ret==-2 && s_batch.dropped==4 && !locked);
 reset();assert(quic_client_write_batch_safe(f,16,5000,&ret)==5);assert(ret==-3 && s_batch.dropped==11 && !locked);
 reset();f[0].len=4096;assert(quic_client_write_batch_safe(f,1,1000000,&ret)==0);assert(ret==-1 && wire_packets==0 && !locked);f[0].len=1036;
 reset();tp.max_datagram_frame_size=1038;assert(quic_client_write_batch_safe(f,1,1000000,&ret)==0);assert(wire_packets==0 && ret==-1);
 reset();packet_failure=1;assert(quic_client_write_batch_safe(f,4,1000000,&ret)==0);assert(ret==-1 && !locked);
 assert(!quic_batch_set(3));puts("PASS: five group sizes, packetization, pacing, expiry, payload limits, send failure, lock ownership, CPU accounting");
}
