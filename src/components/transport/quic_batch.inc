/* Group existing application fragments. ngtcp2 still packetizes each DATAGRAM.
 * No WRITE_MORE: its mandatory same-packet rules cannot span an unlock/yield. */
static unsigned s_batch_kib = 1;
static struct {
    uint64_t calls, fragments, bytes, write_calls, packets, packet_bytes;
    uint64_t locks, wait_us, hold_us, max_hold_us, cpu_us, encode_us, udp_us;
    uint64_t refused, budget_yields, failed, expired, dropped, max_group, short_groups;
} s_batch;

bool quic_batch_set(unsigned kib) {
    if (kib!=0 && kib!=1 && kib!=4 && kib!=8 && kib!=16) return false;
    __atomic_store_n(&s_batch_kib, kib, __ATOMIC_RELAXED); return true;
}
unsigned quic_batch_get(void) { return __atomic_load_n(&s_batch_kib, __ATOMIC_RELAXED); }

static int quic_batch_fragment_locked(const quic_batch_fragment_t *f)
{
    if (!g_network_available || !quic_client_is_connected() || quic_processing || !g_client.conn) return -1;
    /* 1180 includes the 12-byte application header. Peer limit includes the
     * DATAGRAM frame type and length (at most 3 bytes for this payload). */
    const ngtcp2_transport_params *tp = ngtcp2_conn_get_remote_transport_params(g_client.conn);
    if (!f->data || !f->len || f->len>1180 || !tp || tp->max_datagram_frame_size<f->len+3) return -1;
    if (g_client.tx.submitted < g_client.tx.queued) {
        if (client_write(&g_client)) return -1;
        if (g_client.tx.submitted < g_client.tx.queued) return -2;
    }
    if (!g_client.dgram_tx_buf || g_client.dgram_tx_buf_len<1452) return -1;
    ngtcp2_path_storage ps; ngtcp2_pkt_info pi = {0};
    ngtcp2_path_storage_zero(&ps);
    int accepted=0; ngtcp2_tstamp ts=timestamp();
    int64_t start=esp_timer_get_time();
    ++s_batch.write_calls;
#if AIOT_PIPELINE_PROFILE
    uint32_t packet_cpu_start=aiot_task_runtime_now();s_pipe_scope=true;
#endif
    ngtcp2_ssize nwrite=ngtcp2_conn_write_datagram(g_client.conn, &ps.path, &pi,
        g_client.dgram_tx_buf,g_client.dgram_tx_buf_len,&accepted,
        NGTCP2_WRITE_DATAGRAM_FLAG_NONE,f->id,f->data,f->len,ts);
#if AIOT_PIPELINE_PROFILE
    s_pipe_scope=false;s_pipe.packet_cpu+=(uint32_t)(aiot_task_runtime_now()-packet_cpu_start);
#endif
    s_batch.encode_us+=esp_timer_get_time()-start;
    if(nwrite<0) return -1;
    if(!nwrite) return -2;
    start=esp_timer_get_time();
#if AIOT_PIPELINE_PROFILE
    uint32_t udp_cpu_start=aiot_task_runtime_now();s_pipe_scope=true;
#endif
    int result=client_send_packet(&g_client,g_client.dgram_tx_buf,(size_t)nwrite,&ps.path);
#if AIOT_PIPELINE_PROFILE
    s_pipe_scope=false;s_pipe.udp_cpu+=(uint32_t)(aiot_task_runtime_now()-udp_cpu_start);
#endif
    s_batch.udp_us+=esp_timer_get_time()-start;
    if(result) return -1;
    ++s_batch.packets; s_batch.packet_bytes+=nwrite;
    ngtcp2_conn_update_pkt_tx_time(g_client.conn,ts);
    return accepted ? 0 : -2;
}

