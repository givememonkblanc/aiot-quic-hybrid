/* AIOT_MOQT_HELPER: the device as MOQT draft-21 subscriber of one track at a relay.
 *
 * Included by moqt_ngtcp2.inc. The publisher core (moqt_core.c) still owns the control stream
 * (own SETUP, the SETUP of the relay, GOAWAY); this file owns what a publisher core does not
 * know: the SUBSCRIBE request stream this endpoint opens, and the subgroup streams the relay
 * opens to deliver Objects (section 11.3). Every Object of the subscribed track is assembled
 * in PSRAM and handed whole to the application sink; Objects of other tracks are dropped.
 * The parser follows parse_subgroup() of experiments/moq/relay/relay.c. */
#ifndef AIOT_MOQT_HELPER_MAX_KB
#define AIOT_MOQT_HELPER_MAX_KB 320
#endif
#define MOQ_HELPER_STREAMS 3
#define MOQ_HELPER_CAP ((size_t)AIOT_MOQT_HELPER_MAX_KB*1024U)
/* One subgroup stream of the relay. The buffer holds the unparsed bytes; the payload of an
 * Object stays at the front until it is complete, so it is contiguous for the sink. */
typedef struct {
 bool used,header,props,first_object_mode,has_prev,has_priority,overflow;
 int64_t id; uint8_t *buf; size_t len;
 uint64_t alias,group,subgroup,prev_object,first_us; uint8_t priority;
} moq_helper_stream;
static struct {
 bool requested,subscribed,failed,ended;
 int64_t request; uint64_t alias,request_id;
 uint8_t in[512]; size_t in_len;
 moq_helper_stream s[MOQ_HELPER_STREAMS];
 quic_moq_helper_sink sink;
 uint64_t objects,bytes,other_tracks,overflows,resets,dropped_streams,bad_streams;
} moq_helper;
void quic_client_moq_helper_sink(quic_moq_helper_sink sink){moq_helper.sink=sink;}
static void moq_helper_stream_free(moq_helper_stream *s){uint8_t *b=s->buf;memset(s,0,sizeof(*s));s->buf=b;}
static void moq_helper_reset(void){
 for(unsigned i=0;i<MOQ_HELPER_STREAMS;i++)moq_helper_stream_free(&moq_helper.s[i]);
 moq_helper.requested=moq_helper.subscribed=moq_helper.failed=moq_helper.ended=false;
 moq_helper.request=-1;moq_helper.alias=0;moq_helper.in_len=0;
}
static moq_helper_stream *moq_helper_find(int64_t id){
 for(unsigned i=0;i<MOQ_HELPER_STREAMS;i++)if(moq_helper.s[i].used&&moq_helper.s[i].id==id)return &moq_helper.s[i];
 return NULL;
}
static moq_helper_stream *moq_helper_take(int64_t id){
 for(unsigned i=0;i<MOQ_HELPER_STREAMS;i++){
  moq_helper_stream *s=&moq_helper.s[i];
  if(s->used)continue;
  if(!s->buf)s->buf=heap_caps_malloc(MOQ_HELPER_CAP,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  if(!s->buf)return NULL;
  moq_helper_stream_free(s);s->used=true;s->id=id;s->first_us=(uint64_t)esp_timer_get_time();
  return s;
 }
 return NULL;
}
/* Parses as many complete Objects as the buffer holds. 0 more bytes needed, -1 invalid. */
static int moq_helper_parse(moq_helper_stream *u){
 for(;;){
  moqt_rd r=moqt_rd_of(u->buf,u->len);
  if(!u->header){
   uint64_t flags=moqt_rd_vi(&r);
   if(r.bad)return 0;
   if(!(flags<128&&(flags&MOQT_SG_ALWAYS)))return -1;
   unsigned mode=(unsigned)((flags&MOQT_SG_ID_MODE_MASK)>>1);
   if(mode==3)return -1;
   u->alias=moqt_rd_vi(&r);u->group=moqt_rd_vi(&r);
   u->subgroup=0;u->first_object_mode=mode==1;
   if(mode==2)u->subgroup=moqt_rd_vi(&r);
   u->has_priority=!(flags&MOQT_SG_DEFAULT_PRIORITY);
   if(u->has_priority)u->priority=moqt_rd_u8(&r);
   if(r.bad)return 0;
   u->props=flags&MOQT_SG_PROPERTIES;u->header=true;
   memmove(u->buf,u->buf+(u->len-r.n),r.n);u->len=r.n;
   continue;
  }
  if(!r.n)return 0;
  uint64_t delta=moqt_rd_vi(&r);
  const uint8_t *props=NULL;size_t props_len=0;
  if(u->props){props_len=(size_t)moqt_rd_vi(&r);props=moqt_rd_bytes(&r,props_len);}
  uint64_t size=moqt_rd_vi(&r);
  if(r.bad)return 0;
  if(size){
   /* the payload follows; wait until all of it is here */
   size_t head=u->len-r.n;
   if(size>MOQ_HELPER_CAP-head){u->overflow=true;return -1;}
   if(r.n<size)return 0;
  }else{(void)moqt_rd_vi(&r);if(r.bad)return 0;}
  uint64_t object=u->has_prev?u->prev_object+delta+1:delta;
  u->has_prev=true;u->prev_object=object;
  if(u->first_object_mode){u->subgroup=object;u->first_object_mode=false;}
  const uint8_t *payload=r.p;
  uint64_t done_us=(uint64_t)esp_timer_get_time();
  bool ours=!moq_helper.subscribed||u->alias==moq_helper.alias;
  if(size&&ours){
   moq_helper.objects++;moq_helper.bytes+=size;
   if(moq_helper.sink)moq_helper.sink(payload,(size_t)size,u->group,object,props,props_len,u->first_us,done_us);
  }else if(size)moq_helper.other_tracks++;
  size_t used=u->len-(r.n-(size_t)size);
  memmove(u->buf,u->buf+used,u->len-used);u->len-=used;
  u->first_us=done_us;
 }
}
/* SUBSCRIBE_OK, REQUEST_ERROR and PUBLISH_DONE on the request stream. */
static void moq_helper_request_rx(const uint8_t *d,size_t n,bool fin){
 if(n>sizeof(moq_helper.in)-moq_helper.in_len){moq_helper.failed=true;return;}
 memcpy(moq_helper.in+moq_helper.in_len,d,n);moq_helper.in_len+=n;
 for(;;){
  uint64_t t;const uint8_t *body;size_t bl,used;
  if(moqt21_message_read(moq_helper.in,moq_helper.in_len,&t,&body,&bl,&used)!=1)break;
  if(t==MOQT_MSG_SUBSCRIBE_OK){
   moqt_rd r=moqt_rd_of(body,bl);uint64_t alias=moqt_rd_vi(&r);
   if(!r.bad){moq_helper.alias=alias;moq_helper.subscribed=true;}
   printf("[HELPER_SUBSCRIBED] {\"alias\":%llu,\"request_id\":%llu}\n",(unsigned long long)alias,(unsigned long long)moq_helper.request_id);
  }else if(t==MOQT_MSG_REQUEST_ERROR){
   moqt_error e={0};moqt_request_error_read(body,bl,&e);
   moq_helper.failed=true;
   printf("[HELPER_REQUEST_ERROR] {\"code\":%llu}\n",(unsigned long long)e.code);
  }else if(t==MOQT_MSG_PUBLISH_DONE){
   moq_helper.ended=true;
   printf("[HELPER_PUBLISH_DONE] {}\n");
  }
  memmove(moq_helper.in,moq_helper.in+used,moq_helper.in_len-used);moq_helper.in_len-=used;
 }
 if(fin&&!moq_helper.ended){moq_helper.ended=true;printf("[HELPER_REQUEST_FIN] {}\n");}
}
/* 1: the bytes were for the helper; 0: they go to the publisher core. */
static int moq_helper_rx(ngtcp2_conn *conn,int64_t id,const uint8_t *data,size_t len,bool fin){
 if(id==moq_helper.request&&moq_helper.requested){moq_helper_request_rx(data,len,fin);return 1;}
 if((id&3)!=3)return 0;                                /* not a stream the server opened for data */
 moq_helper_stream *s=moq_helper_find(id);
 if(!s){
  /* Section 6.4.1: a subgroup stream starts with its type, below 128 with bit 0x10 set; the
   * control stream starts with SETUP (two bytes), which the core classifies. */
  if(!len||data[0]>=0x80||!(data[0]&MOQT_SG_ALWAYS))return 0;
  s=moq_helper_take(id);
  if(!s){
   moq_helper.dropped_streams++;
   ngtcp2_conn_shutdown_stream_read(conn,0,id,MOQT_RST_CANCELLED);
   return 1;
  }
 }
 if(s->overflow)return 1;
 if(len>MOQ_HELPER_CAP-s->len){
  moq_helper.overflows++;s->overflow=true;
  ngtcp2_conn_shutdown_stream_read(conn,0,id,MOQT_RST_CANCELLED);
  if(fin)moq_helper_stream_free(s);
  return 1;
 }
 memcpy(s->buf+s->len,data,len);s->len+=len;
 if(moq_helper_parse(s)<0){
  if(s->overflow)moq_helper.overflows++;else moq_helper.bad_streams++;
  s->overflow=true;
  ngtcp2_conn_shutdown_stream_read(conn,0,id,MOQT_RST_CANCELLED);
 }
 if(fin)moq_helper_stream_free(s);
 return 1;
}
/* The relay reset a data stream (delivery timeout, cancelled): the partial Object is dropped. */
static bool moq_helper_cancel(int64_t id){
 moq_helper_stream *s=moq_helper_find(id);
 if(s){moq_helper.resets++;moq_helper_stream_free(s);return true;}
 return id==moq_helper.request&&moq_helper.requested;
}
int quic_client_moq_subscribe(const char *const *ns,unsigned ns_count,const char *name,uint8_t priority){
 quic_lock_take(portMAX_DELAY);int result=-1;
 if(moq_session_ready()&&!moq_helper.requested){
  int64_t id;
  if(ngtcp2_conn_open_bidi_stream(g_client.conn,&id,NULL)==0){
   struct moq_tx *s=moq_request_slot(id);
   uint8_t b[256];moqt_wr w=moqt_wr_of(b,sizeof(b));
   /* Section 9.1: the client uses even Request IDs. */
   uint64_t rid=moq_core->next_request_id;moq_core->next_request_id+=2;
   size_t mark=moqt_wr_begin_message(&w,MOQT_MSG_SUBSCRIBE);
   moqt_wr_vi(&w,rid);
   moqt_ns_write(&w,ns,ns_count);
   moqt_wr_vi(&w,strlen(name));moqt_wr_bytes(&w,name,strlen(name));
   moqt_wr_vi(&w,1);moqt_wr_vi(&w,MOQT_P_SUBSCRIBER_PRIORITY);moqt_wr_u8(&w,priority);
   moqt_wr_end_message(&w,mark);
   if(s&&!w.bad&&!moq_append(s,b,w.n,false)){
    moq_helper.requested=true;moq_helper.request=id;moq_helper.request_id=rid;moq_helper.in_len=0;result=0;
    printf("[HELPER_SUBSCRIBE] {\"stream\":%lld,\"request_id\":%llu,\"track\":\"%s\"}\n",(long long)id,(unsigned long long)rid,name);
   }
  }
 }
 quic_lock_give();return result;
}
/* 1 subscribed, 0 waiting, -1 refused or ended (the caller reconnects). */
int quic_client_moq_subscription_state(void){
 if(moq_helper.failed||moq_helper.ended)return -1;
 return moq_helper.subscribed?1:0;
}
void quic_client_moq_helper_stats(char *out,size_t cap){
 snprintf(out,cap,"\"objects\":%llu,\"bytes\":%llu,\"other_tracks\":%llu,\"overflows\":%llu,\"stream_resets\":%llu,\"dropped_streams\":%llu,\"bad_streams\":%llu",
  (unsigned long long)moq_helper.objects,(unsigned long long)moq_helper.bytes,(unsigned long long)moq_helper.other_tracks,
  (unsigned long long)moq_helper.overflows,(unsigned long long)moq_helper.resets,(unsigned long long)moq_helper.dropped_streams,
  (unsigned long long)moq_helper.bad_streams);
}
