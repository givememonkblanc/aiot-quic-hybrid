/* Included in app_main.c to reuse the existing MQTT control-channel helpers. */
#include "mobility_scheduler.h"
#if AIOT_MOBILITY_CAMERA
#include "app_memory.inc"
#endif

#if AIOT_MOBILITY_CAMERA && (!APP_PRODUCT_MODE || !APP_STREAM_ENABLE)
#error "Mobility camera requires the product streaming helpers"
#endif

static void app_mobility_main(void)
{
    static mobility_scheduler_t scheduler;
    static ServerInfo_t server;
    static MQTTQUICConfig_t config = {.timeoutMs = 1000, .nonBlocking = true};
    static char port[16];
    esp_task_wdt_add(NULL);
    esp_log_level_set("QUIC", ESP_LOG_WARN);
    esp_log_level_set("DEBUG", ESP_LOG_WARN);
    esp_log_level_set("NGTCP2-RAW", ESP_LOG_WARN);
    esp_log_level_set("NGTCP2-DISCARD", ESP_LOG_WARN);
    ota_log_status();
    ESP_LOGW("MOBILITY", "event=boot reset_reason=%d", (int)esp_reset_reason());
    app_cfg_load(&g_cfg);
    app_cfg_print(&g_cfg);
    provisioning_console_start(&g_cfg);
    strlcpy(g_client_id, g_cfg.device_id, sizeof(g_client_id));
    g_mqttConnectInfo.pClientIdentifier = g_client_id;
    g_mqttConnectInfo.clientIdentifierLength = strlen(g_client_id);
    g_mqttConnectInfo.keepAliveSeconds = 60;
    server.pHostName = g_cfg.server_host;
    server.port = g_cfg.server_port;
    server.pAlpn = "\x04mqtt";
    if (mqtt_quic_transport_init(&g_networkContext, &server, &config) != pdPASS) {
        ESP_LOGE("MOBILITY", "event=fatal reason=tx_task_init");
        return;
    }
    xTransportInterface.pNetworkContext = &g_networkContext;
    xTransportInterface.recv = mqtt_quic_transport_recv;
    xTransportInterface.send = mqtt_quic_transport_send;
    mobility_scheduler_init(&scheduler);
#if AIOT_MOBILITY_CAMERA
    /* Reserve camera/AI resources before Wi-Fi/TLS; net_tx stack already exists. */
    app_memory_start();
    source_manager_init(&g_source_manager);
    bool camera_healthy = false;
    uint32_t camera_frames = 0;
    uint64_t last_capture = 0;
#endif
    wifi_link_connect(g_cfg.wifi_ssid, g_cfg.wifi_password, APP_WIFI_TIMEOUT_MS);
    uint32_t ip_generation = 0, seq = 0;
    uint64_t last_sample = 0, last_generation = 0, next_retry = 0;
    uint64_t session_started = 0, outage_started = 0, validation_deadline = 0, wifi_retry = 0;
    bool active = false, was_ready = false, need_migration = false;
    bool session_reported = false;
    uint64_t next_ota_poll = 30000000;
    int rssi = 0;
    uint32_t rtt = 0;
    ESP_LOGI("MOBILITY", "event=research_start mode=%d deadline_ms=500",
             mobility_mode_get());
    while (1) {
        esp_task_wdt_reset();
        uint64_t now = esp_timer_get_time();
        int mode = mobility_mode_get();
        wifi_link_status_t link;
        wifi_link_get_status(&link);
        quic_client_set_network_available(link.ready);
        if (was_ready && !link.ready) {
            outage_started = now;
            need_migration = active;
            ESP_LOGI("MOBILITY", "event=link_down session=%lu mode=%d reason=%d t_us=%llu",
                (unsigned long)quic_client_session_id(), mode, link.disconnect_reason,
                (unsigned long long)now);
            if (mode == 0 && active) {
                quic_client_cleanup(); active = false; g_mqtt_connected = false;
            }
        }
        if (link.ready && link.ip_generation != ip_generation) {
            ESP_LOGI("MOBILITY", "event=link_up session=%lu generation=%lu t_us=%llu outage_ms=%llu",
                (unsigned long)quic_client_session_id(), (unsigned long)link.ip_generation,
                (unsigned long long)now, (unsigned long long)(outage_started ? (now-outage_started)/1000 : 0));
            need_migration = active && ip_generation != 0;
            ip_generation = link.ip_generation;
            validation_deadline = now + 6000000;
        }
        was_ready = link.ready;
        if (!link.ready && now >= wifi_retry) {
            esp_wifi_connect();
            wifi_retry = now + 5000000;
        }
        if (active && !link.ready && outage_started && now-outage_started > 20000000) {
            ESP_LOGW("MOBILITY", "event=fallback reason=outage_grace_expired t_us=%llu", (unsigned long long)now);
            quic_client_cleanup(); active = false; g_mqtt_connected = false;
        }
        if (active && link.ready && need_migration) {
            int status = quic_client_migration_status();
            if (status != 1 && quic_client_migrate(link.ipv4) == 0) {
                need_migration = false;
            } else if (now >= validation_deadline) {
                ESP_LOGW("MOBILITY", "event=fallback reason=migration_start_failed t_us=%llu", (unsigned long long)now);
                quic_client_cleanup(); active = false; g_mqtt_connected = false;
            }
        }
        if (active && link.ready && !need_migration) {
            int result = quic_client_process();
            int migration = quic_client_migration_status();
            if (result != 0 || migration == -1 ||
                (migration == 1 && now >= validation_deadline) ||
                (!g_mqtt_connected && now-session_started > 15000000)) {
                ESP_LOGW("MOBILITY", "event=fallback reason=transport_or_validation result=%d migration=%d t_us=%llu",
                    result, migration, (unsigned long long)now);
                quic_client_cleanup(); active = false; g_mqtt_connected = false;
                next_retry = now + 1000000;
            }
        }
        if (!active && link.ready && now >= next_retry) {
            snprintf(port, sizeof(port), "%u", g_cfg.server_port);
            quic_client_config_t quic = {g_cfg.server_host, port, "\x04mqtt"};
            g_mqtt_connected = false; g_mqtt_connect_sent = false;
            g_mqtt_transport_ready = false;
            if (quic_client_init_with_config(&quic) == 0) {
                MQTTFixedBuffer_t fixed = {g_networkBuffer, sizeof(g_networkBuffer)};
                if (MQTT_Init(&g_mqttContext, &xTransportInterface, app_get_time_ms,
                              mqtt_event_callback, &fixed) == MQTTSuccess) {
                    active = true; need_migration = false; session_reported = false;
                    session_started = now; g_mqtt_transport_ready = true;
                    ESP_LOGI("MOBILITY", "event=session_begin session=%lu mode=%d t_us=%llu",
                        (unsigned long)quic_client_session_id(), mode, (unsigned long long)now);
                } else quic_client_cleanup();
            }
            next_retry = now + 2000000;
        }
        bool validating = need_migration || quic_client_migration_status() == 1;
        if (active && link.ready && !validating && quic_client_is_connected()) {
            if (!g_mqtt_connected) {
                if (!g_mqtt_connect_sent) app_mqtt_send_connect();
                else app_mqtt_poll_connect();
            } else {
                MQTTStatus_t status = MQTT_ProcessLoop(&g_mqttContext);
                if (status != MQTTSuccess && status != MQTTNeedMoreBytes && status != MQTTNoDataAvailable) {
                    ESP_LOGW("MOBILITY", "event=mqtt_error status=%d", status);
                    quic_client_cleanup(); active = false; g_mqtt_connected = false;
                }
            }
            if (g_mqtt_connected && !session_reported) {
                session_reported = true;
#if !AIOT_MOBILITY_CAMERA
                ota_confirm_boot();
#endif
                ESP_LOGI("MOBILITY", "event=session_ready session=%lu handshake_ms=%lu t_us=%llu",
                    (unsigned long)quic_client_session_id(), (unsigned long)quic_client_get_handshake_time_ms(),
                    (unsigned long long)esp_timer_get_time());
            }
        }
        now = esp_timer_get_time();
        if (!last_sample || now-last_sample >= 200000) {
            last_sample = now;
            wifi_ap_record_t ap;
            rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
            quic_client_get_metrics(&rtt, NULL, NULL);
            mobility_state_t previous = scheduler.state;
            mobility_scheduler_update(&scheduler, link.ready, validating,
                active && g_mqtt_connected, rssi, rtt, now);
            if (scheduler.state != previous)
                ESP_LOGI("MOBILITY", "event=state state=%s mode=%d rssi=%d rtt_ms=%lu t_us=%llu",
                    mobility_state_name(scheduler.state), mode, rssi, (unsigned long)rtt,
                    (unsigned long long)now);
        }
#if AIOT_MOBILITY_CAMERA
        /* Keep real camera/AI active across healthy paths; lower preview rate
         * under a mobility warning, without replacing frames with test JPEGs. */
        int profile = mobility_profile_get();
        source_manager_evidence_enable(profile == 4 || profile >= 7);
        /* Reserve identical diagnostic evidence-copy buffers in every stage. */
        if (profile && !s_profile_evidence_copy)
            s_profile_evidence_copy = heap_caps_malloc(262144, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        uint64_t capture_interval = 1000000U / mobility_video_fps();
        /* Validate real local camera/inference independently of MQTT/preview
         * admission. A slow server must not masquerade as broken hardware.
         * Only pending OTA boots use this bounded local self-test. */
        bool boot_self_test = !profile && !camera_healthy && ota_boot_pending();
        if (boot_self_test) capture_interval = 1000000U;
        if ((boot_self_test || profile || (active && g_mqtt_connected && link.ready && !validating)) &&
            (!last_capture || now-last_capture >= capture_interval)) {
            last_capture = now;
            data_packet_t packet;
            bool replay = mobility_replay_epoch() != 0;
            bool captured = replay ? source_manager_replay_next(&packet) : source_manager_next_packet(&g_source_manager, &packet);
            if (!replay && captured && packet.type != DATA_TYPE_IMAGE)
                captured = source_manager_next_packet(&g_source_manager, &packet);
            if (captured && packet.type == DATA_TYPE_IMAGE && (replay || g_source_manager.last_frame_real)) {
#if AIOT_CAMERA_INTEGRITY_TEST
                if (!replay) {
                    char diagnostic[640];
                    size_t n = source_manager_camera_diagnostic(diagnostic, sizeof(diagnostic));
                    if (n) mqtt_quic_transport_send_hybrid_ts(&g_networkContext, DELIVERY_UNRELIABLE,
                        (const uint8_t *)diagnostic, n, AIOT_TRACK_TELEMETRY, 0, 2000U,
                        packet.timestamp_us / 1000);
                }
#endif
                camera_frames++;
                s_profile_frames = camera_frames;
                if (boot_self_test) {
                    source_manager_submit_camera_ai(&packet);
                } else if (profile) {
                    if (profile >= 2) source_manager_submit_camera_ai(&packet);
                    static uint64_t last_probe_image;
                    static uint32_t last_replay_image_slot = UINT32_MAX;
                    unsigned probe_hz = mobility_probe_hz();
                    uint32_t replay_image_slot = (packet.sequence >> 20)*100000U + ((packet.sequence & 0xfffffU)*(probe_hz ? probe_hz : 10))/10;
                    bool image_due = replay ? replay_image_slot != last_replay_image_slot :
                        (!probe_hz || packet.timestamp_us-last_probe_image >= 1000000U/probe_hz);
                    if ((profile == 5 || profile == 6 || profile >= 7) && active && g_mqtt_connected &&
                        image_due) {
                        last_replay_image_slot = replay_image_slot;
                        last_probe_image = packet.timestamp_us;
                        uint64_t image_start = esp_timer_get_time();
                        uint32_t accepted = app_stream_frame_at(packet.payload, packet.payload_len, packet.priority, packet.timestamp_us / 1000);
                        if (probe_hz) printf("[IMG_PROFILE] {\"profile\":%d,\"ts_ms\":%lu,\"bytes\":%u,\"accepted\":%d,\"enqueue_us\":%llu}\n",
                            profile, (unsigned long)(packet.timestamp_us/1000), (unsigned)packet.payload_len, !!accepted,
                            (unsigned long long)(esp_timer_get_time()-image_start));
                    }
                } else if (app_stream_frame_at(packet.payload, packet.payload_len, packet.priority,
                                        packet.timestamp_us / 1000))
                    source_manager_submit_camera_ai(&packet);
                ESP_LOGI("MOBILITY", "event=camera_frame frames=%lu bytes=%u ai_us=%lu healthy=%d",
                    (unsigned long)camera_frames, (unsigned)packet.payload_len,
                    (unsigned long)g_source_manager.last_inference_us, camera_healthy);
            } else if (!replay) ESP_LOGW("MOBILITY", "event=camera_frame_failed initialized=%d", source_manager_camera_ready());
            now = esp_timer_get_time();
            esp_task_wdt_reset();
        }
        static uint32_t last_ai_ts;
        aiot_ai_result_t async_result;
        uint32_t async_ts;
        if ((boot_self_test || (active && g_mqtt_connected)) && source_manager_get_async_ai(&async_result, &async_ts) &&
            async_ts != last_ai_ts) {
            if (!profile && active && g_mqtt_connected) app_send_ai_result(&async_result, async_ts, 1);
            if (profile >= 3) {
                char metadata[320];
                int m = snprintf(metadata, sizeof(metadata), "{\"profile\":%d,\"model\":\"%s\",\"source_seq\":%lu,\"capture_ms\":%lu,\"count\":%u,\"confidence\":%u,\"x\":%u,\"y\":%u}",
                    profile, async_result.backend == AIOT_AI_BACKEND_ESPDL ? "espdet_cat224" : "legacy", (unsigned long)async_result.source_sequence, (unsigned long)async_ts,
                    async_result.detection_count, async_result.detections[0].confidence,
                    async_result.detections[0].x, async_result.detections[0].y);
                MQTTPublishInfo_t msg = {.qos=MQTTQoS0, .pTopicName="aiot/profile/detection",
                    .topicNameLength=sizeof("aiot/profile/detection")-1, .pPayload=metadata, .payloadLength=m};
                if (m > 0 && m < sizeof(metadata) && MQTT_Publish(&g_mqttContext, &msg, 0) == MQTTSuccess)
                    s_profile_metadata_ok++;
                if ((profile == 4 || profile >= 7) && async_result.detection_count && s_profile_evidence_copy) {
                    size_t len = source_manager_copy_evidence(async_ts, s_profile_evidence_copy, 262144);
                    uint32_t roi_gen_ms = async_ts;
#ifdef AIOT_ESPDET_RUNTIME
                    roi_gen_ms = esp_timer_get_time()/1000;
#endif
                    bool accepted = len && app_stream_frame_at(s_profile_evidence_copy, len, 1, roi_gen_ms);
                    if (accepted) s_profile_evidence_ok++;
                    printf("[ROI_PROFILE] {\"source_seq\":%lu,\"capture_ms\":%lu,\"gen_ms\":%lu,\"bytes\":%u,\"accepted\":%d}\n",
                        (unsigned long)async_result.source_sequence,(unsigned long)async_ts,(unsigned long)roi_gen_ms,(unsigned)len,accepted);
                }
            }
            last_ai_ts = async_ts;
            if (!camera_healthy && camera_frames && (async_result.backend == AIOT_AI_BACKEND_TFLM || async_result.backend == AIOT_AI_BACKEND_ESPDL)) {
                camera_healthy = true;
                ota_confirm_boot();
                ESP_LOGW("MOBILITY", "event=ota_local_health camera_frames=%lu backend=%d pending=%d mqtt=%d",
                    (unsigned long)camera_frames, (int)async_result.backend,
                    ota_boot_pending(), g_mqtt_connected);
            }
        }
        const uint64_t generation_period = 1000000;
#else
        const uint64_t generation_period = 100000;
#endif
        if ((!AIOT_MOBILITY_CAMERA || !mobility_profile_get()) &&
            (!last_generation || now-last_generation >= generation_period)) {
            last_generation = now;
            seq++;
            mobility_object_t object = {.seq = seq, .generated_us = now, .critical = seq % 10 == 0};
            int len = snprintf(object.payload, sizeof(object.payload),
                "{\"seq\":%lu,\"gen_us\":%llu,\"critical\":%s,\"mode\":%d}",
                (unsigned long)object.seq, (unsigned long long)now,
                object.critical ? "true" : "false", mode);
#if AIOT_MOBILITY_CAMERA
            object.critical = g_source_manager.last_ran &&
                g_source_manager.last_importance == AIOT_IMPORTANCE_CRITICAL;
            len = snprintf(object.payload, sizeof(object.payload),
                "{\"seq\":%lu,\"gen_us\":%llu,\"critical\":%s,\"mode\":%d,\"source\":\"camera\",\"frames\":%lu,\"ai_us\":%lu,\"ok\":%d}",
                (unsigned long)object.seq, (unsigned long long)now,
                object.critical ? "true" : "false", mode, (unsigned long)camera_frames,
                (unsigned long)g_source_manager.last_inference_us, camera_healthy);
#endif
            if (len < 0 || len >= sizeof(object.payload)) {
                ESP_LOGE("MOBILITY", "event=telemetry_too_large");
                continue;
            }
            object.len = len;
            bool queued = mobility_scheduler_push(&scheduler, &object, mode);
            ESP_LOGI("MOBILITY", "event=generate seq=%lu critical=%d queued=%d t_us=%llu",
                (unsigned long)object.seq, object.critical, queued, (unsigned long long)now);
        }
        /* Link and validation gates checked immediately, not only at the 200ms sample. */
        int index = mobility_scheduler_pick(&scheduler, mode, now);
        if (index >= 0 && link.ready && !validating && active && g_mqtt_connected) {
            mobility_object_t *object = &scheduler.queue[index];
            bool sent = false;
            if (object->critical) {
                MQTTPublishInfo_t info = {.qos = MQTTQoS0, .pTopicName = "aiot/mobility/event",
                    .topicNameLength = sizeof("aiot/mobility/event")-1,
                    .pPayload = object->payload, .payloadLength = object->len};
                sent = MQTT_Publish(&g_mqttContext, &info, 0) == MQTTSuccess;
            } else {
                sent = mqtt_quic_transport_send_hybrid_ts(&g_networkContext, DELIVERY_UNRELIABLE,
                    (const uint8_t *)object->payload, object->len, AIOT_TRACK_TELEMETRY, 1,
                    mode == 2 ? (uint32_t)(500-(now-object->generated_us)/1000) : 0,
                    object->generated_us / 1000) == object->len;
            }
            if (sent) {
                ESP_LOGI("MOBILITY", "event=enqueue seq=%lu critical=%d session=%lu age_ms=%llu t_us=%llu",
                    (unsigned long)object->seq, object->critical, (unsigned long)quic_client_session_id(),
                    (unsigned long long)((now-object->generated_us)/1000), (unsigned long long)now);
                mobility_scheduler_commit(&scheduler, index, mode, now);
            }
        }
#if AIOT_MOBILITY_CAMERA
        static uint64_t last_memory_publish;
        if (active && g_mqtt_connected && link.ready && !validating &&
            now - last_memory_publish >= 5000000) {
            last_memory_publish = now;
            app_memory_publish(mobility_state_name(scheduler.state), camera_frames,
                               g_source_manager.last_inference_us);
        }
#endif
        static uint64_t last_metrics;
        if (now-last_metrics >= 1000000) {
            last_metrics = now;
            ESP_LOGI("MOBILITY", "event=metrics mode=%d state=%s queue=%u expired=%lu overflow=%lu critical_overflow=%lu internal_free=%u internal_largest=%u t_us=%llu",
                mode, mobility_state_name(scheduler.state), scheduler.count,
                (unsigned long)scheduler.expired, (unsigned long)scheduler.overflow,
                (unsigned long)scheduler.critical_overflow,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned long long)now);
        }
        if (ota_boot_pending() && now > 60000000) {
            ESP_LOGE("MOBILITY", "event=ota_boot_unhealthy action=rollback");
            esp_restart();
        }
        if (!mobility_profile_get() && ota_pull_enabled() && link.ready && !validating && now >= next_ota_poll) {
            char target[65];
#if AIOT_MOBILITY_CAMERA
            aiot_ai_result_t health_result;
            uint32_t health_timestamp;
            int health_backend = source_manager_get_async_ai(&health_result, &health_timestamp)
                ? (int)health_result.backend : -1;
            ota_set_local_health(source_manager_camera_ready(), camera_frames,
                g_source_manager.last_inference_us, health_backend);
#endif
            ESP_LOGI("MOBILITY", "event=ota_maintenance_begin t_us=%llu", (unsigned long long)now);
            esp_task_wdt_delete(NULL); /* HTTPS has a bounded network timeout. */
            if (ota_pull_check(target)) {
                quic_client_cleanup(); active = false; g_mqtt_connected = false;
                esp_err_t err = ota_pull_install(target);
                ESP_LOGE("MOBILITY", "event=ota_failed error=%s", esp_err_to_name(err));
            }
            esp_task_wdt_add(NULL);
            next_ota_poll = esp_timer_get_time() + 30000000;
            ESP_LOGI("MOBILITY", "event=ota_maintenance_end t_us=%llu", (unsigned long long)esp_timer_get_time());
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
