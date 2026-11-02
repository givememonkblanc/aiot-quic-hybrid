#include "moq_audit.h"
#if AIOT_MOQT_AUDIT
#define printf moq_audit_printf
#endif
/* MOQT draft-21 Original Publisher on ngtcp2. Protocol state and wire encoding
 * live in the portable core (moqt_core.c); this file owns QUIC streams, payload
 * memory and scheduling. Buffers stay immutable until they are acknowledged.
 * Not a relay. Section numbers refer to draft-ietf-moq-transport-21. */
#include "moqt21.h"
#include "moqt_core.h"
#include "moqt_tx_policy.h"
#include "moqt_feasibility.h"
#include "moqt_swma.h"
#include "moqt_scheduler.h"
#ifndef AIOT_MOQT_CACHE_KB
#define AIOT_MOQT_CACHE_KB 1024
#endif
#ifndef AIOT_MOQT_CACHE_OBJECTS
#define AIOT_MOQT_CACHE_OBJECTS 16
#endif
#ifndef AIOT_MOQT_RESULT_CACHE_OBJECTS
#define AIOT_MOQT_RESULT_CACHE_OBJECTS 32
#endif
#ifndef AIOT_MOQT_BEHIND_LIMIT
#define AIOT_MOQT_BEHIND_LIMIT 0
#endif
#ifndef AIOT_MOQT_SUBSCRIPTION_LIFETIME_MS
#define AIOT_MOQT_SUBSCRIPTION_LIFETIME_MS 0
#endif
#ifndef AIOT_MOQT_EXT_LIMITS
#define AIOT_MOQT_EXT_LIMITS 0
#endif
#ifndef AIOT_MOQT_EXT_SOURCE_TIMEOUT
#define AIOT_MOQT_EXT_SOURCE_TIMEOUT 0
#endif
#ifndef AIOT_MOQT_WIRE_POLICY
#define AIOT_MOQT_WIRE_POLICY 6
#endif
static int moq_policy=-1;
static moqt_sched moq_scheduler;
static moqt_rate moq_service_rate;
/* ACK service intervals include queue/ACK waiting. This is intentionally
 * labelled an adaptation of SWMAth math, not original receiver estimation. */
static moqt_swma moq_swma_rate;
static uint64_t moq_completed[2],moq_on_time[2],moq_age_max[2],moq_age_sum[2];
static uint64_t moq_rejected_feasibility,moq_probes,moq_last_probe;
static size_t moq_min_internal,moq_min_psram;
static uint64_t moq_codec_calls,moq_codec_failures,moq_codec_wall_sum,moq_codec_wall_max;
static size_t moq_codec_scratch_max;
static uint64_t moq_codec_decode,moq_codec_encode,moq_codec_cpu,moq_pump_gap;
void quic_client_moq_pump_gap_observed(uint64_t gap){moq_pump_gap=gap;}
static unsigned moq_source_total,moq_source_failures;
static uint64_t moq_source_span,moq_source_gap;
void quic_client_moq_source_observed(unsigned count,unsigned failed,uint64_t span_us,uint64_t max_gap_us){
 moq_source_total=count;moq_source_failures=failed;moq_source_span=span_us;moq_source_gap=max_gap_us;
}
void quic_client_moq_codec_observed(uint64_t decode,uint64_t encode,uint32_t cpu,bool ok,size_t scratch){
 uint64_t wall_us=decode+encode;moq_codec_decode+=decode;moq_codec_encode+=encode;moq_codec_cpu+=cpu;
 moq_codec_calls++;moq_codec_failures+=!ok;moq_codec_wall_sum+=wall_us;
 if(wall_us>moq_codec_wall_max)moq_codec_wall_max=wall_us;
 if(scratch>moq_codec_scratch_max)moq_codec_scratch_max=scratch;
}
static void moq_resource_sample(void) {
 size_t a=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
 size_t b=heap_caps_get_free_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
 if(a<moq_min_internal)moq_min_internal=a;
 if(b<moq_min_psram)moq_min_psram=b;
}
/* Estimator 2: bytes acknowledged for image objects in windows of one second while an
 * object is under way, so that the rate is known before the object ends. */
#if AIOT_MOQT_RATE_ESTIMATOR == 2
static struct {uint64_t start,opened;size_t bytes,total;bool sampled;} moq_rate_window;
static void moq_rate_window_open(uint64_t now){
 moq_rate_window.start=moq_rate_window.opened=now;moq_rate_window.bytes=moq_rate_window.total=0;moq_rate_window.sampled=false;
}
static void moq_rate_window_acked(size_t bytes,uint64_t now,bool closing){
 if(!moq_rate_window.start)return;
 moq_rate_window.bytes+=bytes;moq_rate_window.total+=bytes;
 uint64_t elapsed=now-moq_rate_window.start;
 if(closing){
  /* The rest of a window counts when it is long enough. An object that ended within its
   * first window gives one sample over its whole time. A short rest gives none: its
   * bytes were acknowledged in a burst and say nothing about the rate. */
  if(elapsed>=500000&&moq_rate_window.bytes)moqt_rate_observe_window(&moq_service_rate,moq_rate_window.bytes,elapsed);
  else if(!moq_rate_window.sampled&&moq_rate_window.total&&now-moq_rate_window.opened>=200000)
   moqt_rate_observe_window(&moq_service_rate,moq_rate_window.total,now-moq_rate_window.opened);
  memset(&moq_rate_window,0,sizeof(moq_rate_window));
 }else if(elapsed>=1000000){
  moqt_rate_observe_window(&moq_service_rate,moq_rate_window.bytes,elapsed);
  moq_rate_window.start=now;moq_rate_window.bytes=0;moq_rate_window.sampled=true;
 }
}
#endif
uint64_t quic_client_moq_service_rate(void){
#if AIOT_MOQT_RATE_ESTIMATOR == 1
 return moq_swma_rate.updates?(uint64_t)moq_swma_rate.value:64000;
#else
 return moqt_rate_value(&moq_service_rate);
#endif
}
uint64_t quic_client_moq_service_deviation(void){return moqt_rate_deviation(&moq_service_rate);}
/* The smallest round trip of the connection, 0 before the first sample. The lock is not
 * waited for: the caller plans, and the last value read serves if the lock is busy. */
uint64_t quic_client_moq_min_rtt_us(void){
 static uint64_t last;
 if(g_client.conn&&quic_lock_take(0)==pdTRUE){
  ngtcp2_conn_info ci;ngtcp2_conn_get_conn_info(g_client.conn,&ci);
  if(ci.min_rtt!=UINT64_MAX)last=ci.min_rtt/NGTCP2_MICROSECONDS;
  quic_lock_give();
 }
 return last;
}
/* The smoothed round trip of the connection, us, as quic_client_moq_min_rtt_us. */
uint64_t quic_client_moq_srtt_us(void){
 static uint64_t last;
 if(g_client.conn&&quic_lock_take(0)==pdTRUE){
  ngtcp2_conn_info ci;ngtcp2_conn_get_conn_info(g_client.conn,&ci);
  last=ci.smoothed_rtt/NGTCP2_MICROSECONDS;
  quic_lock_give();
 }
 return last;
}
bool quic_client_moq_service_initialized(void){
#if AIOT_MOQT_RATE_ESTIMATOR == 1
 return moq_swma_rate.updates!=0;
#else
 return moq_service_rate.samples!=0;
#endif
}

#ifndef AIOT_MOQT_IMAGE_SLOTS
#define AIOT_MOQT_IMAGE_SLOTS 2
#endif
#ifndef AIOT_MOQT_OFFLOAD_SCHED
#define AIOT_MOQT_OFFLOAD_SCHED 0
#endif
#ifndef AIOT_MOQT_DOOMED_DROP
#define AIOT_MOQT_DOOMED_DROP 0
#endif
#ifndef AIOT_MOQT_RATE_EVIDENCE_ONLY
#define AIOT_MOQT_RATE_EVIDENCE_ONLY 0
#endif
#ifndef AIOT_MOQT_OFFLOAD_DECOUPLE
#define AIOT_MOQT_OFFLOAD_DECOUPLE 0
#endif
#ifndef AIOT_MOQT_TX_FIFO
#define AIOT_MOQT_TX_FIFO 0
#endif
#ifndef AIOT_MOQT_TX_ORDER
#define AIOT_MOQT_TX_ORDER 0
#endif
/* Offload scheduling (app_moq_concurrent.inc): the offload track (offload/jpeg, alias 1) shares
 * the image slots with the evidence but yields to it: its bytes go only when no evidence is
 * ready, and the evidence planner does not see it as the image object in flight. */
#define MOQ_OFFLOAD_ALIAS 1U
_Static_assert(AIOT_MOQT_IMAGE_SLOTS>=1 && AIOT_MOQT_IMAGE_SLOTS<=2,"invalid JPEG capacity");
/* Slot layout. 0 control stream; 1..8 request streams; 9 small objects (results,
 * status); 10..11 image objects; 12..13 fetch and fill fetch streams. */
enum { MOQ_K_FREE, MOQ_K_CONTROL, MOQ_K_REQUEST, MOQ_K_OBJECT, MOQ_K_FETCH, MOQ_K_NOTICE };
#define MOQ_REQ_FIRST 1
#define MOQ_REQ_SLOTS 8
#define MOQ_AI_SLOT 9
/* Result objects in flight at once. With one, a result waits until the previous one is
 * acknowledged, so results queue on the device whenever an acknowledgement is slow. */
