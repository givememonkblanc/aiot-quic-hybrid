#define _POSIX_C_SOURCE 200809L
#include "moq_codec_worker.h"
#include "freertos/queue.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <stdio.h>
struct test_queue {pthread_mutex_t lock;pthread_cond_t changed;size_t size;bool full;unsigned char data[256];};
static int allocations,fail_create,create_calls;
static bool fail_task;
static pthread_t thread;
static void (*entry)(void*);
static atomic_bool executing,allow_complete;
QueueHandle_t xQueueCreate(unsigned count,size_t size){
 assert(count==1&&size<=256);
 if(++create_calls==fail_create)return NULL;
 QueueHandle_t q=calloc(1,sizeof(*q));assert(q);q->size=size;
 pthread_mutex_init(&q->lock,NULL);pthread_cond_init(&q->changed,NULL);allocations++;return q;
}
BaseType_t xQueueSend(QueueHandle_t q,const void *data,TickType_t wait){
 pthread_mutex_lock(&q->lock);
 while(q->full){if(!wait){pthread_mutex_unlock(&q->lock);return pdFALSE;}pthread_cond_wait(&q->changed,&q->lock);}
 if(q->size)memcpy(q->data,data,q->size);
 q->full=true;pthread_cond_broadcast(&q->changed);pthread_mutex_unlock(&q->lock);return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q,void *data,TickType_t wait){
 pthread_mutex_lock(&q->lock);
 while(!q->full){if(!wait){pthread_mutex_unlock(&q->lock);return pdFALSE;}pthread_cond_wait(&q->changed,&q->lock);}
 if(q->size)memcpy(data,q->data,q->size);
 q->full=false;pthread_cond_broadcast(&q->changed);pthread_mutex_unlock(&q->lock);return pdTRUE;
}
void vQueueDelete(QueueHandle_t q){pthread_cond_destroy(&q->changed);pthread_mutex_destroy(&q->lock);free(q);allocations--;}
static void *launch(void *unused){entry(unused);return NULL;}
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void*),const char *name,unsigned stack,void *arg,unsigned priority,void *handle,unsigned core){
 (void)name;(void)stack;(void)arg;(void)priority;(void)handle;(void)core;
 if(fail_task)return pdFALSE;
 entry=fn;return pthread_create(&thread,NULL,launch,NULL)==0?pdPASS:pdFALSE;
}
void vTaskDelay(TickType_t ms){struct timespec t={.tv_sec=ms/1000,.tv_nsec=(ms%1000)*1000000L};nanosleep(&t,NULL);}
void vTaskDelete(void *unused){(void)unused;pthread_exit(NULL);}
bool moq_transcode(const uint8_t *in,size_t n,unsigned level,uint8_t *out,size_t cap,moq_representation_metrics *m){
 atomic_store(&executing,true);
 while(!atomic_load(&allow_complete))vTaskDelay(1);
 memset(m,0,sizeof(*m));bool ok=level<=2&&cap>=n;
 if(ok){memcpy(out,in,n);m->bytes=n;}
 atomic_store(&executing,false);return ok;
}
static void join_stop(void){moq_codec_worker_stop();pthread_join(thread,NULL);assert(!allocations);}
static void *release_encoder(void *unused){(void)unused;vTaskDelay(30);atomic_store(&allow_complete,true);return NULL;}
int main(void){
 for(int fail=1;fail<=3;fail++){
  fail_create=fail;create_calls=0;assert(!moq_codec_worker_start());assert(!allocations);moq_codec_worker_stop();
 }
 fail_create=0;fail_task=true;assert(!moq_codec_worker_start());assert(!allocations);fail_task=false;
 uint8_t input[]={1,2,3,4},output[4]={0};moq_codec_request q={input,output,4,4,1};moq_codec_result r;
 assert(!moq_codec_worker_submit(&q));assert(!moq_codec_worker_take(&r));
 assert(moq_codec_worker_start());assert(!moq_codec_worker_start());
 atomic_store(&allow_complete,false);assert(moq_codec_worker_submit(&q));assert(moq_codec_worker_busy());
 while(!atomic_load(&executing))vTaskDelay(1);
 assert(!moq_codec_worker_submit(&q));assert(!moq_codec_worker_take(&r));
 atomic_store(&allow_complete,true);
 while(!moq_codec_worker_take(&r))vTaskDelay(1);
 assert(r.ok&&r.metrics.bytes==4&&!memcmp(input,output,4));assert(!moq_codec_worker_busy());
 q.level=3;assert(moq_codec_worker_submit(&q));while(!moq_codec_worker_take(&r))vTaskDelay(1);assert(!r.ok);
 q.level=1;atomic_store(&allow_complete,false);assert(moq_codec_worker_submit(&q));
 while(!atomic_load(&executing))vTaskDelay(1);
 pthread_t release;pthread_create(&release,NULL,release_encoder,NULL);join_stop();pthread_join(release,NULL);
 assert(!atomic_load(&executing));assert(!moq_codec_worker_busy());
 assert(moq_codec_worker_start());join_stop();moq_codec_worker_stop();
 puts("PASS: single-flight ownership, partial init failure, result failure, stop joins live codec, restart");
}
