/* Native MOQT transport milestone; deliberately no concurrent inference yet. */
#include "mbedtls/sha256.h"
#if AIOT_MOQT_HELPER
#include "app_moq_helper.inc"
#elif AIOT_MOQT_CONCURRENT
#include "app_moq_concurrent.inc"
#else
static void app_moq_main(void) {
 esp_task_wdt_add(NULL);
 esp_log_level_set("QUIC",ESP_LOG_WARN);esp_log_level_set("DEBUG",ESP_LOG_WARN);
 esp_log_level_set("NGTCP2-RAW",ESP_LOG_WARN);esp_log_level_set("NGTCP2-DISCARD",ESP_LOG_WARN);
 app_cfg_load(&g_cfg); /* Never print credentials or save NVS in this experiment. */
 source_manager_init(&g_source_manager);
 if(!source_manager_camera_ready() || !wifi_link_connect(g_cfg.wifi_ssid,g_cfg.wifi_password,APP_WIFI_TIMEOUT_MS)){
  printf("[MOQ_ERROR] initialization\n");esp_task_wdt_delete(NULL);return;
 }
 char authority[160];snprintf(authority,sizeof(authority),"%s:14568",g_cfg.server_host);
 quic_client_config_t cfg={.hostname=g_cfg.server_host,.port="14568",.alpn="\x07moqt-21"};
 if(quic_client_init_with_config(&cfg)){printf("[MOQ_ERROR] QUIC init\n");esp_task_wdt_delete(NULL);return;}
 bool started=false;uint64_t next=0,group=0;unsigned sent=0;
 printf("[MOQ_BOOT] profile=draft21-jpeg-only\n");
 for(;;){
  esp_task_wdt_reset();
  int process_result=quic_client_process();
  if(quic_client_moq_finished()){printf("[MOQ_DONE] subscriber_finished=1\n");break;}
  if(process_result!=0){printf("[MOQ_ERROR] QUIC process\n");break;}
  if(sent==30 && quic_client_moq_finish()<0){printf("[MOQ_ERROR] finalize\n");break;}
  if(!started && quic_client_is_connected()){
   if(quic_client_moq_start(authority)){printf("[MOQ_ERROR] SETUP\n");break;}started=true;
  }
  uint64_t now=esp_timer_get_time();
  if(quic_client_moq_ready() && sent<30 && now>=next){
   next=now+1000000;data_packet_t packet;
   bool ok=source_manager_next_packet(&g_source_manager,&packet);
   if(ok&&packet.type!=DATA_TYPE_IMAGE)ok=source_manager_next_packet(&g_source_manager,&packet);
   if(ok&&packet.type==DATA_TYPE_IMAGE&&g_source_manager.last_frame_real){
    uint8_t digest[32];char hex[65];mbedtls_sha256(packet.payload,packet.payload_len,digest,0);
    for(unsigned i=0;i<32;i++)snprintf(hex+i*2,3,"%02x",digest[i]);
    int accepted=quic_client_moq_publish(packet.payload,packet.payload_len,group);
    printf("[MOQ_OBJECT] {\"group\":%llu,\"bytes\":%u,\"accepted\":%d,\"sha256\":\"%s\",\"internal_free\":%u,\"psram_free\":%u}\n",(unsigned long long)group,(unsigned)packet.payload_len,accepted==0,hex,(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),(unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    group++;if(!accepted)sent++;
   }
  }
  vTaskDelay(pdMS_TO_TICKS(5));
 }
 quic_client_cleanup();esp_task_wdt_delete(NULL);
}

#endif
