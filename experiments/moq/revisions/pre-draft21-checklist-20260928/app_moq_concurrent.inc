#include "moq_audit.h"
#if AIOT_MOQT_AUDIT
#define printf moq_audit_printf
#endif
/* Fixed-policy characterization: 10 Hz capture, latest preview, independent AI
 * result track. No adaptive scheduler, deadline guarantee, or GT accuracy claim. */
#include "freertos/semphr.h"
#include "moqt_feasibility.h"
#include "moq_representation.h"
#include "moq_codec_worker.h"
#if AIOT_MOQT_LADDER
extern const uint8_t moq_medium_start[] asm("_binary_moq_medium_start");
extern const uint8_t moq_medium_end[] asm("_binary_moq_medium_end");
extern const uint8_t moq_low_start[] asm("_binary_moq_low_start");
extern const uint8_t moq_low_end[] asm("_binary_moq_low_end");
#endif
#define MOQ_IMAGE_CAP (256U*1024U)
static uint8_t *moq_latest,*moq_stage,*moq_converted;
static size_t last_medium_bytes,last_low_bytes;
#if AIOT_MOQT_ASYNC_CODEC
static struct {uint32_t seq;uint64_t capture,deadline;unsigned level;size_t len;bool ready;} moq_encoding;
#endif
static void moq_publish_media(const uint8_t *media,size_t len,uint32_t seq,uint64_t capture,uint64_t deadline,unsigned level) {
#if AIOT_MOQT_POLICY_EXPERIMENT
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
 char authority[160];snprintf(authority,sizeof(authority),"%s:14568",g_cfg.server_host);
 quic_client_config_t cfg={.hostname=g_cfg.server_host,.port="14568",.alpn="\x07moqt-21"};
#if AIOT_MOQT_POLICY_EXPERIMENT
 if(quic_client_moq_configure(AIOT_MOQT_POLICY)){printf("[MOQ_ERROR] policy\n");goto cleanup;}
 printf("[MOQ_POLICY] selector=%u input=%s admission=%s preview_deadline_ms=%u ai_deadline_ms=%u\n",
        AIOT_MOQT_POLICY,AIOT_MOQT_REPLAY_INPUT?"replay":"camera",
        AIOT_MOQT_FIFO_ADMISSION?"fifo":"latest",
        AIOT_MOQT_PREVIEW_DEADLINE_MS,AIOT_MOQT_AI_DEADLINE_MS);
#endif
 if(quic_client_init_with_config(&cfg)){printf("[MOQ_ERROR] QUIC init\n");goto cleanup;}
 uint64_t pump_max=0,pump_sum=0,pump_calls=0,pump_previous=0,pump_gap_max=0;
 bool started=false,capturing=false;uint64_t epoch=0,last_stats=0,boot=esp_timer_get_time();
#if AIOT_MOQT_OTA_REMOTE
 uint64_t last_ota_poll=boot;
 ota_log_status();
#endif
#if !AIOT_MOQT_POLICY_EXPERIMENT
 uint32_t ai_seq=0;bool ai_seen=false;
#endif
 printf("[MOQ_BOOT] profile=draft21-dual capture_target_hz=10 model=configured backend_recorded_in_results=1\n");
 for(;;){
  esp_task_wdt_reset();uint64_t pump_begin=esp_timer_get_time();
  if(capturing){
   if(pump_previous&&pump_begin-pump_previous>pump_gap_max)pump_gap_max=pump_begin-pump_previous;
   pump_previous=pump_begin;
  }
  int process_result=quic_client_process();uint64_t now=esp_timer_get_time();
  uint64_t pump_elapsed=now-pump_begin;pump_sum+=pump_elapsed;pump_calls++;if(pump_elapsed>pump_max)pump_max=pump_elapsed;
  if(quic_client_moq_finished()){printf("[MOQ_DONE] subscriber_finished=1\n");break;}
  if(process_result!=0||now-boot>100000000){printf("[MOQ_ERROR] process or test timeout\n");break;}
  if(!started&&quic_client_is_connected()){
   if(quic_client_moq_start(authority)){printf("[MOQ_ERROR] SETUP\n");break;}started=true;
  }
  if(!capturing&&quic_client_moq_track_ready(1)&&quic_client_moq_track_ready(2)){
   epoch=now;moq_source_stop_us=epoch+60000000;
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
#if AIOT_MOQT_OTA_REMOTE
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
   if(now-epoch>=60000000)__atomic_store_n(&moq_capture_stop,true,__ATOMIC_RELAXED);
   /* Drain the last active/pending inference for 10 s. This is explicitly a
    * measurement cutoff, not an assurance that every captured frame ran AI. */
   if(now-epoch<70000000){
    aiot_ai_result_t result;uint32_t frame_ms;
#if AIOT_MOQT_POLICY_EXPERIMENT
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
     unsigned len=aiot_ai_result_serialize(&result,wire,sizeof(wire));for(unsigned i=0;i<len;i++)snprintf(hex+2*i,3,"%02x",wire[i]);
     uint64_t ack_group=0,source_to_ack_us=0;bool have_ack=quic_client_moq_last_ai_receipt(&ack_group,&source_to_ack_us);
     int n=snprintf(json,sizeof(json),"{\"schema\":1,\"seq\":%lu,\"capture_ms\":%lu,\"observed_us\":%llu,\"backend\":%u,\"detections\":%u,\"prev_ack_group\":%llu,\"prev_source_to_ack_us\":%llu,\"prev_ack_valid\":%u,\"evidence_required\":%u,\"evidence_group\":%lu,\"result_hex\":\"%s\"}",(unsigned long)result.source_sequence,(unsigned long)frame_ms,(unsigned long long)now,(unsigned)result.backend,(unsigned)result.detection_count,(unsigned long long)ack_group,(unsigned long long)source_to_ack_us,have_ack,(unsigned)(AIOT_MOQT_EVENT_EVIDENCE&&result.detection_count>0),(unsigned long)result.source_sequence,hex);
     int accepted=-1;
     if(n>0&&n<(int)sizeof(json)){
#if AIOT_MOQT_POLICY_EXPERIMENT
      accepted=quic_client_moq_publish_object(2,(uint8_t*)json,n,result.source_sequence,(uint64_t)frame_ms*1000,(uint64_t)frame_ms*1000+(uint64_t)AIOT_MOQT_AI_DEADLINE_MS*1000,result.detection_count?2:1);
#if AIOT_MOQT_EVENT_EVIDENCE
      if(accepted==0&&result.detection_count){
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
#if AIOT_MOQT_ASYNC_CODEC
   moq_codec_result codec_result;
   if(moq_codec_worker_take(&codec_result)){
    moq_observe_codec(codec_result,moq_encoding.level);
    moq_encoding.len=codec_result.metrics.bytes;moq_encoding.ready=codec_result.ok;
   }
   if(moq_encoding.ready){
    if(now-epoch>=70000000){
     printf("[MOQ_CODEC_CUTOFF] group=%lu\n",(unsigned long)moq_encoding.seq);moq_encoding.ready=false;
    }else if(quic_client_moq_image_slot_available()){
     moq_publish_media(moq_converted,moq_encoding.len,moq_encoding.seq,moq_encoding.capture,moq_encoding.deadline,moq_encoding.level);
     moq_encoding.ready=false;
    }
   }
#endif
   if(now-epoch<(AIOT_MOQT_EVENT_EVIDENCE?70000000:60000000)
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
     uint64_t media_deadline=capture+(uint64_t)(AIOT_MOQT_EVENT_EVIDENCE?AIOT_MOQT_AI_DEADLINE_MS:AIOT_MOQT_PREVIEW_DEADLINE_MS)*1000;

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
   if(now-last_stats>=1000000){last_stats=now;
#if AIOT_MOQT_POLICY_EXPERIMENT
    quic_client_moq_policy_stats();
#endif
    printf("[MOQ_PUMP] {\"calls\":%llu,\"sum_us\":%llu,\"max_us\":%llu}\n",(unsigned long long)pump_calls,(unsigned long long)pump_sum,(unsigned long long)pump_max);pump_calls=pump_sum=pump_max=0;
    printf("[MOQ_RESOURCE] {\"ts_us\":%llu,\"ai_completed\":%lu,\"internal_free\":%u,\"psram_free\":%u,\"largest_psram\":%u}\n",(unsigned long long)now,(unsigned long)source_manager_ai_completed(),(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),(unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
   }
   if(now-epoch>=70000000 && __atomic_load_n(&moq_capture_stopped,__ATOMIC_ACQUIRE))
    quic_client_moq_source_observed(moq_source_count,moq_source_failed,moq_source_last-moq_source_first,moq_source_max_gap);
   quic_client_moq_pump_gap_observed(pump_gap_max);
   if(now-epoch>=70000000
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
#if AIOT_MOQT_ASYNC_CODEC
 moq_codec_worker_stop();
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
