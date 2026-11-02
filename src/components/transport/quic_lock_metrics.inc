/* All connection-lock users are counted, including read/process/MQTT paths. */
#if AIOT_QUIC_BATCH_EXPERIMENT
extern uint32_t aiot_task_runtime_now(void);
static uint64_t s_other_quic_cpu_us;
static portMUX_TYPE s_lock_stats_mux = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_lock_attempts, s_lock_acquired, s_lock_wait_us, s_lock_hold_us, s_lock_hold_max;
static int64_t s_lock_start;
static BaseType_t quic_lock_take(TickType_t ticks)
{
    int64_t start = esp_timer_get_time();
    BaseType_t result = xSemaphoreTake(quic_mutex, ticks);
    int64_t end = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock_stats_mux);
    ++s_lock_attempts; s_lock_acquired += result == pdTRUE; s_lock_wait_us += end-start;
    portEXIT_CRITICAL(&s_lock_stats_mux);
    if (result == pdTRUE) s_lock_start = end;
    return result;
}
static void quic_lock_give(void)
{
    uint64_t hold = esp_timer_get_time()-s_lock_start;
    portENTER_CRITICAL(&s_lock_stats_mux);
    s_lock_hold_us += hold;
    if (hold>s_lock_hold_max) s_lock_hold_max=hold;
    portEXIT_CRITICAL(&s_lock_stats_mux);
    xSemaphoreGive(quic_mutex);
}
#else
#define quic_lock_take(ticks) xSemaphoreTake(quic_mutex, (ticks))
#define quic_lock_give() xSemaphoreGive(quic_mutex)
#endif
