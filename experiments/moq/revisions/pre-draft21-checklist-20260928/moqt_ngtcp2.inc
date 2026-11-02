#include "moq_audit.h"
#if AIOT_MOQT_AUDIT
#define printf moq_audit_printf
#endif
/* Bounded two-track MOQT-21 research publisher. Buffers stay immutable until
 * FIN acknowledgement. Not a general relay or complete MOQT implementation. */
#include "moqt21.h"
#include "moqt_tx_policy.h"
#include "moqt_feasibility.h"
#include "moqt_swma.h"
#include "moqt_scheduler.h"
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
uint64_t quic_client_moq_service_rate(void){
#if AIOT_MOQT_RATE_ESTIMATOR == 1
 return moq_swma_rate.updates?(uint64_t)moq_swma_rate.value:64000;
#else
 return moqt_rate_value(&moq_service_rate);
#endif
}
bool quic_client_moq_service_initialized(void){
#if AIOT_MOQT_RATE_ESTIMATOR == 1
 return moq_swma_rate.updates!=0;
#else
 return moq_service_rate.samples!=0;
#endif
}

static unsigned moq_blocked;
static uint64_t moq_choices[2],moq_accepted_bytes[2],moq_select_us;
static uint64_t moq_timeout_resets[2],moq_timeout_discarded[2];
static uint64_t moq_preview_pace_next;
static size_t moq_preview_unacked_max;
#define MOQ_TX_SLOTS 6
#ifndef AIOT_MOQT_IMAGE_SLOTS
#define AIOT_MOQT_IMAGE_SLOTS 2
#endif
_Static_assert(AIOT_MOQT_IMAGE_SLOTS>=1 && AIOT_MOQT_IMAGE_SLOTS<=2,"invalid JPEG capacity");
/* 0 setup, 1/2 request replies, 3 AI object, 4/5 JPEG objects. */
static struct moq_tx { bool used,fin,fin_sent,reset_requested; int64_t id; uint8_t *data; size_t len,sent,acked; uint64_t queued_us,fin_us,source_us,deadline_us,group;unsigned importance; } moq_tx[MOQ_TX_SLOTS];
static uint64_t moq_last_ai_ack_group,moq_last_ai_source_to_ack_us;
static bool moq_last_ai_ack_valid;
static struct moq_rx { bool used,handled; int64_t id; size_t len; uint8_t data[MOQT21_CONTROL_CAP+16]; } moq_rx[3];
static struct moq_track {bool subscribed,has_group,ending,finished;int64_t request_stream;uint64_t request,last_group,count;} moq_tracks[2];
static bool moq_setup_sent,moq_peer_setup,moq_finished;
static void moq_reset(void) {
 for(unsigned i=0;i<MOQ_TX_SLOTS;i++)free(moq_tx[i].data);
 memset(moq_tx,0,sizeof(moq_tx));memset(moq_rx,0,sizeof(moq_rx));memset(moq_tracks,0,sizeof(moq_tracks));
 memset(&moq_service_rate,0,sizeof(moq_service_rate));
 memset(&moq_swma_rate,0,sizeof(moq_swma_rate));
 memset(moq_completed,0,sizeof(moq_completed));memset(moq_on_time,0,sizeof(moq_on_time));
 memset(moq_age_sum,0,sizeof(moq_age_sum));memset(moq_age_max,0,sizeof(moq_age_max));
 moq_rejected_feasibility=moq_probes=moq_last_probe=0;
 moq_min_internal=moq_min_psram=SIZE_MAX;
 moq_codec_calls=moq_codec_failures=moq_codec_wall_sum=moq_codec_wall_max=0;moq_codec_scratch_max=0;moq_codec_decode=moq_codec_encode=moq_codec_cpu=moq_pump_gap=0;
 moq_source_total=moq_source_failures=0;moq_source_span=moq_source_gap=0;
 moq_setup_sent=moq_peer_setup=moq_finished=false;memset(&moq_scheduler,0,sizeof(moq_scheduler));moq_blocked=0;memset(moq_choices,0,sizeof(moq_choices));memset(moq_accepted_bytes,0,sizeof(moq_accepted_bytes));memset(moq_timeout_resets,0,sizeof(moq_timeout_resets));memset(moq_timeout_discarded,0,sizeof(moq_timeout_discarded));moq_select_us=0;moq_preview_pace_next=0;moq_preview_unacked_max=0;
}
static struct moq_tx *moq_slot(int64_t id) {
 for(unsigned i=0;i<MOQ_TX_SLOTS;i++)if(moq_tx[i].used&&moq_tx[i].id==id)return &moq_tx[i];
 return NULL;
}
static int moq_queue(ngtcp2_conn *conn,unsigned slot,int64_t id,const uint8_t *head,size_t hlen,const uint8_t *payload,size_t plen,bool fin) {
 if(slot>=MOQ_TX_SLOTS||moq_tx[slot].used||hlen>MOQT21_CONTROL_CAP||plen>MOQT21_MAX_JPEG||(!fin&&hlen+plen>MOQT21_CONTROL_CAP))return -1;
 struct moq_tx *s=&moq_tx[slot];
 uint8_t *copy=heap_caps_malloc(fin?hlen+plen:MOQT21_CONTROL_CAP,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(!copy)return -1;
 memcpy(copy,head,hlen);if(plen)memcpy(copy+hlen,payload,plen);
 if(id<0 && ngtcp2_conn_open_uni_stream(conn,&id,NULL)!=0){free(copy);return -1;}
 *s=(struct moq_tx){.used=true,.fin=fin,.id=id,.data=copy,.len=hlen+plen,.queued_us=esp_timer_get_time()};return 0;
}
static size_t moq_pending(int64_t *id,int *fin,ngtcp2_vec *vec) {
 struct moq_tx *chosen=NULL;size_t slice=SIZE_MAX;
 unsigned ready=0;uint64_t queued[MOQ_TX_SLOTS];
 for(unsigned i=0;i<MOQ_TX_SLOTS;i++){
  struct moq_tx *s=&moq_tx[i];queued[i]=s->queued_us;
  if(s->used && !s->reset_requested && (s->sent<s->len || (s->fin&&!s->fin_sent)))ready|=1U<<i;
 }
 ready&=~moq_blocked;
 size_t preview_outstanding=0;
 for(unsigned i=4;i<4+AIOT_MOQT_IMAGE_SLOTS;i++)preview_outstanding+=moq_tx[i].sent-moq_tx[i].acked;
 if(preview_outstanding>moq_preview_unacked_max)moq_preview_unacked_max=preview_outstanding;
 size_t preview_cap=(size_t)AIOT_MOQT_PREVIEW_UNACKED_KB*1024U;
 size_t preview_allow=moqt_preview_allowance(preview_cap,preview_outstanding,
     AIOT_MOQT_PREVIEW_KBPS?moq_preview_pace_next:0,esp_timer_get_time());
 if(!preview_allow)ready&=~0x30U;
 if(moq_policy>=0){
  moqt_job jobs[6];for(unsigned i=0;i<6;i++)jobs[i]=(moqt_job){
   .admitted_us=moq_tx[i].queued_us,.deadline_us=moq_tx[i].deadline_us,
   .group_id=moq_tx[i].group,.remaining=moq_tx[i].len-moq_tx[i].sent,
   .importance=moq_tx[i].importance};
  uint64_t begin=esp_timer_get_time();int k=moqt_sched_pick(&moq_scheduler,(moqt_policy)moq_policy,ready,jobs,&slice);moq_select_us+=esp_timer_get_time()-begin;
  if(k>=0){chosen=&moq_tx[k];if(k>=3)moq_choices[k==3?0:1]++;}
 }else{
#if AIOT_MOQT_BOUNDED_PUMP
 int selected=moqt_tx_oldest_ready(ready,queued);
 if(selected>=0)chosen=&moq_tx[selected];
#else
 (void)queued;
 for(unsigned i=0;i<MOQ_TX_SLOTS;i++)if(ready&(1U<<i)){chosen=&moq_tx[i];break;}
#endif
 }
 if(chosen){*id=chosen->id;vec->base=chosen->data+chosen->sent;vec->len=chosen->len-chosen->sent;if(chosen>=&moq_tx[4]&&vec->len>preview_allow)vec->len=preview_allow;if(vec->len>slice)vec->len=slice;*fin=chosen->fin&&vec->len==chosen->len-chosen->sent;return 1;}
 *id=-1;*fin=0;vec->base=NULL;vec->len=0;return 0;
}
static int moq_begin_pump(ngtcp2_conn *conn){
 moq_blocked=0;moq_resource_sample();
#if AIOT_MOQT_DELIVERY_TIMEOUT_RESET
 uint64_t now=(uint64_t)esp_timer_get_time();
 for(unsigned i=3;i<MOQ_TX_SLOTS;i++){
  struct moq_tx *s=&moq_tx[i];
  if(!s->used||s->reset_requested||s->deadline_us==UINT64_MAX||now<s->deadline_us)continue;
  int rv=ngtcp2_conn_shutdown_stream_write(conn,0,s->id,MOQT21_STREAM_RESET_DELIVERY_TIMEOUT);
  if(rv)return rv;
  s->reset_requested=true;
  unsigned track=i==3?0:1;size_t discarded=s->len-s->acked;
  moq_timeout_resets[track]++;moq_timeout_discarded[track]+=discarded;
  printf("[MOQ_TIMEOUT_RESET] {\"alias\":%u,\"group\":%llu,\"stream\":%lld,\"age_us\":%llu,\"sent\":%u,\"acked\":%u,\"discarded\":%u}\n",i==3?2U:1U,(unsigned long long)s->group,(long long)s->id,(unsigned long long)(now-s->source_us),(unsigned)s->sent,(unsigned)s->acked,(unsigned)discarded);
 }
#else
 (void)conn;
#endif
 return 0;
}
static void moq_mark_blocked(int64_t id){struct moq_tx *s=moq_slot(id);if(s)moq_blocked|=1U<<(unsigned)(s-moq_tx);}
int quic_client_moq_configure(unsigned policy){if(policy>=MOQT_POLICY_COUNT||g_client.conn)return -1;moq_policy=(int)policy;return 0;}
void quic_client_moq_policy_stats(void){size_t outstanding=0;for(unsigned i=4;i<4+AIOT_MOQT_IMAGE_SLOTS;i++)outstanding+=moq_tx[i].sent-moq_tx[i].acked;printf("[MOQ_POLICY_STATS] {\"ai_choices\":%llu,\"image_choices\":%llu,\"ai_bytes\":%llu,\"image_bytes\":%llu,\"selector_wall_us\":%llu,\"preview_unacked\":%u,\"preview_unacked_max\":%u,\"ai_timeout_resets\":%llu,\"image_timeout_resets\":%llu,\"ai_timeout_discarded\":%llu,\"image_timeout_discarded\":%llu}\n",(unsigned long long)moq_choices[0],(unsigned long long)moq_choices[1],(unsigned long long)moq_accepted_bytes[0],(unsigned long long)moq_accepted_bytes[1],(unsigned long long)moq_select_us,(unsigned)outstanding,(unsigned)moq_preview_unacked_max,(unsigned long long)moq_timeout_resets[0],(unsigned long long)moq_timeout_resets[1],(unsigned long long)moq_timeout_discarded[0],(unsigned long long)moq_timeout_discarded[1]);}
bool quic_client_moq_ai_slot_available(void){return !moq_tx[3].used;}
static void moq_submitted(int64_t id,ngtcp2_ssize n) {
 struct moq_tx *s=moq_slot(id);if(!s||n<0)return;
 if(s>=&moq_tx[3])moq_accepted_bytes[s==&moq_tx[3]?0:1]+=(size_t)n;
 if(moq_policy>=0)moqt_sched_commit(&moq_scheduler,(moqt_policy)moq_policy,(unsigned)(s-moq_tx),(size_t)n);
 s->sent+=(size_t)n;if(s>=&moq_tx[4])moq_preview_pace_next=moqt_preview_pace_advance(moq_preview_pace_next,esp_timer_get_time(),(size_t)n,AIOT_MOQT_PREVIEW_KBPS);if(s->fin&&s->sent==s->len&&!s->fin_sent){s->fin_sent=true;s->fin_us=esp_timer_get_time();}
}
static int moq_acked(int64_t id,uint64_t offset,uint64_t datalen){struct moq_tx *s=moq_slot(id);if(!s)return 0;if(offset!=s->acked||datalen>s->sent-s->acked)return -1;s->acked+=(size_t)datalen;return 0;}
static int moq_stream_close(ngtcp2_conn *conn,uint32_t flags,int64_t id,uint64_t error,void *user,void *stream_user) {
 (void)conn;(void)user;(void)stream_user;
 struct moq_tx *s=moq_slot(id);
 if(s){
  bool app_error=(flags&NGTCP2_STREAM_CLOSE_FLAG_APP_ERROR_CODE_SET)!=0;
  uint64_t closed_us=(uint64_t)esp_timer_get_time();
  if(s>=&moq_tx[4]){
   moqt_rate_observe(&moq_service_rate,s->acked,closed_us-s->queued_us);
   moqt_swma_observe(&moq_swma_rate,s->acked,closed_us-s->queued_us);
  }
  if(!app_error&&s>=&moq_tx[3]){
   unsigned k=s==&moq_tx[3]?0:1;uint64_t age=closed_us-s->source_us;
   moq_completed[k]++;moq_on_time[k]+=closed_us<=s->deadline_us;
   moq_age_sum[k]+=age;if(age>moq_age_max[k])moq_age_max[k]=age;
  }
  if(!app_error&&s==&moq_tx[3]){moq_last_ai_ack_group=s->group;moq_last_ai_source_to_ack_us=(uint64_t)esp_timer_get_time()-s->source_us;moq_last_ai_ack_valid=true;}
  if(!app_error&&moq_policy>=0&&s>=&moq_tx[3])printf("[MOQ_RECEIPT] {\"alias\":%u,\"group\":%llu,\"ack_us\":%llu,\"source_us\":%llu,\"deadline_us\":%llu}\n",s==&moq_tx[3]?2U:1U,(unsigned long long)s->group,(unsigned long long)esp_timer_get_time(),(unsigned long long)s->source_us,(unsigned long long)s->deadline_us);
  if(s>=&moq_tx[3])printf("[MOQ_SLOT] {\"stream\":%lld,\"held_us\":%llu,\"submit_us\":%llu,\"ack_wait_us\":%llu,\"alias\":%u,\"bytes\":%u}\n",(long long)id,(unsigned long long)(esp_timer_get_time()-s->queued_us),(unsigned long long)(s->fin_us?s->fin_us-s->queued_us:0),(unsigned long long)(s->fin_us?esp_timer_get_time()-s->fin_us:0),s==&moq_tx[3]?2U:1U,(unsigned)s->len);
  free(s->data);memset(s,0,sizeof(*s));
 }
 printf("[MOQ_STREAM_CLOSED] id=%lld error=%llu\n",(long long)id,(unsigned long long)error);
 /* RESET_STREAM is an object-level outcome in MOQT, not a connection error. */
 return 0;
}
bool quic_client_moq_last_ai_receipt(uint64_t *group,uint64_t *source_to_ack_us){
 if(!moq_last_ai_ack_valid||!group||!source_to_ack_us)return false;
 *group=moq_last_ai_ack_group;*source_to_ack_us=moq_last_ai_source_to_ack_us;return true;
}
static int moq_recv(ngtcp2_conn *conn,uint32_t flags,int64_t id,uint64_t offset,const uint8_t *data,size_t len) {
 struct moq_rx *s=NULL;
 for(unsigned i=0;i<3;i++)if(moq_rx[i].used&&moq_rx[i].id==id)s=&moq_rx[i];
 if(!s)for(unsigned i=0;i<3;i++)if(!moq_rx[i].used){s=&moq_rx[i];s->used=true;s->id=id;break;}
 if(!s||offset!=s->len||len>sizeof(s->data)-s->len)return NGTCP2_ERR_CALLBACK_FAILURE;
 if(flags&NGTCP2_STREAM_DATA_FLAG_FIN){
  for(unsigned i=0;i<2;i++)if(moq_tracks[i].subscribed&&moq_tracks[i].ending&&id==moq_tracks[i].request_stream&&!len){
   moq_tracks[i].finished=true;moq_finished=true;
   for(unsigned j=0;j<2;j++)if(moq_tracks[j].subscribed&&!moq_tracks[j].finished)moq_finished=false;
   return 0;
  }
  return NGTCP2_ERR_CALLBACK_FAILURE;
 }
 if(s->handled)return NGTCP2_ERR_CALLBACK_FAILURE;
 if(len)memcpy(s->data+s->len,data,len);
 s->len+=len;
 uint64_t type;size_t n,bl;const uint8_t *body;
 int r=moqt21_message_read(s->data,s->len,&type,&body,&bl,&n);
 if(r<0)return NGTCP2_ERR_CALLBACK_FAILURE;
 if(r==1){
  if(n!=s->len)return NGTCP2_ERR_CALLBACK_FAILURE;
  if(type==0x2f00 && (id&3)==3 && !moq_peer_setup){
   if(moqt21_server_setup(body,bl)!=1)return NGTCP2_ERR_CALLBACK_FAILURE;
   moq_peer_setup=true;s->handled=true;printf("[MOQ_SETUP] peer=accepted\n");
   for(unsigned i=0;i<3;i++)if(moq_rx[i].used && (moq_rx[i].id&3)==1 && moq_rx[i].len&&!moq_rx[i].handled){
    if(moq_recv(conn,0,moq_rx[i].id,moq_rx[i].len,NULL,0))return NGTCP2_ERR_CALLBACK_FAILURE;
   }
  }else if(type==3 && (id&3)==1 && !moq_peer_setup){
   /* Independent streams can deliver SUBSCRIBE before SETUP. */
  }else if(type==3 && (id&3)==1 && moq_peer_setup){
   uint64_t request;unsigned alias;
   if(moqt21_subscribe_track(body,bl,&request,&alias)!=1)return NGTCP2_ERR_CALLBACK_FAILURE;
   struct moq_track *t=&moq_tracks[alias-1];if(t->subscribed)return NGTCP2_ERR_CALLBACK_FAILURE;
   for(unsigned i=0;i<2;i++)if(moq_tracks[i].subscribed&&moq_tracks[i].request==request)return NGTCP2_ERR_CALLBACK_FAILURE;
   uint8_t answer[32];size_t alen=moqt21_subscribe_ok_track(answer,sizeof(answer),alias);
   if(moq_queue(conn,alias,id,answer,alen,NULL,0,false))return NGTCP2_ERR_CALLBACK_FAILURE;
   t->request_stream=id;t->request=request;t->subscribed=true;s->handled=true;
   printf("[MOQ_SUBSCRIBED] request=%llu alias=%u\n",(unsigned long long)request,alias);
  }else return NGTCP2_ERR_CALLBACK_FAILURE;
 }
 ngtcp2_conn_extend_max_stream_offset(conn,id,len);ngtcp2_conn_extend_max_offset(conn,len);return 0;
}
int quic_client_moq_start(const char *authority) {
 printf("[MOQ_CONFIG] image_slots=%u preview_kbps=%u preview_unacked_kb=%u timeout_reset=%u\n",(unsigned)AIOT_MOQT_IMAGE_SLOTS,(unsigned)AIOT_MOQT_PREVIEW_KBPS,(unsigned)AIOT_MOQT_PREVIEW_UNACKED_KB,(unsigned)AIOT_MOQT_DELIVERY_TIMEOUT_RESET);
 if(moq_policy>=0)printf("[MOQ_POLICY] %s slice=1024\n",moqt_policy_name((moqt_policy)moq_policy));
 quic_lock_take(portMAX_DELAY);int result=-1;
 if(g_client.conn && !moq_setup_sent && quic_client_is_connected()){
  const ngtcp2_transport_params *rp=ngtcp2_conn_get_remote_transport_params(g_client.conn);
  uint8_t b[256];size_t n=moqt21_setup(b,sizeof(b),authority);
  if(rp&&rp->max_datagram_frame_size&&n && !moq_queue(g_client.conn,0,-1,b,n,NULL,0,false)){moq_setup_sent=true;result=0;}
 }
 quic_lock_give();return result;
}
bool quic_client_moq_track_ready(unsigned alias) {return alias>=1&&alias<=2&&moq_setup_sent&&moq_peer_setup&&moq_tracks[alias-1].subscribed&&!moq_tracks[alias-1].ending;}
/* Called by the single QUIC owner task; capture/AI workers never touch TX. */
static unsigned moq_free_image_slot(void){
 for(unsigned i=4;i<4+AIOT_MOQT_IMAGE_SLOTS;i++)if(!moq_tx[i].used)return i;
 return MOQ_TX_SLOTS;
}
bool quic_client_moq_image_slot_available(void){return moq_free_image_slot()<MOQ_TX_SLOTS;}
bool quic_client_moq_ready(void) {return quic_client_moq_track_ready(1);}
int quic_client_moq_publish_object(unsigned alias,const uint8_t *data,size_t len,uint64_t group,uint64_t source_us,uint64_t deadline_us,unsigned importance) {
 if((uint64_t)esp_timer_get_time()>=deadline_us)return -2;
 if(alias<1||alias>2||!data||!len||len>MOQT21_MAX_JPEG||(alias==2&&len>4096))return -1;
 quic_lock_take(portMAX_DELAY);int result=-1;struct moq_track *t=&moq_tracks[alias-1];
 if(quic_client_moq_track_ready(alias) && (!t->has_group||group>t->last_group)){
#if AIOT_MOQT_FEASIBILITY
  if(alias==1){
   uint64_t now=(uint64_t)esp_timer_get_time();size_t queued=0;
   for(unsigned i=3;i<MOQ_TX_SLOTS;i++)if(moq_tx[i].used)queued+=moq_tx[i].len-moq_tx[i].acked;
   if(!moqt_fits(len,queued,quic_client_moq_service_rate(),now,deadline_us)){
    /* Explicit bounded probe avoids permanent starvation after an outage.
     * It may miss its deadline; it is counted, never called feasible. */
    if(!queued&&(!moq_last_probe||now-moq_last_probe>=5000000)){
     moq_last_probe=now;moq_probes++;
    }else{moq_rejected_feasibility++;quic_lock_give();return -3;}
   }
  }
#endif
  uint8_t h[32];size_t n=moqt21_object_header_track(h,sizeof(h),alias,group,len);
  unsigned slot=alias==2?3:moq_free_image_slot();
  if(n)result=moq_queue(g_client.conn,slot,-1,h,n,data,len,true);
  if(!result){t->has_group=true;t->last_group=group;t->count++;moq_tx[slot].source_us=source_us;moq_tx[slot].deadline_us=deadline_us;moq_tx[slot].importance=importance;moq_tx[slot].group=group;
   if(moq_policy>=0)printf("[MOQ_ADMITTED] {\"alias\":%u,\"group\":%llu,\"source_us\":%llu,\"deadline_us\":%llu,\"admitted_us\":%llu}\n",alias,(unsigned long long)group,(unsigned long long)source_us,(unsigned long long)deadline_us,(unsigned long long)moq_tx[slot].queued_us);
  }
 }
 quic_lock_give();return result;
}
int quic_client_moq_publish_track(unsigned alias,const uint8_t *data,size_t len,uint64_t group){return quic_client_moq_publish_object(alias,data,len,group,esp_timer_get_time(),UINT64_MAX,0);}
int quic_client_moq_publish(const uint8_t *jpeg,size_t len,uint64_t group){return quic_client_moq_publish_track(1,jpeg,len,group);}
int quic_client_moq_finish(void) {
 quic_lock_take(portMAX_DELAY);int result=1;
 for(unsigned i=3;i<MOQ_TX_SLOTS;i++)if(moq_tx[i].used)goto out;
 for(unsigned i=0;i<2;i++){
  struct moq_track *t=&moq_tracks[i];if(!t->subscribed||t->ending)continue;
  struct moq_tx *s=moq_slot(t->request_stream);if(!s){result=-1;goto out;}
  uint8_t body[1024],message[1040];size_t n=0;char reason[900];
  unsigned k=i==0?1:0;
  int reason_len=snprintf(reason,sizeof(reason),"{\"schema\":1,\"completed\":%llu,\"ack_on_time\":%llu,\"ack_age_sum_us\":%llu,\"ack_age_max_us\":%llu,\"timeout_resets\":%llu,\"feasibility_rejects\":%llu,\"probes\":%llu,\"service_Bps\":%llu,\"min_internal\":%u,\"min_psram\":%u,\"codec_calls\":%llu,\"codec_failures\":%llu,\"codec_wall_sum_us\":%llu,\"codec_wall_max_us\":%llu,\"codec_scratch_max\":%u,\"source_count\":%u,\"source_failed\":%u,\"source_span_us\":%llu,\"source_max_gap_us\":%llu,\"codec_decode_us\":%llu,\"codec_encode_us\":%llu,\"codec_cpu_us\":%llu,\"pump_gap_max_us\":%llu}",
    (unsigned long long)moq_completed[k],(unsigned long long)moq_on_time[k],
    (unsigned long long)moq_age_sum[k],(unsigned long long)moq_age_max[k],
    (unsigned long long)moq_timeout_resets[k],(unsigned long long)(i==0?moq_rejected_feasibility:0),
    (unsigned long long)(i==0?moq_probes:0),(unsigned long long)quic_client_moq_service_rate(),
    (unsigned)moq_min_internal,(unsigned)moq_min_psram,
    (unsigned long long)moq_codec_calls,(unsigned long long)moq_codec_failures,
    (unsigned long long)moq_codec_wall_sum,(unsigned long long)moq_codec_wall_max,(unsigned)moq_codec_scratch_max,moq_source_total,moq_source_failures,
    (unsigned long long)moq_source_span,(unsigned long long)moq_source_gap,
    (unsigned long long)moq_codec_decode,(unsigned long long)moq_codec_encode,(unsigned long long)moq_codec_cpu,(unsigned long long)moq_pump_gap);
  if(reason_len<0||reason_len>=(int)sizeof(reason)){result=-1;goto out;}
  body[n++]=2;n+=moqt21_vi_write(body+n,sizeof(body)-n,t->count);
  n+=moqt21_vi_write(body+n,sizeof(body)-n,(size_t)reason_len);
  memcpy(body+n,reason,(size_t)reason_len);n+=(size_t)reason_len;
  size_t m=moqt21_message(message,sizeof(message),0x0b,body,n);
  if(!m||s->len+m>MOQT21_CONTROL_CAP){result=-1;goto out;}
  memcpy(s->data+s->len,message,m);s->len+=m;s->fin=true;t->ending=true;
  printf("[MOQ_PUBLISH_DONE] alias=%u streams=%llu\n",i+1,(unsigned long long)t->count);
 }
 result=0;
 out:quic_lock_give();return result;
}
bool quic_client_moq_finished(void){return moq_finished;}

#if AIOT_MOQT_AUDIT
#undef printf
#endif
