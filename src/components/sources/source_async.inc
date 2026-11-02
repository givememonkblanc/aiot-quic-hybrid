#include "moq_audit.h"
#if AIOT_MOQT_AUDIT
#define printf moq_audit_printf
#endif
#include "mobility_scheduler.h"
#ifndef AIOT_PROFILE_AI_CORE
#define AIOT_PROFILE_AI_CORE 0
#endif
#ifndef AIOT_PROFILE_AI_PRIORITY
#define AIOT_PROFILE_AI_PRIORITY 0
#endif
/* One pending and one active JPEG; inference never queues an unbounded backlog. */
static SemaphoreHandle_t s_ai_lock;
static StaticSemaphore_t s_ai_lock_storage;
static TaskHandle_t s_ai_task;
static StaticTask_t s_ai_task_storage;
static StackType_t s_ai_stack[12288 / sizeof(StackType_t)];
static uint8_t *s_ai_pending, *s_ai_active;
static size_t s_ai_pending_len;
static uint32_t s_ai_pending_seq, s_ai_pending_ts, s_ai_result_ts;
static aiot_ai_result_t s_ai_result;
static bool s_ai_result_valid;
static uint8_t *s_ai_evidence;
static size_t s_ai_evidence_len;
static uint32_t s_ai_evidence_ts, s_ai_completed;
static uint32_t s_ai_evidence_seq;
#if AIOT_MOQT_POLICY_EXPERIMENT
#ifndef AIOT_MOQT_RESULT_QUEUE
#define AIOT_MOQT_RESULT_QUEUE 32
#endif
#define MOQ_AI_EVENTS AIOT_MOQT_RESULT_QUEUE
static struct {aiot_ai_result_t result;uint32_t ts,count_new,count_total;} s_ai_events[MOQ_AI_EVENTS];
static unsigned s_ai_event_head,s_ai_event_count;
/* Called for every result when it is produced, whether or not the queue has room for it:
 * what the application counts must not depend on what is transmitted. */
static source_result_hook s_result_hook;
void source_manager_set_result_hook(source_result_hook hook){s_result_hook=hook;}
static source_split_decide s_split_decide;
static source_split_deliver s_split_deliver;
void source_manager_set_split_hooks(source_split_decide decide,source_split_deliver deliver){s_split_deliver=deliver;s_split_decide=decide;}
bool source_manager_peek_ai_counts(uint32_t seq,uint32_t *count_new,uint32_t *count_total){
 if(!s_ai_lock)return false;
 xSemaphoreTake(s_ai_lock,portMAX_DELAY);
 bool ok=s_ai_event_count&&s_ai_events[s_ai_event_head].result.source_sequence==seq;
 if(ok){*count_new=s_ai_events[s_ai_event_head].count_new;*count_total=s_ai_events[s_ai_event_head].count_total;}
 xSemaphoreGive(s_ai_lock);return ok;
}
bool source_manager_peek_ai_event(aiot_ai_result_t *r,uint32_t *ts){
 if(!s_ai_lock||xSemaphoreTake(s_ai_lock,0)!=pdTRUE)return false;
 bool ok=s_ai_event_count>0;if(ok){*r=s_ai_events[s_ai_event_head].result;*ts=s_ai_events[s_ai_event_head].ts;}
 xSemaphoreGive(s_ai_lock);return ok;
}
void source_manager_audit_pending(void){
 if(!s_ai_lock)return;
 xSemaphoreTake(s_ai_lock,portMAX_DELAY);
 for(unsigned n=0;n<s_ai_event_count;n++){
  unsigned i=(s_ai_event_head+n)%MOQ_AI_EVENTS;
  moq_audit_printf("[MOQ_PENDING] {\"alias\":2,\"group\":%lu}\n",(unsigned long)s_ai_events[i].result.source_sequence);
 }
 xSemaphoreGive(s_ai_lock);
}
unsigned source_manager_ai_event_count(void){
 if(!s_ai_lock)return 0;
 xSemaphoreTake(s_ai_lock,portMAX_DELAY);unsigned n=s_ai_event_count;xSemaphoreGive(s_ai_lock);return n;
}
unsigned source_manager_ai_event_capacity(void){return MOQ_AI_EVENTS;}
bool source_manager_pop_ai_event(uint32_t seq){
 if(!s_ai_lock)return false;
 xSemaphoreTake(s_ai_lock,portMAX_DELAY);
 bool ok=s_ai_event_count&&s_ai_events[s_ai_event_head].result.source_sequence==seq;
 if(ok){s_ai_event_head=(s_ai_event_head+1)%MOQ_AI_EVENTS;s_ai_event_count--;}
 xSemaphoreGive(s_ai_lock);return ok;
}
#endif
static bool s_ai_busy;
bool source_manager_ai_idle(void){
 if(!s_ai_lock)return true;
 xSemaphoreTake(s_ai_lock,portMAX_DELAY);bool idle=!s_ai_busy&&!s_ai_pending_len;xSemaphoreGive(s_ai_lock);return idle;
}
static bool s_evidence_enabled;
void source_manager_evidence_enable(bool enabled) { __atomic_store_n(&s_evidence_enabled, enabled, __ATOMIC_RELAXED); }
uint32_t source_manager_ai_completed(void) { return __atomic_load_n(&s_ai_completed, __ATOMIC_RELAXED); }
/* The earliest a result can come: an inference started at s_ai_start32 (low 32 bits of the
 * microsecond clock) ends no sooner than the shortest of the recent inferences (wall time from
 * its start), less 5%. Until four inferences are known nothing is promised. */