size_t quic_client_write_batch_safe(const quic_batch_fragment_t *f, size_t count,
                                  int64_t deadline_us, int *result)
{
    uint32_t before=aiot_task_runtime_now();
    ++s_batch.calls;
    if(count>s_batch.max_group) s_batch.max_group=count;
    unsigned target=quic_batch_get();
    if(target && count<target) ++s_batch.short_groups;
    size_t done=0; bool locked=false;
    int ret=-1; int64_t held_since=0, retry_until=esp_timer_get_time()+100000;
    uint32_t session=quic_client_session_id();
    if(!f || !count || count>QUIC_BATCH_MAX_FRAGMENTS || !quic_mutex) goto finish;
    while(done<count) {
        int64_t now=esp_timer_get_time();
        if(deadline_us && now>=deadline_us) { ret=-3; break; }
        if(quic_client_session_id()!=session) { ret=-1; break; }
        if(!locked) {
            /* Never wait longer than the remaining frame deadline. */
            int64_t remaining=deadline_us ? deadline_us-now : 1000000;
            if(remaining>1000000) remaining=1000000;
            TickType_t ticks=pdMS_TO_TICKS((uint32_t)(remaining/1000));
            int64_t wait_start=esp_timer_get_time();
            BaseType_t ok=quic_lock_take(ticks);
            s_batch.wait_us+=esp_timer_get_time()-wait_start;
            if(ok!=pdTRUE) { ret=-1; break; }
            ++s_batch.locks; locked=true; held_since=esp_timer_get_time();
            /* Re-check after potentially blocking on the connection lock. */
            if(deadline_us && held_since>=deadline_us) { ret=-3; break; }
            if(quic_client_session_id()!=session) { ret=-1; break; }
        }
        ret=quic_batch_fragment_locked(&f[done]);
        now=esp_timer_get_time();
        if(ret==0) {
            ++s_batch.fragments; s_batch.bytes+=f[done].len; ++done;
            retry_until=now+100000;
        } else if(ret==-2) ++s_batch.refused;
        else break;
        bool budget=now-held_since>=20000;
        if(ret==-2 || budget || done==count) {
            uint64_t held=now-held_since;
            s_batch.hold_us+=held; if(held>s_batch.max_hold_us) s_batch.max_hold_us=held;
            quic_lock_give(); locked=false;
            if(done==count) break;
            if(ret==-2 && now>=retry_until) break;
            s_batch.budget_yields+=budget;
            vTaskDelay(1); /* Never sleep holding the connection mutex. */
        }
    }
finish:
    if(locked) {
        uint64_t held=esp_timer_get_time()-held_since;
        s_batch.hold_us+=held; if(held>s_batch.max_hold_us) s_batch.max_hold_us=held;
        quic_lock_give();
    }
    if(done<count) { s_batch.dropped+=count-done; s_batch.failed+=ret!=-3; s_batch.expired+=ret==-3; }
    s_batch.cpu_us+=(uint32_t)(aiot_task_runtime_now()-before);
    *result=done==count ? 0 : ret;
    return done;
}

void quic_batch_report(void)
{
    static int64_t last;
    int64_t now=esp_timer_get_time();
    if(now-last<1000000) return;
    last=now;
    uint64_t attempts, acquired, wait, hold, maxhold, other_cpu;
    portENTER_CRITICAL(&s_lock_stats_mux);
    other_cpu=s_other_quic_cpu_us;
    attempts=s_lock_attempts; acquired=s_lock_acquired; wait=s_lock_wait_us;
    hold=s_lock_hold_us; maxhold=s_lock_hold_max;
    portEXIT_CRITICAL(&s_lock_stats_mux);
    char line[1536];
    int pos=snprintf(line,sizeof(line),"[BATCH_PERF] {\"us\":%lld,\"kib\":%u", (long long)now,quic_batch_get());
#define BATCH_FIELD(key) pos+=snprintf(line+pos,sizeof(line)-pos,",\"" #key "\":%llu",(unsigned long long)s_batch.key)
    BATCH_FIELD(calls); BATCH_FIELD(fragments); BATCH_FIELD(bytes); BATCH_FIELD(write_calls);
    BATCH_FIELD(packets); BATCH_FIELD(packet_bytes); BATCH_FIELD(locks); BATCH_FIELD(wait_us);
    BATCH_FIELD(hold_us); BATCH_FIELD(max_hold_us); BATCH_FIELD(cpu_us);
    BATCH_FIELD(encode_us); BATCH_FIELD(udp_us); BATCH_FIELD(refused); BATCH_FIELD(budget_yields);
    BATCH_FIELD(failed); BATCH_FIELD(expired); BATCH_FIELD(dropped); BATCH_FIELD(max_group); BATCH_FIELD(short_groups);
#undef BATCH_FIELD
    pos+=snprintf(line+pos,sizeof(line)-pos,",\"other_quic_cpu_us\":%llu",(unsigned long long)other_cpu);
    snprintf(line+pos,sizeof(line)-pos,",\"all_lock_attempts\":%llu,\"all_lock_acquired\":%llu,\"all_lock_wait_us\":%llu,\"all_lock_hold_us\":%llu,\"all_lock_max_hold_us\":%llu}\n",
        (unsigned long long)attempts,(unsigned long long)acquired,(unsigned long long)wait,
        (unsigned long long)hold,(unsigned long long)maxhold);
    printf("%s",line);
#if AIOT_PIPELINE_PROFILE
    pipe_report(now);
#endif
}
