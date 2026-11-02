#include "moq_codec_worker.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_task_wdt.h"
#include "sdkconfig.h"

static QueueHandle_t requests,results;
static SemaphoreHandle_t stopped;
static bool outstanding;

static uint32_t runtime(void) {
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
    TaskStatus_t status;
    vTaskGetInfo(NULL,&status,pdFALSE,eInvalid);
    return (uint32_t)status.ulRunTimeCounter;
#else
    return 0;
#endif
}
moq_codec_result moq_codec_execute(const moq_codec_request *q) {
    moq_codec_result r={0};uint32_t before=runtime();
    r.ok=moq_transcode(q->input,q->length,q->level,q->output,q->capacity,&r.metrics);
    r.cpu_us=runtime()-before;
    return r;
}
static void worker(void *unused) {
    (void)unused;moq_codec_request q;
    for(;;){
        xQueueReceive(requests,&q,portMAX_DELAY);
        if(!q.input)break;
        moq_codec_result r=moq_codec_execute(&q);
        xQueueSend(results,&r,portMAX_DELAY);
    }
    xSemaphoreGive(stopped);
    vTaskDelete(NULL);
}
bool moq_codec_worker_start(void) {
    if(requests)return false;
    requests=xQueueCreate(1,sizeof(moq_codec_request));
    results=xQueueCreate(1,sizeof(moq_codec_result));
    stopped=xSemaphoreCreateBinary();outstanding=false;
    if(requests&&results&&stopped&&
       xTaskCreatePinnedToCore(worker,"moq_codec",8192,NULL,1,NULL,0)==pdPASS)return true;
    if(requests)vQueueDelete(requests);
    if(results)vQueueDelete(results);
    if(stopped)vSemaphoreDelete(stopped);
    requests=results=NULL;stopped=NULL;return false;
}
bool moq_codec_worker_submit(const moq_codec_request *q) {
    if(!requests||outstanding||!q||!q->input)return false;
    if(xQueueSend(requests,q,0)!=pdTRUE)return false;
    outstanding=true;return true;
}
bool moq_codec_worker_busy(void) {return outstanding;}
bool moq_codec_worker_take(moq_codec_result *r) {
    if(!outstanding||!r||xQueueReceive(results,r,0)!=pdTRUE)return false;
    outstanding=false;return true;
}
void moq_codec_worker_stop(void) {
    if(!requests)return;
    moq_codec_result r;
    while(outstanding&&!moq_codec_worker_take(&r)){
        esp_task_wdt_reset();vTaskDelay(pdMS_TO_TICKS(20));
    }
    moq_codec_request end={0};xQueueSend(requests,&end,portMAX_DELAY);
    while(xSemaphoreTake(stopped,pdMS_TO_TICKS(20))!=pdTRUE)esp_task_wdt_reset();
    vQueueDelete(requests);vQueueDelete(results);vSemaphoreDelete(stopped);
    requests=results=NULL;stopped=NULL;
}