#define AI_WALL_RING 16
static volatile uint32_t s_ai_start32, s_ai_floor_us;
static uint32_t s_ai_wall[AI_WALL_RING];
static unsigned s_ai_wall_n;
/* Phase of the detector: time since the running (or last) inference started, the shortest recent
 * inference (floor, 0 while unknown), and the number of inferences started so far. */
static volatile uint32_t s_ai_started;
void source_manager_ai_phase(uint32_t *elapsed_us, uint32_t *floor_us, uint32_t *started)
{
    *elapsed_us = (uint32_t)source_manager_now_us() - s_ai_start32;
    *floor_us = s_ai_floor_us;
    *started = s_ai_started;
}
uint32_t source_manager_alert_safe_us(void)
{
    uint32_t floor_us = s_ai_floor_us;
    if (!floor_us) return 0;
    if (!s_ai_busy) return floor_us;
    uint32_t elapsed = (uint32_t)source_manager_now_us() - s_ai_start32;
    return elapsed < floor_us ? floor_us - elapsed : 0;
}
size_t source_manager_copy_evidence(uint32_t ts, uint8_t *dst, size_t capacity) {
    if (!s_ai_lock) return 0;
    xSemaphoreTake(s_ai_lock, portMAX_DELAY);
    size_t len = ts == s_ai_evidence_ts ? s_ai_evidence_len : 0;
    if (len > capacity) len = 0;
    if (len) memcpy(dst, s_ai_evidence, len);
    xSemaphoreGive(s_ai_lock); return len;
}
size_t source_manager_copy_evidence_for_sequence(uint32_t seq,uint32_t ts,uint8_t *dst,size_t capacity) {
    if (!s_ai_lock || !dst) return 0;
    xSemaphoreTake(s_ai_lock, portMAX_DELAY);
    size_t len = seq == s_ai_evidence_seq && ts == s_ai_evidence_ts ? s_ai_evidence_len : 0;
    if (len > capacity) len = 0;
    if (len) memcpy(dst,s_ai_evidence,len);
    xSemaphoreGive(s_ai_lock);return len;
}

