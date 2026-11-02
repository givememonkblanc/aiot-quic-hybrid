/* AIOT_MOQT_HELPER: a second ESP32 that adds inference capacity to the camera ESP32.
 *
 * No camera and no publishing. The board connects to the downstream side of the MOQT relay
 * (draft-21, QUIC client, ALPN moqt-21, the relay certificate pinned as in every MoQ build),
 * subscribes to one track (aiot/esp32 AIOT_MOQT_HELPER_TRACK, default offload/jpeg) and runs
 * the detector of the camera (ai_inference_run, same model and ROI options) on every JPEG
 * Object it receives. Admission is latest-frame: one frame is inferred, one waits; a newer
 * frame replaces the waiting one, which is counted as dropped.
 *
 * Every inference gives one JSON object, on the console as
 *   [HELPER_DET] {"group":..,"capture_ms":..,"recv_us":..,"start_us":..,"end_us":..,"dets":[[x,y],..],..}
 * and the same object, without the tag, as one UDP datagram to
 * AIOT_MOQT_HELPER_REPORT_HOST:AIOT_MOQT_HELPER_REPORT_PORT (debug), and the same object as one MoQ
 * Object on the helper's own track aiot/esp32 AIOT_MOQT_HELPER_EVENTS_TRACK (default helper/events),
 * published on the same session: the relay recognizes the helper by its SETUP Implementation
 * "aiot-helper/1" and subscribes to that track (relay --helper-events). Group = the frame's Group
 * (camera sequence), property 0x78 = capture_ms, publisher priority 64, delivery timeout 2 s,
 * not retained. A Group older than the last published one (the camera restarted) opens a new
 * session, whose publisher state starts afresh. capture_ms is property 0x78 of the
 * Object (capture time in ms on the camera clock), 0 when absent; the *_us times are of this
 * board's clock. The connection is opened again 2 s after any failure and the track is
 * subscribed again, so the relay may restart between runs. */
#include "lwip/sockets.h"
#include "../components/ai_inference/include/ai_inference.h"
#include "esp_mac.h"
#if AIOT_MQTT_HELPER
/* AIOT_MQTT_HELPER, the MQTT baseline of the helper: the same worker, fed from the broker instead of
 * the relay. MQTT 3.1.1 over TLS to AIOT_MOQT_HELPER_HOST:PORT (the broker's subscriber listener),
 * subscribed to the camera's frames (aiot/esp32/offload/jpeg, payload [alias u8][group u32 BE]
 * [properties length u16 BE][properties][JPEG], as moq_mqtt_shim.c), results published with QoS 1 on
 * aiot/esp32/helper/events (the same JSON). */
#include "mqtt5_client.h"
#define HELPER_MQ_FRAMES "aiot/esp32/offload/jpeg"
#define HELPER_MQ_EVENTS "aiot/esp32/helper/events"
extern const char helper_mq_ca[] asm("_binary_mqtt_ca_pem_start");
static esp_mqtt_client_handle_t helper_mq;
static volatile bool helper_mq_connected;
static uint8_t *helper_mq_buf;static size_t helper_mq_cap;
static uint64_t helper_mq_first_us;
#endif
#ifndef AIOT_MOQT_HELPER_FRAME_KB
#define AIOT_MOQT_HELPER_FRAME_KB 320
#endif
#define HELPER_FRAME_CAP ((size_t)AIOT_MOQT_HELPER_FRAME_KB*1024U)
#define HELPER_PROP_CAPTURE_MS 0x78
#define HELPER_RETRY_MS 2000
#define HELPER_CONNECT_LIMIT_US 10000000ULL   /* handshake or subscription not done: start again */
#define HELPER_EVENTS_ALIAS 2                  /* alias 2: the small-object (result) slots of the adapter */
#define HELPER_EVENTS_TIMEOUT_MS 2000
/* Published tracks: alias 1 is unused (alias 1 and 3 take the image slots of the adapter). */
static const moqt_track_cfg helper_tracks[]={
 {.ns={"aiot","esp32"},.ns_count=2,.name="helper/status",.priority=224},
 {.ns={"aiot","esp32"},.ns_count=2,.name=AIOT_MOQT_HELPER_EVENTS_TRACK,.priority=64,.object_timeout_ms=HELPER_EVENTS_TIMEOUT_MS},
};
static uint32_t helper_published,helper_publish_failed,helper_unsubscribed;
static uint64_t helper_last_group;static bool helper_has_last,helper_regressed;

