/* TX-worker only. Keep queue allocations/fragment boundaries identical in all
 * cases, grouping references rather than copying large JPEGs a second time. */
static iot_route_msg_t *s_batch_messages;
static quic_batch_fragment_t *s_batch_fragments;
static iot_route_msg_t s_batch_pending;
static bool s_batch_has_pending;
static bool s_batch_image_busy;
bool mqtt_quic_image_inflight(void) { return __atomic_load_n(&s_batch_image_busy,__ATOMIC_RELAXED); }
static void batch_image_busy(bool busy) { __atomic_store_n(&s_batch_image_busy,busy,__ATOMIC_RELAXED); }

static bool batch_same_frame(const iot_route_msg_t *m, uint32_t frame)
{
    return m->mode==DELIVERY_UNRELIABLE && m->track_id==AIOT_TRACK_IMAGE &&
        m->payload_len>=AIOT_OBJECT_HEADER_SIZE &&
        ((const aiot_object_header_t *)m->payload)->gen_ts_ms==frame;
}
static int batch_image_send(NetworkContext_t *ctx, const iot_route_msg_t *first,
                            uint32_t frame)
{
    batch_image_busy(true);
    unsigned kib=quic_batch_get();
    size_t limit=kib ? kib : QUIC_BATCH_MAX_FRAGMENTS;
    size_t count=1;
    s_batch_messages[0]=*first;
    while(count<limit) {
        iot_route_msg_t *last=&s_batch_messages[count-1];
        if(last->payload_len>=2 && last->payload[last->payload_len-2]==0xff &&
            last->payload[last->payload_len-1]==0xd9) break;
        iot_route_msg_t next;
        /* The producer may still be enqueueing this same frame. Bound its
         * wait and do not hold a connection lock while assembling references. */
        if(xQueueReceive(ctx->route_queue,&next,pdMS_TO_TICKS(10))!=pdTRUE) break;
        if(!batch_same_frame(&next,frame) || next.object_seq!=last->object_seq+1) {
            s_batch_pending=next; s_batch_has_pending=true; break;
        }
        s_batch_messages[count++]=next;
    }
    int64_t deadline=first->deadline_us;
    for(size_t i=0;i<count;++i) {
        iot_route_msg_t *m=&s_batch_messages[i];
        s_batch_fragments[i]=(quic_batch_fragment_t){m->payload,m->payload_len,m->object_seq};
        if(m->deadline_us && (!deadline || m->deadline_us<deadline)) deadline=m->deadline_us;
    }
    iot_route_msg_t *tail=&s_batch_messages[count-1];
    bool final=tail->payload_len>=2 && tail->payload[tail->payload_len-2]==0xff && tail->payload[tail->payload_len-1]==0xd9;
    int ret;
    size_t sent=quic_client_write_batch_safe(s_batch_fragments,count,deadline,&ret);
    ctx->datagram_send_ok+=sent;
    if(ret==-3) ctx->datagram_expired+=count-sent;
    else if(sent<count) { ++ctx->datagram_send_fail; ctx->datagram_expired+=count-sent-1; }
    for(size_t i=0;i<count;++i)
        route_payload_free(ctx,s_batch_messages[i].payload,s_batch_messages[i].payload_len);
    if(final || ret) batch_image_busy(false);
    quic_batch_report();
    return ret;
}
