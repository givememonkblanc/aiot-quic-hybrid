#include "moq_audit.h"
#if AIOT_MQTT_BASELINE
#include "moq_mqtt_shim.h"
#endif
#if AIOT_MOQT_AUDIT
#define printf moq_audit_printf
#endif
/* Fixed-policy characterization: 10 Hz capture, latest preview, independent AI
 * result track. No adaptive scheduler, deadline guarantee, or GT accuracy claim. */
#include "freertos/semphr.h"
#include "esp_attr.h"
#include "esp_system.h"
/* Breadcrumb that survives a reset (not a power loss): the state of the last second before the reset, printed by the
 * next boot with the reset reason, to tell a watchdog, a panic, a brownout and a memory exhaustion apart. */
static RTC_NOINIT_ATTR struct {uint32_t magic,uptime_s,heap_min,internal_free,internal_largest,psram_free,ai_completed,boots;} moq_bc;
#define MOQ_BC_MAGIC 0x4D4F5142U
static void moq_bc_update(uint64_t now){
 moq_bc.magic=MOQ_BC_MAGIC;moq_bc.uptime_s=(uint32_t)(now/1000000ULL);moq_bc.heap_min=(uint32_t)esp_get_minimum_free_heap_size();
 moq_bc.internal_free=(uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
 moq_bc.internal_largest=(uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
 moq_bc.psram_free=(uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);moq_bc.ai_completed=(uint32_t)source_manager_ai_completed();
}
#include "moqt_feasibility.h"
#include "moq_representation.h"
#if AIOT_MOQT_OFFLOAD_RASTER
#include "../components/ai_inference/include/ai_inference.h"
#endif
#include "moq_codec_worker.h"
/* the helper's frame is the detector's raster (split inference, direction task), not the captured JPEG; with
 * AIOT_MOQT_SHARED_INPUT the raster quality serves the evidence only and the helper gets the captured JPEG */
#define MOQ_HELPER_RASTER (AIOT_MOQT_OFFLOAD_RASTER&&!AIOT_MOQT_SHARED_INPUT)
#include "moq_passage.h"
#ifndef AIOT_MOQT_PASSAGE_ASSOC
#define AIOT_MOQT_PASSAGE_ASSOC 0
#endif
#if AIOT_MOQT_LADDER
extern const uint8_t moq_medium_start[] asm("_binary_moq_medium_start");
extern const uint8_t moq_medium_end[] asm("_binary_moq_medium_end");
extern const uint8_t moq_low_start[] asm("_binary_moq_low_start");
extern const uint8_t moq_low_end[] asm("_binary_moq_low_end");
#endif
#define MOQ_IMAGE_CAP (256U*1024U)
#define MOQ_RUN_US ((uint64_t)AIOT_MOQT_RUN_SECONDS*1000000ULL)
#define MOQ_DRAIN_US (MOQ_RUN_US+10000000ULL)
#define MOQ_LIMIT_US (MOQ_RUN_US+40000000ULL)
static uint8_t *moq_latest,*moq_stage,*moq_converted;
static size_t last_medium_bytes,last_low_bytes;
#if AIOT_MOQT_ASYNC_CODEC
static struct {uint32_t seq;uint64_t capture,deadline;unsigned level;size_t len;bool ready;} moq_encoding;
#endif
/* Application Object Properties (draft-21 section 8.4 reserves 0x78-0x7F). Even types
 * carry integers, so subscribers can select on them with OBJECT_PROPERTY_FILTER. */
#define MOQ_PROP_CAPTURE_MS 0x78
#define MOQ_PROP_DETECTIONS 0x7A
#define MOQ_PROP_EVIDENCE_REQUIRED 0x7C
#define MOQ_PROP_REPRESENTATION 0x7E
/* Two-byte application range (section 8.4). */
#define MOQ_PROP_PASSAGES_TOTAL 0x3800
#define MOQ_PROP_PASSAGES_NEW 0x3802
#define MOQ_ALIAS_PREVIEW 1
#define MOQ_ALIAS_RESULTS 2
#define MOQ_ALIAS_EVIDENCE 3
#define MOQ_ALIAS_STATUS 4
#if AIOT_MOQT_WIRE_PROFILE && AIOT_MOQT_EVENT_EVIDENCE
#define MOQ_ALIAS_IMAGE MOQ_ALIAS_EVIDENCE
#else
#define MOQ_ALIAS_IMAGE MOQ_ALIAS_PREVIEW
#endif
#if AIOT_MOQT_MISSION_COUNT
/* Mission: no passage event is lost, even when the link is poor. Results carry the
 * events, so they have the highest priority and no delivery timeout; images give way. */
static moq_passage_counter moq_passages;
#if AIOT_MOQT_PASSAGE_SYNTHETIC
static unsigned moq_passage_results;
#endif
/* Runs in the inference task for every result, before the result is queued. A result that
 * finds no room in the queue is counted all the same. */
#if AIOT_MOQT_MUS
static void mus_add(uint32_t seq,uint64_t capture_us,const uint8_t *image,size_t len);
#endif
#if AIOT_MOQT_OFFLOAD_TASK_MS
/* Direction task (AIOT_MOQT_OFFLOAD_TASK_MS = T_max): every counted passage asks for the
 * orientation of its screw within T_max of the passage. The camera has no time for it (its
 * inference runs back to back for the count), so the task is done at the helper on the newest
 * frame, which still shows the screw near the line; a task that has not gone out by T_max lapses.
 * One producer (the inference task) and one consumer (the publishing loop). */
#define MOQ_TASKS 8
static struct {uint32_t id[MOQ_TASKS],seq[MOQ_TASKS];uint64_t at[MOQ_TASKS];volatile uint32_t head,tail;unsigned added,sent,lapsed,dropped;} moq_task;
static void moq_task_add(uint32_t id,uint32_t seq){
 if(moq_task.head-moq_task.tail>=MOQ_TASKS){moq_task.dropped++;return;}
 moq_task.id[moq_task.head%MOQ_TASKS]=id;moq_task.seq[moq_task.head%MOQ_TASKS]=seq;moq_task.at[moq_task.head%MOQ_TASKS]=esp_timer_get_time();
 moq_task.head++;moq_task.added++;
}
/* the age of the oldest open task; the loop reads its clock before a task of the inference task
 * may arrive, so a task can be younger than now */
static uint64_t moq_task_age(uint64_t now){
 uint64_t at=moq_task.at[moq_task.tail%MOQ_TASKS];
 return now>at?now-at:0;
}
/* whether a task is open, after the ones past T_max have lapsed */
static bool moq_task_open(uint64_t now){
 while(moq_task.tail!=moq_task.head&&moq_task_age(now)>(uint64_t)AIOT_MOQT_OFFLOAD_TASK_MS*1000){
  printf("[MOQ_TASK_LAPSED] {\"passage\":%lu}\n",(unsigned long)moq_task.id[moq_task.tail%MOQ_TASKS]);
  moq_task.tail++;moq_task.lapsed++;
 }
 return moq_task.tail!=moq_task.head;
}
/* time left to the T_max of the oldest open task, us */
static uint64_t moq_task_left(uint64_t now){
 uint64_t end=moq_task.at[moq_task.tail%MOQ_TASKS]+(uint64_t)AIOT_MOQT_OFFLOAD_TASK_MS*1000;
 return end>now?end-now:0;
}
#define MOQ_TASK_GATE(now) moq_task_open(now)
#else
#define MOQ_TASK_GATE(now) true
#endif
#if AIOT_MOQT_OFFLOAD_RASTER
/* AIOT_MOQT_OFFLOAD_RASTER (JPEG quality): the frame that goes to the helper is the frame as the
 * camera's detector decoded it (1/8, 200x150), kept by the inference task and encoded when it goes
 * out; the camera decodes nothing more. With a direction task, the newest passage frame; with
 * AIOT_MOQT_OFFLOAD_SPLIT, the frame whose model runs at the helper. */
#define MOQ_RASTER_CAP (200U*150U*2U)
static struct {uint8_t *buf;uint16_t w,h;uint32_t seq;uint64_t cap_us;bool valid;SemaphoreHandle_t lock;unsigned kept,busy,failed;} moq_rs;
#if AIOT_MOQT_OFFLOAD_TASK_MS
static void moq_raster_keep(const aiot_ai_result_t *result,uint32_t frame_ms){
 if(!moq_rs.lock||!moq_rs.buf)return;
 if(xSemaphoreTake(moq_rs.lock,0)!=pdTRUE){moq_rs.busy++;return;}   /* being encoded */
 if(ai_inference_copy_raster(moq_rs.buf,MOQ_RASTER_CAP,result->input_width,result->input_height)){
  moq_rs.w=result->input_width;moq_rs.h=result->input_height;moq_rs.seq=result->source_sequence;
  moq_rs.cap_us=(uint64_t)frame_ms*1000;moq_rs.valid=true;moq_rs.kept++;
 }
 xSemaphoreGive(moq_rs.lock);
}
#endif
#endif
#if AIOT_MOQT_SHARED_FRAMES
/* Shared frame track: the evidence of a passage is the detector's own raster of that frame (200x150, already
 * decoded for the inference), queued here by the inference task and published by the publishing loop on the
 * helper's frame track (offload/jpeg), Group = frame. The operator fetches it by Group from the relay when a
 * result asks for evidence; the relay pushes the same track live to the helper. No evidence conversion. */
#define MOQ_EV_SLOTS 4U
static struct {SemaphoreHandle_t lock;uint8_t *buf[MOQ_EV_SLOTS];uint16_t w[MOQ_EV_SLOTS],h[MOQ_EV_SLOTS];uint32_t seq[MOQ_EV_SLOTS];
 uint64_t cap[MOQ_EV_SLOTS];unsigned head,tail,queued,dropped,published,expired,failed;} moq_ev;
static void moq_ev_keep(const aiot_ai_result_t *result,uint32_t frame_ms){
 if(!moq_ev.lock)return;
 xSemaphoreTake(moq_ev.lock,portMAX_DELAY);
 if(moq_ev.head-moq_ev.tail>=MOQ_EV_SLOTS){moq_ev.dropped++;xSemaphoreGive(moq_ev.lock);return;}
 unsigned k=moq_ev.head%MOQ_EV_SLOTS;
 if(moq_ev.buf[k]&&ai_inference_copy_raster(moq_ev.buf[k],MOQ_RASTER_CAP,result->input_width,result->input_height)){
  moq_ev.w[k]=result->input_width;moq_ev.h[k]=result->input_height;moq_ev.seq[k]=result->source_sequence;
  moq_ev.cap[k]=(uint64_t)frame_ms*1000;moq_ev.head++;moq_ev.queued++;
 }else moq_ev.failed++;
 xSemaphoreGive(moq_ev.lock);
}
#endif
#if AIOT_MOQT_SHARED_INPUT
/* the evidence goes before the next helper frame: a captured JPEG (~60 kB) holds an image slot for a long time, and
 * frames sent back to back would keep the evidence out of the slots until its deadline */
#define MOQ_EV_WAITING (moq_ev.head!=moq_ev.tail)
#else
#define MOQ_EV_WAITING 0
#endif
#if AIOT_MOQT_OFFLOAD_GENERAL
static uint64_t moq_gen_active_us;   /* the last detection of the camera */
static uint64_t moq_gen_local_at,moq_gen_local_per=1000000;   /* the camera's last result and its period */
#endif
static void moq_passage_hook(const aiot_ai_result_t *result,uint32_t frame_ms,const uint8_t *image,size_t image_len,
                             uint32_t *count_new,uint32_t *count_total){
 moq_passage_point points[AIOT_AI_RESULT_MAX_DETECTIONS];moq_passage_event events[MOQ_PASSAGE_EVENTS];
 unsigned used=result->detection_count<AIOT_AI_RESULT_MAX_DETECTIONS?result->detection_count:AIOT_AI_RESULT_MAX_DETECTIONS;
#if AIOT_MOQT_OFFLOAD_GENERAL
 if(used)moq_gen_active_us=esp_timer_get_time();   /* the general rule's value signal: activity */
 {uint64_t t=esp_timer_get_time();
  if(moq_gen_local_at&&t-moq_gen_local_at<5000000ULL)moq_gen_local_per=(3*moq_gen_local_per+(t-moq_gen_local_at))/4;
  moq_gen_local_at=t;}
#endif
 for(unsigned i=0;i<used;i++)points[i]=(moq_passage_point){result->detections[i].x,result->detections[i].y};
#if AIOT_MOQT_PASSAGE_SYNTHETIC
 /* Test workload for a replayed still image: the detections keep their y and are
  * spread over x, moving 20 units per result, so that they cross the line. */
 for(unsigned i=0;i<used;i++)points[i].x=(uint8_t)(i*256U/used+moq_passage_results*20U);
 moq_passage_results++;
#endif
 unsigned fresh=moq_passage_update_at(&moq_passages,points,used,result->source_sequence,frame_ms,events);
#if AIOT_MOQT_PASSAGE_TRACE
 /* Per inference: the detections and the tracks after the update, to tell a passage that fell
  * between two inferences from one that was seen but not counted (event-level recall). */
 {char line[640];int n=snprintf(line,sizeof(line),"[MOQ_TRACK] {\"seq\":%lu,\"cap_ms\":%lu,\"pts\":[",
   (unsigned long)result->source_sequence,(unsigned long)frame_ms);
  for(unsigned i=0;i<used&&n<(int)sizeof(line)-40;i++)n+=snprintf(line+n,sizeof(line)-n,"%s[%u,%u]",i?",":"",points[i].x,points[i].y);
  n+=snprintf(line+n,sizeof(line)-n,"],\"tracks\":[");
  bool first=true;
  for(unsigned i=0;i<MOQ_PASSAGE_TRACKS&&n<(int)sizeof(line)-48;i++){
   if(!moq_passages.tracks[i].used)continue;
   n+=snprintf(line+n,sizeof(line)-n,"%s[%u,%u,%u,%u,%d]",first?"":",",moq_passages.tracks[i].x,moq_passages.tracks[i].y,
     (unsigned)moq_passages.tracks[i].counted,(unsigned)moq_passages.tracks[i].missed,(int)moq_passages.tracks[i].v);first=false;
  }
  if(n<(int)sizeof(line))snprintf(line+n,sizeof(line)-n,"],\"flow\":%d,\"new\":%u}\n",(int)moq_passages.flow,fresh);
  fputs(line,stdout);}
#endif
 for(unsigned i=0;i<fresh&&i<MOQ_PASSAGE_EVENTS;i++)
  printf("[MOQ_PASSAGE] {\"id\":%lu,\"seq\":%lu,\"x\":%u,\"y\":%u,\"direction\":%d}\n",(unsigned long)events[i].id,(unsigned long)events[i].source_sequence,(unsigned)events[i].x,(unsigned)events[i].y,(int)events[i].direction);
 *count_new=fresh;*count_total=moq_passages.total;
#if AIOT_MOQT_OFFLOAD_TASK_MS
 for(unsigned i=0;i<fresh&&i<MOQ_PASSAGE_EVENTS;i++)moq_task_add(events[i].id,result->source_sequence);
#if AIOT_MOQT_OFFLOAD_RASTER
 if(fresh)moq_raster_keep(result,frame_ms);
#endif
#endif
#if AIOT_MOQT_SHARED_FRAMES
 if(fresh)moq_ev_keep(result,frame_ms);
 (void)image;(void)image_len;
#elif AIOT_MOQT_MUS
 /* The source is kept when the result is produced; it may be replaced before the result is sent. */
 if(fresh)mus_add(result->source_sequence,(uint64_t)frame_ms*1000,image,image_len);
#else
 (void)frame_ms;(void)image;(void)image_len;
#endif
}
#if AIOT_FRAME_TRACE_PORT
/* Frame trace: every captured JPEG, with its sequence and capture time, over plain TCP to the host
 * (AIOT_NETLOG_HOST:AIOT_FRAME_TRACE_PORT), for offline replay of inference policies (1, 2, 3
 * workers) at the full input rate. A low-priority task sends from three PSRAM slots; when they are
 * full a frame is dropped and counted, the capture is never delayed. */
#include "lwip/sockets.h"
#define FT_SLOTS 3U
#define FT_CAP (256U*1024U)
static uint8_t *ft_buf[FT_SLOTS];static size_t ft_len[FT_SLOTS];static uint32_t ft_seq[FT_SLOTS];static uint64_t ft_ts[FT_SLOTS];
static volatile unsigned ft_head,ft_tail;static unsigned ft_drops,ft_sent;static SemaphoreHandle_t ft_sem;
static bool ft_send_all(int s,const void *b,size_t n){const uint8_t *p=b;while(n){int k=send(s,p,n,0);if(k<=0)return false;p+=k;n-=(size_t)k;}return true;}
static void ft_task(void *arg){
 (void)arg;int s=-1;
 for(;;){
  xSemaphoreTake(ft_sem,pdMS_TO_TICKS(1000));
  while(ft_tail!=ft_head){
   if(s<0){
    struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(AIOT_FRAME_TRACE_PORT)};inet_pton(AF_INET,AIOT_NETLOG_HOST,&a.sin_addr);
    s=socket(AF_INET,SOCK_STREAM,0);
    if(s<0||connect(s,(struct sockaddr*)&a,sizeof(a))!=0){if(s>=0)close(s);s=-1;vTaskDelay(pdMS_TO_TICKS(1000));break;}
   }
   unsigned k=ft_tail%FT_SLOTS;uint8_t h[20]={'F','R','M','1'};
   memcpy(h+4,&ft_seq[k],4);memcpy(h+8,&ft_ts[k],8);uint32_t n=(uint32_t)ft_len[k];memcpy(h+16,&n,4);
   if(!ft_send_all(s,h,sizeof(h))||!ft_send_all(s,ft_buf[k],ft_len[k])){close(s);s=-1;break;}
   ft_tail++;ft_sent++;
   if(ft_sent%100==0)printf("[FRAME_TRACE] {\"sent\":%u,\"drops\":%u}\n",ft_sent,ft_drops);
  }
 }
}
static void ft_push(const uint8_t *data,size_t len,uint32_t seq,uint64_t ts){
 if(!ft_sem){
  for(unsigned i=0;i<FT_SLOTS;i++)ft_buf[i]=heap_caps_malloc(FT_CAP,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  ft_sem=xSemaphoreCreateBinary();
  xTaskCreatePinnedToCore(ft_task,"frame_trace",4096,NULL,2,NULL,0);
 }
 if(!ft_buf[FT_SLOTS-1]||len>FT_CAP||ft_head-ft_tail>=FT_SLOTS){ft_drops++;return;}
 unsigned k=ft_head%FT_SLOTS;memcpy(ft_buf[k],data,len);ft_len[k]=len;ft_seq[k]=seq;ft_ts[k]=ts;ft_head++;
 xSemaphoreGive(ft_sem);
}
#endif
#if AIOT_MOQT_OFFLOAD_MS
#define MOQ_OFF_CAP (256U*1024U)
#define MOQ_OFF_SMALL_CAP (64U*1024U)
/* the bytes that an offload decision takes: the frame, or with OFFLOAD_EIGHTH the size of the
 * last frame at 1/8 (the frame is converted only once the decision to send it is taken) */
#if AIOT_MOQT_OFFLOAD_EIGHTH||MOQ_HELPER_RASTER
static size_t moq_off_small_last=4096;
#define MOQ_OFF_DECIDE_LEN (moq_off_small_last)
#else
#define MOQ_OFF_DECIDE_LEN (moq_off_len)
#endif
#if MOQ_HELPER_RASTER
#define MOQ_OFF_HAVE (moq_rs.valid&&moq_rs.seq!=moq_off_sent_seq)
#else
#define MOQ_OFF_HAVE (moq_off_len&&moq_off_seq!=moq_off_sent_seq)
#endif
static uint8_t *moq_off_buf;static size_t moq_off_len;static uint32_t moq_off_seq,moq_off_sent_seq;static uint64_t moq_off_capture,moq_off_at;
static unsigned moq_off_sent,moq_off_busy,moq_off_skipped;static uint32_t moq_off_cycle;
#endif
static const moqt_track_cfg moq_wire_tracks[]={
#if AIOT_MOQT_OFFLOAD_MS
 /* Offload: frames that the local detector does not take go to a helper (another ESP32 that
  * subscribes through the relay); stale frames are useless, so the timeout is short and runs from
  * the creation of the Object. The track takes the unused preview slot: an image slot, so the MBM
  * window bounds its bytes together with the evidence. */
 {.ns={"aiot","esp32"},.ns_count=2,.name="offload/jpeg",.priority=96,.group_order=MOQT_ORDER_DESCENDING,
  .object_timeout_ms=AIOT_MOQT_OFFLOAD_TIMEOUT_MS},
#else
 {.ns={"aiot","esp32"},.ns_count=2,.name="preview/jpeg",.priority=224,.group_order=MOQT_ORDER_DESCENDING,
  .object_timeout_ms=AIOT_MOQT_PREVIEW_DEADLINE_MS,.source_timeout_ms=AIOT_MOQT_PREVIEW_DEADLINE_MS},
#endif
#if AIOT_MOQT_TIMEOUT_FROM_CREATION
 /* Base reproduction: only the delivery timeout, from the creation of the Object; no source
  * timeout from the capture (a result is created about 1 s after its capture, the evidence 2 s) */
 {.ns={"aiot","esp32"},.ns_count=2,.name="ai/events",.priority=AIOT_MOQT_MISSION_RESULT_PRIORITY,
  .object_timeout_ms=AIOT_MOQT_MISSION_RESULT_TIMEOUT_MS},
 {.ns={"aiot","esp32"},.ns_count=2,.name="evidence/jpeg",.priority=128,.max_cache_ms=60000,
  .object_timeout_ms=AIOT_MOQT_AI_DEADLINE_MS},
#else
 {.ns={"aiot","esp32"},.ns_count=2,.name="ai/events",.priority=AIOT_MOQT_MISSION_RESULT_PRIORITY,
  .object_timeout_ms=AIOT_MOQT_MISSION_RESULT_TIMEOUT_MS,.source_timeout_ms=AIOT_MOQT_MISSION_RESULT_TIMEOUT_MS},
 {.ns={"aiot","esp32"},.ns_count=2,.name="evidence/jpeg",.priority=128,.max_cache_ms=60000,
  .source_timeout_ms=AIOT_MOQT_MISSION_IMAGE_TIMEOUT?AIOT_MOQT_AI_DEADLINE_MS:0},
#endif
 {.ns={"aiot","esp32"},.ns_count=2,.name="status",.priority=240,.object_timeout_ms=1000,.dynamic_groups=true},
};
#elif AIOT_MOQT_WIRE_PROFILE
static const moqt_track_cfg moq_wire_tracks[]={
 {.ns={"aiot","esp32"},.ns_count=2,.name="preview/jpeg",.priority=192,.group_order=MOQT_ORDER_DESCENDING,
  .object_timeout_ms=AIOT_MOQT_PREVIEW_DEADLINE_MS,.source_timeout_ms=AIOT_MOQT_PREVIEW_DEADLINE_MS},
 /* The source timeout counts from the capture; it is announced when the extension is built in. */
 {.ns={"aiot","esp32"},.ns_count=2,.name="ai/events",.priority=64,.object_timeout_ms=AIOT_MOQT_AI_DEADLINE_MS,
  .source_timeout_ms=AIOT_MOQT_AI_DEADLINE_MS},
 {.ns={"aiot","esp32"},.ns_count=2,.name="evidence/jpeg",.priority=128,.object_timeout_ms=AIOT_MOQT_AI_DEADLINE_MS,
  .max_cache_ms=60000,.source_timeout_ms=AIOT_MOQT_AI_DEADLINE_MS},
 {.ns={"aiot","esp32"},.ns_count=2,.name="status",.priority=224,.object_timeout_ms=1000,.dynamic_groups=true},
};
#endif
#if AIOT_MOQT_WIRE_PROFILE
static size_t moq_props(uint8_t *b,size_t cap,const uint64_t pairs[][2],unsigned count){
 moqt_wr w=moqt_wr_of(b,cap);uint64_t prev=0;
 for(unsigned i=0;i<count;i++)moqt_kvp_put_int(&w,&prev,pairs[i][0],pairs[i][1]);
 return w.bad?0:w.n;
}
static void moq_publish_status(uint64_t now,uint32_t group){
 char json[160];
 int n=snprintf(json,sizeof(json),"{\"schema\":1,\"uptime_ms\":%llu,\"internal_free\":%u,\"psram_free\":%u,\"ai_completed\":%lu}",
  (unsigned long long)(now/1000),(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT),(unsigned long)source_manager_ai_completed());
 if(n<=0||n>=(int)sizeof(json))return;
 quic_moq_object q={.alias=MOQ_ALIAS_STATUS,.data=(const uint8_t*)json,.len=(size_t)n,.group=group,.end_of_group=true,
  .datagram=true,.source_us=now,.deadline_us=UINT64_MAX};
 quic_client_moq_publish_ex(&q);
}
#endif
/* With the mission-utility scheduler an event may receive several evidence objects, a
 * smaller one after an abandoned one or a better one after a smaller one. They are the
 * objects of one Group, each in its own Subgroup. */
static uint64_t moq_media_object;
static void moq_publish_media(const uint8_t *media,size_t len,uint32_t seq,uint64_t capture,uint64_t deadline,unsigned level) {
#if AIOT_MOQT_TIMEOUT_FROM_CREATION
 /* Implicit adaptation (Freeman et al., MoQ point clouds): the delivery timeout runs from the
  * creation of the Object, here the moment it is published, not from the capture. */
 deadline=esp_timer_get_time()+(uint64_t)(AIOT_MOQT_EVENT_EVIDENCE?AIOT_MOQT_AI_DEADLINE_MS:AIOT_MOQT_PREVIEW_DEADLINE_MS)*1000;
#endif
#if AIOT_MOQT_WIRE_PROFILE
 uint8_t props[32];const uint64_t pairs[][2]={{MOQ_PROP_CAPTURE_MS,capture/1000},{MOQ_PROP_REPRESENTATION,level}};
 quic_moq_object q={.alias=MOQ_ALIAS_IMAGE,.data=media,.len=len,.group=seq,.object=moq_media_object,.subgroup=moq_media_object,
  .end_of_group=!AIOT_MOQT_MUS,
  .retain=MOQ_ALIAS_IMAGE==MOQ_ALIAS_EVIDENCE,.props=props,.props_len=moq_props(props,sizeof(props),pairs,2),
  .source_us=capture,.deadline_us=deadline};
 int accepted=quic_client_moq_publish_ex(&q);
#elif AIOT_MOQT_POLICY_EXPERIMENT
 int accepted=quic_client_moq_publish_object(1,media,len,seq,capture,deadline,0);
#else
 int accepted=quic_client_moq_publish(media,len,seq);
#endif
 uint8_t digest[32];char hex[65];
 mbedtls_sha256(media,len,digest,0);for(unsigned i=0;i<32;i++)snprintf(hex+2*i,3,"%02x",digest[i]);
 printf("[MOQ_REPRESENTATION] {\"group\":%lu,\"level\":%u,\"bytes\":%u,\"service_Bps\":%llu,\"admission\":%d}\n",(unsigned long)seq,level,(unsigned)len,(unsigned long long)quic_client_moq_service_rate(),accepted);
 printf("[MOQ_OBJECT] {\"group\":%lu,\"capture_us\":%llu,\"bytes\":%u,\"accepted\":%d,\"sha256\":\"%s\"}\n",(unsigned long)seq,(unsigned long long)capture,(unsigned)len,accepted==0,hex);
}
static void moq_observe_codec(moq_codec_result r,unsigned level) {
 moq_representation_metrics m=r.metrics;
 quic_client_moq_codec_observed(m.decode_us,m.encode_us,r.cpu_us,r.ok,m.scratch_bytes);
 if(r.ok){if(level==1)last_medium_bytes=m.bytes;else last_low_bytes=m.bytes;}
}
static SemaphoreHandle_t moq_image_lock;
static size_t moq_latest_len;
static uint32_t moq_latest_seq;
static uint64_t moq_latest_capture;
static bool moq_capture_stop,moq_capture_stopped;
static uint64_t moq_source_first,moq_source_last,moq_source_max_gap;
/* Set before task creation; producer enforces its own measurement cutoff. */
static uint64_t moq_source_stop_us;
static unsigned moq_source_count,moq_source_failed;
#if AIOT_MOQT_MUS
#include "app_moq_mus.inc"
#endif
#if AIOT_MOQT_OFFLOAD_SCHED
/* Offload scheduling: the helper's frame uses what the evidence leaves of the link, and only
 * when it can arrive. The rate is that of the offload objects themselves (bytes acknowledged
 * over the time under way, smoothed with its deviation): the service rate of the image track
 * reflects the small evidence objects that the MBM window keeps short, a fraction of what a
 * large object gets (10/10: 10 kB/s for the evidence, 55 kB/s for offload frames at 20 Mbit/s).
 * Without a sample, and every MOQ_OFF_PROBE_US while frames are refused as infeasible, one frame
 * is sent as a probe; the cut stops it when its own progress cannot end it in time.
 * why: 0 not used, 1 evidence first, 2 cannot end within the timeout at rate - deviation,
 * 3 an offload is under way; cut: offloads stopped early. */
#define MOQ_OFF_PROBE_US 4000000ULL
static unsigned moq_off_refused[4],moq_off_cut,moq_off_probes;static uint32_t moq_off_refused_seq;
static struct {uint64_t group,start,t;size_t acked;bool on;} moq_off_fl;
static uint64_t moq_off_rate,moq_off_dev,moq_off_last_try;static unsigned moq_off_samples;
static void moq_off_observe(uint64_t now){
 uint64_t g,dl;size_t b,a;bool on=quic_client_moq_offload_flight(&g,&b,&a,&dl);
 if(moq_off_fl.on&&(!on||g!=moq_off_fl.group)){
  uint64_t el=moq_off_fl.t-moq_off_fl.start;
  if(el>=200000&&moq_off_fl.acked){
   uint64_t r=(uint64_t)moq_off_fl.acked*1000000ULL/el;
   if(!moq_off_samples){moq_off_rate=r;moq_off_dev=r/4;}
   else{uint64_t d=r>moq_off_rate?r-moq_off_rate:moq_off_rate-r;moq_off_dev=(3*moq_off_dev+d)/4;moq_off_rate=(3*moq_off_rate+r)/4;}
   moq_off_samples++;
  }
  moq_off_fl.on=false;
 }
 if(on){
  if(!moq_off_fl.on){moq_off_fl.on=true;moq_off_fl.group=g;moq_off_fl.start=now;}
  moq_off_fl.acked=a;moq_off_fl.t=now;
 }
}
static uint64_t moq_off_rate_low(void){
 uint64_t dev=moq_off_dev<moq_off_rate/20?moq_off_rate/20:moq_off_dev;
 return moq_off_rate>dev?moq_off_rate-dev:0;
}
static int moq_off_decide(uint64_t now,size_t len){
 if(moq_off_fl.on)return 3;
#if AIOT_MOQT_MUS
 /* 1: while the planner may still improve an event; 2: only while the evidence needs the link */
 if(AIOT_MOQT_OFFLOAD_SCHED==2?mus_needs_link(now):mus_busy(now))return 1;
#endif
 if(!moq_off_samples||now-moq_off_last_try>=MOQ_OFF_PROBE_US){moq_off_probes++;return -1;}
 uint64_t low=moq_off_rate_low();
 if(!low||(uint64_t)len*1000000ULL/low>(uint64_t)AIOT_MOQT_OFFLOAD_TIMEOUT_MS*1000)return 2;
 return -1;
}
/* decided at most every 20 ms: a mission-aware offload asks on every pass of the loop */
static int moq_off_admit(uint64_t now,size_t len){
 static uint64_t at;static int why;
 if(at&&now-at<20000)return why;
 at=now;why=moq_off_decide(now,len);
 if(why<0)moq_off_last_try=now;
 return why;
}
/* Every 20 ms: the progress of the offload under way; one that cannot end before its deadline
 * at its own pace is stopped, so its remaining bytes do not delay the evidence and the results. */
static void moq_off_cut_late(uint64_t now){
 static uint64_t at,report;
 if(now-at<20000)return;
 at=now;moq_off_observe(now);
 if(now-report>=20000000ULL){report=now;
  printf("[MOQ_OFFLOAD_SCHED] {\"rate\":%llu,\"dev\":%llu,\"samples\":%u,\"probes\":%u,\"refused_evidence\":%u,\"refused_infeasible\":%u,\"refused_in_flight\":%u,\"cut\":%u}\n",
         (unsigned long long)moq_off_rate,(unsigned long long)moq_off_dev,moq_off_samples,moq_off_probes,moq_off_refused[1],moq_off_refused[2],moq_off_refused[3],moq_off_cut);}
 uint64_t g,dl;size_t b,a;
 if(!moq_off_fl.on||!quic_client_moq_offload_flight(&g,&b,&a,&dl))return;
 uint64_t el=now-moq_off_fl.start;
 if(el<300000||!a)return;
 uint64_t finish=now+(uint64_t)(b-a)*el/a;
 if(finish>dl&&quic_client_moq_image_abort(g)){
  moq_off_cut++;
  printf("[MOQ_OFFLOAD_CUT] {\"seq\":%lu,\"bytes\":%u,\"acked\":%u,\"elapsed_us\":%llu,\"left_us\":%lld}\n",(unsigned long)g,(unsigned)b,(unsigned)a,
         (unsigned long long)el,(long long)(dl-now));
 }
}
#endif
#if AIOT_MOQT_OFFLOAD_MS && AIOT_MOQT_OFFLOAD_MODE==2
/* Vigil offload (Zhang et al., MobiCom 2015, §3.1 and §3.2.1), the base system's frame upload,
 * with the helper in the place of the cloud: every epoch Te (AIOT_MOQT_OFFLOAD_MS) the frames
 * are ranked by frameUtility, the number of objects in them, and uploaded in decreasing utility
 * as long as they fit in C x Te, C the estimated capacity of the link; a frame of utility 0 is not
 * uploaded, nor one that does not fit. Vigil's ECN analyses every frame; the camera here analyses
 * one frame per inference, so a frame's utility is the number of screws tracked when it arrives.
 * One frame per epoch (the candidate buffer holds one: the frame of the highest utility, the
 * newer on a tie). C: the congestion window over the smoothed round trip (Vigil: R (1 - L), the
 * physical rate less the losses, at the ECN's radio); AIOT_MOQT_VIGIL_CAPACITY_BPS > 0 gives C as
 * the true rate of the bottleneck instead (an oracle). Urgent data keeps its priority (Vigil's
 * high-priority queue): the evidence goes first by the priorities of the tracks. No deadline. */
static uint8_t *moq_vg_buf;static size_t moq_vg_len;static uint32_t moq_vg_seq,moq_vg_seen;
static uint64_t moq_vg_capture,moq_vg_epoch;static int moq_vg_util=-1;
static unsigned moq_vg_epochs,moq_vg_zero,moq_vg_over,moq_vg_busy;
static void moq_vigil(uint64_t now,uint64_t epoch){
 if(!moq_vg_buf)moq_vg_buf=heap_caps_malloc(MOQ_OFF_CAP,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
 if(!moq_vg_buf||now-epoch>=MOQ_RUN_US)return;
 if(moq_off_len&&moq_off_seq!=moq_vg_seen){
  moq_vg_seen=moq_off_seq;int util=0;
  for(unsigned i=0;i<MOQ_PASSAGE_TRACKS;i++)if(moq_passages.tracks[i].used)util++;
  if(util>=moq_vg_util){memcpy(moq_vg_buf,moq_off_buf,moq_off_len);moq_vg_len=moq_off_len;moq_vg_seq=moq_off_seq;
   moq_vg_capture=moq_off_capture;moq_vg_util=util;}
 }
 if(!moq_vg_epoch)moq_vg_epoch=now;
 if(now-moq_vg_epoch<(uint64_t)AIOT_MOQT_OFFLOAD_MS*1000)return;
 uint64_t c=AIOT_MOQT_VIGIL_CAPACITY_BPS?(uint64_t)AIOT_MOQT_VIGIL_CAPACITY_BPS:quic_client_moq_capacity_Bps();
 uint64_t te=now-moq_vg_epoch,budget=c*te/1000000ULL;
 moq_vg_epoch=now;moq_vg_epochs++;
 int util=moq_vg_util;moq_vg_util=-1;
 if(util<=0||!moq_vg_len){moq_vg_zero++;return;}
 if(moq_vg_len>budget){moq_vg_over++;return;}
 if(!quic_client_moq_image_slot_available()){moq_vg_busy++;moq_off_busy++;return;}
 uint8_t props[16];const uint64_t pairs[][2]={{MOQ_PROP_CAPTURE_MS,moq_vg_capture/1000}};
 quic_moq_object q={.alias=1,.data=moq_vg_buf,.len=moq_vg_len,.group=moq_vg_seq,.end_of_group=true,
  .props=props,.props_len=moq_props(props,sizeof(props),pairs,1),.source_us=moq_vg_capture,
  .deadline_us=now+(uint64_t)AIOT_MOQT_OFFLOAD_TIMEOUT_MS*1000};
 int r=quic_client_moq_publish_ex(&q);
 printf("[MOQ_OFFLOAD] {\"seq\":%lu,\"capture_us\":%llu,\"bytes\":%u,\"result\":%d,\"utility\":%d,\"budget\":%llu}\n",(unsigned long)moq_vg_seq,
        (unsigned long long)moq_vg_capture,(unsigned)moq_vg_len,r,util,(unsigned long long)budget);
 if(r==0)moq_off_sent++;
 moq_off_sent_seq=moq_vg_seq;moq_off_at=now;
}
static void moq_vigil_report(uint64_t now){
 static uint64_t at;
 if(now-at<20000000ULL)return;
 at=now;
 printf("[MOQ_VIGIL] {\"epochs\":%u,\"sent\":%u,\"zero_utility\":%u,\"over_budget\":%u,\"busy\":%u,\"capacity_Bps\":%llu}\n",
        moq_vg_epochs,moq_off_sent,moq_vg_zero,moq_vg_over,moq_vg_busy,
        (unsigned long long)(AIOT_MOQT_VIGIL_CAPACITY_BPS?(uint64_t)AIOT_MOQT_VIGIL_CAPACITY_BPS:quic_client_moq_capacity_Bps()));
}
#endif
#if AIOT_MOQT_OFFLOAD_MS && (AIOT_MOQT_OFFLOAD_MODE==3||AIOT_MOQT_OFFLOAD_MODE==4)
/* Argus offload (Min et al., IEEE TMC 2024, Sec. 5.2): a task goes to the device where it ends
 * first, the end at a peer being its transmission delay TD = size / BW plus its processing there;
 * BW is the transmitted size over the transmission time, smoothed by an EWMA (weight 1/4 here,
 * not given by the paper), updated on every transmission. Here the task is the newest camera frame
 * the detector has not taken: at the camera it ends after the running inference and one more
 * (the shortest recent inference), at the helper after the helper is free, TD and one helper
 * inference (AIOT_MOQT_OFFLOAD_MS, the helper's service time). One frame at a time; the first is
 * sent to measure BW. No priority and no deadline in Argus: the transport is chosen by the build. */
static struct {uint64_t start,helper_free,bw;size_t len;uint32_t local_seq;bool on,seen;unsigned samples,local,failed;} moq_ar;
#if AIOT_MOQT_OFFLOAD_GENERAL&&AIOT_MOQT_GEN_RECALL
static void moq_gen_recall(uint64_t now);
#endif
static void moq_argus_observe(uint64_t now){
#if AIOT_MOQT_OFFLOAD_GENERAL&&AIOT_MOQT_GEN_RECALL
 moq_gen_recall(now);
#endif
 uint64_t g,dl;size_t b,a;
 bool on=moq_ar.on&&quic_client_moq_offload_flight(&g,&b,&a,&dl);
 if(on)moq_ar.seen=true;
 if(moq_ar.on&&!moq_ar.seen&&now-moq_ar.start>5000000ULL)moq_ar.on=false; /* never seen under way */
 if(moq_ar.on&&moq_ar.seen&&!on){
  moq_ar.on=moq_ar.seen=false;
  uint64_t el=now-moq_ar.start;
  /* the bytes that reached the helper's side over the time taken: a frame given up (timeout or
   * doomed) counts with what was acknowledged, not with its size, or an early give-up would read
   * as a fast link */
  size_t got=moq_ar.len;
  {uint64_t lg;size_t lb,la;bool ld;
   if(quic_client_moq_offload_last(&lg,&lb,&la,&ld)&&!ld){got=la;moq_ar.failed++;}}
  if(el){uint64_t s=(uint64_t)got*1000000ULL/el;moq_ar.bw=moq_ar.samples?(3*moq_ar.bw+s)/4:s;moq_ar.samples++;}
  moq_ar.helper_free=now+(uint64_t)AIOT_MOQT_OFFLOAD_MS*1000;
 }
}
static bool moq_argus_due(uint64_t now,size_t len){
 if(moq_ar.on)return false;
 if(!moq_ar.samples)return true;
 uint32_t el,fl,st;source_manager_ai_phase(&el,&fl,&st);
 if(!fl)return false;
 uint64_t cam=(uint64_t)(el<fl?fl-el:0)+fl;
 uint64_t td=moq_ar.bw?(uint64_t)len*1000000ULL/moq_ar.bw:UINT64_MAX/4;
 uint64_t hlp=(moq_ar.helper_free>now?moq_ar.helper_free-now:0)+td+(uint64_t)AIOT_MOQT_OFFLOAD_MS*1000;
 if(hlp<cam)return true;
 if(moq_ar.local_seq!=moq_off_seq){moq_ar.local_seq=moq_off_seq;moq_ar.local++;}
 return false;
}
/* Chen offload (Chen et al., IEEE VTC2023-Fall, Sec. III), throughput-aware collaboration for
 * stacking tasks: a device whose tasks cannot end within the restricted time T_max locally offloads
 * to a collaborative device, pipelined: each stage (transmission, helper inference) takes the next
 * task without waiting for the end of the previous one, so the throughput is set by the slowest
 * stage; a scheme is feasible when one task's latency, T_comm = size / R plus the helper's
 * inference, is at most T_max. The camera captures several frames per inference (stacking tasks,
 * the local latency of the backlog exceeds T_max), so it offloads; the whole model runs at the
 * helper (the partition at the input). The next frame goes when the previous transmission has
 * ended and one helper inference (AIOT_MOQT_OFFLOAD_MS) has passed since the previous send; R is
 * the measured rate (the EWMA of Argus above; the paper's devices sense their resources). */
static unsigned moq_ch_infeasible,moq_ch_probes;
static bool moq_chen_due(uint64_t now,size_t len){
 if(moq_ar.on||now-moq_off_at<(uint64_t)AIOT_MOQT_OFFLOAD_MS*1000)return false;
 if(!moq_ar.samples)return true;
 /* the rate R that the rule takes as known is measured again: a frame goes out when none has for
  * AIOT_MOQT_CH_PROBE_MS, so a low first sample does not keep the camera local for the run */
 if(now-moq_off_at>=(uint64_t)AIOT_MOQT_CH_PROBE_MS*1000){moq_ch_probes++;return true;}
 uint64_t td=moq_ar.bw?(uint64_t)len*1000000ULL/moq_ar.bw:UINT64_MAX/4;
 uint64_t budget=(uint64_t)AIOT_MOQT_CH_TMAX_MS*1000;
#if AIOT_MOQT_OFFLOAD_TASK_MS
 if(moq_task_left(now)<budget)budget=moq_task_left(now);   /* the task waited already */
#endif
 if(td+(uint64_t)AIOT_MOQT_OFFLOAD_MS*1000<=budget)return true;
 if(moq_ar.local_seq!=moq_off_seq){moq_ar.local_seq=moq_off_seq;moq_ch_infeasible++;}
 return false;
}
#if AIOT_MOQT_OFFLOAD_GENERAL
/* General offload rule (no knowledge of the application): a frame goes to the helper only when
 * (0) it can be done by its capture time + T_max: the time since capture, the camera's own stage
 *     before the cut, the queueing delay (smoothed minus minimum round trip), size / R and the
 *     helper's service (AIOT_MOQT_OFFLOAD_MS) within AIOT_MOQT_CH_TMAX_MS;
 * (1) the queueing delay is below AIOT_MOQT_GEN_QDELAY_MS, so the mission objects keep their share;
 * (2) the scene is active: the camera's detector found an object within AIOT_MOQT_GEN_ACTIVE_MS.
 * A frame sent is dropped in flight at its capture time + T_max minus the helper's service. When no
 * frame has gone for AIOT_MOQT_CH_PROBE_MS, (0) is waived once so that R is measured again. */
static uint64_t moq_gen_stage_us=400000,moq_gen_decide_us;
static unsigned moq_gen_refused[4],moq_gen_slack_refused;
#if AIOT_MOQT_GEN_RECALL
#define MOQ_GEN_RECALLED moq_gen_recalled
#else
#define MOQ_GEN_RECALLED 0U
#endif
/* (1) The mission's share as its slack: the frame, sent before the evidence (its deadline is earlier),
 * must leave every open event whose evidence has not arrived able to end by its deadline, in deadline
 * order: now + queueing + len / R + the preparation and transmission of the evidence due by then <= deadline. */
static bool moq_gen_slack(uint64_t now,size_t len,uint64_t qd){
#if AIOT_MOQT_MUS
 if(!moq_ar.bw)return true;
 uint64_t fg=0;size_t fb=0,fs=0,fa=0;bool fab=false,fl=quic_client_moq_image_flight(&fg,&fb,&fs,&fa,&fab)&&!fab;
 /* an event's evidence takes its preparation on the camera (one at a time, the measured time of the lowest
  * representation, unless prepared or under way) and its bytes (the lowest representation, ~1/12 of the
  * source, or what is left of the object under way) */
 uint64_t dl[MUS_STORE],us[MUS_STORE];unsigned n=0;
 xSemaphoreTake(moq_image_lock,portMAX_DELAY);
 for(unsigned i=0;i<MUS_STORE;i++){
  const mus_slot *e=&mus_store[i];
  if(!e->used||e->closed||e->best>=0||mus_deadline(e)<=now)continue;
  bool going=fl&&fg==e->seq&&fb>=fa;
  size_t b=going?fb-fa:e->len/12;
  dl[n]=mus_deadline(e);
  us[n]=(going||e->prepared>=0?0:(uint64_t)mus_prepare_us[0])+(uint64_t)b*1000000ULL/moq_ar.bw;n++;
 }
 xSemaphoreGive(moq_image_lock);
 for(unsigned i=0;i<n;i++){
  uint64_t t=now+qd+(uint64_t)len*1000000ULL/moq_ar.bw;
  for(unsigned j=0;j<n;j++)if(dl[j]<=dl[i])t+=us[j];
  if(t>dl[i]){moq_gen_slack_refused++;return false;}
 }
#else
 (void)now;(void)len;(void)qd;
#endif
 return true;
}
#if AIOT_MOQT_GEN_RECALL
/* Recall: the offload under way is stopped when, with its bytes left before the evidence, an open event could
 * no longer get its evidence by its deadline, while without them it could (the slack test of (1) in flight). */
static unsigned moq_gen_recalled;
static void moq_gen_recall(uint64_t now){
 static uint64_t at;
 if(now-at<50000)return;
 at=now;
 uint64_t g,dl;size_t b,a;
 if(!quic_client_moq_offload_flight(&g,&b,&a,&dl)||b<=a)return;
 uint64_t srtt=quic_client_moq_srtt_us(),mrtt=quic_client_moq_min_rtt_us(),qd=mrtt&&srtt>mrtt?srtt-mrtt:0;
 unsigned keep=moq_gen_slack_refused;
 bool with=moq_gen_slack(now,b-a,qd),without=with||moq_gen_slack(now,0,qd);
 moq_gen_slack_refused=keep;
 if(!with&&without&&quic_client_moq_offload_abort(g)){
  moq_gen_recalled++;
  printf("[MOQ_GEN_RECALL] {\"group\":%llu,\"bytes\":%u,\"acked\":%u}\n",(unsigned long long)g,(unsigned)b,(unsigned)a);
 }
}
#endif
static int moq_gen_admit(uint64_t now,uint64_t cap_us,size_t len,uint64_t stage_us){
 uint64_t srtt=quic_client_moq_srtt_us(),mrtt=quic_client_moq_min_rtt_us(),qd=mrtt&&srtt>mrtt?srtt-mrtt:0;
#if AIOT_MOQT_GEN_ACTIVE_MS
 if(!moq_gen_active_us||now-moq_gen_active_us>(uint64_t)AIOT_MOQT_GEN_ACTIVE_MS*1000)return 2;
#endif
#if AIOT_MOQT_GEN_NOVELTY&&!AIOT_MOQT_OFFLOAD_SPLIT
 /* (3) novelty: the camera takes the newest frame when its result is out, again one period P later; a frame
  * adds coverage only in the middle third between the two, otherwise it repeats what the camera infers */
 if(moq_gen_local_at&&(cap_us<moq_gen_local_at+moq_gen_local_per/3||cap_us>moq_gen_local_at+2*moq_gen_local_per/3))return 3;
#endif
 if(AIOT_MOQT_GEN_QDELAY_MS&&qd>(uint64_t)AIOT_MOQT_GEN_QDELAY_MS*1000)return 1;
 if(!moq_ar.samples)return -1;
 if(!moq_gen_slack(now,len,qd))return 1;
 uint64_t age=now>cap_us?now-cap_us:0,td=moq_ar.bw?(uint64_t)len*1000000ULL/moq_ar.bw:UINT64_MAX/4;
 if(age+stage_us+qd+td+(uint64_t)AIOT_MOQT_OFFLOAD_MS*1000>(uint64_t)AIOT_MOQT_CH_TMAX_MS*1000)return 0;
 return -1;
}
static bool moq_gen_due(uint64_t now,uint64_t cap_us,size_t len,uint64_t stage_us){
 if(moq_ar.on||now-moq_off_at<(uint64_t)AIOT_MOQT_OFFLOAD_MS*1000)return false;
 int why=moq_gen_admit(now,cap_us,len,stage_us);
 if(why==0&&now-moq_off_at>=(uint64_t)AIOT_MOQT_CH_PROBE_MS*1000){moq_ch_probes++;why=-1;}
 if(why>=0){moq_gen_refused[why]++;return false;}
 return true;
}
#endif
#if AIOT_MOQT_OFFLOAD_GENERAL
#define MOQ_OFF_DUE(now) moq_gen_due((now),moq_off_capture,MOQ_OFF_DECIDE_LEN,0)
#else
#define MOQ_OFF_DUE(now) moq_chen_due((now),MOQ_OFF_DECIDE_LEN)
#endif
static void moq_argus_report(uint64_t now){
 static uint64_t at;
 if(now-at<20000000ULL)return;
 at=now;
 printf("[%s] {\"sent\":%u,\"local\":%u,\"samples\":%u,\"bw_Bps\":%llu,\"probes\":%u,\"failed\":%u}\n",AIOT_MOQT_OFFLOAD_MODE==4?"MOQ_CHEN":"MOQ_ARGUS",moq_off_sent,
        AIOT_MOQT_OFFLOAD_MODE==4?moq_ch_infeasible:moq_ar.local,moq_ar.samples,(unsigned long long)moq_ar.bw,moq_ch_probes,moq_ar.failed);
#if AIOT_MOQT_OFFLOAD_GENERAL
 printf("[MOQ_GEN] {\"deadline\":%u,\"share\":%u,\"value\":%u,\"novelty\":%u,\"slack\":%u,\"recalled\":%u,\"stage_us\":%llu,\"period_us\":%llu}\n",moq_gen_refused[0],
        moq_gen_refused[1],moq_gen_refused[2],moq_gen_refused[3],moq_gen_slack_refused,MOQ_GEN_RECALLED,(unsigned long long)moq_gen_stage_us,(unsigned long long)moq_gen_local_per);
#endif
}
#if AIOT_MOQT_OFFLOAD_SPLIT
/* Split inference (Chen et al., VTC2023-Fall, Sec. II-B: the model partitioned between devices,
 * pipelined). The camera runs the first stage, the decode into the detector's raster: the cut with the
 * least data (the raster at JPEG AIOT_MOQT_OFFLOAD_RASTER, ~2.4 kB, against the 53 kB frame); the
 * helper runs the model. A frame is split when the Chen rule would send now (the previous transmission
 * has ended, the helper's stage is free, size/R + its inference <= T_max); the camera infers the
 * others itself. Decided and delivered in the inference task; the publishing loop sends. */
static volatile bool moq_split_window;
static unsigned moq_split_frames,moq_split_local;
#if AIOT_MOQT_SPLIT_CORE0
static unsigned moq_s0_yield,moq_s0_backoff;
static volatile bool moq_s0_stop,moq_s0_stopped;
#endif
#if AIOT_MOQT_SPLIT0_HYBRID
static bool moq_hy_raw;static unsigned moq_hy_sent;
#endif
#if AIOT_MOQT_SPLIT_LOCAL_MS
/* the camera's own inference at least every AIOT_MOQT_SPLIT_LOCAL_MS: its results count the passages and raise the
 * alerts; the frames between go to the helpers as often as the Chen rule allows (a short AIOT_MOQT_OFFLOAD_MS) */
static uint64_t moq_last_local_us;
#endif
static bool moq_split_decide(uint32_t seq,uint32_t frame_ms){
 (void)seq;(void)frame_ms;
#if AIOT_MOQT_SPLIT_LOCAL_MS
 {uint64_t t=esp_timer_get_time();
  if(t-moq_last_local_us>=(uint64_t)AIOT_MOQT_SPLIT_LOCAL_MS*1000){moq_last_local_us=t;moq_split_local++;return false;}}
#endif
 if(!moq_split_window||!moq_rs.lock||!moq_rs.buf)return false;
 if(moq_rs.valid&&moq_rs.seq!=moq_off_sent_seq){moq_split_local++;return false;}   /* the last one is not out */
#if AIOT_MOQT_OFFLOAD_GENERAL
 moq_gen_decide_us=esp_timer_get_time();
 bool go=moq_gen_due(moq_gen_decide_us,(uint64_t)frame_ms*1000,moq_off_small_last,moq_gen_stage_us);
#else
 bool go=moq_chen_due(esp_timer_get_time(),moq_off_small_last);
#endif
 if(go)moq_split_frames++;else moq_split_local++;
 return go;
}
static void moq_split_deliver(uint32_t seq,uint32_t frame_ms,uint16_t width,uint16_t height){
 xSemaphoreTake(moq_rs.lock,portMAX_DELAY);
 if(ai_inference_copy_raster(moq_rs.buf,MOQ_RASTER_CAP,width,height)){
  moq_rs.w=width;moq_rs.h=height;moq_rs.seq=seq;moq_rs.cap_us=(uint64_t)frame_ms*1000;moq_rs.valid=true;moq_rs.kept++;
 }else moq_rs.failed++;
 xSemaphoreGive(moq_rs.lock);
#if AIOT_MOQT_OFFLOAD_GENERAL
 {uint64_t d=esp_timer_get_time()-moq_gen_decide_us;moq_gen_stage_us=(3*moq_gen_stage_us+d)/4;}   /* the camera's stage */
#endif
}
static void moq_split_report(uint64_t now){
 static uint64_t at;
 if(now-at<20000000ULL)return;
 at=now;
 printf("[MOQ_SPLIT] {\"split\":%u,\"local\":%u,\"kept\":%u,\"failed\":%u,\"sent\":%u}\n",moq_split_frames,moq_split_local,moq_rs.kept,moq_rs.failed,moq_off_sent);
#if AIOT_MOQT_SPLIT0_HYBRID
 printf("[MOQ_HYBRID] {\"raw\":%u}\n",moq_hy_sent);
#endif
#if AIOT_MOQT_SPLIT_CORE0
 printf("[MOQ_SPLIT0_YIELD] {\"yield\":%u,\"backoff\":%u}\n",moq_s0_yield,moq_s0_backoff);
#endif
}
#if AIOT_MOQT_SPLIT_CORE0
/* Structure A: the helper's frames are decoded on core 0 by their own task, so the camera's detector on core 1
 * infers every frame it takes itself; the newest captured frame goes to the helper when the split rule sends. */
static SemaphoreHandle_t moq_off_lock;static TaskHandle_t moq_s0_task;static uint8_t *moq_s0_jpeg,*moq_s0_rgb;
#if AIOT_MOQT_MUS
/* Mission first on the shared core: an event's evidence is being converted, or waits for its conversion (open,
 * nothing arrived yet, not prepared, not under way). The evidence conversion runs on core 0 as well. */
static bool moq_evidence_pending(uint64_t now){
 bool p=false;
 xSemaphoreTake(moq_image_lock,portMAX_DELAY);
 for(unsigned i=0;i<MUS_STORE&&!p;i++){
  const mus_slot *e=&mus_store[i];
  if(!e->used)continue;
  if(e->converting>=0||(!e->closed&&e->best<0&&e->prepared<0&&e->active<0&&mus_deadline(e)>now))p=true;
 }
 xSemaphoreGive(moq_image_lock);
 return p;
}
#endif
static void moq_split0_worker(void *unused){
 (void)unused;uint32_t last=0;
#if AIOT_MOQT_SPLIT0_ADAPT
 uint64_t ewma=0,back=0;
#endif
 for(;;){
  ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(100));
  /* the run ends: the cleanup frees the buffers and deletes the locks this task uses; it parks here first */
  if(moq_s0_stop){moq_s0_stopped=true;vTaskSuspend(NULL);}
  if(!moq_off_len||moq_off_seq==last)continue;
#if AIOT_MOQT_MUS&&AIOT_MOQT_SPLIT0_MISSION_FIRST
  if(moq_evidence_pending(esp_timer_get_time())){moq_s0_yield++;continue;}
#endif
#if AIOT_MOQT_SPLIT0_ADAPT
  /* slack-based: core 0 adds rasters only from the CPU the mission leaves (idle priority, no evidence pending) and
   * only while its decode is fast enough to matter; otherwise the camera's own split on core 1 (the Chen rule in
   * the detector task) is the supply, as without this option */
  if(back&&esp_timer_get_time()<back)continue;
  {bool need;xSemaphoreTake(moq_rs.lock,portMAX_DELAY);need=!moq_rs.valid||moq_rs.seq==moq_off_sent_seq;xSemaphoreGive(moq_rs.lock);
   if(!need||!moq_split_window)continue;}
#elif AIOT_MOQT_SPLIT0_PREFETCH
  /* prefetch: one raster is kept ready while the helper works on the previous one; a new one is decoded once the
   * ready one has gone out (or has aged past half of T_max), and the publishing loop sends it when the Chen rule
   * finds the helper free, so the decode overlaps the helper's inference instead of preceding it */
  {bool need;xSemaphoreTake(moq_rs.lock,portMAX_DELAY);
   need=!moq_rs.valid||moq_rs.seq==moq_off_sent_seq||esp_timer_get_time()-moq_rs.cap_us>(uint64_t)AIOT_MOQT_CH_TMAX_MS*500;
   xSemaphoreGive(moq_rs.lock);
   if(!need||!moq_split_window)continue;}
#else
  if(!moq_split_decide(moq_off_seq,(uint32_t)(moq_off_capture/1000)))continue;
#endif
  xSemaphoreTake(moq_off_lock,portMAX_DELAY);
  size_t len=moq_off_len;uint32_t seq=moq_off_seq;uint64_t cap=moq_off_capture;
  if(len<=MOQ_OFF_CAP)memcpy(moq_s0_jpeg,moq_off_buf,len);else len=0;
  xSemaphoreGive(moq_off_lock);
  last=seq;
  uint64_t t0=esp_timer_get_time();uint16_t w=0,h=0;
  size_t n=len?ai_inference_decode_raster(moq_s0_jpeg,(uint32_t)len,moq_s0_rgb,MOQ_RASTER_CAP,&w,&h):0;
  uint64_t t1=esp_timer_get_time();
  xSemaphoreTake(moq_rs.lock,portMAX_DELAY);
  if(n){memcpy(moq_rs.buf,moq_s0_rgb,n);moq_rs.w=w;moq_rs.h=h;moq_rs.seq=seq;moq_rs.cap_us=cap;moq_rs.valid=true;moq_rs.kept++;}
  else moq_rs.failed++;
  xSemaphoreGive(moq_rs.lock);
#if AIOT_MOQT_SPLIT0_ADAPT
  {uint64_t d=t1-t0;ewma=ewma?(3*ewma+d)/4:d;
   if(ewma>(uint64_t)AIOT_MOQT_CH_TMAX_MS*500){back=esp_timer_get_time()+2000000ULL;moq_s0_backoff++;}}
#endif
  printf("[AI_SPLIT] {\"seq\":%lu,\"capture_ms\":%lu,\"start_us\":%llu,\"decode_us\":%llu,\"deliver_us\":%llu,\"ok\":%d,\"core\":0}\n",
         (unsigned long)seq,(unsigned long)(cap/1000),(unsigned long long)t0,(unsigned long long)(t1-t0),(unsigned long long)(esp_timer_get_time()-t1),(int)(n!=0));
 }
}
#endif
#endif
#endif
static void moq_capture_worker(void *unused) {
 (void)unused;TickType_t tick=xTaskGetTickCount();uint64_t last=0;unsigned count=0,fail=0,cache_miss=0,overwritten=0,fifo_rejected=0;bool pending=false;
 while(!__atomic_load_n(&moq_capture_stop,__ATOMIC_RELAXED)){
  uint64_t begin=esp_timer_get_time();
  if(begin>=moq_source_stop_us)break;
  data_packet_t p;
#if AIOT_MOQT_REPLAY_INPUT
  bool ok=source_manager_replay_next(&p);
#else
  bool ok=source_manager_next_packet(&g_source_manager,&p);
  if(ok&&p.type!=DATA_TYPE_IMAGE)ok=source_manager_next_packet(&g_source_manager,&p);
#endif
  uint64_t done=esp_timer_get_time();
  if(ok&&p.type==DATA_TYPE_IMAGE&&
#if AIOT_MOQT_REPLAY_INPUT
      true
#else
      g_source_manager.last_frame_real
#endif
      &&p.payload_len<=MOQ_IMAGE_CAP){
   count++;source_manager_submit_camera_ai(&p);
   if(!moq_source_count)moq_source_first=done;
   if(moq_source_last&&done-moq_source_last>moq_source_max_gap)moq_source_max_gap=done-moq_source_last;
   moq_source_last=done;moq_source_count++;
#if !AIOT_MOQT_EVENT_EVIDENCE
   if(xSemaphoreTake(moq_image_lock,0)==pdTRUE){
    pending=moq_latest_len!=0;
#if AIOT_MOQT_POLICY_EXPERIMENT && AIOT_MOQT_FIFO_ADMISSION
    if(pending){printf("[MOQ_ADMISSION] {\"seq\":%lu,\"action\":\"reject_new\"}\n",(unsigned long)p.sequence);fifo_rejected++;xSemaphoreGive(moq_image_lock);goto preview_stored;}
#endif
    if(pending){overwritten++;
#if AIOT_MOQT_POLICY_EXPERIMENT
     printf("[MOQ_ADMISSION] {\"seq\":%lu,\"action\":\"replace\",\"old_seq\":%lu}\n",(unsigned long)p.sequence,(unsigned long)moq_latest_seq);
#endif
    }
    memcpy(moq_latest,p.payload,p.payload_len);moq_latest_len=p.payload_len;moq_latest_seq=p.sequence;moq_latest_capture=p.timestamp_us;
    xSemaphoreGive(moq_image_lock);
   }else{cache_miss++;
#if AIOT_MOQT_AUDIT
    printf("[MOQ_ADMISSION] {\"seq\":%lu,\"action\":\"cache_miss\"}\n",(unsigned long)p.sequence);
#endif
   }
#if AIOT_MOQT_POLICY_EXPERIMENT && AIOT_MOQT_FIFO_ADMISSION
   preview_stored:
#endif
#endif
#if AIOT_FRAME_TRACE_PORT
   ft_push(p.payload,p.payload_len,p.sequence,p.timestamp_us);
#endif
#if AIOT_MOQT_OFFLOAD_MS
   /* the newest frame is the candidate for the helper */
   if(!moq_off_buf)moq_off_buf=heap_caps_malloc(MOQ_OFF_CAP,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#if AIOT_MOQT_SPLIT_CORE0
   if(moq_off_lock)xSemaphoreTake(moq_off_lock,portMAX_DELAY);
#endif
   if(moq_off_buf&&p.payload_len<=MOQ_OFF_CAP){memcpy(moq_off_buf,p.payload,p.payload_len);moq_off_len=p.payload_len;moq_off_seq=p.sequence;moq_off_capture=p.timestamp_us;}
#if AIOT_MOQT_SPLIT_CORE0
   if(moq_off_lock)xSemaphoreGive(moq_off_lock);
   if(moq_s0_task)xTaskNotifyGive(moq_s0_task);
#endif
#endif
   printf("[MOQ_CAPTURE] {\"seq\":%lu,\"done_us\":%llu,\"acquire_us\":%llu,\"gap_us\":%llu,\"bytes\":%u,\"cache_miss\":%u,\"overwritten\":%u}\n",(unsigned long)p.sequence,(unsigned long long)done,(unsigned long long)(done-begin),(unsigned long long)(last?done-last:0),(unsigned)p.payload_len,cache_miss,overwritten);last=done;
  }else fail++;
  /* Keep the absolute 100 ms grid. If over one whole period late, skip
   * missed releases, rather than adding another full period to every overrun. */
  while(xTaskGetTickCount()-tick>=pdMS_TO_TICKS(200))tick+=pdMS_TO_TICKS(100);
  vTaskDelayUntil(&tick,pdMS_TO_TICKS(100));
 }
 moq_source_failed=fail;
 printf("[MOQ_CAPTURE_END] count=%u failed=%u cache_miss=%u overwritten=%u fifo_rejected=%u\n",count,fail,cache_miss,overwritten,fifo_rejected);
 __atomic_store_n(&moq_capture_stopped,true,__ATOMIC_RELEASE);vTaskDelete(NULL);
}
static void app_moq_main(void) {
 esp_task_wdt_add(NULL);
 if(!moq_audit_init()){printf("[MOQ_ERROR] audit allocation\n");esp_task_wdt_delete(NULL);return;}
 esp_log_level_set("QUIC",ESP_LOG_WARN);esp_log_level_set("DEBUG",ESP_LOG_WARN);
 esp_log_level_set("NGTCP2-RAW",ESP_LOG_WARN);esp_log_level_set("NGTCP2-DISCARD",ESP_LOG_WARN);
 app_cfg_load(&g_cfg);source_manager_init(&g_source_manager);
#if AIOT_MOQT_EVENT_EVIDENCE
 source_manager_evidence_enable(true);
#endif
 if(!source_manager_camera_ready()||!wifi_link_connect(g_cfg.wifi_ssid,g_cfg.wifi_password,APP_WIFI_TIMEOUT_MS)){
  printf("[MOQ_ERROR] initialization\n");esp_task_wdt_delete(NULL);return;
 }
 moq_latest=heap_caps_malloc(MOQ_IMAGE_CAP,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
 moq_stage=heap_caps_malloc(MOQ_IMAGE_CAP,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#if AIOT_MOQT_TRANSCODE
 moq_converted=heap_caps_malloc(MOQ_IMAGE_CAP,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
 if(!moq_converted)goto cleanup;
#endif
#if AIOT_MOQT_ASYNC_CODEC
 if(!moq_codec_worker_start())goto cleanup;
#endif
 moq_image_lock=xSemaphoreCreateMutex();
 if(!moq_latest||!moq_stage||!moq_image_lock){printf("[MOQ_ERROR] capture buffers\n");goto cleanup;}
#if AIOT_MOQT_MUS
 if(!mus_init()){printf("[MOQ_ERROR] evidence store\n");goto cleanup;}
#endif
 char authority[160];snprintf(authority,sizeof(authority),"%s:14568",g_cfg.server_host);
 quic_client_config_t cfg={.hostname=g_cfg.server_host,.port="14568",.alpn="\x07moqt-21"};
#if AIOT_MOQT_POLICY_EXPERIMENT
 if(quic_client_moq_configure(AIOT_MOQT_POLICY)){printf("[MOQ_ERROR] policy\n");goto cleanup;}
 printf("[MOQ_POLICY] selector=%u input=%s admission=%s preview_deadline_ms=%u ai_deadline_ms=%u\n",
        AIOT_MOQT_POLICY,AIOT_MOQT_REPLAY_INPUT?"replay":"camera",
        AIOT_MOQT_FIFO_ADMISSION?"fifo":"latest",
        AIOT_MOQT_PREVIEW_DEADLINE_MS,AIOT_MOQT_AI_DEADLINE_MS);
#endif
#if AIOT_MOQT_WIRE_PROFILE
 if(quic_client_moq_tracks(moq_wire_tracks,4)){printf("[MOQ_ERROR] tracks\n");goto cleanup;}
 printf("[MOQ_PROFILE] wire=1 image_alias=%u evidence_delivery=%s mission_count=%u\n",(unsigned)MOQ_ALIAS_IMAGE,AIOT_MOQT_EVIDENCE_DELIVERY?"fetch":"push",(unsigned)AIOT_MOQT_MISSION_COUNT);
#if AIOT_MOQT_MISSION_COUNT
 {
  moq_passage_cfg pc={.line=AIOT_MOQT_PASSAGE_LINE,.vertical_axis=AIOT_MOQT_PASSAGE_AXIS,
   .direction=AIOT_MOQT_PASSAGE_DIRECTION==1?1:AIOT_MOQT_PASSAGE_DIRECTION==2?-1:0,
   .max_step=AIOT_MOQT_PASSAGE_STEP,.max_missed=AIOT_MOQT_PASSAGE_MISSED,
   .predict=AIOT_MOQT_PASSAGE_PREDICT,.count_unseen=AIOT_MOQT_PASSAGE_UNSEEN,.settled=AIOT_MOQT_PASSAGE_SETTLED,
   .assoc=AIOT_MOQT_PASSAGE_ASSOC};
  moq_passage_init(&moq_passages,&pc);
#if AIOT_MOQT_OFFLOAD_RASTER
  moq_rs.lock=xSemaphoreCreateMutex();
  moq_rs.buf=heap_caps_malloc(MOQ_RASTER_CAP,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#endif
#if AIOT_MOQT_SHARED_FRAMES
  if(!moq_ev.lock){moq_ev.lock=xSemaphoreCreateMutex();
   for(unsigned i=0;i<MOQ_EV_SLOTS;i++)moq_ev.buf[i]=heap_caps_malloc(MOQ_RASTER_CAP,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);}
  printf("[MOQ_SHARED] {\"slots\":%u,\"ready\":%d}\n",MOQ_EV_SLOTS,moq_ev.lock&&moq_ev.buf[MOQ_EV_SLOTS-1]);
#endif
#if AIOT_MOQT_OFFLOAD_SPLIT&&AIOT_MOQT_SPLIT_CORE0
#if AIOT_MOQT_SPLIT0_ADAPT
  source_manager_set_split_hooks(moq_split_decide,moq_split_deliver);   /* the camera's own split stays the floor */
#endif
  moq_off_lock=xSemaphoreCreateMutex();
  moq_s0_jpeg=heap_caps_malloc(MOQ_OFF_CAP,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  moq_s0_rgb=heap_caps_malloc(MOQ_RASTER_CAP,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  /* with MISSION_FIRST the decoder runs at the idle priority: every other task of core 0 (capture, the evidence
   * conversion, the network) preempts it */
  if(moq_off_lock&&moq_s0_jpeg&&moq_s0_rgb)xTaskCreatePinnedToCore(moq_split0_worker,"moq_split0",6144,NULL,AIOT_MOQT_SPLIT0_MISSION_FIRST?0:1,&moq_s0_task,0);
  printf("[MOQ_SPLIT0] {\"task\":%d}\n",moq_s0_task!=NULL);
#elif AIOT_MOQT_OFFLOAD_SPLIT
  source_manager_set_split_hooks(moq_split_decide,moq_split_deliver);
#endif
  source_manager_set_result_hook(moq_passage_hook);
  printf("[MOQ_PASSAGE_CONFIG] {\"line\":%u,\"axis\":%u,\"direction\":%d,\"max_step\":%u,\"assoc\":%u,\"synthetic_motion\":%u,\"reconnect_limit\":%u,\"result_timeout_ms\":%u,\"result_priority\":%u,\"retain\":%u,\"image_timeout\":%u,\"result_queue\":%u}\n",(unsigned)pc.line,(unsigned)pc.vertical_axis,(int)pc.direction,(unsigned)pc.max_step,(unsigned)pc.assoc,(unsigned)AIOT_MOQT_PASSAGE_SYNTHETIC,(unsigned)AIOT_MOQT_RECONNECT_LIMIT,(unsigned)AIOT_MOQT_MISSION_RESULT_TIMEOUT_MS,(unsigned)AIOT_MOQT_MISSION_RESULT_PRIORITY,(unsigned)AIOT_MOQT_MISSION_RETAIN,(unsigned)AIOT_MOQT_MISSION_IMAGE_TIMEOUT,source_manager_ai_event_capacity());
 }
#endif
 bool announced=false;uint32_t status_group=0;
#endif
 if(quic_client_init_with_config(&cfg)){printf("[MOQ_ERROR] QUIC init\n");goto cleanup;}
 uint64_t pump_max=0,pump_sum=0,pump_calls=0,pump_previous=0,pump_gap_max=0;
#if AIOT_MOQT_RECONNECT_LIMIT
 unsigned moq_reconnects=0;uint64_t moq_attempt_us=0;
#endif
#if AIOT_MOQT_MISSION_COUNT
 bool moq_backlog=false;
#endif
 bool started=false,capturing=false;uint64_t epoch=0,last_stats=0,boot=esp_timer_get_time();
#if AIOT_MOQT_OTA_REMOTE
 uint64_t last_ota_poll=boot;
 ota_log_status();
#endif
#if !AIOT_MOQT_POLICY_EXPERIMENT
 uint32_t ai_seq=0;bool ai_seen=false;
#endif
 printf("[MOQ_BOOT] profile=draft21-dual capture_target_hz=10 model=configured backend_recorded_in_results=1\n");
 {bool had=moq_bc.magic==MOQ_BC_MAGIC;
  printf("[MOQ_RESET] {\"reason\":%d,\"previous\":%d,\"uptime_s\":%lu,\"heap_min\":%lu,\"internal_free\":%lu,\"internal_largest\":%lu,\"psram_free\":%lu,\"ai_completed\":%lu,\"boots\":%lu}\n",
         (int)esp_reset_reason(),(int)had,had?(unsigned long)moq_bc.uptime_s:0UL,had?(unsigned long)moq_bc.heap_min:0UL,had?(unsigned long)moq_bc.internal_free:0UL,
         had?(unsigned long)moq_bc.internal_largest:0UL,had?(unsigned long)moq_bc.psram_free:0UL,had?(unsigned long)moq_bc.ai_completed:0UL,had?(unsigned long)moq_bc.boots:0UL);
  if(!had)memset(&moq_bc,0,sizeof(moq_bc));
  moq_bc.boots++;moq_bc.magic=MOQ_BC_MAGIC;moq_bc.uptime_s=0;}
 for(;;){
  esp_task_wdt_reset();uint64_t pump_begin=esp_timer_get_time();
  if(capturing){
   if(pump_previous&&pump_begin-pump_previous>pump_gap_max)pump_gap_max=pump_begin-pump_previous;
   pump_previous=pump_begin;
  }
  int process_result=quic_client_process();uint64_t now=esp_timer_get_time();
  uint64_t pump_elapsed=now-pump_begin;pump_sum+=pump_elapsed;pump_calls++;if(pump_elapsed>pump_max)pump_max=pump_elapsed;
  if(quic_client_moq_finished()){printf("[MOQ_DONE] subscriber_finished=1\n");break;}
#if AIOT_MOQT_RECONNECT_LIMIT
  /* Mission: a lost connection does not end the run. Capture and inference go on, results
   * wait in the queue, and the next session serves what was retained. */
  /* A handshake that meets an outage backs off for up to many seconds. A new attempt after
   * AIOT_MOQT_RECONNECT_RETRY_MS finds the link soon after it returns. */
  bool moq_stalled=AIOT_MOQT_RECONNECT_RETRY_MS&&moq_reconnects&&!started&&!quic_client_is_connected()
     &&now-moq_attempt_us>=(uint64_t)AIOT_MOQT_RECONNECT_RETRY_MS*1000;
  if((process_result!=0||moq_stalled)&&now-boot<=MOQ_LIMIT_US&&moq_reconnects<AIOT_MOQT_RECONNECT_LIMIT
     &&(!capturing||now-epoch<MOQ_DRAIN_US)){
   moq_reconnects++;moq_attempt_us=now;moq_backlog=true;
   printf("[MOQ_RECONNECT] {\"attempt\":%u,\"elapsed_us\":%llu,\"stalled\":%u}\n",moq_reconnects,(unsigned long long)(now-boot),(unsigned)moq_stalled);
   quic_client_cleanup();vTaskDelay(pdMS_TO_TICKS(moq_stalled?100:1000));esp_task_wdt_reset();
   if(quic_client_init_with_config(&cfg)){printf("[MOQ_ERROR] QUIC init after reconnect\n");break;}
   started=false;
   continue;
  }
#endif
  if(process_result!=0||now-boot>MOQ_LIMIT_US){printf("[MOQ_ERROR] process or test timeout\n");break;}
  if(!started&&quic_client_is_connected()){
   if(quic_client_moq_start(authority)){printf("[MOQ_ERROR] SETUP\n");break;}started=true;
  }
#if AIOT_MOQT_WIRE_PROFILE
  if(started&&!announced&&quic_client_moq_session_ready()){announced=true;quic_client_moq_announce();}
  /* A GOAWAY from the subscriber ends the measurement early (draft-21 section 9.2). */
  if(capturing&&quic_client_moq_going_away()&&now-epoch<MOQ_RUN_US){
   printf("[MOQ_GOAWAY_STOP] elapsed_us=%llu\n",(unsigned long long)(now-epoch));epoch=now-MOQ_RUN_US;
  }
#endif
  if(!capturing&&quic_client_moq_track_ready(MOQ_ALIAS_RESULTS)
     &&(AIOT_MOQT_EVIDENCE_DELIVERY||quic_client_moq_track_ready(MOQ_ALIAS_IMAGE))){
   epoch=now;moq_source_stop_us=epoch+MOQ_RUN_US;
#if AIOT_MOQT_REPLAY_INPUT
   mobility_replay_set(1);
#endif
   if(xTaskCreatePinnedToCore(moq_capture_worker,"moq_capture",4096,NULL,2,NULL,0)!=pdPASS){printf("[MOQ_ERROR] capture task\n");break;}
   capturing=true;
#if AIOT_MOQT_OTA_REMOTE
   /* Camera/AI allocation, Wi-Fi, QUIC and both MoQ subscriptions are healthy. */
   ota_confirm_boot();
#endif
  }
#if AIOT_MOQT_OTA_REMOTE && AIOT_MOQT_OTA_RUN_POLL
  /* The update server is asked every 30 s during the run as well. The request blocks this loop
   * until it ends (esp_http_client, 10 s timeout): when the uplink's buffers are full it takes the
   * whole timeout and the MoQ session stalls with it (10/13). AIOT_MOQT_OTA_RUN_POLL OFF asks only
   * after the run (below), where the campaign installs the next image. */
  if(capturing && now-last_ota_poll>=30000000){
   last_ota_poll=now;char target[65];
   if(ota_pull_check(target)){
    printf("[MOQ_OTA] update_available=1\n");
    esp_err_t ota_err=ota_pull_install(target);
    printf("[MOQ_OTA] install_failed=%s\n",esp_err_to_name(ota_err));
   }
  }
#endif
  if(capturing){
   if(now-epoch>=MOQ_RUN_US)__atomic_store_n(&moq_capture_stop,true,__ATOMIC_RELAXED);
   /* Drain the last active/pending inference for 10 s. This is explicitly a
    * measurement cutoff, not an assurance that every captured frame ran AI. */
   if(now-epoch<MOQ_DRAIN_US){
    aiot_ai_result_t result;uint32_t frame_ms;
#if AIOT_MOQT_MISSION_COUNT
    /* Results wait in the queue for the stream slot. When the queue is three quarters
     * full they are moved to the FETCH cache instead, so that new results still fit. */
    bool moq_crowded=AIOT_MOQT_MISSION_RETAIN&&!AIOT_MOQT_MISSION_RESULT_TIMEOUT_MS&&source_manager_ai_event_count()*4U>=source_manager_ai_event_capacity()*3U;
    /* After a reconnection the newest result goes first, because it carries the count. The
     * results that waited are retained, and the receiver fetches them in one stream. */
    if(moq_backlog&&(source_manager_ai_event_count()<=1||!AIOT_MOQT_MISSION_RETAIN||AIOT_MOQT_MISSION_RESULT_TIMEOUT_MS))moq_backlog=false;
    bool moq_subscribed=quic_client_moq_track_ready(MOQ_ALIAS_RESULTS);
    bool moq_bulk=moq_backlog&&moq_subscribed,moq_hold=moq_backlog&&!moq_subscribed&&!moq_crowded;
    if(!moq_hold&&((quic_client_moq_session_ready()&&quic_client_moq_ai_slot_available())||moq_crowded||moq_bulk)&&source_manager_peek_ai_event(&result,&frame_ms)){
#elif AIOT_MOQT_POLICY_EXPERIMENT
    if(quic_client_moq_ai_slot_available()&&source_manager_peek_ai_event(&result,&frame_ms)){
#else
    if(source_manager_get_async_ai(&result,&frame_ms)&&(!ai_seen||result.source_sequence!=ai_seq)){
#endif
     uint8_t wire[AIOT_AI_RESULT_MAX_PAYLOAD_LEN];
     char hex[2*AIOT_AI_RESULT_MAX_PAYLOAD_LEN+1];
     /* result_hex alone is 2 * MAX_PAYLOAD_LEN characters.  Size the JSON
      * from that bound so increasing the detection capacity cannot silently
      * suppress publication through snprintf truncation. */
     char json[2*AIOT_AI_RESULT_MAX_PAYLOAD_LEN+512U];
     unsigned len=aiot_ai_result_serialize(&result,wire,sizeof(wire));
#if AIOT_MOQT_RESULT_BINARY
     /* The payload is the binary result, 20 bytes and 6 per detection; the count, the capture
      * time and the passages travel as properties. A result of 10 detections is 80 bytes
      * instead of about 380 of JSON with the result in hexadecimal, which on a 128 kbit/s
      * link is 19 ms less before the alert has arrived, and it is not formatted. */
     int n=(int)len;(void)hex;(void)json;
#else
     for(unsigned i=0;i<len;i++)snprintf(hex+2*i,3,"%02x",wire[i]);
     uint64_t ack_group=0,source_to_ack_us=0;bool have_ack=quic_client_moq_last_ai_receipt(&ack_group,&source_to_ack_us);
     int n=snprintf(json,sizeof(json),"{\"schema\":1,\"seq\":%lu,\"capture_ms\":%lu,\"observed_us\":%llu,\"backend\":%u,\"detections\":%u,\"prev_ack_group\":%llu,\"prev_source_to_ack_us\":%llu,\"prev_ack_valid\":%u,\"evidence_required\":%u,\"evidence_group\":%lu,\"result_hex\":\"%s\"}",(unsigned long)result.source_sequence,(unsigned long)frame_ms,(unsigned long long)now,(unsigned)result.backend,(unsigned)result.detection_count,(unsigned long long)ack_group,(unsigned long long)source_to_ack_us,have_ack,(unsigned)(AIOT_MOQT_EVENT_EVIDENCE&&result.detection_count>0),(unsigned long)result.source_sequence,hex);
#endif
#if AIOT_MOQT_MISSION_COUNT
     /* The passage events of a result were counted when the result was produced. */
     uint32_t moq_passage_fresh=0,moq_passage_total=0;
     source_manager_peek_ai_counts(result.source_sequence,&moq_passage_fresh,&moq_passage_total);
     if(!AIOT_MOQT_RESULT_BINARY&&n>0&&n<(int)sizeof(json)){
      /* Replace the closing brace with the mission fields. */
      int m=snprintf(json+n-1,sizeof(json)-(size_t)n+1,",\"passages_new\":%lu,\"passages_total\":%lu}",(unsigned long)moq_passage_fresh,(unsigned long)moq_passage_total);
      n=m>0&&m<(int)sizeof(json)-n+1?n-1+m:-1;
     }
#endif
     int accepted=-1;
     if(n>0&&n<(int)sizeof(json)){
#if AIOT_MOQT_POLICY_EXPERIMENT
#if AIOT_MOQT_MISSION_COUNT
      {
       /* Evidence is wanted for the results that report a passage event. */
       bool evidence=AIOT_MOQT_EVENT_EVIDENCE&&moq_passage_fresh>0;
       uint8_t props[56];const uint64_t pairs[][2]={{MOQ_PROP_CAPTURE_MS,frame_ms},{MOQ_PROP_DETECTIONS,result.detection_count},{MOQ_PROP_EVIDENCE_REQUIRED,evidence},{MOQ_PROP_PASSAGES_TOTAL,moq_passage_total},{MOQ_PROP_PASSAGES_NEW,moq_passage_fresh}};
       quic_moq_object q={.alias=MOQ_ALIAS_RESULTS,.data=AIOT_MOQT_RESULT_BINARY?wire:(const uint8_t*)json,.len=(size_t)n,
        .group=result.source_sequence,
#if AIOT_MOQT_MISSION_RESULT_TIMEOUT_MS
        /* Comparison alternative: results expire like any other object and are not kept. */
        .end_of_group=true,
        .props=props,.props_len=moq_props(props,sizeof(props),pairs,5),
        .source_us=(uint64_t)frame_ms*1000,
#if AIOT_MOQT_TIMEOUT_FROM_CREATION
        .deadline_us=esp_timer_get_time()+(uint64_t)AIOT_MOQT_MISSION_RESULT_TIMEOUT_MS*1000,.importance=2};
#else
        .deadline_us=(uint64_t)frame_ms*1000+(uint64_t)AIOT_MOQT_MISSION_RESULT_TIMEOUT_MS*1000,.importance=2};
#endif
#else
        .end_of_group=true,.retain=AIOT_MOQT_MISSION_RETAIN,.never_drop=true,.announce_gap=AIOT_MOQT_MISSION_RETAIN,
        .retain_only=moq_bulk,
        .props=props,.props_len=moq_props(props,sizeof(props),pairs,5),
        .source_us=(uint64_t)frame_ms*1000,.deadline_us=UINT64_MAX,.importance=2};
#endif
       accepted=quic_client_moq_publish_ex(&q);
      }
#elif AIOT_MOQT_WIRE_PROFILE
      {
       bool evidence=AIOT_MOQT_EVENT_EVIDENCE&&result.detection_count>0;
       uint8_t props[40];const uint64_t pairs[][2]={{MOQ_PROP_CAPTURE_MS,frame_ms},{MOQ_PROP_DETECTIONS,result.detection_count},{MOQ_PROP_EVIDENCE_REQUIRED,evidence}};
       /* Results that report a detection use a higher publisher priority than the track default. */
       quic_moq_object q={.alias=MOQ_ALIAS_RESULTS,.data=(const uint8_t*)json,.len=(size_t)n,.group=result.source_sequence,
        .end_of_group=true,.retain=true,.has_priority=result.detection_count>0,.priority=32,
        .props=props,.props_len=moq_props(props,sizeof(props),pairs,3),
        .source_us=(uint64_t)frame_ms*1000,.deadline_us=(uint64_t)frame_ms*1000+(uint64_t)AIOT_MOQT_AI_DEADLINE_MS*1000,
        .importance=result.detection_count?2:1};
       accepted=quic_client_moq_publish_ex(&q);
      }
#else
      accepted=quic_client_moq_publish_object(2,(uint8_t*)json,n,result.source_sequence,(uint64_t)frame_ms*1000,(uint64_t)frame_ms*1000+(uint64_t)AIOT_MOQT_AI_DEADLINE_MS*1000,result.detection_count?2:1);
#endif
#if AIOT_MOQT_EVENT_EVIDENCE && !AIOT_MOQT_MUS
#if AIOT_MOQT_MISSION_COUNT
      if(accepted==0&&moq_passage_fresh){
#else
      if(accepted==0&&result.detection_count){
#endif
       /* Camera worker does not write this cache in evidence mode. Never use
        * the current camera frame when the exact inference source expired. */
       xSemaphoreTake(moq_image_lock,portMAX_DELAY);
       size_t evidence_len=source_manager_copy_evidence_for_sequence(result.source_sequence,frame_ms,moq_latest,MOQ_IMAGE_CAP);
       if(evidence_len){
        printf("[MOQ_EVIDENCE] {\"group\":%lu,\"bytes\":%u,\"replaced_group\":%lu}\n",(unsigned long)result.source_sequence,(unsigned)evidence_len,(unsigned long)(moq_latest_len?moq_latest_seq:0));
        moq_latest_len=evidence_len;moq_latest_seq=result.source_sequence;moq_latest_capture=(uint64_t)frame_ms*1000;
       }else printf("[MOQ_EVIDENCE_MISSING] group=%lu\n",(unsigned long)result.source_sequence);
       xSemaphoreGive(moq_image_lock);
      }
#endif
      if(accepted==0||accepted==-2)source_manager_pop_ai_event(result.source_sequence);
#else
      accepted=quic_client_moq_publish_track(2,(uint8_t*)json,n,result.source_sequence);
#endif
     }
#if AIOT_MOQT_POLICY_EXPERIMENT
     if(accepted==-2)printf("[MOQ_EXPIRED] alias=2 group=%lu\n",(unsigned long)result.source_sequence);
#endif
     printf("[MOQ_AI] {\"seq\":%lu,\"accepted\":%d,\"backend\":%u,\"detections\":%u,\"observed_us\":%llu}\n",(unsigned long)result.source_sequence,accepted==0,(unsigned)result.backend,(unsigned)result.detection_count,(unsigned long long)now);
     /* One attempt per observed result. Rejection remains a measured failure. */
#if !AIOT_MOQT_POLICY_EXPERIMENT
     ai_seen=true;ai_seq=result.source_sequence;
#endif
    }
   }
#if AIOT_MOQT_OFFLOAD_SCHED
   moq_off_cut_late(now);
#endif
#if AIOT_MOQT_OFFLOAD_TASK_MS
   {static uint64_t task_at;
    if(now-task_at>=20000000ULL){task_at=now;
     printf("[MOQ_TASKS] {\"added\":%u,\"sent\":%u,\"lapsed\":%u,\"dropped\":%u}\n",moq_task.added,moq_task.sent,moq_task.lapsed,moq_task.dropped);}}
#endif
#if AIOT_MOQT_OFFLOAD_MS && AIOT_MOQT_OFFLOAD_MODE==2
   moq_vigil(now,epoch);moq_vigil_report(now);
#elif AIOT_MOQT_OFFLOAD_MS
   /* One frame per helper service time (AIOT_MOQT_OFFLOAD_MS), the newest not sent yet. In an MBM
    * build the window of the image slots bounds it like the evidence; in a priority build (naive
    * peer offload) only the priorities and the free slot apply. */
#if AIOT_MOQT_OFFLOAD_MODE==1
   /* Mission-aware offload: one frame per local inference, taken halfway through it, so that the
    * samples of the camera and of the helper alternate at even spacing (the counter fails when a
    * screw moves far between two samples); skipped when no tracked screw is within one step of
    * the line on its way to it (nothing can cross before the next local sample). */
   bool off_due=false;uint32_t off_prev_cycle=moq_off_cycle;(void)off_prev_cycle;
   {uint32_t el,fl,st;source_manager_ai_phase(&el,&fl,&st);
    if(fl&&st!=moq_off_cycle&&el>=fl/2&&el<fl){
     int line=moq_passages.cfg.line,step=moq_passages.cfg.max_step;bool active=false;
     for(unsigned i=0;i<MOQ_PASSAGE_TRACKS;i++)
      if(moq_passages.tracks[i].used&&!moq_passages.tracks[i].counted&&
         (int)moq_passages.tracks[i].x>=line-step&&(int)moq_passages.tracks[i].x<=line+step/2)active=true;
     if(active)off_due=true;else{moq_off_cycle=st;moq_off_skipped++;}
     if(off_due)moq_off_cycle=st;
    }}
   if(now-epoch<MOQ_RUN_US&&MOQ_OFF_HAVE&&MOQ_TASK_GATE(now)&&off_due){
#elif AIOT_MOQT_OFFLOAD_SPLIT
   /* the split was decided in the inference task: the frame's raster goes out as soon as it is kept */
   moq_argus_observe(now);moq_argus_report(now);moq_split_report(now);moq_split_window=now-epoch<MOQ_RUN_US;
#if AIOT_MOQT_SPLIT0_HYBRID
   /* hybrid supply: the ready raster when the general rule admits it (helper free, in time, evidence slack); else,
    * when no raster is ready, the newest captured frame itself if the rule admits its size */
   moq_hy_raw=false;
   bool hy_go=MOQ_OFF_HAVE&&moq_gen_due(now,moq_rs.cap_us,moq_off_small_last,0);
   if(!hy_go&&!MOQ_OFF_HAVE&&moq_off_len&&moq_off_seq!=moq_off_sent_seq&&moq_gen_due(now,moq_off_capture,moq_off_len,0))
    hy_go=moq_hy_raw=true;
   if(now-epoch<MOQ_RUN_US&&hy_go&&++moq_split_frames){
#elif AIOT_MOQT_SPLIT_CORE0&&AIOT_MOQT_SPLIT0_PREFETCH
   /* the raster is ready ahead: it goes when the Chen rule finds the helper free */
   if(now-epoch<MOQ_RUN_US&&MOQ_OFF_HAVE&&moq_chen_due(now,moq_off_small_last)&&++moq_split_frames){
#else
   if(now-epoch<MOQ_RUN_US&&MOQ_OFF_HAVE){
#endif
#elif AIOT_MOQT_OFFLOAD_MODE==3
   moq_argus_observe(now);moq_argus_report(now);
   if(now-epoch<MOQ_RUN_US&&MOQ_OFF_HAVE&&MOQ_TASK_GATE(now)&&moq_argus_due(now,MOQ_OFF_DECIDE_LEN)){
#elif AIOT_MOQT_OFFLOAD_MODE==4
   moq_argus_observe(now);moq_argus_report(now);
   if(now-epoch<MOQ_RUN_US&&MOQ_OFF_HAVE&&MOQ_TASK_GATE(now)&&!MOQ_EV_WAITING&&MOQ_OFF_DUE(now)){
#else
   if(now-epoch<MOQ_RUN_US&&MOQ_OFF_HAVE&&MOQ_TASK_GATE(now)&&now-moq_off_at>=(uint64_t)AIOT_MOQT_OFFLOAD_MS*1000){
#endif
#if AIOT_MOQT_OFFLOAD_SCHED
    int why=moq_off_admit(now,moq_off_len);
    if(why>=0){
     /* counted once per frame; a mission-aware offload tries again while its cycle lasts */
     if(moq_off_refused_seq!=moq_off_seq){moq_off_refused[why]++;moq_off_refused_seq=moq_off_seq;}
#if AIOT_MOQT_OFFLOAD_MODE==1
     moq_off_cycle=off_prev_cycle;
#endif
    }else
#endif
    if(quic_client_moq_image_slot_available()){
     uint32_t off_seq=moq_off_seq;uint64_t off_cap=moq_off_capture;bool off_ok=true;
     const uint8_t *off_data=moq_off_buf;size_t off_len=moq_off_len;
#if AIOT_MOQT_SPLIT0_HYBRID
     if(moq_hy_raw){
      /* hybrid supply: the helper is free and no raster is ready, the uplink and the evidence slack allow the
       * newest captured frame itself (the helper decodes it); copied under the capture's lock */
      static uint8_t *off_raw;
      if(!off_raw)off_raw=heap_caps_malloc(MOQ_OFF_CAP,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
      xSemaphoreTake(moq_off_lock,portMAX_DELAY);
      off_seq=moq_off_seq;off_cap=moq_off_capture;off_len=moq_off_len;
      off_ok=off_raw&&off_len<=MOQ_OFF_CAP;
      if(off_ok)memcpy(off_raw,moq_off_buf,off_len);
      xSemaphoreGive(moq_off_lock);
      off_data=off_raw;moq_hy_sent++;
     }else
#endif
#if MOQ_HELPER_RASTER
     {static uint8_t *off_small;moq_representation_metrics tm={0};
      if(!off_small)off_small=heap_caps_malloc(MOQ_OFF_SMALL_CAP,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
      xSemaphoreTake(moq_rs.lock,portMAX_DELAY);
      off_seq=moq_rs.seq;off_cap=moq_rs.cap_us;
      off_ok=off_small&&moq_encode_rgb565(moq_rs.buf,moq_rs.w,moq_rs.h,AIOT_MOQT_OFFLOAD_RASTER,off_small,MOQ_OFF_SMALL_CAP,&tm);
      xSemaphoreGive(moq_rs.lock);
      if(off_ok){off_data=off_small;off_len=tm.bytes;moq_off_small_last=tm.bytes;}
      else{moq_rs.failed++;moq_off_sent_seq=off_seq;}   /* the task stays open for the next passage frame */
      printf("[MOQ_OFFLOAD_CODEC] {\"seq\":%lu,\"out\":%u,\"w\":%u,\"h\":%u,\"encode_us\":%llu,\"ok\":%d}\n",
             (unsigned long)off_seq,(unsigned)tm.bytes,tm.width,tm.height,(unsigned long long)tm.encode_us,(int)off_ok);}
#elif AIOT_MOQT_OFFLOAD_EIGHTH
     /* The helper runs the detector of the camera, which sees the frame at 1/8 (200x150): the
      * full UXGA JPEG (~70 KB) does not cross the first hop within the offload timeout, the
      * frame at 1/8 (a few KB) does, with the pixels the detector uses. */
     static uint8_t *off_small;moq_representation_metrics tm;
     if(!off_small)off_small=heap_caps_malloc(MOQ_OFF_SMALL_CAP,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
     if(off_small&&moq_transcode_eighth(moq_off_buf,moq_off_len,AIOT_MOQT_OFFLOAD_EIGHTH,off_small,MOQ_OFF_SMALL_CAP,&tm)){
      off_data=off_small;off_len=tm.bytes;moq_off_small_last=tm.bytes;
      printf("[MOQ_OFFLOAD_CODEC] {\"seq\":%lu,\"in\":%u,\"out\":%u,\"w\":%u,\"h\":%u,\"decode_us\":%llu,\"encode_us\":%llu}\n",
             (unsigned long)moq_off_seq,(unsigned)moq_off_len,(unsigned)tm.bytes,tm.width,tm.height,
             (unsigned long long)tm.decode_us,(unsigned long long)tm.encode_us);
     }
#endif
     if(off_ok){
     uint8_t props[16];const uint64_t pairs[][2]={{MOQ_PROP_CAPTURE_MS,off_cap/1000}};
     quic_moq_object q={.alias=1,.data=off_data,.len=off_len,.group=off_seq,.end_of_group=true,
      .props=props,.props_len=moq_props(props,sizeof(props),pairs,1),.source_us=off_cap,
      .deadline_us=now+(uint64_t)AIOT_MOQT_OFFLOAD_TIMEOUT_MS*1000};
#if AIOT_MOQT_SHARED_FRAMES
     q.deadline_us=now+(uint64_t)AIOT_MOQT_SHARED_HELPER_MS*1000;   /* the track keeps evidence 10 s; a helper frame is stale after this */
#endif
#if AIOT_MOQT_OFFLOAD_GENERAL
     /* dropped in flight when the helper could no longer end it by capture + T_max */
     q.deadline_us=off_cap+(uint64_t)(AIOT_MOQT_CH_TMAX_MS-AIOT_MOQT_OFFLOAD_MS)*1000;
     int r=q.deadline_us>now?quic_client_moq_publish_ex(&q):-2;
#else
     int r=quic_client_moq_publish_ex(&q);
#endif
     printf("[MOQ_OFFLOAD] {\"seq\":%lu,\"capture_us\":%llu,\"bytes\":%u,\"result\":%d}\n",(unsigned long)off_seq,
            (unsigned long long)off_cap,(unsigned)off_len,r);
     if(r==0)moq_off_sent++;
#if AIOT_MOQT_OFFLOAD_TASK_MS
     /* the frame serves its task; with OFFLOAD_RASTER every open task whose passage frame is this one
      * or older (their frames were replaced by this newer passage frame) */
     if(r==0)do{
      printf("[MOQ_TASK] {\"passage\":%lu,\"frame\":%lu,\"seq\":%lu,\"wait_us\":%llu}\n",(unsigned long)moq_task.id[moq_task.tail%MOQ_TASKS],
             (unsigned long)moq_task.seq[moq_task.tail%MOQ_TASKS],(unsigned long)off_seq,(unsigned long long)moq_task_age(now));
      moq_task.tail++;moq_task.sent++;
     }while(AIOT_MOQT_OFFLOAD_RASTER&&moq_task.tail!=moq_task.head&&(int32_t)(moq_task.seq[moq_task.tail%MOQ_TASKS]-off_seq)<=0);
#endif
#if AIOT_MOQT_OFFLOAD_MODE==3||AIOT_MOQT_OFFLOAD_MODE==4
     if(r==0){moq_ar.on=true;moq_ar.seen=false;moq_ar.start=now;moq_ar.len=off_len;}
#endif
     moq_off_sent_seq=off_seq;moq_off_at=now;
     }
    }else moq_off_busy++;
   }
#endif
#if AIOT_MOQT_SHARED_FRAMES
   /* evidence frames on the shared track: oldest first, while an image slot is free; dropped at its deadline */
   if(moq_ev.lock&&moq_ev.head!=moq_ev.tail&&quic_client_moq_image_slot_available()){
    static uint8_t *ev_out;moq_representation_metrics tm={0};
    if(!ev_out)ev_out=heap_caps_malloc(MOQ_OFF_SMALL_CAP,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    xSemaphoreTake(moq_ev.lock,portMAX_DELAY);
    unsigned k=moq_ev.tail%MOQ_EV_SLOTS;uint32_t seq=moq_ev.seq[k];uint64_t cap=moq_ev.cap[k];
    uint64_t dl=cap+(uint64_t)AIOT_MOQT_AI_DEADLINE_MS*1000;
    bool ok=now<dl&&ev_out&&moq_encode_rgb565(moq_ev.buf[k],moq_ev.w[k],moq_ev.h[k],AIOT_MOQT_OFFLOAD_RASTER,ev_out,MOQ_OFF_SMALL_CAP,&tm);
    xSemaphoreGive(moq_ev.lock);
    int r=-3;
    if(ok){
     /* the evidence property (0x7C) marks the evidence among the frames of the shared track: a relay can deliver
      * it to the operator and keep the helper's frames for the helper (relay --roles) */
     uint8_t props[24];const uint64_t pairs[][2]={{MOQ_PROP_CAPTURE_MS,cap/1000},{MOQ_PROP_EVIDENCE_REQUIRED,1}};
     quic_moq_object q={.alias=1,.data=ev_out,.len=tm.bytes,.group=seq,.end_of_group=true,
      .props=props,.props_len=moq_props(props,sizeof(props),pairs,2),.source_us=cap,.deadline_us=dl};
     r=quic_client_moq_publish_ex(&q);
    }
    if(r==0||now>=dl||!ok){
     xSemaphoreTake(moq_ev.lock,portMAX_DELAY);moq_ev.tail++;xSemaphoreGive(moq_ev.lock);
     if(r==0)moq_ev.published++;else moq_ev.expired++;
     printf("[MOQ_SHARED_EVIDENCE] {\"seq\":%lu,\"bytes\":%u,\"encode_us\":%llu,\"result\":%d,\"age_us\":%llu}\n",(unsigned long)seq,
            (unsigned)tm.bytes,(unsigned long long)tm.encode_us,r,(unsigned long long)(now-cap));
    }
   }
   {static uint64_t at;if(now-at>=20000000ULL){at=now;
    printf("[MOQ_SHARED] {\"queued\":%u,\"published\":%u,\"expired\":%u,\"dropped\":%u,\"failed\":%u}\n",moq_ev.queued,moq_ev.published,
           moq_ev.expired,moq_ev.dropped,moq_ev.failed);}}
#endif
#if AIOT_MOQT_MUS
   if(now-epoch<MOQ_DRAIN_US)mus_service(now);
#else
#if AIOT_MOQT_TRACE_MS
   {static uint64_t trace_at;
    if(now-trace_at>=(uint64_t)AIOT_MOQT_TRACE_MS*1000){trace_at=now;
     static char tr[400];quic_client_moq_trace(tr,sizeof(tr));
     wifi_ap_record_t ap;int rssi=esp_wifi_sta_get_ap_info(&ap)==ESP_OK?ap.rssi:0;
     printf("[MOQ_TRACE] {\"t_us\":%llu,%s,\"rssi\":%d,\"ai_safe_us\":%lu,\"ai_done\":%lu}\n",(unsigned long long)now,tr,rssi,
      (unsigned long)source_manager_alert_safe_us(),(unsigned long)source_manager_ai_completed());}}
#endif
#if AIOT_MOQT_VIDEO_WINDOW_MS
   /* MBM on the media track: a result waits behind the media bytes in the network, at the
    * service rate at most AIOT_MOQT_VIDEO_WINDOW_MS (the device budget, as the window of
    * app_moq_mus.inc); with AIOT_MOQT_MUS_WINDOW_BDP the window also holds the bytes on the wire. */
   {static uint64_t window_at;
    if(now-window_at>=100000){window_at=now;
     uint64_t wait_us=(uint64_t)AIOT_MOQT_VIDEO_WINDOW_MS*1000+(AIOT_MOQT_MUS_WINDOW_BDP?quic_client_moq_min_rtt_us():0);
     uint64_t rate=quic_client_moq_service_initialized()?quic_client_moq_service_rate():8192;
     size_t window=(size_t)(rate*wait_us/1000000ULL);
     quic_client_moq_image_window(window<1200?1200:window);}}
#endif
#if AIOT_MOQT_ASYNC_CODEC
   moq_codec_result codec_result;
   if(moq_codec_worker_take(&codec_result)){
    moq_observe_codec(codec_result,moq_encoding.level);
    moq_encoding.len=codec_result.metrics.bytes;moq_encoding.ready=codec_result.ok;
   }
   if(moq_encoding.ready){
    if(now-epoch>=MOQ_DRAIN_US){
     printf("[MOQ_CODEC_CUTOFF] group=%lu\n",(unsigned long)moq_encoding.seq);moq_encoding.ready=false;
    }else if(quic_client_moq_image_slot_available()){
     moq_publish_media(moq_converted,moq_encoding.len,moq_encoding.seq,moq_encoding.capture,moq_encoding.deadline,moq_encoding.level);
     moq_encoding.ready=false;
    }
   }
#endif
   if(now-epoch<(AIOT_MOQT_EVENT_EVIDENCE?MOQ_DRAIN_US:MOQ_RUN_US)
#if AIOT_MOQT_BOUNDED_PUMP && !AIOT_MOQT_CODEC_PIPELINE
      && quic_client_moq_image_slot_available()
#endif
#if AIOT_MOQT_ASYNC_CODEC
      && !moq_codec_worker_busy() && !moq_encoding.ready
#endif
      && xSemaphoreTake(moq_image_lock,0)==pdTRUE){
    size_t len=moq_latest_len;uint32_t seq=moq_latest_seq;uint64_t capture=moq_latest_capture;
    if(len){memcpy(moq_stage,moq_latest,len);moq_latest_len=0;}xSemaphoreGive(moq_image_lock);
    if(len){
     const uint8_t *media=moq_stage;unsigned representation=0;
#if AIOT_MOQT_MISSION_COUNT && !AIOT_MOQT_MISSION_IMAGE_TIMEOUT
     uint64_t media_deadline=UINT64_MAX;
#else
     uint64_t media_deadline=capture+(uint64_t)(AIOT_MOQT_EVENT_EVIDENCE?AIOT_MOQT_AI_DEADLINE_MS:AIOT_MOQT_PREVIEW_DEADLINE_MS)*1000;
#endif

#if AIOT_MOQT_LADDER
     const uint8_t *levels[]={moq_stage,moq_medium_start,moq_low_start};
     size_t sizes[]={len,(size_t)(moq_medium_end-moq_medium_start),(size_t)(moq_low_end-moq_low_start)};
     if(AIOT_MOQT_REPRESENTATION==1)representation=2;
     else if(AIOT_MOQT_REPRESENTATION==3)representation=1;
     else if(AIOT_MOQT_REPRESENTATION==2){
      int choice=moqt_choose_representation(sizes,3,quic_client_moq_service_rate(),
        esp_timer_get_time(),media_deadline);
      /* If none fits, offer the lowest to admission for rejection/bounded probe. */
      representation=choice<0?2:(unsigned)choice;
      if(AIOT_MOQT_SAFE_START&&!quic_client_moq_service_initialized())representation=2;
     }
     media=levels[representation];len=sizes[representation];
#endif
#if AIOT_MOQT_TRANSCODE

     if(AIOT_MOQT_REPRESENTATION==1)representation=2;
     else if(AIOT_MOQT_REPRESENTATION==3)representation=1;
     else if(AIOT_MOQT_REPRESENTATION==2){
      /* Bootstrap size estimates are explicit priors. Recheck actual encoded
       * size and remaining deadline at transport admission after conversion. */
      size_t sizes[]={len,last_medium_bytes?last_medium_bytes:len/3,
                            last_low_bytes?last_low_bytes:len/8};
      int choice=moqt_choose_representation(sizes,3,quic_client_moq_service_rate(),
          esp_timer_get_time(),media_deadline);
      representation=choice<0?2:(unsigned)choice;
      if(AIOT_MOQT_SAFE_START&&!quic_client_moq_service_initialized())representation=2;
     }else if(AIOT_MOQT_REPRESENTATION==4){
      /* Throughput rule of rate adaptation: the largest representation whose mean rate
       * stays below a share of the estimated throughput. Deadlines are not considered. */
      static uint64_t previous_capture,interval_us=5600000;
      if(previous_capture&&capture>previous_capture)interval_us=(3*interval_us+(capture-previous_capture))/4;
      previous_capture=capture;
      size_t sizes[]={len,last_medium_bytes?last_medium_bytes:len/4,last_low_bytes?last_low_bytes:len/12};
      uint64_t budget=quic_client_moq_service_rate()*8/10*interval_us/1000000;
      representation=2;
      for(unsigned r=0;r<3;r++)if(sizes[r]<=budget){representation=r;break;}
      if(!quic_client_moq_service_initialized())representation=2;
     }
     if(representation){
      moq_codec_request q={.input=moq_stage,.length=len,.level=representation,.output=moq_converted,.capacity=MOQ_IMAGE_CAP};
#if AIOT_MOQT_ASYNC_CODEC
      moq_encoding.seq=seq;moq_encoding.capture=capture;moq_encoding.deadline=media_deadline;moq_encoding.level=representation;
      if(!moq_codec_worker_submit(&q))printf("[MOQ_CODEC_ERROR] submit\n");
      /* moq_stage remains immutable until completion is consumed below. */
      len=0;
#else
      moq_codec_result r=moq_codec_execute(&q);moq_observe_codec(r,representation);
      if(r.ok){media=moq_converted;len=r.metrics.bytes;}else len=0;
#endif
     }
#endif
     if(len)moq_publish_media(media,len,seq,capture,media_deadline,representation);
    }
   }
#endif /* AIOT_MOQT_MUS */
#if AIOT_MOQT_WIRE_PROFILE
   /* Status goes out once a second as a datagram, or at once when a subscriber asks
    * for a new Group (draft-21 section 9.20.20). */
   if(now-last_stats>=1000000||quic_client_moq_new_group_requested(MOQ_ALIAS_STATUS))moq_publish_status(now,++status_group);
#endif
   if(now-last_stats>=1000000){last_stats=now;
#if AIOT_MOQT_POLICY_EXPERIMENT
    quic_client_moq_policy_stats();
#endif
    printf("[MOQ_PUMP] {\"calls\":%llu,\"sum_us\":%llu,\"max_us\":%llu}\n",(unsigned long long)pump_calls,(unsigned long long)pump_sum,(unsigned long long)pump_max);pump_calls=pump_sum=pump_max=0;
    moq_bc_update(now);
    printf("[MOQ_RESOURCE] {\"ts_us\":%llu,\"ai_completed\":%lu,\"internal_free\":%u,\"psram_free\":%u,\"largest_psram\":%u}\n",(unsigned long long)now,(unsigned long)source_manager_ai_completed(),(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),(unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
   }
   if(now-epoch>=MOQ_DRAIN_US && __atomic_load_n(&moq_capture_stopped,__ATOMIC_ACQUIRE))
    quic_client_moq_source_observed(moq_source_count,moq_source_failed,moq_source_last-moq_source_first,moq_source_max_gap);
   quic_client_moq_pump_gap_observed(pump_gap_max);
#if AIOT_MOQT_MISSION_COUNT
   /* Nothing is left to send: the publication ends without waiting for the cutoff. */
   bool moq_drained=now-epoch>=MOQ_RUN_US&&__atomic_load_n(&moq_capture_stopped,__ATOMIC_ACQUIRE)&&source_manager_ai_idle()
     &&!source_manager_ai_event_count()&&quic_client_moq_session_ready()&&quic_client_moq_ai_slot_available()
     &&quic_client_moq_image_slot_available()&&!moq_latest_len
#if AIOT_MOQT_MUS
     &&!mus_busy(now)
#endif
     ;
#else
   bool moq_drained=false;
#endif
   if((now-epoch>=MOQ_DRAIN_US||moq_drained)
#if AIOT_MOQT_ASYNC_CODEC
      && !moq_codec_worker_busy() && !moq_encoding.ready
#endif
      && quic_client_moq_finish()<0){printf("[MOQ_ERROR] finalize\n");break;}
  }
  vTaskDelay(pdMS_TO_TICKS(5));
 }
 __atomic_store_n(&moq_capture_stop,true,__ATOMIC_RELAXED);
 /* Do not free producer memory while a camera acquisition is in progress. */
 if(capturing)while(!__atomic_load_n(&moq_capture_stopped,__ATOMIC_ACQUIRE)){esp_task_wdt_reset();vTaskDelay(pdMS_TO_TICKS(20));}
 quic_client_cleanup();
 cleanup:
#if AIOT_MOQT_SPLIT_CORE0
 /* park the core-0 decoder before anything it uses is freed (a decode in progress takes up to ~2 s) */
 if(moq_s0_task){
  moq_s0_stop=true;xTaskNotifyGive(moq_s0_task);
  uint64_t w0=esp_timer_get_time();
  while(!moq_s0_stopped&&esp_timer_get_time()-w0<5000000ULL){esp_task_wdt_reset();vTaskDelay(pdMS_TO_TICKS(20));}
  printf("[MOQ_SPLIT0] {\"stopped\":%d}\n",(int)moq_s0_stopped);
 }
#endif
#if AIOT_MOQT_ASYNC_CODEC
 moq_codec_worker_stop();
#endif
#if AIOT_MOQT_MUS
 mus_report();mus_free();
#endif
 free(moq_converted);free(moq_latest);free(moq_stage);if(moq_image_lock)vSemaphoreDelete(moq_image_lock);
#if AIOT_MOQT_AUDIT
 uint64_t audit_wait=esp_timer_get_time();
 while(!source_manager_ai_idle()&&esp_timer_get_time()-audit_wait<15000000){esp_task_wdt_reset();vTaskDelay(pdMS_TO_TICKS(20));}
 if(!source_manager_ai_idle())printf("[MOQ_ERROR] AI not quiescent\n");
 source_manager_audit_pending();
 if(moq_latest_len)printf("[MOQ_PENDING] {\"alias\":1,\"group\":%lu}\n",(unsigned long)moq_latest_seq);
 if(!moq_audit_export())puts("[AUDIT_ERROR] incomplete journal");
#endif
#if AIOT_MOQT_OTA_REMOTE
 /* A finite measurement must remain remotely recoverable after the receiver
  * closes the MoQ session.  Leave Wi-Fi up and poll the signed OTA manifest;
  * otherwise a completed headless run would require USB to restore it. */
 esp_task_wdt_delete(NULL);
 /* A run that never started leaves its image unconfirmed, and an unconfirmed image accepts no
  * update; the previous image is restored instead, so that the device stays reachable. */
 printf("[MOQ_OTA] experiment_complete=1 pending=%d\n",(int)ota_boot_pending());fflush(stdout);
 vTaskDelay(pdMS_TO_TICKS(1000));
 ota_rollback_if_pending();
 printf("[MOQ_OTA] experiment_complete=1 waiting_for_update=1\n");
 for(;;){
  char target[65];
  if(ota_pull_check(target)){
   printf("[MOQ_OTA] update_available=1\n");
   esp_err_t ota_err=ota_pull_install(target);
   printf("[MOQ_OTA] install_failed=%s\n",esp_err_to_name(ota_err));
  }
  vTaskDelay(pdMS_TO_TICKS(30000));
 }
#else
 esp_task_wdt_delete(NULL);
#endif
}

#if AIOT_MOQT_AUDIT
#undef printf
#endif