typedef struct { uint8_t *data; size_t len; uint64_t group,object,capture_ms,first_us,recv_us; bool has_capture; } helper_frame;
static helper_frame helper_pending,helper_work;
static bool helper_has_pending;
static SemaphoreHandle_t helper_lock;
static TaskHandle_t helper_task;
static uint32_t helper_received,helper_inferred,helper_dropped,helper_too_big,helper_failed,helper_sessions;
static uint64_t helper_infer_sum_us,helper_infer_max_us;
static int helper_sock=-1;
static struct sockaddr_in helper_report_addr;
static ai_inference_t helper_inference;
static StackType_t helper_stack[12288/sizeof(StackType_t)];
static StaticTask_t helper_task_storage;

/* QUIC task, transport lock held: keep the newest frame only. */
static void helper_sink(const uint8_t *data,size_t len,uint64_t group,uint64_t object,const uint8_t *props,size_t props_len,
                        uint64_t first_us,uint64_t done_us){
 helper_received++;
 if(len>HELPER_FRAME_CAP){helper_too_big++;return;}
 uint64_t capture=0;bool has_capture=props_len&&moqt_props_find_int(props,props_len,HELPER_PROP_CAPTURE_MS,&capture);
 xSemaphoreTake(helper_lock,portMAX_DELAY);
 if(helper_has_pending){
  helper_dropped++;
  printf("[HELPER_DROP] {\"group\":%llu,\"by\":%llu}\n",(unsigned long long)helper_pending.group,(unsigned long long)group);
 }
 memcpy(helper_pending.data,data,len);
 helper_pending.len=len;helper_pending.group=group;helper_pending.object=object;
 helper_pending.capture_ms=has_capture?capture:0;helper_pending.has_capture=has_capture;
 helper_pending.first_us=first_us;helper_pending.recv_us=done_us;
 helper_has_pending=true;
 xSemaphoreGive(helper_lock);
 xTaskNotifyGive(helper_task);
}