static void source_ai_worker(void *arg)
{
    source_manager_t *manager = arg;
    source_manager_t work = {0};
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (uxTaskPriorityGet(NULL) != mobility_ai_priority()) vTaskPrioritySet(NULL,mobility_ai_priority());
        xSemaphoreTake(s_ai_lock, portMAX_DELAY);
        size_t len = s_ai_pending_len;
        uint32_t seq = s_ai_pending_seq, ts = s_ai_pending_ts;
        if (len) memcpy(s_ai_active, s_ai_pending, len);
        s_ai_pending_len = 0;
        s_ai_busy=len!=0;
        xSemaphoreGive(s_ai_lock);
        if (!len) continue;
        s_ai_start32 = (uint32_t)source_manager_now_us();
        s_ai_started++;
        printf("[AI_START] {\"seq\":%lu,\"capture_ms\":%lu,\"start_us\":%llu,\"priority\":%u}\n", (unsigned long)seq, (unsigned long)ts, (unsigned long long)source_manager_now_us(), (unsigned)uxTaskPriorityGet(NULL));
        if (s_split_decide && s_split_deliver && s_split_decide(seq, ts)) {
            /* the first stage here, the model at the helper */
            aiot_ai_result_t decoded;
            uint64_t d0 = source_manager_now_us();
            bool ok = ai_inference_decode_only(&g_inference, s_ai_active, (uint32_t)len, seq, &decoded);
            uint64_t d1 = source_manager_now_us();
            if (ok) s_split_deliver(seq, ts, decoded.input_width, decoded.input_height);
            printf("[AI_SPLIT] {\"seq\":%lu,\"capture_ms\":%lu,\"decode_us\":%llu,\"deliver_us\":%llu,\"ok\":%d}\n",
                (unsigned long)seq, (unsigned long)ts, (unsigned long long)(d1 - d0),
                (unsigned long long)(source_manager_now_us() - d1), (int)ok);
            xSemaphoreTake(s_ai_lock, portMAX_DELAY);
            s_ai_busy = false;
            xSemaphoreGive(s_ai_lock);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        TaskStatus_t ai_before, ai_after;
        vTaskGetInfo(NULL, &ai_before, pdFALSE, eInvalid);
        source_manager_run_inference(&work, s_ai_active, (uint32_t)len, seq);
        vTaskGetInfo(NULL, &ai_after, pdFALSE, eInvalid);
        {
            uint32_t wall = (uint32_t)source_manager_now_us() - s_ai_start32, low = UINT32_MAX;
            s_ai_wall[s_ai_wall_n++ % AI_WALL_RING] = wall;
            unsigned n = s_ai_wall_n < AI_WALL_RING ? s_ai_wall_n : AI_WALL_RING;
            for (unsigned i = 0; i < n; i++) if (s_ai_wall[i] < low) low = s_ai_wall[i];
            s_ai_floor_us = n >= 4 ? low / 100 * 95 : 0;
        }
        printf("[AI_CPU] {\"seq\":%lu,\"cpu_us\":%lu,\"core\":%d}\n", (unsigned long)seq,
            (unsigned long)(uint32_t)(ai_after.ulRunTimeCounter-ai_before.ulRunTimeCounter), AIOT_PROFILE_AI_CORE);
        xSemaphoreTake(s_ai_lock, portMAX_DELAY);
        __atomic_fetch_add(&s_ai_completed, 1, __ATOMIC_RELAXED);
        s_ai_evidence_len = 0;
        if (__atomic_load_n(&s_evidence_enabled, __ATOMIC_RELAXED) && work.last_result.detection_count) {
            #ifdef AIOT_ESPDET_RUNTIME
            s_ai_evidence_len = ai_inference_copy_roi(s_ai_evidence, SOURCE_IMAGE_CAPACITY);
#else
            memcpy(s_ai_evidence, s_ai_active, len);
            s_ai_evidence_len = len;
#endif
            s_ai_evidence_ts = ts;
            s_ai_evidence_seq = seq;
        }
        printf("[AI_PROFILE] {\"ts_ms\":%lu,\"seq\":%lu,\"duration_us\":%lu,\"detections\":%u}\n",
            (unsigned long)ts, (unsigned long)seq, (unsigned long)work.last_inference_us,
            (unsigned)work.last_result.detection_count);
#if AIOT_MOQT_POLICY_EXPERIMENT
        if(work.last_ran){
            uint32_t count_new=0,count_total=0;
            if(s_result_hook)s_result_hook(&work.last_result,ts,s_ai_evidence_len?s_ai_evidence:NULL,s_ai_evidence_len,&count_new,&count_total);
            bool admitted=s_ai_event_count<MOQ_AI_EVENTS;
            if(admitted){unsigned i=(s_ai_event_head+s_ai_event_count)%MOQ_AI_EVENTS;s_ai_events[i].result=work.last_result;s_ai_events[i].ts=ts;
                s_ai_events[i].count_new=count_new;s_ai_events[i].count_total=count_total;s_ai_event_count++;}
            printf("[MOQ_AI_PRODUCED] {\"seq\":%lu,\"source_ms\":%lu,\"enqueued\":%d,\"queue_count\":%u}\n",(unsigned long)seq,(unsigned long)ts,admitted,s_ai_event_count);
        }
#endif
        s_ai_result = work.last_result;
        s_ai_result_ts = ts;
        s_ai_result_valid = work.last_ran;
        manager->last_inference_us = work.last_inference_us;
        manager->last_importance = work.last_importance;
        manager->last_ran = work.last_ran;
        s_ai_busy=false;
#if AIOT_MOQT_AUDIT
        printf("[AI_OUTCOME] {\"seq\":%lu,\"success\":%d,\"finish_us\":%llu}\n",(unsigned long)seq,work.last_ran,(unsigned long long)source_manager_now_us());
#endif
        xSemaphoreGive(s_ai_lock);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static bool source_ai_start(source_manager_t *manager)
{
    s_ai_lock = xSemaphoreCreateMutexStatic(&s_ai_lock_storage);
    s_ai_pending = heap_caps_malloc(SOURCE_IMAGE_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_ai_active = heap_caps_malloc(SOURCE_IMAGE_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_ai_evidence = heap_caps_malloc(SOURCE_IMAGE_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ai_pending || !s_ai_active || !s_ai_evidence) return false;
    /* Background inference shares priority with IDLE0 by default so continuous capture
     * plus inference cannot starve the idle watchdog between their work bursts. At that
     * priority the idle task takes every other time slice, half of the core. On core 1,
     * away from the network, priority 1 gives inference the whole core; the idle task
     * runs in the pause after each inference, well within the watchdog timeout. */
    s_ai_task = xTaskCreateStaticPinnedToCore(source_ai_worker, "camera_ai", sizeof(s_ai_stack),
        manager, tskIDLE_PRIORITY + AIOT_PROFILE_AI_PRIORITY, s_ai_stack, &s_ai_task_storage, AIOT_PROFILE_AI_CORE);
    return s_ai_task != NULL;
}

static void source_ai_offer(const uint8_t *jpeg, size_t len, uint32_t seq, uint32_t ts)
{
    static uint64_t last_offer;
    uint64_t now = source_manager_now_us();
    /* Inference ceiling 5Hz; preview capture runs independently. */
    if (!s_ai_task || now - last_offer < 200000 || xSemaphoreTake(s_ai_lock, 0) != pdTRUE) return;
    if (mobility_replay_epoch()) printf("[AI_OFFER] {\"seq\":%lu,\"ts_ms\":%lu,\"overwrite\":%d,\"old_seq\":%lu}\n",
        (unsigned long)seq,(unsigned long)ts,s_ai_pending_len != 0,(unsigned long)s_ai_pending_seq);
    memcpy(s_ai_pending, jpeg, len);
    s_ai_pending_len = len; s_ai_pending_seq = seq; s_ai_pending_ts = ts;
    last_offer = now;
    xSemaphoreGive(s_ai_lock);
    xTaskNotifyGive(s_ai_task);
}

bool source_manager_get_async_ai(aiot_ai_result_t *result, uint32_t *frame_ts)
{
    if (!s_ai_lock || xSemaphoreTake(s_ai_lock, 0) != pdTRUE) return false;
    bool valid = s_ai_result_valid;
    if (valid) { *result = s_ai_result; *frame_ts = s_ai_result_ts; }
    xSemaphoreGive(s_ai_lock);
    return valid;
}

void source_manager_submit_camera_ai(const data_packet_t *packet)
{
    if (packet && packet->type == DATA_TYPE_IMAGE && packet->payload_len <= SOURCE_IMAGE_CAPACITY)
        source_ai_offer(packet->payload, packet->payload_len, packet->sequence, packet->timestamp_us / 1000);
}

#if AIOT_MOQT_AUDIT
#undef printf
#endif
