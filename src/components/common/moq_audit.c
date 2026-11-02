#include "moq_audit.h"
#include <stdio.h>
#include <stdarg.h>
#if AIOT_MOQT_AUDIT
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#define CAPACITY (384U*1024U)
#define PAGE 256U
static uint8_t *journal;
static StaticSemaphore_t lock_storage;
static SemaphoreHandle_t lock;
static size_t used;
static unsigned records,overflow,truncated,late;
static uint64_t append_wall_us;
static bool frozen;
static uint32_t crc32(const uint8_t *p,size_t n){uint32_t c=~0U;while(n--){c^=*p++;for(unsigned i=0;i<8;i++)c=(c>>1)^(0xedb88320U&-(c&1));}return ~c;}
bool moq_audit_init(void){
 if(journal)return false;
 journal=heap_caps_malloc(CAPACITY,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
 lock=xSemaphoreCreateMutexStatic(&lock_storage);
 return journal&&lock;
}
int moq_audit_printf(const char *fmt,...){
 va_list ap;va_start(ap,fmt);
 bool selected=!strncmp(fmt,"[MOQ_",5)||!strncmp(fmt,"[AI_",4)||!strncmp(fmt,"[REPLAY]",8)||!strncmp(fmt,"[ESPDET_PHASE]",14);
 if(!selected||!journal){int n=vprintf(fmt,ap);va_end(ap);return n;}
 uint64_t t=esp_timer_get_time();char line[512];int n=vsnprintf(line,sizeof(line),fmt,ap);va_end(ap);
 xSemaphoreTake(lock,portMAX_DELAY);
 if(frozen)late++;
 else if(n<0||(size_t)n>=sizeof(line))truncated++;
 else if(used+(size_t)n+2>CAPACITY)overflow++;
 else{journal[used++]=(uint8_t)n;journal[used++]=(uint8_t)((unsigned)n>>8);memcpy(journal+used,line,n);used+=n;records++;}
 append_wall_us+=esp_timer_get_time()-t;xSemaphoreGive(lock);
 if(!strncmp(fmt,"[MOQ_ERROR]",11))fputs(line,stdout);
 return n;
}
/* Export only after producers quiesce. Retain journal until reset. Each page is
 * independently numbered/CRC'd and retried; missing terminal receipt is failure. */
static bool ack(int wanted){
 char line[48];unsigned n=0;int64_t until=esp_timer_get_time()+1500000;
 while(esp_timer_get_time()<until){
  esp_task_wdt_reset();char c;
  if(usb_serial_jtag_read_bytes(&c,1,pdMS_TO_TICKS(10))==1){
   if(c=='\n'){line[n]=0;int got;char extra;if(sscanf(line,"A %d %c",&got,&extra)==1&&got==wanted)return true;n=0;}
   else if(n<sizeof(line)-1)line[n++]=c;else n=0;
  }
 }
 return false;
}
bool moq_audit_export(void){
 if(!journal)return false;
 xSemaphoreTake(lock,portMAX_DELAY);frozen=true;xSemaphoreGive(lock);
 usb_serial_jtag_driver_config_t usb={.tx_buffer_size=2048,.rx_buffer_size=512};
 if(usb_serial_jtag_driver_install(&usb)!=ESP_OK)return false;
 usb_serial_jtag_vfs_use_driver();
 uint32_t crc=crc32(journal,used);unsigned pages=(used+PAGE-1)/PAGE;bool ok=false;
 for(unsigned attempt=0;attempt<5;attempt++){
  printf("[AUDIT_BEGIN] {\"schema\":1,\"capacity\":%u,\"bytes\":%u,\"records\":%u,\"pages\":%u,\"crc\":%lu,\"overflow\":%u,\"truncated\":%u,\"late\":%u,\"append_wall_us\":%llu}\n",CAPACITY,(unsigned)used,records,pages,(unsigned long)crc,overflow,truncated,late,(unsigned long long)append_wall_us);fflush(stdout);
  if(ack(-1)){ok=true;break;}
 }
 for(unsigned i=0;ok&&i<pages;i++){
  size_t offset=i*PAGE,n=used-offset;if(n>PAGE)n=PAGE;char hex[PAGE*2+1];
  for(size_t j=0;j<n;j++)snprintf(hex+2*j,3,"%02x",journal[offset+j]);
  ok=false;
  for(unsigned attempt=0;attempt<5;attempt++){
   printf("[AUDIT_CHUNK] {\"i\":%u,\"o\":%u,\"n\":%u,\"crc\":%lu,\"hex\":\"%s\"}\n",i,(unsigned)offset,(unsigned)n,(unsigned long)crc32(journal+offset,n),hex);fflush(stdout);
   if(ack((int)i)){ok=true;break;}
  }
 }
 bool confirmed=false;
 for(unsigned attempt=0;ok&&attempt<5;attempt++){
  printf("[AUDIT_END] {\"bytes\":%u,\"crc\":%lu,\"late\":%u}\n",(unsigned)used,(unsigned long)crc,late);fflush(stdout);
  if(ack(-2)){confirmed=true;break;}
 }
 usb_serial_jtag_vfs_use_nonblocking();usb_serial_jtag_driver_uninstall();
 if(!confirmed)printf("[AUDIT_ERROR] export timeout\n");
 return confirmed&&!overflow&&!truncated&&!late;
}
#else
bool moq_audit_init(void){return true;}
int moq_audit_printf(const char *fmt,...){va_list ap;va_start(ap,fmt);int n=vprintf(fmt,ap);va_end(ap);return n;}
bool moq_audit_export(void){return true;}
#endif