#define MOQ_AI_SLOTS AIOT_MOQT_RESULT_SLOTS
#define MOQ_IMAGE_FIRST (MOQ_AI_SLOT+MOQ_AI_SLOTS)
#define MOQ_FETCH_FIRST (MOQ_IMAGE_FIRST+2)
#define MOQ_FETCH_SLOTS 2
#define MOQ_TX_SLOTS (MOQ_FETCH_FIRST+MOQ_FETCH_SLOTS)
#define MOQ_HEAD_CAP (MOQT_CORE_HEAD+32)
/* Payload shared between a data stream and the FETCH cache. */
typedef struct moq_blob { unsigned refs; size_t len; uint8_t data[]; } moq_blob;
static moq_blob *moq_blob_new(const uint8_t *data,size_t len){
 moq_blob *b=heap_caps_malloc(sizeof(*b)+len,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
 if(!b)return NULL;
 b->refs=1;b->len=len;memcpy(b->data,data,len);return b;
}
static void moq_blob_drop(moq_blob *b){if(b&&!--b->refs)free(b);}
static size_t moq_cache_bytes; /* image tracks only */
static uint64_t moq_retained_only,moq_cache_evicted;
static struct moq_tx {
 uint8_t kind; bool used,fin,fin_sent,reset_requested,reset_when_acked;
 int64_t id;
 /* Control and request streams own `data`; data streams send `head` then `blob`. */
 uint8_t *data; uint8_t head[MOQ_HEAD_CAP]; size_t head_len; moq_blob *blob;
 size_t len,sent,acked; uint64_t base; /* stream offset of the first byte of this buffer */
 uint64_t queued_us,fin_us,source_us,deadline_us,group,subgroup,object;
 uint64_t first_tx_us,first_ack_us;   /* the first byte handed to QUIC and the first acknowledged (0: none yet) */
 uint64_t object_expiry_us,subgroup_timeout_us; /* section 5.2; 0 means none */
 uint64_t source_expiry_us;                     /* extension: capture time + SOURCE_DELIVERY_TIMEOUT */
 unsigned importance,alias,sub,fetch,reset_code;
 uint8_t subscriber_priority,publisher_priority,group_order;
 bool fill,done_sent;
 bool resend; /* mission data that is not retained: sent again after a lost connection */
} moq_tx[MOQ_TX_SLOTS];
static uint64_t moq_last_ai_ack_group,moq_last_ai_source_to_ack_us;
static bool moq_last_ai_ack_valid;
static moqt_core *moq_core;
static moqt_track_cfg moq_track_cfg[MOQT_CORE_TRACKS];
static unsigned moq_track_count;
static struct { bool has_group; uint64_t last_group,last_object,count; bool new_group; } moq_tracks[MOQT_CORE_TRACKS];
static bool moq_sub_ending[MOQT_CORE_SUBS];
static unsigned moq_done_streams,moq_done_closed;
static int64_t moq_done_ids[2*MOQT_CORE_SUBS];
static unsigned moq_busy_streak[MOQT_CORE_TRACKS];
static bool moq_setup_sent,moq_finished,moq_going_away,moq_session_failed;
static unsigned moq_blocked;
static uint64_t moq_choices[2],moq_accepted_bytes[2],moq_select_us;
static uint64_t moq_timeout_resets[2],moq_timeout_discarded[2];
static uint64_t moq_preview_pace_next;
static size_t moq_preview_unacked_max;
/* Bound on the image bytes in the network that the application sets while it runs; 0: none
 * but the configured one. */
static size_t moq_image_window;
/* The outcome of the last offload Object (alias 1): its acknowledged bytes and whether it was
 * delivered whole or given up (timeout or doomed), for the offload rule's rate sample. */
static struct {uint64_t group;size_t len,acked;bool delivered,valid;} moq_off_last;
static unsigned moq_doomed_resets;   /* Objects given up before their timeout (AIOT_MOQT_DOOMED_DROP) */
void quic_client_moq_image_window(size_t bytes){moq_image_window=bytes;}
/* The capacity of the link as the sender sees it: congestion window over smoothed round trip
 * (bytes/s), the near-term sending rate that loss lowers; 0 before the first sample. As
 * quic_client_moq_min_rtt_us, the last value serves if the lock is busy. */
uint64_t quic_client_moq_capacity_Bps(void){
 static uint64_t last;
 if(g_client.conn&&quic_lock_take(0)==pdTRUE){
  ngtcp2_conn_info ci;ngtcp2_conn_get_conn_info(g_client.conn,&ci);
  if(ci.smoothed_rtt)last=(uint64_t)ci.cwnd*NGTCP2_SECONDS/ci.smoothed_rtt;
  quic_lock_give();
 }
 return last;
}
#if AIOT_MOQT_TRACE_MS
/* Training trace (learned MBM): cumulative bytes handed to the transport and acknowledged, for
 * results and control (0) and media (1), with the state of the connection. */
static uint64_t moq_trace_sent[2],moq_trace_acked[2];
void quic_client_moq_trace(char *out,size_t cap){
 uint64_t srtt=0,rttvar=0,latest=0,minrtt=0,cwnd=0,inflight=0;
 if(g_client.conn&&quic_lock_take(0)==pdTRUE){
  ngtcp2_conn_info ci;ngtcp2_conn_get_conn_info(g_client.conn,&ci);
  srtt=ci.smoothed_rtt/NGTCP2_MICROSECONDS;rttvar=ci.rttvar/NGTCP2_MICROSECONDS;latest=ci.latest_rtt/NGTCP2_MICROSECONDS;
  minrtt=ci.min_rtt==UINT64_MAX?0:ci.min_rtt/NGTCP2_MICROSECONDS;cwnd=ci.cwnd;inflight=ci.bytes_in_flight;
  quic_lock_give();
 }
 size_t media_out=0;
 for(unsigned i=MOQ_IMAGE_FIRST;i<MOQ_IMAGE_FIRST+AIOT_MOQT_IMAGE_SLOTS;i++)media_out+=moq_tx[i].sent-moq_tx[i].acked;
 snprintf(out,cap,"\"sent\":[%llu,%llu],\"acked\":[%llu,%llu],\"media_out\":%u,\"window\":%u,\"rate\":%llu,\"dev\":%llu,"
  "\"srtt\":%llu,\"rttvar\":%llu,\"latest\":%llu,\"min_rtt\":%llu,\"cwnd\":%llu,\"inflight\":%llu",
  (unsigned long long)moq_trace_sent[0],(unsigned long long)moq_trace_sent[1],(unsigned long long)moq_trace_acked[0],
  (unsigned long long)moq_trace_acked[1],(unsigned)media_out,(unsigned)moq_image_window,
  (unsigned long long)quic_client_moq_service_rate(),(unsigned long long)quic_client_moq_service_deviation(),
  (unsigned long long)srtt,(unsigned long long)rttvar,(unsigned long long)latest,(unsigned long long)minrtt,
  (unsigned long long)cwnd,(unsigned long long)inflight);
}
#endif
#if AIOT_MOQT_MUS_PREDICT
/* Predictive window: the time left before the next result can exist at the earliest
 * (source_manager_alert_safe_us). Image bytes that the link drains at rate - deviation
 * within that time, less the hand-over of a result, are admitted beyond the window: when the
 * result comes, the evidence ahead of it is no more than the window. */
#define MOQ_PREDICT_HANDOVER_US 10000U
static uint32_t (*moq_alert_safe)(void);
static uint64_t moq_predict_extra_max,moq_predict_extra_sum,moq_predict_extra_n;
void quic_client_moq_alert_clock(uint32_t (*safe_us)(void)){moq_alert_safe=safe_us;}
static size_t moq_predict_extra(void){
 if(!moq_alert_safe||!quic_client_moq_service_initialized())return 0;
 uint32_t safe=moq_alert_safe();
 if(safe<=MOQ_PREDICT_HANDOVER_US)return 0;
 uint64_t rate=quic_client_moq_service_rate(),dev=quic_client_moq_service_deviation();
 uint64_t low=rate>dev?rate-dev:0;
 return (size_t)(low*(uint64_t)(safe-MOQ_PREDICT_HANDOVER_US)/1000000ULL);
}
void quic_client_moq_predict_stats(uint64_t *max,uint64_t *mean){
 *max=moq_predict_extra_max;*mean=moq_predict_extra_n?moq_predict_extra_sum/moq_predict_extra_n:0;
 moq_predict_extra_max=moq_predict_extra_sum=moq_predict_extra_n=0;
}
#endif
static uint64_t moq_fetch_served,moq_fetch_objects,moq_fetch_bytes,moq_datagrams_sent,moq_datagrams_dropped;
#define MOQ_DGRAM_QUEUE 4
#define MOQ_DGRAM_CAP 1200
static struct { bool used; uint16_t len; uint64_t expiry_us; uint8_t data[MOQ_DGRAM_CAP]; } moq_dgram[MOQ_DGRAM_QUEUE];

static const moqt_track_cfg moq_default_tracks[]={
 {.ns={"aiot","esp32"},.ns_count=2,.name="preview/jpeg",.priority=192,.group_order=MOQT_ORDER_DESCENDING},
 {.ns={"aiot","esp32"},.ns_count=2,.name="ai/events",.priority=64},
 {.ns={"aiot","esp32"},.ns_count=2,.name="evidence/jpeg",.priority=128},
 {.ns={"aiot","esp32"},.ns_count=2,.name="status",.priority=224,.dynamic_groups=true},
};
int quic_client_moq_tracks(const moqt_track_cfg *tracks,unsigned count){
 if(g_client.conn||!tracks||!count||count>MOQT_CORE_TRACKS)return -1;
 memcpy(moq_track_cfg,tracks,count*sizeof(*tracks));moq_track_count=count;return 0;
}
/* Extensions advertised in SETUP (MOQT_AIOT_EXTENSIONS_KO.md). The limits are the real
 * capacity of this adapter: two fetch streams, and the subscriptions of the core. */
static void moq_apply_extensions(void){
 moqt_setup_ext x={0};
#if AIOT_MOQT_EXT_LIMITS
 x.has_control_message=true;x.max_control_message=MOQT_CORE_RX-16;
 x.has_subscriptions=true;x.max_subscriptions=MOQT_CORE_SUBS;
 x.has_fetches=true;x.max_fetches=MOQ_FETCH_SLOTS;
#endif
#if AIOT_MOQT_EXT_SOURCE_TIMEOUT
 x.source_timeout=true;
#endif
 moqt_core_extensions(moq_core,&x);
 moq_core->subscription_lifetime_ms=AIOT_MOQT_SUBSCRIPTION_LIFETIME_MS;
}
/* A result that was in its stream when the connection ended and that nobody can fetch. */
static struct {bool used;uint8_t head[MOQ_HEAD_CAP];size_t head_len;moq_blob *blob;unsigned alias;uint64_t group,source_us;} moq_carry;
static uint64_t moq_resent;
static void moq_slot_free(struct moq_tx *s){
 free(s->data);moq_blob_drop(s->blob);memset(s,0,sizeof(*s));
}
#ifndef AIOT_MOQT_KEEP_CACHE
#define AIOT_MOQT_KEEP_CACHE 0
#endif
static unsigned moq_sessions;
#if AIOT_MOQT_HELPER
static void moq_helper_reset(void);
static bool moq_helper_cancel(int64_t id);
#endif
static void moq_reset(void) {
 struct moq_tx *last=&moq_tx[MOQ_AI_SLOT];
 if(AIOT_MOQT_KEEP_CACHE&&moq_sessions&&last->used&&last->kind==MOQ_K_OBJECT&&last->resend&&last->acked<last->len&&!moq_carry.used){
  moq_carry.used=true;memcpy(moq_carry.head,last->head,last->head_len);moq_carry.head_len=last->head_len;
  moq_carry.blob=last->blob;if(moq_carry.blob)moq_carry.blob->refs++;
  moq_carry.alias=last->alias;moq_carry.group=last->group;moq_carry.source_us=last->source_us;
 }
 for(unsigned i=0;i<MOQ_TX_SLOTS;i++)moq_slot_free(&moq_tx[i]);
 /* With AIOT_MOQT_KEEP_CACHE the retained objects, the Largest Object of every track and
  * the counters survive a lost connection, so that the next session can serve FETCH for
  * what was published before. */
 bool resume=AIOT_MOQT_KEEP_CACHE&&moq_core&&moq_sessions;
 memset(moq_sub_ending,0,sizeof(moq_sub_ending));memset(moq_dgram,0,sizeof(moq_dgram));
#if AIOT_MOQT_HELPER
 moq_helper_reset();
#endif
 moq_setup_sent=moq_finished=moq_going_away=moq_session_failed=false;moq_done_streams=moq_done_closed=0;
 memset(&moq_scheduler,0,sizeof(moq_scheduler));moq_blocked=0;moq_preview_pace_next=0;
 memset(moq_busy_streak,0,sizeof(moq_busy_streak));
 if(resume){
  moqt_core_session_reset(moq_core);
  static unsigned announced;
  if(announced!=moq_sessions+1){announced=moq_sessions+1;printf("[MOQ_SESSION_RESUME] {\"session\":%u,\"cache_objects\":%u}\n",moq_sessions+1,moqt_core_cache_count(moq_core,MOQT_CORE_TRACKS));}
  return;
 }
 if(moq_core){
  void *p;while((p=moqt_core_cache_evict(moq_core,MOQT_CORE_TRACKS)))moq_blob_drop(p);
  if(!moq_track_count){memcpy(moq_track_cfg,moq_default_tracks,sizeof(moq_default_tracks));moq_track_count=4;}
  moqt_core_init(moq_core,moq_track_cfg,moq_track_count,true);
  moq_apply_extensions();
 }
 moq_cache_bytes=0;
 memset(moq_tracks,0,sizeof(moq_tracks));
 memset(&moq_service_rate,0,sizeof(moq_service_rate));
#if AIOT_MOQT_RATE_ESTIMATOR == 2
 memset(&moq_rate_window,0,sizeof(moq_rate_window));
#endif
 memset(&moq_swma_rate,0,sizeof(moq_swma_rate));
 memset(moq_completed,0,sizeof(moq_completed));memset(moq_on_time,0,sizeof(moq_on_time));
 memset(moq_age_sum,0,sizeof(moq_age_sum));memset(moq_age_max,0,sizeof(moq_age_max));
 moq_rejected_feasibility=moq_probes=moq_last_probe=0;
 moq_min_internal=moq_min_psram=SIZE_MAX;
 moq_codec_calls=moq_codec_failures=moq_codec_wall_sum=moq_codec_wall_max=0;moq_codec_scratch_max=0;moq_codec_decode=moq_codec_encode=moq_codec_cpu=moq_pump_gap=0;
 moq_source_total=moq_source_failures=0;moq_source_span=moq_source_gap=0;
 memset(moq_choices,0,sizeof(moq_choices));memset(moq_accepted_bytes,0,sizeof(moq_accepted_bytes));memset(moq_timeout_resets,0,sizeof(moq_timeout_resets));memset(moq_timeout_discarded,0,sizeof(moq_timeout_discarded));moq_select_us=0;moq_preview_unacked_max=0;
 moq_fetch_served=moq_fetch_objects=moq_fetch_bytes=moq_datagrams_sent=moq_datagrams_dropped=0;
 moq_retained_only=moq_cache_evicted=0;
}
static bool moq_core_ready(void){
 if(moq_core)return true;
 moq_core=heap_caps_malloc(sizeof(*moq_core),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
 if(!moq_core)return false;
 if(!moq_track_count){memcpy(moq_track_cfg,moq_default_tracks,sizeof(moq_default_tracks));moq_track_count=4;}
 moqt_core_init(moq_core,moq_track_cfg,moq_track_count,true);
 moq_apply_extensions();
 return true;
}
static struct moq_tx *moq_slot(int64_t id) {
 for(unsigned i=0;i<MOQ_TX_SLOTS;i++)if(moq_tx[i].used&&moq_tx[i].id==id)return &moq_tx[i];
 return NULL;
}
static bool moq_is_result(const struct moq_tx *s){return s>=&moq_tx[MOQ_AI_SLOT]&&s<&moq_tx[MOQ_AI_SLOT+MOQ_AI_SLOTS];}
/* A result lost in an outage is resent when a later packet is acknowledged (RFC 9002, 6.1)
 * or when the probe timeout fires, which doubles with every expiry. A sender that sends
 * little after the outage waits for that timeout, a second and more. While a result is not
 * acknowledged the connection therefore sends a PING whenever it has been idle for
 * AIOT_MOQT_RESULT_PROBE_MS: the first one through brings an acknowledgement that declares
 * the result lost. Called with the lock held. */
static void moq_result_probe_update(void){
#if AIOT_MOQT_RESULT_PROBE_MS
 if(!g_client.conn)return;
 bool waiting=false;
 for(unsigned i=MOQ_AI_SLOT;i<MOQ_AI_SLOT+MOQ_AI_SLOTS;i++)
  if(moq_tx[i].used&&moq_tx[i].kind==MOQ_K_OBJECT&&moq_tx[i].alias==2)waiting=true;
 ngtcp2_conn_set_keep_alive_timeout(g_client.conn,waiting?(ngtcp2_duration)AIOT_MOQT_RESULT_PROBE_MS*NGTCP2_MILLISECONDS:UINT64_MAX);
#endif
}
static bool moq_is_image(const struct moq_tx *s){return s>=&moq_tx[MOQ_IMAGE_FIRST]&&s<&moq_tx[MOQ_IMAGE_FIRST+AIOT_MOQT_IMAGE_SLOTS];}
/* The streams whose acknowledged bytes make the service rate of the evidence planner. With
 * AIOT_MOQT_RATE_EVIDENCE_ONLY the offload track (a helper's frames in an image slot) is left out:
 * the planner measures the service its own objects receive, not the link's total. */
static bool moq_rate_counts(const struct moq_tx *s){
 return moq_is_image(s)&&!(AIOT_MOQT_RATE_EVIDENCE_ONLY&&s->alias==MOQ_OFFLOAD_ALIAS);
}
/* Appends a control message to a control or request stream buffer. */
static int moq_append(struct moq_tx *s,const uint8_t *data,size_t len,bool fin){
 if(s->fin)return -1;
 if(len){
  if(!s->data){s->data=heap_caps_malloc(MOQT21_CONTROL_CAP,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(!s->data)return -1;}
  /* Acknowledged bytes are no longer referenced by the transport. */
  if(s->len&&s->acked==s->len){s->base+=s->len;s->len=s->sent=s->acked=0;}
  if(len>MOQT21_CONTROL_CAP-s->len)return -1;
  memcpy(s->data+s->len,data,len);s->len+=len;
 }
 if(fin)s->fin=true;
 if(!s->queued_us)s->queued_us=esp_timer_get_time();
 return 0;
}
static struct moq_tx *moq_request_slot(int64_t id){
 struct moq_tx *s=moq_slot(id);
 if(s)return s->kind==MOQ_K_REQUEST?s:NULL;
 for(unsigned pass=0;pass<2;pass++)for(unsigned i=MOQ_REQ_FIRST;i<MOQ_REQ_FIRST+MOQ_REQ_SLOTS;i++){
  s=&moq_tx[i];
  /* Second pass: a request whose FIN is acknowledged sends nothing further, even when
   * the peer has not closed its half yet. */
  bool idle=s->used&&s->fin_sent&&s->acked==s->len&&!s->reset_requested;
  if(s->used&&!(pass&&idle))continue;
  if(s->used)moq_slot_free(s);
  memset(s,0,sizeof(*s));s->used=true;s->kind=MOQ_K_REQUEST;s->id=id;s->subscriber_priority=128;return s;
 }
 return NULL;
}
static void moq_fail(uint64_t code){
 if(moq_session_failed)return;
 moq_session_failed=true;
 ngtcp2_ccerr_set_application_error(&g_client.last_error,code,NULL,0);
 printf("[MOQ_SESSION_CLOSE] code=%llu\n",(unsigned long long)code);
}
static unsigned moq_slot_priority_class(const struct moq_tx *s){
 return s->kind==MOQ_K_CONTROL?MOQT_CLASS_CONTROL:s->kind==MOQ_K_REQUEST?MOQT_CLASS_REQUEST:MOQT_CLASS_OBJECT;
}
static size_t moq_remaining(const struct moq_tx *s){return s->len-s->sent;}
/* Loads the next object of a fetch stream once the previous one is acknowledged. */
static void moq_fetch_advance(struct moq_tx *s){
 if(s->kind!=MOQ_K_FETCH||s->fin||s->reset_requested)return;
 if(s->len&&s->acked<s->len)return;
 s->base+=s->len;s->len=s->sent=s->acked=0;s->head_len=0;
 moq_blob_drop(s->blob);s->blob=NULL;
 void *payload;size_t hl=0,pl=0;
 int rv=moqt_core_fetch_next(moq_core,s->fetch,s->head,sizeof(s->head),&hl,&payload,&pl);
 if(rv==1){
  s->head_len=hl;s->len=hl+pl;
  if(payload){s->blob=payload;s->blob->refs++;moq_fetch_objects++;moq_fetch_bytes+=pl;}
  return;
 }
 s->fin=true;
 if(rv<0){
  ngtcp2_conn_shutdown_stream_write(g_client.conn,0,s->id,MOQT_RST_INTERNAL_ERROR);
  s->reset_requested=true;
 }
}
static void moq_sub_maybe_finish(unsigned sub);
static void moq_drain(void);
static void moq_publish_done_reason(unsigned track,char *reason,size_t cap,int *len){
 unsigned k=track==1?0:1; /* statistics index: 0 results, 1 images */
 *len=snprintf(reason,cap,"{\"schema\":1,\"completed\":%llu,\"ack_on_time\":%llu,\"ack_age_sum_us\":%llu,\"ack_age_max_us\":%llu,\"timeout_resets\":%llu,\"feasibility_rejects\":%llu,\"probes\":%llu,\"service_Bps\":%llu,\"min_internal\":%u,\"min_psram\":%u,\"codec_calls\":%llu,\"codec_failures\":%llu,\"codec_wall_sum_us\":%llu,\"codec_wall_max_us\":%llu,\"codec_scratch_max\":%u,\"source_count\":%u,\"source_failed\":%u,\"source_span_us\":%llu,\"source_max_gap_us\":%llu,\"codec_decode_us\":%llu,\"codec_encode_us\":%llu,\"codec_cpu_us\":%llu,\"pump_gap_max_us\":%llu}",
    (unsigned long long)moq_completed[k],(unsigned long long)moq_on_time[k],
    (unsigned long long)moq_age_sum[k],(unsigned long long)moq_age_max[k],
    (unsigned long long)moq_timeout_resets[k],(unsigned long long)(track!=1?moq_rejected_feasibility:0),
    (unsigned long long)(track!=1?moq_probes:0),(unsigned long long)quic_client_moq_service_rate(),
    (unsigned)moq_min_internal,(unsigned)moq_min_psram,
    (unsigned long long)moq_codec_calls,(unsigned long long)moq_codec_failures,
    (unsigned long long)moq_codec_wall_sum,(unsigned long long)moq_codec_wall_max,(unsigned)moq_codec_scratch_max,moq_source_total,moq_source_failures,
    (unsigned long long)moq_source_span,(unsigned long long)moq_source_gap,
    (unsigned long long)moq_codec_decode,(unsigned long long)moq_codec_encode,(unsigned long long)moq_codec_cpu,(unsigned long long)moq_pump_gap);
 if(*len<0||*len>=(int)cap)*len=0;
}
static void moq_reset_slot(struct moq_tx *s,unsigned code){
 if(!s->used||s->reset_requested)return;
 if(ngtcp2_conn_shutdown_stream_write(g_client.conn,0,s->id,code)==0)s->reset_requested=true;
}
static void moq_handle(const moqt_action *a){
 ngtcp2_conn *conn=g_client.conn;
 switch(a->kind){
 case MOQT_ACT_SEND:{
  struct moq_tx *s=moq_request_slot(a->stream);
  if(!s||moq_append(s,a->data,a->len,a->fin)){moq_fail(MOQT_S_INTERNAL_ERROR);return;}
  if(a->len>=1&&a->data[0]==MOQT_MSG_PUBLISH_DONE&&!s->done_sent&&moq_done_streams<2*MOQT_CORE_SUBS){
   s->done_sent=true;moq_done_ids[moq_done_streams++]=a->stream;
  }
  break;}
 case MOQT_ACT_OPEN_BIDI:{
  int64_t id;
  if(ngtcp2_conn_open_bidi_stream(conn,&id,NULL)!=0){printf("[MOQ_ERROR] no bidirectional stream for request\n");return;}
  struct moq_tx *s=moq_request_slot(id);
  if(!s||moq_append(s,a->data,a->len,false)){moq_fail(MOQT_S_INTERNAL_ERROR);return;}
  moqt_core_bidi_opened(moq_core,a->token,id);
  printf("[MOQ_REQUEST_OPENED] {\"stream\":%lld,\"token\":%u}\n",(long long)id,a->token);
  break;}
 case MOQT_ACT_FETCH_START:{
  struct moq_tx *s=NULL;int64_t id;
  for(unsigned i=MOQ_FETCH_FIRST;i<MOQ_FETCH_FIRST+MOQ_FETCH_SLOTS;i++)if(!moq_tx[i].used){s=&moq_tx[i];break;}
  if(!s||ngtcp2_conn_open_uni_stream(conn,&id,NULL)!=0){
   printf("[MOQ_FETCH_REFUSED] {\"fetch\":%u}\n",a->index);
   if(moq_core->fetches[a->index].sub>=0)moqt_core_fetch_closed(moq_core,a->index);
   else moqt_core_stream_cancelled(moq_core,moq_core->fetches[a->index].stream);
   return;
  }
  memset(s,0,sizeof(*s));
  s->used=true;s->kind=MOQ_K_FETCH;s->id=id;s->fetch=a->index;s->alias=a->track+1;s->queued_us=esp_timer_get_time();
  s->subscriber_priority=moq_core->fetches[a->index].priority;s->publisher_priority=128;s->group_order=1;
  s->fill=moq_core->fetches[a->index].sub>=0;s->sub=s->fill?(unsigned)moq_core->fetches[a->index].sub:MOQT_CORE_SUBS;
  moq_fetch_served++;
  printf("[MOQ_FETCH] {\"request\":%llu,\"alias\":%u,\"stream\":%lld,\"fill\":%u}\n",(unsigned long long)a->code,s->alias,(long long)id,(unsigned)s->fill);
  moq_fetch_advance(s);
  break;}
 case MOQT_ACT_FETCH_CANCEL:
  for(unsigned i=MOQ_FETCH_FIRST;i<MOQ_FETCH_FIRST+MOQ_FETCH_SLOTS;i++)
   if(moq_tx[i].used&&moq_tx[i].fetch==a->index)moq_reset_slot(&moq_tx[i],(unsigned)a->code);
  break;
 case MOQT_ACT_FILL_FAILED:{
  /* Section 3.4.1: open the fill fetch stream and reset it after the header. */
  struct moq_tx *s=NULL;int64_t id;
  for(unsigned i=MOQ_FETCH_FIRST;i<MOQ_FETCH_FIRST+MOQ_FETCH_SLOTS;i++)if(!moq_tx[i].used){s=&moq_tx[i];break;}
  if(!s||a->len>sizeof(s->head)||ngtcp2_conn_open_uni_stream(conn,&id,NULL)!=0)return;
  memset(s,0,sizeof(*s));
  s->used=true;s->kind=MOQ_K_NOTICE;s->id=id;memcpy(s->head,a->data,a->len);s->head_len=s->len=a->len;
  s->reset_when_acked=true;s->reset_code=(unsigned)a->code;s->queued_us=esp_timer_get_time();s->sub=a->index;
  break;}
 case MOQT_ACT_SUB_ENDING:
  if(a->index<MOQT_CORE_SUBS){moq_sub_ending[a->index]=true;moq_sub_maybe_finish(a->index);}
  break;
 case MOQT_ACT_SUB_CANCELLED:{
  for(unsigned i=MOQ_AI_SLOT;i<MOQ_TX_SLOTS;i++)
   if(moq_tx[i].used&&moq_tx[i].sub==a->index&&moq_tx[i].kind!=MOQ_K_FETCH)moq_reset_slot(&moq_tx[i],MOQT_RST_CANCELLED);
  struct moq_tx *s=moq_slot(a->stream);
  if(s&&s->kind==MOQ_K_REQUEST)moq_reset_slot(s,MOQT_RST_CANCELLED);
  else ngtcp2_conn_shutdown_stream_write(conn,0,a->stream,MOQT_RST_CANCELLED);
  if(a->index<MOQT_CORE_SUBS)moq_sub_ending[a->index]=false;
  printf("[MOQ_UNSUBSCRIBED] {\"sub\":%u,\"alias\":%u,\"code\":%llu}\n",a->index,a->track+1,(unsigned long long)a->code);
  break;}
 case MOQT_ACT_RESET_STREAM:{
  struct moq_tx *s=moq_slot(a->stream);
  if(s)moq_reset_slot(s,(unsigned)a->code);else ngtcp2_conn_shutdown_stream_write(conn,0,a->stream,a->code);
  break;}
 case MOQT_ACT_STOP_SENDING:ngtcp2_conn_shutdown_stream_read(conn,0,a->stream,a->code);break;
 case MOQT_ACT_CLOSE_SESSION:moq_fail(a->code);break;
 case MOQT_ACT_SUBSCRIBED:
  printf("[MOQ_SUBSCRIBED] request=%llu alias=%u\n",(unsigned long long)moq_core->subs[a->index].request_id,a->track+1);
  printf("[MOQ_SUBSCRIPTION] {\"sub\":%u,\"alias\":%u,\"forward\":%u,\"subscriber_priority\":%u,\"group_order\":%u,\"object_timeout_ms\":%llu,\"filters\":%u}\n",
   a->index,a->track+1,(unsigned)moq_core->subs[a->index].forward,(unsigned)moq_core->subs[a->index].subscriber_priority,
   (unsigned)moq_core->subs[a->index].group_order,(unsigned long long)moq_core->subs[a->index].object_timeout_ms,
   (unsigned)moq_core->subs[a->index].filters.count);
  break;
 case MOQT_ACT_UPDATED:
  printf("[MOQ_UPDATED] {\"sub\":%u,\"alias\":%u,\"forward\":%u,\"subscriber_priority\":%u}\n",a->index,a->track+1,
   (unsigned)moq_core->subs[a->index].forward,(unsigned)moq_core->subs[a->index].subscriber_priority);
  break;
 case MOQT_ACT_NEW_GROUP:if(a->track<MOQT_CORE_TRACKS)moq_tracks[a->track].new_group=true;break;
 case MOQT_ACT_GOAWAY:
  if(!a->index)moq_going_away=true;
  printf("[MOQ_GOAWAY] {\"stream\":%lld,\"timeout_ms\":%llu,\"uri_bytes\":%u}\n",(long long)a->stream,(unsigned long long)a->code,(unsigned)a->len);
  break;
 case MOQT_ACT_PEER_FIN:break;
 case MOQT_ACT_NAMESPACE_RESULT:printf("[MOQ_NAMESPACE] {\"accepted\":%u,\"code\":%llu}\n",a->code==0,(unsigned long long)(a->code?a->code-1:0));break;
 case MOQT_ACT_REQUEST_CLOSED:
  /* The subscriber closed its half after PUBLISH_DONE. */
  for(unsigned i=0;i<moq_done_streams;i++)if(moq_done_ids[i]==a->stream){
   moq_done_ids[i]=-1;moq_done_closed++;
   if(moq_done_closed>=moq_done_streams)moq_finished=true;
  }
  break;
 default:break;
 }
}
static void moq_drain(void){
 moqt_action a;
 while(moqt_core_next_action(moq_core,&a))moq_handle(&a);
}
static void moq_sub_maybe_finish(unsigned sub){
 if(sub>=MOQT_CORE_SUBS||!moq_sub_ending[sub])return;
 for(unsigned i=MOQ_AI_SLOT;i<MOQ_TX_SLOTS;i++)if(moq_tx[i].used&&moq_tx[i].sub==sub)return;
 moq_sub_ending[sub]=false;
 static char reason[900];int n=0;unsigned track=moq_core->subs[sub].track;
 moq_publish_done_reason(track,reason,sizeof(reason),&n);
 printf("[MOQ_PUBLISH_DONE] alias=%u streams=%llu\n",track+1,(unsigned long long)moq_core->subs[sub].streams);
 moqt_core_sub_finish(moq_core,sub,reason,(size_t)n);
}

/* AIOT_MOQT_WMM: the IP precedence of a packet by the stream it carries, which the Wi-Fi driver
 * maps to an access category (ESP-IDF api-guides/wifi.rst, Wi-Fi QoS: 6-7 voice, 0 best effort).
 * Image and fetch streams go best effort; results, control, requests and packets without stream
 * data (acknowledgements, PING) go voice. */
static int moq_stream_precedence(int64_t id){
 if(id<0)return 6;
 for(unsigned i=MOQ_IMAGE_FIRST;i<MOQ_TX_SLOTS;i++)if(moq_tx[i].used&&moq_tx[i].id==id)return 0;
 return 6;
}
static size_t moq_pending(int64_t *id,int *fin,ngtcp2_vec *vec) {
 struct moq_tx *chosen=NULL;size_t slice=SIZE_MAX;
 unsigned ready=0;
 for(unsigned i=0;i<MOQ_TX_SLOTS;i++){
  struct moq_tx *s=&moq_tx[i];
  if(s->used && !s->reset_requested && (s->sent<s->len || (s->fin&&!s->fin_sent)))ready|=1U<<i;
 }
 ready&=~moq_blocked;
 size_t preview_outstanding=0;
 for(unsigned i=MOQ_IMAGE_FIRST;i<MOQ_IMAGE_FIRST+AIOT_MOQT_IMAGE_SLOTS;i++)preview_outstanding+=moq_tx[i].sent-moq_tx[i].acked;
 if(preview_outstanding>moq_preview_unacked_max)moq_preview_unacked_max=preview_outstanding;
 size_t preview_cap=(size_t)AIOT_MOQT_PREVIEW_UNACKED_KB*1024U;
 if(moq_image_window&&(!preview_cap||moq_image_window<preview_cap))preview_cap=moq_image_window;
#if AIOT_MOQT_MUS_PREDICT
 if(moq_image_window){
  size_t extra=moq_predict_extra();
  preview_cap=moq_image_window+extra;
  if(AIOT_MOQT_PREVIEW_UNACKED_KB&&preview_cap>(size_t)AIOT_MOQT_PREVIEW_UNACKED_KB*1024U)preview_cap=(size_t)AIOT_MOQT_PREVIEW_UNACKED_KB*1024U;
  if(extra>moq_predict_extra_max)moq_predict_extra_max=extra;
  moq_predict_extra_sum+=extra;moq_predict_extra_n++;
 }
#endif
 /* Section 5.2: data handed to the transport can no longer time out, so the amount of
  * submitted but unacknowledged image data is bounded. */
 size_t preview_allow=moqt_preview_allowance(preview_cap,preview_outstanding,
     AIOT_MOQT_PREVIEW_KBPS?moq_preview_pace_next:0,esp_timer_get_time());
 if(!preview_allow)ready&=~(((1U<<AIOT_MOQT_IMAGE_SLOTS)-1U)<<MOQ_IMAGE_FIRST);
 const unsigned first_class=((1U<<(MOQ_REQ_FIRST+MOQ_REQ_SLOTS))-1U);
 const unsigned object_class=((1U<<(MOQ_AI_SLOTS+AIOT_MOQT_IMAGE_SLOTS))-1U)<<MOQ_AI_SLOT;
 uint64_t begin=esp_timer_get_time();int k=-1;
 moqt_wire_job wire[MOQ_TX_SLOTS];
 for(unsigned i=0;i<MOQ_TX_SLOTS;i++){
  const struct moq_tx *s=&moq_tx[i];
  wire[i]=(moqt_wire_job){.klass=(uint8_t)moq_slot_priority_class(s),.subscriber_priority=s->subscriber_priority,
   .publisher_priority=s->publisher_priority,.group_order=s->group_order,.fill=s->fill,
   .subscription=s->sub,.group=s->group,.subgroup=s->subgroup,.object=s->object,.admitted_us=s->queued_us};
 }
 if(moq_policy==AIOT_MOQT_WIRE_POLICY){
#if AIOT_MOQT_TX_FIFO
  /* Synergy (Gong et al., TMC 2025, Sec. IV-F): one queue per communication unit, a later task
   * waits for the earlier ones; control and request streams still go first. */
  if(ready&first_class)k=moqt_wire_pick(wire,MOQ_TX_SLOTS,ready&first_class);
  else{
   uint64_t oldest=UINT64_MAX;
   for(unsigned i=0;i<MOQ_TX_SLOTS;i++)if((ready&(1U<<i))&&moq_tx[i].queued_us<oldest){oldest=moq_tx[i].queued_us;k=(int)i;}
  }
#elif AIOT_MOQT_TX_ORDER
  /* Classic schedulers on the same send path (same slices, same window), only the choice of the next
   * stream differs; control and request streams first. Flows: results (0), evidence (1), offload (2).
   * 1 strict priority: results, then the oldest other object (oldest first within a flow);
   * 2 round robin over the flows, one slice per turn (deficit round robin with equal quanta);
   * 3 earliest deadline first over the objects (oldest first among equal deadlines). */
  if(ready&first_class)k=moqt_wire_pick(wire,MOQ_TX_SLOTS,ready&first_class);
  else{
   uint64_t best=UINT64_MAX,oldest=UINT64_MAX;int flow_k[3]={-1,-1,-1};uint64_t flow_old[3]={UINT64_MAX,UINT64_MAX,UINT64_MAX};
   for(unsigned i=0;i<MOQ_TX_SLOTS;i++){
    if(!(ready&(1U<<i)))continue;
    const struct moq_tx *t=&moq_tx[i];
    if(!(object_class&(1U<<i))){if(k<0||t->queued_us<oldest){oldest=t->queued_us;k=(int)i;}continue;}
    unsigned f=i<MOQ_IMAGE_FIRST?0:t->alias==MOQ_OFFLOAD_ALIAS?2:1;
    if(t->queued_us<flow_old[f]){flow_old[f]=t->queued_us;flow_k[f]=(int)i;}
   }
   int obj=-1;
#if AIOT_MOQT_TX_ORDER==1
   if(flow_k[0]>=0)obj=flow_k[0];
   else obj=flow_old[1]<=flow_old[2]?flow_k[1]:flow_k[2];
#elif AIOT_MOQT_TX_ORDER==2
   {static unsigned turn;
    for(unsigned n=0;n<3&&obj<0;n++){unsigned f=(turn+n)%3;if(flow_k[f]>=0){obj=flow_k[f];turn=(f+1)%3;}}}
#else
   for(unsigned i=0;i<MOQ_TX_SLOTS;i++){
    if(!(ready&(1U<<i))||!(object_class&(1U<<i)))continue;
    const struct moq_tx *t=&moq_tx[i];uint64_t d=t->deadline_us?t->deadline_us:UINT64_MAX;
    if(obj<0||d<best||(d==best&&t->queued_us<moq_tx[obj].queued_us)){best=d;obj=(int)i;}
   }
#endif
   if(obj>=0)k=obj;
  }
#else
  /* Section 5.1.2 over every pending stream. */
  k=moqt_wire_pick(wire,MOQ_TX_SLOTS,ready);
#endif
  if(k>=(int)MOQ_AI_SLOT)slice=MOQT_SCHED_QUANTUM;
 }else if(ready&first_class){
  /* Control and request streams always precede Objects. */
  k=moqt_wire_pick(wire,MOQ_TX_SLOTS,ready&first_class);
 }else if(moq_policy>=0&&(ready&object_class)){
  /* Slot policies S0-S5 see the historical six-slot view. */
  moqt_job jobs[6]={{0}};unsigned legacy=0;
  /* the results are one job: the oldest of them that is ready */
  unsigned map[3]={MOQ_AI_SLOT,MOQ_IMAGE_FIRST,MOQ_IMAGE_FIRST+1};
  for(unsigned i=MOQ_AI_SLOT;i<MOQ_AI_SLOT+MOQ_AI_SLOTS;i++)
   if((ready&(1U<<i))&&(!(ready&(1U<<map[0]))||moq_tx[i].queued_us<moq_tx[map[0]].queued_us))map[0]=i;
  for(unsigned j=0;j<3;j++){
   const struct moq_tx *s=&moq_tx[map[j]];
   jobs[3+j]=(moqt_job){.admitted_us=s->queued_us,.deadline_us=s->deadline_us,.group_id=s->group,
    .remaining=moq_remaining(s),.importance=s->importance};
   if(ready&(1U<<map[j]))legacy|=1U<<(3+j);
  }
#if AIOT_MOQT_OFFLOAD_SCHED
  /* evidence before offload: an offload object waits while an evidence object is ready */
  for(unsigned j=1;j<3;j++){
   const struct moq_tx *o=&moq_tx[map[3-j]];
   if((legacy&(1U<<(3+j)))&&(legacy&(1U<<(6-j)))&&moq_tx[map[j]].alias==MOQ_OFFLOAD_ALIAS&&o->alias!=MOQ_OFFLOAD_ALIAS)
    legacy&=~(1U<<(3+j));
  }
#endif
  int pick=moqt_sched_pick(&moq_scheduler,(moqt_policy)moq_policy,legacy,jobs,&slice);
  if(pick>=3){k=(int)map[pick-3];moq_choices[pick==3?0:1]++;}
 }else{
  /* No slot policy, or only fetch streams are pending: the oldest stream first. */
  uint64_t oldest=UINT64_MAX;
  for(unsigned i=0;i<MOQ_TX_SLOTS;i++)if((ready&(1U<<i))&&moq_tx[i].queued_us<oldest){oldest=moq_tx[i].queued_us;k=(int)i;}
 }
 moq_select_us+=esp_timer_get_time()-begin;
 if(k>=0)chosen=&moq_tx[k];
 if(chosen){
  *id=chosen->id;
  size_t left=moq_remaining(chosen);
  if(chosen->data){vec->base=chosen->data+chosen->sent;vec->len=left;}
  else if(chosen->sent<chosen->head_len){vec->base=chosen->head+chosen->sent;vec->len=chosen->head_len-chosen->sent;}
  else{vec->base=chosen->blob?chosen->blob->data+(chosen->sent-chosen->head_len):NULL;vec->len=left;}
  if(moq_is_image(chosen)&&vec->len>preview_allow)vec->len=preview_allow;
  if(vec->len>slice)vec->len=slice;
  *fin=chosen->fin&&vec->len==left;
  return 1;
 }
 *id=-1;*fin=0;vec->base=NULL;vec->len=0;return 0;
}
static int client_send_packet(struct client *c,const uint8_t *data,size_t datalen,const ngtcp2_path *path);
static void moq_flush_datagrams(ngtcp2_conn *conn){
 uint64_t now=(uint64_t)esp_timer_get_time();
 for(unsigned i=0;i<MOQ_DGRAM_QUEUE;i++){
  if(!moq_dgram[i].used)continue;
  if(moq_dgram[i].expiry_us&&now>=moq_dgram[i].expiry_us){
   /* Section 5.2: an expired datagram is dropped. */
   moq_dgram[i].used=false;moq_datagrams_dropped++;continue;
  }
  if(!g_client.dgram_tx_buf||g_client.dgram_tx_buf_len<1452U)return;
  ngtcp2_path_storage ps;ngtcp2_pkt_info pi;int accepted=0;
  ngtcp2_path_storage_zero(&ps);
  ngtcp2_tstamp ts=timestamp();
  ngtcp2_ssize n=ngtcp2_conn_write_datagram(conn,&ps.path,&pi,g_client.dgram_tx_buf,g_client.dgram_tx_buf_len,
    &accepted,NGTCP2_WRITE_DATAGRAM_FLAG_NONE,moq_datagrams_sent+1,moq_dgram[i].data,moq_dgram[i].len,ts);
  if(n<=0)return;
  if(client_send_packet(&g_client,g_client.dgram_tx_buf,(size_t)n,&ps.path)!=0)return;
  ngtcp2_conn_update_pkt_tx_time(conn,ts);
  if(accepted){moq_dgram[i].used=false;moq_datagrams_sent++;}
 }
}
static bool moq_session_ready(void);
static int moq_begin_pump(ngtcp2_conn *conn){
 moq_blocked=0;moq_resource_sample();
 if(moq_session_failed)return -1;
 if(moq_carry.used&&moq_session_ready()&&!moq_tx[MOQ_AI_SLOT].used){
  for(unsigned i=0;i<MOQT_CORE_SUBS;i++){
   const moqt_sub *b=&moq_core->subs[i];int64_t id;
   if(b->state!=MOQT_SUB_ESTABLISHED||b->track!=moq_carry.alias-1||!b->forward)continue;
   if(ngtcp2_conn_open_uni_stream(conn,&id,NULL)!=0)break;
   struct moq_tx *s=&moq_tx[MOQ_AI_SLOT];memset(s,0,sizeof(*s));
   s->used=true;s->kind=MOQ_K_OBJECT;s->id=id;s->fin=true;s->resend=true;
   memcpy(s->head,moq_carry.head,moq_carry.head_len);s->head_len=moq_carry.head_len;
   s->blob=moq_carry.blob;s->len=s->head_len+(s->blob?s->blob->len:0);
   s->queued_us=(uint64_t)esp_timer_get_time();s->source_us=moq_carry.source_us;s->deadline_us=UINT64_MAX;s->importance=2;
   s->group=moq_carry.group;s->alias=moq_carry.alias;s->sub=i;
   s->subscriber_priority=b->subscriber_priority;s->publisher_priority=moq_core->tracks[b->track].cfg.priority;s->group_order=b->group_order;
   moqt_core_stream_opened(moq_core,i);
   memset(&moq_carry,0,sizeof(moq_carry));moq_resent++;
   printf("[MOQ_RESENT] {\"alias\":%u,\"group\":%llu}\n",s->alias,(unsigned long long)s->group);
   break;
  }
 }
 uint64_t now=(uint64_t)esp_timer_get_time();
 for(unsigned i=MOQ_AI_SLOT;i<MOQ_TX_SLOTS;i++){
  struct moq_tx *s=&moq_tx[i];
  if(!s->used||s->reset_requested)continue;
  if(s->reset_when_acked){
   if(s->acked==s->len&&s->len)moq_reset_slot(s,s->reset_code);
   continue;
  }
  if(s->kind==MOQ_K_FETCH){moq_fetch_advance(s);continue;}
  bool expired=false;
#if AIOT_MOQT_DELIVERY_TIMEOUT_RESET
  /* Application deadline measured from the capture timestamp. */
  if(s->deadline_us!=UINT64_MAX&&s->deadline_us&&now>=s->deadline_us)expired=true;
#endif
  /* Section 5.2. OBJECT_DELIVERY_TIMEOUT applies while data is still waiting for the
   * transport; SUBGROUP_DELIVERY_TIMEOUT runs from the end of the subgroup. */
  if(s->object_expiry_us&&s->sent<s->len&&now>=s->object_expiry_us)expired=true;
  if(s->subgroup_timeout_us&&s->fin_sent&&s->acked<s->len&&now-s->fin_us>=s->subgroup_timeout_us)expired=true;
  /* Extension: the source is older than the subscriber accepts; the reset also stops
   * retransmission of what is already in flight. */
  if(s->source_expiry_us&&s->acked<s->len&&now>=s->source_expiry_us)expired=true;
#if AIOT_MOQT_DOOMED_DROP
  /* An Object that cannot end before its delivery timeout is given up as soon as that is clear,
   * not at the timeout: the rest of its bytes would take the link and be discarded all the same.
   * The rate is the Object's own acknowledged bytes over the time since its first byte (after
   * 500 ms of sending); doomed when the rest needs a quarter more than the time left. */
  bool doomed=false;
  if(!expired&&s->object_expiry_us&&s->acked<s->len&&s->first_tx_us&&now-s->first_tx_us>=500000&&s->acked){
   uint64_t rate=(uint64_t)s->acked*1000000ULL/(now-s->first_tx_us);
   uint64_t need=rate?(uint64_t)(s->len-s->acked)*1000000ULL/rate:UINT64_MAX;
   uint64_t left=s->object_expiry_us>now?s->object_expiry_us-now:0;
   if(need>left+left/4){doomed=expired=true;moq_doomed_resets++;}
  }
#endif
  if(!expired)continue;
  int rv=ngtcp2_conn_shutdown_stream_write(conn,0,s->id,MOQT21_STREAM_RESET_DELIVERY_TIMEOUT);
  if(rv)return rv;
  s->reset_requested=true;
  if(s->alias==MOQ_OFFLOAD_ALIAS)moq_off_last=(typeof(moq_off_last)){.group=s->group,.len=s->len,.acked=s->acked,.delivered=false,.valid=true};
  unsigned track=s->alias==2?0:1;size_t discarded=s->len-s->acked;
  moq_timeout_resets[track]++;moq_timeout_discarded[track]+=discarded;
  printf("[MOQ_TIMEOUT_RESET] {\"alias\":%u,\"group\":%llu,\"stream\":%lld,\"age_us\":%llu,\"sent\":%u,\"acked\":%u,\"discarded\":%u,\"doomed\":%d}\n",s->alias,(unsigned long long)s->group,(long long)s->id,(unsigned long long)(now-s->source_us),(unsigned)s->sent,(unsigned)s->acked,(unsigned)discarded,
#if AIOT_MOQT_DOOMED_DROP
         (int)doomed);
#else
         0);
#endif
 }
 if(moq_core){moqt_core_tick(moq_core,now/1000);moq_drain();}
 void *old;
 /* MAX_CACHE_DURATION is configured for image tracks only. */
 while(moq_core&&(old=moqt_core_cache_expire(moq_core,now/1000))){moq_cache_bytes-=((moq_blob*)old)->len;moq_blob_drop(old);}
 moq_flush_datagrams(conn);
 return 0;
}
static void moq_mark_blocked(int64_t id){struct moq_tx *s=moq_slot(id);if(s)moq_blocked|=1U<<(unsigned)(s-moq_tx);}
int quic_client_moq_configure(unsigned policy){if(policy>AIOT_MOQT_WIRE_POLICY||g_client.conn)return -1;moq_policy=(int)policy;return 0;}
/* the congestion state of the connection with the policy statistics (once a second): with
 * [MOQ_SLOT] first_tx_us it tells a wait for the image window from one for the congestion window */
static void moq_cc_stats(size_t images_outstanding){
 if(!g_client.conn||quic_lock_take(0)!=pdTRUE)return;   /* skipped when the transport holds the lock */
 ngtcp2_conn_info ci;ngtcp2_conn_get_conn_info(g_client.conn,&ci);
 quic_lock_give();
 printf("[MOQ_CC] {\"cwnd\":%llu,\"in_flight\":%llu,\"srtt_us\":%llu,\"min_rtt_us\":%llu,\"image_window\":%u,\"images_unacked\":%u}\n",
  (unsigned long long)ci.cwnd,(unsigned long long)ci.bytes_in_flight,(unsigned long long)(ci.smoothed_rtt/NGTCP2_MICROSECONDS),
  (unsigned long long)(ci.min_rtt==UINT64_MAX?0:ci.min_rtt/NGTCP2_MICROSECONDS),(unsigned)moq_image_window,(unsigned)images_outstanding);
}
void quic_client_moq_policy_stats(void){size_t outstanding=0;for(unsigned i=MOQ_IMAGE_FIRST;i<MOQ_IMAGE_FIRST+AIOT_MOQT_IMAGE_SLOTS;i++)outstanding+=moq_tx[i].sent-moq_tx[i].acked;printf("[MOQ_POLICY_STATS] {\"ai_choices\":%llu,\"image_choices\":%llu,\"ai_bytes\":%llu,\"image_bytes\":%llu,\"selector_wall_us\":%llu,\"preview_unacked\":%u,\"preview_unacked_max\":%u,\"ai_timeout_resets\":%llu,\"image_timeout_resets\":%llu,\"ai_timeout_discarded\":%llu,\"image_timeout_discarded\":%llu,\"fetch_streams\":%llu,\"fetch_objects\":%llu,\"fetch_bytes\":%llu,\"datagrams_sent\":%llu,\"datagrams_dropped\":%llu,\"cache_objects\":%u,\"cache_bytes\":%u,\"source_expired\":%llu,\"retained_only\":%llu,\"cache_evicted\":%llu}\n",(unsigned long long)moq_choices[0],(unsigned long long)moq_choices[1],(unsigned long long)moq_accepted_bytes[0],(unsigned long long)moq_accepted_bytes[1],(unsigned long long)moq_select_us,(unsigned)outstanding,(unsigned)moq_preview_unacked_max,(unsigned long long)moq_timeout_resets[0],(unsigned long long)moq_timeout_resets[1],(unsigned long long)moq_timeout_discarded[0],(unsigned long long)moq_timeout_discarded[1],(unsigned long long)moq_fetch_served,(unsigned long long)moq_fetch_objects,(unsigned long long)moq_fetch_bytes,(unsigned long long)moq_datagrams_sent,(unsigned long long)moq_datagrams_dropped,moq_core?moqt_core_cache_count(moq_core,MOQT_CORE_TRACKS):0,(unsigned)moq_cache_bytes,(unsigned long long)(moq_core?moq_core->source_expired:0),(unsigned long long)moq_retained_only,(unsigned long long)moq_cache_evicted);moq_cc_stats(outstanding);}
static unsigned moq_free_result_slot(void){
 for(unsigned i=MOQ_AI_SLOT;i<MOQ_AI_SLOT+MOQ_AI_SLOTS;i++)if(!moq_tx[i].used)return i;
 return MOQ_TX_SLOTS;
}
bool quic_client_moq_ai_slot_available(void){return moq_free_result_slot()<MOQ_TX_SLOTS&&!moq_carry.used;}
static void moq_submitted(int64_t id,ngtcp2_ssize n) {
 struct moq_tx *s=moq_slot(id);if(!s||n<0)return;
#if AIOT_MOQT_TRACE_MS
 moq_trace_sent[moq_is_image(s)?1:0]+=(uint64_t)n;
#endif
 if(s->kind==MOQ_K_OBJECT)moq_accepted_bytes[moq_is_result(s)?0:1]+=(size_t)n;
 if(moq_policy>=0&&moq_policy!=AIOT_MOQT_WIRE_POLICY&&s->kind==MOQ_K_OBJECT)
  moqt_sched_commit(&moq_scheduler,(moqt_policy)moq_policy,moq_is_result(s)?3U:4U,(size_t)n);
 if(n>0&&!s->first_tx_us)s->first_tx_us=esp_timer_get_time();
 s->sent+=(size_t)n;if(moq_is_image(s))moq_preview_pace_next=moqt_preview_pace_advance(moq_preview_pace_next,esp_timer_get_time(),(size_t)n,AIOT_MOQT_PREVIEW_KBPS);if(s->fin&&s->sent==s->len&&!s->fin_sent){s->fin_sent=true;s->fin_us=esp_timer_get_time();}
}
/* Outcome of the image objects, for a scheduler that plans the next one. */
#define MOQ_IMAGE_RESULTS 8
static struct {uint64_t group;size_t bytes;uint64_t held_us;bool delivered;} moq_image_results[MOQ_IMAGE_RESULTS];
static unsigned moq_image_result_head,moq_image_result_count;
static void moq_image_result_push(uint64_t group,size_t bytes,uint64_t held_us,bool delivered){
 unsigned i=(moq_image_result_head+moq_image_result_count)%MOQ_IMAGE_RESULTS;
 if(moq_image_result_count==MOQ_IMAGE_RESULTS)moq_image_result_head=(moq_image_result_head+1)%MOQ_IMAGE_RESULTS;
 else moq_image_result_count++;
 moq_image_results[i].group=group;moq_image_results[i].bytes=bytes;moq_image_results[i].held_us=held_us;
 moq_image_results[i].delivered=delivered;
}
bool quic_client_moq_image_result(uint64_t *group,size_t *bytes,uint64_t *held_us,bool *delivered){
 quic_lock_take(portMAX_DELAY);
 bool ok=moq_image_result_count!=0;
 if(ok){
  unsigned i=moq_image_result_head;
  *group=moq_image_results[i].group;*bytes=moq_image_results[i].bytes;*held_us=moq_image_results[i].held_us;
  *delivered=moq_image_results[i].delivered;
  moq_image_result_head=(i+1)%MOQ_IMAGE_RESULTS;moq_image_result_count--;
 }
 quic_lock_give();return ok;
}
/* The image object that the transport holds: its size, what was sent and acknowledged of
 * it, and whether it was abandoned and only waits for the end of its stream; with OFFLOAD_SCHED or
 * OFFLOAD_DECOUPLE an offload object is not one (it does not hold back the evidence). */
bool quic_client_moq_image_flight(uint64_t *group,size_t *bytes,size_t *sent,size_t *acked,bool *abandoned){
 quic_lock_take(portMAX_DELAY);bool ok=false;
 for(unsigned i=MOQ_IMAGE_FIRST;i<MOQ_IMAGE_FIRST+AIOT_MOQT_IMAGE_SLOTS&&!ok;i++){
  struct moq_tx *s=&moq_tx[i];
  if(!s->used||s->kind!=MOQ_K_OBJECT)continue;
  if((AIOT_MOQT_OFFLOAD_SCHED||AIOT_MOQT_OFFLOAD_DECOUPLE)&&s->alias==MOQ_OFFLOAD_ALIAS)continue;
  *group=s->group;*bytes=s->len;*sent=s->sent;*acked=s->acked;*abandoned=s->reset_requested;ok=true;
 }
 quic_lock_give();return ok;
}
/* The offload object in flight (not abandoned): its Group, size, bytes acknowledged, deadline. */
bool quic_client_moq_offload_last(uint64_t *group,size_t *bytes,size_t *acked,bool *delivered){
 quic_lock_take(portMAX_DELAY);bool ok=moq_off_last.valid;
 if(ok){*group=moq_off_last.group;*bytes=moq_off_last.len;*acked=moq_off_last.acked;*delivered=moq_off_last.delivered;}
 quic_lock_give();return ok;
}
bool quic_client_moq_offload_flight(uint64_t *group,size_t *bytes,size_t *acked,uint64_t *deadline_us){
 quic_lock_take(portMAX_DELAY);bool ok=false;
 for(unsigned i=MOQ_IMAGE_FIRST;i<MOQ_IMAGE_FIRST+AIOT_MOQT_IMAGE_SLOTS&&!ok;i++){
  struct moq_tx *s=&moq_tx[i];
  if(!s->used||s->kind!=MOQ_K_OBJECT||s->alias!=MOQ_OFFLOAD_ALIAS||s->reset_requested)continue;
  *group=s->group;*bytes=s->len;*acked=s->acked;*deadline_us=s->deadline_us;ok=true;
 }
 quic_lock_give();return ok;
}
/* Stops the image object of this Group; the stream is reset with DELIVERY_TIMEOUT. */
bool quic_client_moq_image_abort(uint64_t group){
 quic_lock_take(portMAX_DELAY);bool ok=false;
 for(unsigned i=MOQ_IMAGE_FIRST;i<MOQ_IMAGE_FIRST+AIOT_MOQT_IMAGE_SLOTS;i++){
  struct moq_tx *s=&moq_tx[i];
  if(!s->used||s->kind!=MOQ_K_OBJECT||s->group!=group)continue;
  moq_reset_slot(s,MOQT_RST_DELIVERY_TIMEOUT);ok=s->reset_requested;
 }
 quic_lock_give();return ok;
}
/* Stops the offload object of this Group (an evidence object of the same Group is left alone). */
bool quic_client_moq_offload_abort(uint64_t group){
 quic_lock_take(portMAX_DELAY);bool ok=false;
 for(unsigned i=MOQ_IMAGE_FIRST;i<MOQ_IMAGE_FIRST+AIOT_MOQT_IMAGE_SLOTS;i++){
  struct moq_tx *s=&moq_tx[i];
  if(!s->used||s->kind!=MOQ_K_OBJECT||s->alias!=MOQ_OFFLOAD_ALIAS||s->group!=group||s->reset_requested)continue;
  moq_reset_slot(s,MOQT_RST_DELIVERY_TIMEOUT);ok=s->reset_requested;
 }
 quic_lock_give();return ok;
}
static int moq_acked(int64_t id,uint64_t offset,uint64_t datalen){
 struct moq_tx *s=moq_slot(id);if(!s)return 0;
 if(offset!=s->base+s->acked||datalen>s->sent-s->acked)return -1;
 s->acked+=(size_t)datalen;
 if(datalen&&!s->first_ack_us)s->first_ack_us=esp_timer_get_time();
#if AIOT_MOQT_TRACE_MS
 moq_trace_acked[moq_is_image(s)?1:0]+=datalen;
#endif
#if AIOT_MOQT_RATE_ESTIMATOR == 2
 if(moq_rate_counts(s)&&s->kind==MOQ_K_OBJECT)moq_rate_window_acked((size_t)datalen,(uint64_t)esp_timer_get_time(),false);
#endif
 return 0;
}
static int moq_stream_close(ngtcp2_conn *conn,uint32_t flags,int64_t id,uint64_t error,void *user,void *stream_user) {
 (void)user;(void)stream_user;
 struct moq_tx *s=moq_slot(id);
 if(s){
  bool app_error=(flags&NGTCP2_STREAM_CLOSE_FLAG_APP_ERROR_CODE_SET)!=0;
  uint64_t closed_us=(uint64_t)esp_timer_get_time();
  bool image=moq_is_image(s),result=moq_is_result(s)&&s->alias==2;
  if(image){
   if(moq_rate_counts(s)){
#if AIOT_MOQT_RATE_ESTIMATOR == 2
   moq_rate_window_acked(0,closed_us,true);
#else
   moqt_rate_observe(&moq_service_rate,s->acked,closed_us-s->queued_us);
#endif
   moqt_swma_observe(&moq_swma_rate,s->acked,closed_us-s->queued_us);
   }
   moq_image_result_push(s->group,s->acked,closed_us-s->queued_us,!app_error&&!s->reset_requested&&s->acked==s->len);
   if(s->alias==MOQ_OFFLOAD_ALIAS&&!s->reset_requested)
    moq_off_last=(typeof(moq_off_last)){.group=s->group,.len=s->len,.acked=s->acked,.delivered=!app_error&&s->acked==s->len,.valid=true};
  }
  if(!app_error&&(image||result)){
   unsigned k=result?0:1;uint64_t age=closed_us-s->source_us;
   moq_completed[k]++;moq_on_time[k]+=closed_us<=s->deadline_us;
   moq_age_sum[k]+=age;if(age>moq_age_max[k])moq_age_max[k]=age;
  }
  if(!app_error&&result){moq_last_ai_ack_group=s->group;moq_last_ai_source_to_ack_us=(uint64_t)esp_timer_get_time()-s->source_us;moq_last_ai_ack_valid=true;}
  if(!app_error&&moq_policy>=0&&s->kind==MOQ_K_OBJECT)printf("[MOQ_RECEIPT] {\"alias\":%u,\"group\":%llu,\"ack_us\":%llu,\"source_us\":%llu,\"deadline_us\":%llu}\n",s->alias,(unsigned long long)s->group,(unsigned long long)esp_timer_get_time(),(unsigned long long)s->source_us,(unsigned long long)s->deadline_us);
  /* the stages of an Object: queued -> first byte handed to QUIC (first_tx_us, the wait for the
   * scheduler and the image window) -> first byte acknowledged (first_ack_us) -> last byte handed
   * over (submit_us) -> closed (held_us); all from queued */
  if(s->kind==MOQ_K_OBJECT)printf("[MOQ_SLOT] {\"stream\":%lld,\"held_us\":%llu,\"submit_us\":%llu,\"ack_wait_us\":%llu,\"first_tx_us\":%llu,\"first_ack_us\":%llu,\"alias\":%u,\"bytes\":%u}\n",(long long)id,(unsigned long long)(esp_timer_get_time()-s->queued_us),(unsigned long long)(s->fin_us?s->fin_us-s->queued_us:0),(unsigned long long)(s->fin_us?esp_timer_get_time()-s->fin_us:0),
         (unsigned long long)(s->first_tx_us?s->first_tx_us-s->queued_us:0),(unsigned long long)(s->first_ack_us?s->first_ack_us-s->queued_us:0),s->alias,(unsigned)s->len);
  unsigned sub=s->sub;uint8_t kind=s->kind;unsigned fetch=s->fetch;bool done=s->done_sent;
  (void)done;
  moq_slot_free(s);
  moq_result_probe_update();
  if(kind==MOQ_K_FETCH){moqt_core_fetch_closed(moq_core,fetch);moq_drain();}
  if(kind!=MOQ_K_REQUEST&&kind!=MOQ_K_CONTROL&&sub<MOQT_CORE_SUBS)moq_sub_maybe_finish(sub);
  if(moq_core)moq_drain();
 }
 /* Streams opened by the peer return their credit (RFC 9000, section 4.6). */
 if((id&1)==1){
  if(id&2)ngtcp2_conn_extend_max_streams_uni(conn,1);else ngtcp2_conn_extend_max_streams_bidi(conn,1);
 }
 printf("[MOQ_STREAM_CLOSED] id=%lld error=%llu\n",(long long)id,(unsigned long long)error);
 /* RESET_STREAM is an object-level outcome in MOQT, not a connection error. */
 return 0;
}
/* The peer reset a stream or asked us to stop sending on it. */
static int moq_stream_cancel(ngtcp2_conn *conn,int64_t id,uint64_t code,bool stop_sending){
 (void)conn;
#if AIOT_MOQT_HELPER
 if(moq_helper_cancel(id))return 0;
#endif
 if(!moq_core)return 0;
 struct moq_tx *s=moq_slot(id);
 if(s&&s->kind!=MOQ_K_REQUEST&&s->kind!=MOQ_K_CONTROL){
  /* Section 11.3.2: no new stream is opened for a subgroup the subscriber refused. */
  if(stop_sending)s->reset_requested=true;
  printf("[MOQ_STOP_SENDING] {\"stream\":%lld,\"alias\":%u,\"group\":%llu,\"code\":%llu}\n",(long long)id,s->alias,(unsigned long long)s->group,(unsigned long long)code);
  return 0;
 }
 moqt_core_stream_cancelled(moq_core,id);moq_drain();
 return 0;
}
static int moq_stream_reset_cb(ngtcp2_conn *conn,int64_t id,uint64_t final_size,uint64_t code,void *user,void *stream_user){
 (void)final_size;(void)user;(void)stream_user;return moq_stream_cancel(conn,id,code,false);
}
static int moq_stream_stop_cb(ngtcp2_conn *conn,int64_t id,uint64_t code,void *user,void *stream_user){
 (void)user;(void)stream_user;return moq_stream_cancel(conn,id,code,true);
}
bool quic_client_moq_last_ai_receipt(uint64_t *group,uint64_t *source_to_ack_us){
 if(!moq_last_ai_ack_valid||!group||!source_to_ack_us)return false;
 *group=moq_last_ai_ack_group;*source_to_ack_us=moq_last_ai_source_to_ack_us;return true;
}
#if AIOT_MOQT_HELPER
#include "moqt_helper_sub.inc"
#endif
static int moq_recv(ngtcp2_conn *conn,uint32_t flags,int64_t id,uint64_t offset,const uint8_t *data,size_t len) {
 (void)offset;
 if(!moq_core_ready())return NGTCP2_ERR_CALLBACK_FAILURE;
#if AIOT_MOQT_HELPER
 if(moq_helper_rx(conn,id,data,len,(flags&NGTCP2_STREAM_DATA_FLAG_FIN)!=0)){
  if(len){ngtcp2_conn_extend_max_stream_offset(conn,id,len);ngtcp2_conn_extend_max_offset(conn,len);}
  return 0;
 }
#endif
 moqt_core_rx(moq_core,id,data,len,(flags&NGTCP2_STREAM_DATA_FLAG_FIN)!=0,(uint64_t)esp_timer_get_time()/1000);
 moq_drain();
 if(moq_session_failed)return NGTCP2_ERR_CALLBACK_FAILURE;
 if(len){ngtcp2_conn_extend_max_stream_offset(conn,id,len);ngtcp2_conn_extend_max_offset(conn,len);}
 return 0;
}
int quic_client_moq_start(const char *authority) {
 printf("[MOQ_CONFIG] image_slots=%u preview_kbps=%u preview_unacked_kb=%u timeout_reset=%u cache_kb=%u cache_objects=%u draft=21 ext_limits=%u ext_source_timeout=%u\n",(unsigned)AIOT_MOQT_IMAGE_SLOTS,(unsigned)AIOT_MOQT_PREVIEW_KBPS,(unsigned)AIOT_MOQT_PREVIEW_UNACKED_KB,(unsigned)AIOT_MOQT_DELIVERY_TIMEOUT_RESET,(unsigned)AIOT_MOQT_CACHE_KB,(unsigned)AIOT_MOQT_CACHE_OBJECTS,(unsigned)AIOT_MOQT_EXT_LIMITS,(unsigned)AIOT_MOQT_EXT_SOURCE_TIMEOUT);
 if(moq_policy>=0)printf("[MOQ_POLICY] %s slice=1024\n",moq_policy==AIOT_MOQT_WIRE_POLICY?"moqt21_wire":moqt_policy_name((moqt_policy)moq_policy));
 quic_lock_take(portMAX_DELAY);int result=-1;
 if(g_client.conn && !moq_setup_sent && quic_client_is_connected() && moq_core_ready()){
  const ngtcp2_transport_params *rp=ngtcp2_conn_get_remote_transport_params(g_client.conn);
#if AIOT_MOQT_HELPER
  /* The relay recognizes the helper by its Implementation option (section 9.1) and subscribes
   * to the helper's result track on this session. */
  uint8_t b[256];size_t n=moqt_setup_write_ext(b,sizeof(b),"/",authority,MOQT_HELPER_IMPLEMENTATION,MOQT_MAX_RANGES,MOQT_CORE_MAX_UPDATES,&moq_core->ext);int64_t id;
  if(n)moq_core->setup_sent=true;
#else
  uint8_t b[256];size_t n=moqt_core_setup(moq_core,b,sizeof(b),"/",authority);int64_t id;
#endif
  struct moq_tx *s=&moq_tx[0];
  if(rp&&rp->max_datagram_frame_size&&n&&!s->used&&ngtcp2_conn_open_uni_stream(g_client.conn,&id,NULL)==0){
   memset(s,0,sizeof(*s));s->used=true;s->kind=MOQ_K_CONTROL;s->id=id;
   if(!moq_append(s,b,n,false)){moq_setup_sent=true;moq_sessions++;result=0;}
  }
 }
 quic_lock_give();return result;
}
static bool moq_session_ready(void){return moq_core&&moq_setup_sent&&moq_core->peer_setup&&!moq_session_failed;}
bool quic_client_moq_track_ready(unsigned alias) {
 return alias>=1&&alias<=moq_track_count&&moq_session_ready()&&moqt_core_any_forwarding(moq_core,alias-1);
}
bool quic_client_moq_session_ready(void){return moq_session_ready();}
bool quic_client_moq_going_away(void){return moq_going_away;}
unsigned quic_client_moq_sessions(void){return moq_sessions;}
bool quic_client_moq_new_group_requested(unsigned alias){
 if(alias<1||alias>moq_track_count||!moq_tracks[alias-1].new_group)return false;
 moq_tracks[alias-1].new_group=false;return true;
}
/* Called by the single QUIC owner task; capture/AI workers never touch TX. */
static unsigned moq_free_image_slot(void){
 for(unsigned i=MOQ_IMAGE_FIRST;i<MOQ_IMAGE_FIRST+AIOT_MOQT_IMAGE_SLOTS;i++)if(!moq_tx[i].used)return i;
 return MOQ_TX_SLOTS;
}
bool quic_client_moq_image_slot_available(void){return moq_free_image_slot()<MOQ_TX_SLOTS;}
bool quic_client_moq_ready(void) {return quic_client_moq_track_ready(1);}
static bool moq_track_is_image(unsigned track){return track==0||track==2;}
/* Image tracks share a byte and object budget. Every other track has its own object
 * budget, so image traffic cannot push mission results out of the cache. */
static void moq_cache_store(unsigned track,const moqt_offer *o,moq_blob *blob){
 void *evicted=NULL;bool image=moq_track_is_image(track);
 if(image){
  if(blob->len>(size_t)AIOT_MOQT_CACHE_KB*1024U)return;
  for(;;){
   unsigned images=moqt_core_cache_count(moq_core,0)+moqt_core_cache_count(moq_core,2);
   if(!images||(images<AIOT_MOQT_CACHE_OBJECTS&&moq_cache_bytes+blob->len<=(size_t)AIOT_MOQT_CACHE_KB*1024U))break;
   /* The older of the two image tracks goes first. */
   unsigned victim=moqt_core_cache_count(moq_core,0)?0U:2U;
   if(moqt_core_cache_count(moq_core,0)&&moqt_core_cache_count(moq_core,2)){
    uint64_t a=UINT64_MAX,b=UINT64_MAX;
    for(unsigned i=0;i<MOQT_CORE_CACHE;i++){
     const moqt_cached *e=&moq_core->cache[i];
     if(e->used&&e->track==0&&e->serial<a)a=e->serial;
     if(e->used&&e->track==2&&e->serial<b)b=e->serial;
    }
    victim=a<b?0U:2U;
   }
   moq_blob *old=moqt_core_cache_evict(moq_core,victim);
   if(!old)break;
   moq_cache_bytes-=old->len;moq_blob_drop(old);moq_cache_evicted++;
  }
 }else while(moqt_core_cache_count(moq_core,track)>=AIOT_MOQT_RESULT_CACHE_OBJECTS){
  moq_blob *old=moqt_core_cache_evict(moq_core,track);
  if(!old)break;
  moq_blob_drop(old);moq_cache_evicted++;
  printf("[MOQ_CACHE_EVICTED] {\"alias\":%u}\n",track+1);
 }
 if(moqt_core_cache_put(moq_core,track,o,blob,(uint64_t)esp_timer_get_time()/1000,&evicted))return;
 blob->refs++;
 if(image)moq_cache_bytes+=blob->len;
 if(evicted){
  /* Only when the core itself is full; its oldest entry may belong to any track. */
  moq_blob_drop(evicted);moq_cache_evicted++;
 }
}
/* Places PRIOR_GROUP_ID_GAP (draft-21 section 10.8) before the application properties. */
static size_t moq_props_with_gap(const uint8_t *props,size_t len,uint64_t gap,uint8_t *out,size_t cap){
 moqt_wr w=moqt_wr_of(out,cap);uint64_t prev=0;
 moqt_kvp_put_int(&w,&prev,MOQT_X_PRIOR_GROUP_ID_GAP,gap);
 if(len){
  moqt_rd r=moqt_rd_of(props,len);uint64_t first=moqt_rd_vi(&r);
  if(r.bad||first<=MOQT_X_PRIOR_GROUP_ID_GAP)return 0;
  moqt_wr_vi(&w,first-MOQT_X_PRIOR_GROUP_ID_GAP);moqt_wr_bytes(&w,r.p,r.n);
 }
 return w.bad?0:w.n;
}
/* 0 accepted; -1 busy or invalid; -2 expired before admission; -3 refused by the
 * feasibility estimate; -4 nobody receives the object and it is not retained. */
int quic_client_moq_publish_ex(const quic_moq_object *q) {
 uint64_t entry_us=(uint64_t)esp_timer_get_time();   /* before the lock: the wait for it is measured */
 if(!q||(!q->never_drop&&(uint64_t)esp_timer_get_time()>=q->deadline_us))return -2;
 unsigned alias=q->alias;
 if(alias<1||alias>moq_track_count||(!q->data&&q->len)||q->len>MOQT21_MAX_JPEG||q->props_len+12>MOQT_CORE_PROPS)return -1;
 if(!q->len&&q->status==MOQT_STATUS_NORMAL)return -1;
 quic_lock_take(portMAX_DELAY);int result=-1;unsigned track=alias-1;
 /* Between two sessions mission data goes to the retained set; the next session serves it. */
 bool offline=!moq_session_ready();
 if(offline&&!(AIOT_MOQT_KEEP_CACHE&&moq_core&&moq_sessions&&q->never_drop&&q->retain&&!q->datagram))goto out;
 if(moq_tracks[track].has_group&&(q->group<moq_tracks[track].last_group||
    (q->group==moq_tracks[track].last_group&&q->object<=moq_tracks[track].last_object)))goto out;
 bool image=alias==1||alias==3; /* image tracks use the image slots, other tracks the small slot */
 unsigned slot=image?moq_free_image_slot():moq_free_result_slot();
 bool forwarding=!offline&&moqt_core_any_forwarding(moq_core,track);
 if(!forwarding&&!q->retain){result=-4;goto out;}
 bool busy=forwarding&&!q->datagram&&(slot>=MOQ_TX_SLOTS||moq_tx[slot].used);
 /* Mission data is never refused for lack of a stream: it is retained, and the
  * subscriber recovers it with FETCH once it notices the gap. */
 bool retain_only=offline||(q->retain&&q->never_drop&&(busy||q->retain_only));
 if(busy&&!retain_only){
  /* Section 5.2: a subscriber that cannot keep up may be dropped with TOO_FAR_BEHIND. */
  if(AIOT_MOQT_BEHIND_LIMIT&&++moq_busy_streak[track]>=AIOT_MOQT_BEHIND_LIMIT){
   moq_busy_streak[track]=0;
   for(unsigned i=0;i<MOQT_CORE_SUBS;i++)
    if(moq_core->subs[i].state==MOQT_SUB_ESTABLISHED&&moq_core->subs[i].track==track)moqt_core_sub_end(moq_core,i,MOQT_D_TOO_FAR_BEHIND);
   moq_drain();
  }
  goto out;
 }
 moq_busy_streak[track]=0;
 if(q->datagram&&q->len+MOQ_HEAD_CAP>MOQ_DGRAM_CAP)goto out;
#if AIOT_MOQT_FEASIBILITY
 if(alias==1&&forwarding){
  uint64_t now=(uint64_t)esp_timer_get_time();size_t queued=0;
  for(unsigned i=MOQ_AI_SLOT;i<MOQ_FETCH_FIRST;i++)if(moq_tx[i].used)queued+=moq_tx[i].len-moq_tx[i].acked;
  if(!moqt_fits(q->len,queued,quic_client_moq_service_rate(),now,q->deadline_us)){
   /* Explicit bounded probe avoids permanent starvation after an outage.
    * It may miss its deadline; it is counted, never called feasible. */
   if(!queued&&(!moq_last_probe||now-moq_last_probe>=5000000)){
    moq_last_probe=now;moq_probes++;
   }else{moq_rejected_feasibility++;result=-3;goto out;}
  }
 }
#endif
 static uint8_t gap_props[MOQT_CORE_PROPS];
 const uint8_t *props=q->props;size_t props_len=q->props_len;
 if(q->announce_gap&&q->status==MOQT_STATUS_NORMAL&&(!moq_tracks[track].has_group||q->group>moq_tracks[track].last_group)){
  uint64_t gap=moq_tracks[track].has_group?q->group-moq_tracks[track].last_group-1:q->group;
  props_len=moq_props_with_gap(q->props,q->props_len,gap,gap_props,sizeof(gap_props));
  if(!props_len)goto out;
  props=gap_props;
 }
 moqt_offer o={.group=q->group,.object=q->object,.subgroup=q->subgroup,.priority=q->priority,.status=q->status,
  .has_priority=q->has_priority,.end_of_group=q->end_of_group,.datagram=q->datagram,
  .props=props,.props_len=props_len,.payload_len=q->len};
 static moqt_delivery d[MOQT_CORE_SUBS];
 uint64_t now_us=(uint64_t)esp_timer_get_time();
 /* The capture timestamp and this clock are the same device clock. */
 if(q->source_us&&q->source_us<=now_us){o.has_source_age=true;o.source_age_ms=(now_us-q->source_us)/1000;}
 int n=moqt_core_publish(moq_core,track,&o,now_us/1000,d,retain_only?0U:MOQT_CORE_SUBS);
 if(n<0)goto out;
 if(retain_only){
  moq_retained_only++;
  printf("[MOQ_RETAINED_ONLY] {\"alias\":%u,\"group\":%llu,\"offline\":%u}\n",alias,(unsigned long long)q->group,(unsigned)offline);
 }
 moq_blob *blob=q->len?moq_blob_new(q->data,q->len):NULL;
 if(q->len&&!blob)goto out;
 result=0;
 for(int i=0;i<n;i++){
  uint64_t expiry=d[i].object_timeout_ms?now_us+d[i].object_timeout_ms*1000:0;
  if(d[i].datagram){
   bool queued=false;
   for(unsigned j=0;j<MOQ_DGRAM_QUEUE&&!queued;j++)if(!moq_dgram[j].used){
    memcpy(moq_dgram[j].data,d[i].head,d[i].head_len);
    if(q->len)memcpy(moq_dgram[j].data+d[i].head_len,q->data,q->len);
    moq_dgram[j].len=(uint16_t)(d[i].head_len+q->len);moq_dgram[j].expiry_us=expiry;moq_dgram[j].used=queued=true;
   }
   if(!queued)moq_datagrams_dropped++;
   continue;
  }
  struct moq_tx *s=NULL;
  if(i==0)s=&moq_tx[slot];
  else if(image){unsigned k=moq_free_image_slot();if(k<MOQ_TX_SLOTS)s=&moq_tx[k];}
  int64_t id;
  if(!s||s->used||d[i].head_len>sizeof(s->head)||ngtcp2_conn_open_uni_stream(g_client.conn,&id,NULL)!=0){
   /* One subscription misses this object; the others still receive it. */
   if(i==0)result=-1;
   printf("[MOQ_DELIVERY_SKIPPED] {\"alias\":%u,\"group\":%llu,\"sub\":%u}\n",alias,(unsigned long long)q->group,d[i].sub);
   continue;
  }
  memset(s,0,sizeof(*s));
  s->used=true;s->kind=MOQ_K_OBJECT;s->id=id;s->fin=true;
  memcpy(s->head,d[i].head,d[i].head_len);s->head_len=d[i].head_len;s->len=d[i].head_len+q->len;
  if(blob){s->blob=blob;blob->refs++;}
  s->queued_us=now_us;s->source_us=q->source_us;s->deadline_us=q->never_drop?UINT64_MAX:q->deadline_us;s->importance=q->importance;
  s->group=q->group;s->subgroup=q->subgroup;s->object=q->object;s->alias=alias;s->sub=d[i].sub;
  s->subscriber_priority=d[i].subscriber_priority;s->publisher_priority=d[i].publisher_priority;s->group_order=d[i].group_order;
  s->object_expiry_us=expiry;s->subgroup_timeout_us=d[i].subgroup_timeout_ms*1000;
  s->source_expiry_us=d[i].source_timeout_ms?q->source_us+d[i].source_timeout_ms*1000:0;
  /* A subscriber may still ask for timeouts; mission data ignores them on this side. */
  if(q->never_drop)s->object_expiry_us=s->subgroup_timeout_us=s->source_expiry_us=0;
  s->resend=q->never_drop&&!q->retain;
#if AIOT_MOQT_RATE_ESTIMATOR == 2
  if(image&&!(AIOT_MOQT_RATE_EVIDENCE_ONLY&&alias==MOQ_OFFLOAD_ALIAS)&&!moq_rate_window.start)moq_rate_window_open(now_us);
#endif
  moqt_core_stream_opened(moq_core,d[i].sub);
  if(moq_policy>=0)printf("[MOQ_ADMITTED] {\"alias\":%u,\"group\":%llu,\"source_us\":%llu,\"deadline_us\":%llu,\"admitted_us\":%llu,\"entry_us\":%llu}\n",alias,(unsigned long long)q->group,(unsigned long long)q->source_us,(unsigned long long)q->deadline_us,(unsigned long long)s->queued_us,(unsigned long long)entry_us);
 }
 moq_result_probe_update();
 if(!result){
  moq_tracks[track].has_group=true;moq_tracks[track].last_group=q->group;moq_tracks[track].last_object=q->object;moq_tracks[track].count++;
  if(q->retain&&blob&&q->status==MOQT_STATUS_NORMAL)moq_cache_store(track,&o,blob);
 }
 moq_blob_drop(blob);
 if(!offline)moq_drain();
 out:quic_lock_give();return result;
}
int quic_client_moq_publish_object(unsigned alias,const uint8_t *data,size_t len,uint64_t group,uint64_t source_us,uint64_t deadline_us,unsigned importance) {
 if(alias==2&&len>4096)return -1;
 quic_moq_object q={.alias=alias,.data=data,.len=len,.group=group,.source_us=source_us,.deadline_us=deadline_us,
  .importance=importance,.end_of_group=true};
 return quic_client_moq_publish_ex(&q);
}
int quic_client_moq_publish_track(unsigned alias,const uint8_t *data,size_t len,uint64_t group){return quic_client_moq_publish_object(alias,data,len,group,esp_timer_get_time(),UINT64_MAX,0);}
int quic_client_moq_publish(const uint8_t *jpeg,size_t len,uint64_t group){return quic_client_moq_publish_track(1,jpeg,len,group);}
/* Publisher-initiated subscription (PUBLISH) and namespace advertisement. */
int quic_client_moq_offer_track(unsigned alias,bool forward){
 if(alias<1||alias>moq_track_count)return -1;
 quic_lock_take(portMAX_DELAY);int result=-1;
 if(moq_session_ready()){result=moqt_core_publish_track(moq_core,alias-1,forward,true)<0?-1:0;moq_drain();}
 quic_lock_give();return result;
}
int quic_client_moq_announce(void){
 quic_lock_take(portMAX_DELAY);int result=-1;
 if(moq_session_ready()){result=moqt_core_publish_namespace(moq_core);moq_drain();}
 quic_lock_give();return result;
}
int quic_client_moq_send_goaway(uint32_t timeout_ms){
 quic_lock_take(portMAX_DELAY);int result=-1;uint8_t b[32];
 if(moq_session_ready()&&moq_tx[0].used){
  int n=moqt_core_goaway(moq_core,b,sizeof(b),timeout_ms);
  if(n>0&&!moq_append(&moq_tx[0],b,(size_t)n,false))result=0;
 }
 quic_lock_give();return result;
}
int quic_client_moq_finish(void) {
 quic_lock_take(portMAX_DELAY);int result=1;
 if(!moq_core)goto out;
 for(unsigned i=MOQ_AI_SLOT;i<MOQ_FETCH_FIRST;i++)if(moq_tx[i].used)goto out;
 for(unsigned t=0;t<moq_track_count;t++)if(!moq_core->tracks[t].ended)moqt_core_track_end(moq_core,t);
 moq_drain();
 /* A session that only served FETCH has no PUBLISH_DONE to be acknowledged. */
 if(!moq_done_streams)moq_finished=true;
 result=moq_session_failed?-1:0;
 out:quic_lock_give();return result;
}
bool quic_client_moq_finished(void){return moq_finished;}

#if AIOT_MOQT_AUDIT
#undef printf
#endif
