/* Shared counters are diagnostic only; per-stage warm-up excludes in-flight work. */
static uint32_t s_profile_frames, s_profile_metadata_ok, s_profile_evidence_ok;
static uint8_t *s_profile_evidence_copy;
/* Heap sampler runs on the existing esp_timer task, including during AI/OTA. */
typedef struct {
    uint64_t uptime_us;
    uint32_t seq;
    multi_heap_info_t internal, psram, dma;
} app_memory_sample_t;
static app_memory_sample_t s_memory;
static portMUX_TYPE s_memory_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t s_memory_timer;

static void app_memory_sample(void *arg)
{
    (void)arg;
    app_memory_sample_t sample = {.uptime_us = esp_timer_get_time()};
    heap_caps_get_info(&sample.internal, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    heap_caps_get_info(&sample.psram, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    heap_caps_get_info(&sample.dma, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    portENTER_CRITICAL(&s_memory_lock);
    sample.seq = s_memory.seq + 1;
    s_memory = sample;
    portEXIT_CRITICAL(&s_memory_lock);
    if (mobility_profile_get()) {
        static TaskStatus_t tasks[48];
        uint32_t total = 0, idle0 = 0, idle1 = 0;
        UBaseType_t count = uxTaskGetSystemState(tasks, 48, &total);
        for (UBaseType_t i = 0; i < count; i++) {
            if (!strcmp(tasks[i].pcTaskName, "IDLE0")) idle0 = tasks[i].ulRunTimeCounter;
            if (!strcmp(tasks[i].pcTaskName, "IDLE1")) idle1 = tasks[i].ulRunTimeCounter;
        }
        if (sample.seq % 5 == 0) {
            for (UBaseType_t i = 0; i < count; i++)
                printf("[TASK_PROFILE] {\"uptime_us\":%llu,\"id\":%u,\"name\":\"%s\",\"runtime_us\":%lu,\"core\":%d,\"priority\":%u,\"stack_free\":%u}\n",
                    (unsigned long long)sample.uptime_us, (unsigned)tasks[i].xTaskNumber,
                    tasks[i].pcTaskName, (unsigned long)tasks[i].ulRunTimeCounter,
                    (int)xTaskGetCoreID(tasks[i].xHandle), (unsigned)tasks[i].uxCurrentPriority,
                    (unsigned)tasks[i].usStackHighWaterMark);
        }
        printf("[PROFILE] {\"profile\":%d,\"uptime_us\":%llu,\"internal_free\":%u,\"internal_used\":%u,\"internal_largest\":%u,\"psram_free\":%u,\"psram_used\":%u,\"psram_largest\":%u,\"queue_bytes\":%lu,\"frames\":%lu,\"ai_completed\":%lu,\"metadata_ok\":%lu,\"evidence_ok\":%lu,\"tasks\":%u,\"idle0_us\":%lu,\"idle1_us\":%lu}\n",
            mobility_profile_get(), (unsigned long long)sample.uptime_us,
            (unsigned)sample.internal.total_free_bytes, (unsigned)sample.internal.total_allocated_bytes,
            (unsigned)sample.internal.largest_free_block, (unsigned)sample.psram.total_free_bytes,
            (unsigned)sample.psram.total_allocated_bytes, (unsigned)sample.psram.largest_free_block,
            (unsigned long)__atomic_load_n(&g_networkContext.route_payload_bytes, __ATOMIC_RELAXED),
            (unsigned long)s_profile_frames, (unsigned long)source_manager_ai_completed(),
            (unsigned long)s_profile_metadata_ok, (unsigned long)s_profile_evidence_ok,
            (unsigned)count, (unsigned long)idle0, (unsigned long)idle1);
    }
}

static void app_memory_start(void)
{
    const esp_timer_create_args_t args = {
        .callback = app_memory_sample, .name = "ram_sample",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_memory_timer));
    app_memory_sample(NULL);
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_memory_timer, 1000000));
}

static void app_memory_publish(const char *state, uint32_t frames, uint32_t ai_us)
{
    app_memory_sample_t s;
    portENTER_CRITICAL(&s_memory_lock);
    s = s_memory;
    portEXIT_CRITICAL(&s_memory_lock);
    /* Static storage avoids a large extra main-task stack allocation. */
    static char payload[896];
    uint32_t rtt = 0, cwnd = 0, inflight = 0;
    quic_client_get_metrics(&rtt, &cwnd, &inflight);
    int n = snprintf(payload, sizeof(payload),
        "{\"kind\":\"ram\",\"schema\":1,\"seq\":%lu,\"uptime_us\":%llu,"
        "\"internal_free\":%u,\"internal_total\":%u,\"internal_min\":%u,\"internal_largest\":%u,"
        "\"psram_free\":%u,\"psram_total\":%u,\"psram_min\":%u,\"psram_largest\":%u,"
        "\"dma_free\":%u,\"dma_min\":%u,\"dma_largest\":%u,"
        "\"main_stack_min\":%u,\"state\":\"%s\",\"frames\":%lu,\"ai_us\":%lu,"
        "\"tx_queue\":%u,\"tx_queue_max\":%lu,\"tx_failed\":%lu,\"tx_expired\":%lu,"
        "\"queue_bytes\":%lu,\"queue_peak\":%lu,\"queue_budget\":%u,\"target_fps\":%u,\"mode\":%d,"
        "\"video_policy\":%u,\"quic_rtt_ms\":%lu,\"quic_cwnd\":%lu,\"quic_inflight\":%lu}",
        (unsigned long)s.seq, (unsigned long long)s.uptime_us,
        (unsigned)s.internal.total_free_bytes,
        (unsigned)(s.internal.total_free_bytes + s.internal.total_allocated_bytes),
        (unsigned)s.internal.minimum_free_bytes, (unsigned)s.internal.largest_free_block,
        (unsigned)s.psram.total_free_bytes,
        (unsigned)(s.psram.total_free_bytes + s.psram.total_allocated_bytes),
        (unsigned)s.psram.minimum_free_bytes, (unsigned)s.psram.largest_free_block,
        (unsigned)s.dma.total_free_bytes, (unsigned)s.dma.minimum_free_bytes,
        (unsigned)s.dma.largest_free_block, (unsigned)uxTaskGetStackHighWaterMark(NULL),
        state, (unsigned long)frames, (unsigned long)ai_us,
        (unsigned)uxQueueMessagesWaiting(g_networkContext.route_queue),
        (unsigned long)g_networkContext.route_queue_max_depth,
        (unsigned long)g_networkContext.datagram_send_fail,
        (unsigned long)g_networkContext.datagram_expired,
        (unsigned long)__atomic_load_n(&g_networkContext.route_payload_bytes, __ATOMIC_RELAXED),
        (unsigned long)g_networkContext.route_payload_peak,
        mobility_video_budget(), mobility_video_fps(), mobility_mode_get(),
        video_policy_get(), (unsigned long)rtt, (unsigned long)cwnd, (unsigned long)inflight);
    if (n <= 0 || n >= sizeof(payload)) return;
    MQTTPublishInfo_t info = {.qos = MQTTQoS0,
        .pTopicName = "aiot/diagnostics/ram", .topicNameLength = sizeof("aiot/diagnostics/ram") - 1,
        .pPayload = payload, .payloadLength = n};
    (void)MQTT_Publish(&g_mqttContext, &info, 0);
}