static void helper_report(const helper_frame *f,const aiot_ai_result_t *r,bool ran,uint64_t start,uint64_t end){
 static char json[1024];
 int n=snprintf(json,sizeof(json),"{\"group\":%llu,\"capture_ms\":%llu,\"recv_us\":%llu,\"start_us\":%llu,\"end_us\":%llu,\"dets\":[",
   (unsigned long long)f->group,(unsigned long long)f->capture_ms,(unsigned long long)f->recv_us,(unsigned long long)start,(unsigned long long)end);
 unsigned count=ran?r->detection_count:0;
 if(count>AIOT_AI_RESULT_MAX_DETECTIONS)count=AIOT_AI_RESULT_MAX_DETECTIONS;
 for(unsigned i=0;i<count&&n<(int)sizeof(json)-200;i++)n+=snprintf(json+n,sizeof(json)-n,"%s[%u,%u]",i?",":"",r->detections[i].x,r->detections[i].y);
 n+=snprintf(json+n,sizeof(json)-n,"],\"n\":%u,\"ran\":%u,\"backend\":%u,\"bytes\":%u,\"first_us\":%llu,\"has_capture\":%u,\"dropped\":%lu,\"inferred\":%lu}",
   count,(unsigned)ran,(unsigned)(ran?r->backend:0),(unsigned)f->len,(unsigned long long)f->first_us,(unsigned)f->has_capture,
   (unsigned long)helper_dropped,(unsigned long)helper_inferred);
 if(n<=0||n>=(int)sizeof(json))return;
 printf("[HELPER_DET] %s\n",json);
 if(helper_sock>=0)sendto(helper_sock,json,(size_t)n,0,(struct sockaddr*)&helper_report_addr,sizeof(helper_report_addr));
#if AIOT_MQTT_HELPER
 if(helper_mq&&helper_mq_connected&&esp_mqtt_client_enqueue(helper_mq,HELPER_MQ_EVENTS,json,n,1,0,true)>=0)helper_published++;
 else helper_publish_failed++;
 printf("[HELPER_EVENT] {\"group\":%llu,\"published\":%u,\"rv\":0}\n",(unsigned long long)f->group,(unsigned)helper_mq_connected);
 return;
#endif
 /* the same object over MoQ, to whoever subscribed to the events track (the relay) */
#if AIOT_MOQT_HELPER_ANY_GROUP
 /* several cameras share this helper through the relay: their groups interleave, only a repeat is skipped */
 if(helper_has_last&&f->group==helper_last_group)return;
 if(0){
#else
 if(helper_has_last&&f->group<=helper_last_group){
#endif
  helper_regressed=true;helper_publish_failed++;
  printf("[HELPER_EVENT] {\"group\":%llu,\"published\":0,\"why\":\"group_regressed\"}\n",(unsigned long long)f->group);
  return;
 }
 uint8_t props[16];moqt_wr w=moqt_wr_of(props,sizeof(props));uint64_t prev=0;
 moqt_kvp_put_int(&w,&prev,HELPER_PROP_CAPTURE_MS,f->capture_ms);
 uint64_t now=esp_timer_get_time();
 quic_moq_object q={.alias=HELPER_EVENTS_ALIAS,.data=(const uint8_t*)json,.len=(size_t)n,.group=f->group,.end_of_group=true,
  .props=props,.props_len=w.bad?0:w.n,.source_us=f->recv_us,.deadline_us=now+(uint64_t)HELPER_EVENTS_TIMEOUT_MS*1000,.importance=2};
 int rv=quic_client_moq_publish_ex(&q);
 if(rv==0){helper_published++;helper_last_group=f->group;helper_has_last=true;}
 else if(rv==-4)helper_unsubscribed++;
 else helper_publish_failed++;
 printf("[HELPER_EVENT] {\"group\":%llu,\"published\":%u,\"rv\":%d}\n",(unsigned long long)f->group,(unsigned)(rv==0),rv);
}

/* Inference worker on core 1, away from Wi-Fi and QUIC, as the camera's AI worker. */
static void helper_worker(void *unused){
 (void)unused;
 static aiot_ai_result_t result;
 for(;;){
  ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
  for(;;){
   xSemaphoreTake(helper_lock,portMAX_DELAY);
   bool have=helper_has_pending;
   if(have){helper_frame t=helper_work;helper_work=helper_pending;helper_pending=t;helper_has_pending=false;}
   xSemaphoreGive(helper_lock);
   if(!have)break;
   const helper_frame *f=&helper_work;
   uint64_t start=esp_timer_get_time();
   bool ran=f->len>=100&&f->data[0]==0xFF&&f->data[1]==0xD8&&
            ai_inference_run(&helper_inference,f->data,(uint32_t)f->len,(uint32_t)f->group,&result);
   uint64_t end=esp_timer_get_time();
   if(ran){helper_inferred++;helper_infer_sum_us+=end-start;if(end-start>helper_infer_max_us)helper_infer_max_us=end-start;}
   else helper_failed++;
   helper_report(f,&result,ran,start,end);
   vTaskDelay(1); /* the idle task of core 1 runs between two inferences (task watchdog) */
  }
 }
}

static void helper_stats(void){
#if AIOT_MQTT_HELPER
 char t[64];snprintf(t,sizeof(t),"\"mqtt_connected\":%u",(unsigned)helper_mq_connected);
#else
 char t[256];quic_client_moq_helper_stats(t,sizeof(t));
#endif
 printf("[HELPER_STATS] {\"received\":%lu,\"inferred\":%lu,\"dropped\":%lu,\"too_big\":%lu,\"failed\":%lu,\"sessions\":%lu,\"published\":%lu,\"publish_failed\":%lu,\"no_subscriber\":%lu,\"infer_mean_us\":%llu,\"infer_max_us\":%llu,%s,\"internal_free\":%u,\"psram_free\":%u}\n",
  (unsigned long)helper_received,(unsigned long)helper_inferred,(unsigned long)helper_dropped,(unsigned long)helper_too_big,
  (unsigned long)helper_failed,(unsigned long)helper_sessions,(unsigned long)helper_published,(unsigned long)helper_publish_failed,(unsigned long)helper_unsubscribed,
  (unsigned long long)(helper_inferred?helper_infer_sum_us/helper_inferred:0),(unsigned long long)helper_infer_max_us,t,
  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),(unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
}

#if AIOT_MQTT_HELPER
/* MQTT task: a frame may come in pieces (data longer than the client's buffer); it is assembled and
 * handed to the worker as a relay Object would be. */
static void helper_mq_event(void *arg,esp_event_base_t base,int32_t id,void *data){
 (void)arg;(void)base;esp_mqtt_event_handle_t e=data;
 switch((esp_mqtt_event_id_t)id){
 case MQTT_EVENT_CONNECTED:
  helper_mq_connected=true;helper_sessions++;
  esp_mqtt_client_subscribe(helper_mq,HELPER_MQ_FRAMES,0);
  printf("[HELPER_MQTT] {\"connected\":1,\"session\":%lu}\n",(unsigned long)helper_sessions);
  break;
 case MQTT_EVENT_DISCONNECTED:
  if(helper_mq_connected)printf("[HELPER_MQTT] {\"connected\":0}\n");
  helper_mq_connected=false;break;
 case MQTT_EVENT_DATA:{
  if(e->current_data_offset==0)helper_mq_first_us=esp_timer_get_time();
  if(e->total_data_len<=0||(size_t)e->total_data_len>helper_mq_cap){if(e->current_data_offset==0)helper_too_big++;break;}
  if(e->current_data_offset+e->data_len>e->total_data_len)break;
  memcpy(helper_mq_buf+e->current_data_offset,e->data,(size_t)e->data_len);
  if(e->current_data_offset+e->data_len<e->total_data_len)break;   /* more pieces follow */
  size_t n=(size_t)e->total_data_len;const uint8_t *b=helper_mq_buf;
  if(n<7)break;
  uint64_t group=((uint64_t)b[1]<<24)|((uint64_t)b[2]<<16)|((uint64_t)b[3]<<8)|b[4];
  size_t pl=((size_t)b[5]<<8)|b[6];
  if(7+pl>n)break;
  helper_sink(b+7+pl,n-7-pl,group,0,b+7,pl,helper_mq_first_us,esp_timer_get_time());
  break;}
 default:break;
 }
}
static void helper_mq_run(void){
 static char uri[96];snprintf(uri,sizeof(uri),"mqtts://%s:%s",AIOT_MOQT_HELPER_HOST,AIOT_MOQT_HELPER_PORT);
 uint8_t mac[6]={0};esp_read_mac(mac,ESP_MAC_WIFI_STA);
 static char id[32];snprintf(id,sizeof(id),"aiot-helper-%02x%02x",mac[4],mac[5]);
 helper_mq_cap=HELPER_FRAME_CAP+64;
 helper_mq_buf=heap_caps_malloc(helper_mq_cap,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
 if(!helper_mq_buf){printf("[HELPER_ERROR] mqtt buffer\n");return;}
 /* the settings of the camera's MQTT baseline (moq_mqtt_shim.c) */
 esp_mqtt_client_config_t c={
  .broker.address.uri=uri,.broker.verification.certificate=helper_mq_ca,
  .broker.verification.skip_cert_common_name_check=true,.credentials.client_id=id,
  .session.keepalive=5,.network.reconnect_timeout_ms=1000,.network.timeout_ms=10000,
  .buffer.size=8192,.buffer.out_size=2048,.task.stack_size=8192,
 };
 helper_mq=esp_mqtt_client_init(&c);
 if(!helper_mq){printf("[HELPER_ERROR] mqtt init\n");return;}
 esp_mqtt_client_register_event(helper_mq,ESP_EVENT_ANY_ID,helper_mq_event,NULL);
 if(esp_mqtt_client_start(helper_mq)!=ESP_OK){printf("[HELPER_ERROR] mqtt start\n");return;}
 printf("[HELPER_MQTT] {\"broker\":\"%s\",\"frames\":\"%s\",\"events\":\"%s\"}\n",uri,HELPER_MQ_FRAMES,HELPER_MQ_EVENTS);
 bool confirmed=false;uint64_t last_stats=0;
 for(;;){
  esp_task_wdt_reset();
  uint64_t now=esp_timer_get_time();
  if(now-last_stats>=10000000){last_stats=now;helper_stats();}
  if(helper_mq_connected&&!confirmed){confirmed=true;ota_confirm_boot();}
  vTaskDelay(pdMS_TO_TICKS(100));
 }
}
#endif
static void app_moq_main(void) {
 esp_task_wdt_add(NULL);
 esp_log_level_set("QUIC",ESP_LOG_WARN);esp_log_level_set("DEBUG",ESP_LOG_WARN);
 esp_log_level_set("NGTCP2-RAW",ESP_LOG_WARN);esp_log_level_set("NGTCP2-DISCARD",ESP_LOG_WARN);
 app_cfg_load(&g_cfg); /* credentials stay in NVS; never printed */
 /* Field-service console over USB (set wifi.ssid ...), as in the product build. */
 provisioning_console_start(&g_cfg);
 uint8_t mac[6]={0};esp_read_mac(mac,ESP_MAC_WIFI_STA);
 printf("[HELPER_BOOT] {\"relay\":\"%s:%s\",\"track\":\"%s\",\"events\":\"%s\",\"report\":\"%s:%u\",\"frame_kb\":%u,\"mac\":\"%02x%02x\"}\n",
  AIOT_MOQT_HELPER_HOST,AIOT_MOQT_HELPER_PORT,AIOT_MOQT_HELPER_TRACK,AIOT_MOQT_HELPER_EVENTS_TRACK,AIOT_MOQT_HELPER_REPORT_HOST,(unsigned)AIOT_MOQT_HELPER_REPORT_PORT,
  (unsigned)AIOT_MOQT_HELPER_FRAME_KB,mac[4],mac[5]);
 helper_lock=xSemaphoreCreateMutex();
 helper_pending.data=heap_caps_malloc(HELPER_FRAME_CAP,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
 helper_work.data=heap_caps_malloc(HELPER_FRAME_CAP,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
 if(!helper_lock||!helper_pending.data||!helper_work.data){printf("[HELPER_ERROR] buffers\n");esp_task_wdt_delete(NULL);return;}
 ai_inference_init(&helper_inference);
 helper_task=xTaskCreateStaticPinnedToCore(helper_worker,"helper_ai",sizeof(helper_stack),NULL,tskIDLE_PRIORITY+1,helper_stack,&helper_task_storage,1);
 if(!helper_task){printf("[HELPER_ERROR] task\n");esp_task_wdt_delete(NULL);return;}
 for(;;){
  esp_task_wdt_reset();
  if(wifi_link_connect(g_cfg.wifi_ssid,g_cfg.wifi_password,APP_WIFI_TIMEOUT_MS))break;
  printf("[HELPER_ERROR] wifi; retrying (provision with the console: set wifi.ssid / wifi.pass, save)\n");
  vTaskDelay(pdMS_TO_TICKS(HELPER_RETRY_MS));
 }
 helper_sock=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
 helper_report_addr=(struct sockaddr_in){.sin_family=AF_INET,.sin_port=htons(AIOT_MOQT_HELPER_REPORT_PORT)};
 inet_pton(AF_INET,AIOT_MOQT_HELPER_REPORT_HOST,&helper_report_addr.sin_addr);
#if AIOT_MQTT_HELPER
 helper_mq_run();esp_task_wdt_delete(NULL);return;
#endif
 quic_client_moq_helper_sink(helper_sink);
 if(quic_client_moq_tracks(helper_tracks,2)){printf("[HELPER_ERROR] tracks\n");esp_task_wdt_delete(NULL);return;}
 static const char *const ns[]={"aiot","esp32"};
 char authority[96];snprintf(authority,sizeof(authority),"%s:%s",AIOT_MOQT_HELPER_HOST,AIOT_MOQT_HELPER_PORT);
 quic_client_config_t cfg={.hostname=AIOT_MOQT_HELPER_HOST,.port=AIOT_MOQT_HELPER_PORT,.alpn="\x07moqt-21"};
 bool confirmed=false;uint64_t last_stats=0;
 for(;;){
  esp_task_wdt_reset();
  if(quic_client_init_with_config(&cfg)){
   printf("[HELPER_ERROR] QUIC init\n");vTaskDelay(pdMS_TO_TICKS(HELPER_RETRY_MS));continue;
  }
  uint64_t begin=esp_timer_get_time();bool started=false,requested=false,subscribed=false;
  helper_has_last=false;helper_regressed=false; /* a new session publishes afresh (AIOT_MOQT_KEEP_CACHE off) */
  for(;;){
   esp_task_wdt_reset();
   int rv=quic_client_process();uint64_t now=esp_timer_get_time();
   if(now-last_stats>=10000000){last_stats=now;helper_stats();}
   if(rv!=0){printf("[HELPER_RECONNECT] {\"why\":\"connection\",\"subscribed\":%u}\n",(unsigned)subscribed);break;}
   if(!started&&quic_client_is_connected()){
    if(quic_client_moq_start(authority)){printf("[HELPER_RECONNECT] {\"why\":\"setup\"}\n");break;}
    started=true;
   }
   if(started&&!requested&&quic_client_moq_session_ready()){
    if(quic_client_moq_subscribe(ns,2,AIOT_MOQT_HELPER_TRACK,128)==0)requested=true;
   }
   int st=requested?quic_client_moq_subscription_state():0;
   if(st<0){printf("[HELPER_RECONNECT] {\"why\":\"subscription\"}\n");break;}
   if(st==1&&!subscribed){
    subscribed=true;helper_sessions++;
    if(!confirmed){confirmed=true;ota_confirm_boot();} /* healthy image: cancel a pending rollback */
   }
   if(helper_regressed){helper_regressed=false;helper_has_last=false;printf("[HELPER_RECONNECT] {\"why\":\"group_regressed\"}\n");break;}
   if(!subscribed&&now-begin>=HELPER_CONNECT_LIMIT_US){printf("[HELPER_RECONNECT] {\"why\":\"timeout\",\"connected\":%u}\n",(unsigned)quic_client_is_connected());break;}
   vTaskDelay(1);
  }
  quic_client_cleanup();
  vTaskDelay(pdMS_TO_TICKS(HELPER_RETRY_MS));
 }
}
