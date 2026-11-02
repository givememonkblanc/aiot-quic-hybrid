/* Publisher core checks. The test plays the subscriber (a QUIC server): its request
 * streams are 1, 5, 9, ..., its control stream is 3, its Request IDs are odd.
 * Comments name the checklist item in MOQT_DRAFT21_CHECKLIST_KO.md. */
#include "moqt_core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const moqt_track_cfg TRACKS[]={
 {.ns={"aiot","esp32"},.ns_count=2,.name="preview/jpeg",.priority=192,.group_order=MOQT_ORDER_DESCENDING,.object_timeout_ms=2000},
 {.ns={"aiot","esp32"},.ns_count=2,.name="ai/events",.priority=64},
 {.ns={"aiot","esp32"},.ns_count=2,.name="evidence/jpeg",.priority=128,.object_timeout_ms=10000,.max_cache_ms=60000},
 {.ns={"aiot","status"},.ns_count=2,.name="health",.dynamic_groups=true},
};
static const moqt_track_cfg TRACKS_EXT[]={
 {.ns={"aiot","esp32"},.ns_count=2,.name="preview/jpeg",.priority=192,.group_order=MOQT_ORDER_DESCENDING,.source_timeout_ms=2000},
 {.ns={"aiot","esp32"},.ns_count=2,.name="ai/events",.priority=64},
 {.ns={"aiot","esp32"},.ns_count=2,.name="evidence/jpeg",.source_timeout_ms=5000},
 {.ns={"aiot","status"},.ns_count=2,.name="health"},
};
static moqt_core core;
static moqt_action acts[32];static unsigned nacts;static uint8_t act_data[32][1200];

static void drain(void){
 moqt_action a;nacts=0;
 while(moqt_core_next_action(&core,&a)){
  assert(nacts<32&&a.len<=sizeof(act_data[0]));
  if(a.len){memcpy(act_data[nacts],a.data,a.len);a.data=act_data[nacts];}
  acts[nacts++]=a;
 }
}
static const moqt_action *find(moqt_act_kind kind,unsigned nth){
 for(unsigned i=0;i<nacts;i++)if(acts[i].kind==kind&&!nth--)return &acts[i];
 return NULL;
}
static unsigned count(moqt_act_kind kind){unsigned n=0;for(unsigned i=0;i<nacts;i++)n+=acts[i].kind==kind;return n;}
/* Decodes the message carried by the nth SEND on a stream. */
static bool sent(int64_t stream,unsigned nth,uint64_t *type,const uint8_t **body,size_t *len,bool *fin){
 for(unsigned i=0;i<nacts;i++){
  const moqt_action *a=&acts[i];
  if(a->kind!=MOQT_ACT_SEND||a->stream!=stream||!a->len)continue;
  if(nth--)continue;
  size_t used;
  assert(moqt21_message_read(a->data,a->len,type,body,len,&used)==1&&used==a->len);
  if(fin)*fin=a->fin;
  return true;
 }
 return false;
}
static uint64_t error_code(int64_t stream){
 uint64_t type;const uint8_t *body;size_t len;bool fin;moqt_error e;
 assert(sent(stream,0,&type,&body,&len,&fin)&&type==MOQT_MSG_REQUEST_ERROR&&fin);
 assert(moqt_request_error_read(body,len,&e)==0);
 return e.code;
}

static void fresh(bool with_setup){
 moqt_core_init(&core,TRACKS,4,true);
 uint8_t b[128];assert(moqt_core_setup(&core,b,sizeof(b),"/","host:1"));
 if(with_setup){
  size_t n=moqt_setup_write(b,sizeof(b),NULL,NULL,"test-subscriber",4,0);
  moqt_core_rx(&core,3,b,n,false,0);drain();assert(core.peer_setup&&!nacts);
 }
}
static size_t request(uint8_t *b,size_t cap,uint64_t type,uint64_t id,const char *const *ns,unsigned nsn,const char *name,
                      unsigned count,const uint8_t *params,size_t plen){
 moqt_wr w=moqt_wr_of(b,cap);size_t mark=moqt_wr_begin_message(&w,type);
 moqt_wr_vi(&w,id);moqt_ns_write(&w,ns,nsn);
 if(name){moqt_wr_vi(&w,strlen(name));moqt_wr_bytes(&w,name,strlen(name));}
 moqt_wr_vi(&w,count);moqt_wr_bytes(&w,params,plen);
 moqt_wr_end_message(&w,mark);assert(!w.bad);return w.n;
}
static size_t update(uint8_t *b,size_t cap,uint64_t id,unsigned count,const uint8_t *params,size_t plen){
 moqt_wr w=moqt_wr_of(b,cap);size_t mark=moqt_wr_begin_message(&w,MOQT_MSG_REQUEST_UPDATE);
 moqt_wr_vi(&w,id);moqt_wr_vi(&w,count);moqt_wr_bytes(&w,params,plen);
 moqt_wr_end_message(&w,mark);assert(!w.bad);return w.n;
}
static const char *NS[]={"aiot","esp32"};
static void subscribe(int64_t stream,uint64_t id,const char *name,unsigned count,const uint8_t *params,size_t plen){
 uint8_t b[256];size_t n=request(b,sizeof(b),MOQT_MSG_SUBSCRIBE,id,NS,2,name,count,params,plen);
 moqt_core_rx(&core,stream,b,n,false,0);drain();
}
static int offer(unsigned track,uint64_t group,uint64_t object,uint64_t subgroup,const uint8_t *props,size_t plen,moqt_delivery *d){
 moqt_offer o={.group=group,.object=object,.subgroup=subgroup,.props=props,.props_len=plen,.payload_len=100,.end_of_group=true};
 return moqt_core_publish(&core,track,&o,0,d,4);
}
static void *cache(unsigned track,uint64_t group,uint64_t object,uint64_t subgroup,const uint8_t *props,size_t plen,void *payload){
 moqt_offer o={.group=group,.object=object,.subgroup=subgroup,.props=props,.props_len=plen,.payload_len=100};
 moqt_delivery d[4];assert(moqt_core_publish(&core,track,&o,0,d,4)>=0);
 void *evicted;assert(moqt_core_cache_put(&core,track,&o,payload,0,&evicted)==0);
 return evicted;
}

static void test_setup_and_buffering(void){ /* A2, A9, A10, A12 */
 fresh(false);
 /* A request that arrives before SETUP is buffered (6.3). */
 subscribe(1,1,"ai/events",0,NULL,0);assert(!nacts);
 uint8_t b[64];size_t n=moqt_setup_write(b,sizeof(b),NULL,NULL,"s",4,0);
 moqt_core_rx(&core,3,b,n,false,0);drain();
 uint64_t type;const uint8_t *body;size_t len;bool fin;
 assert(sent(1,0,&type,&body,&len,&fin)&&type==MOQT_MSG_SUBSCRIBE_OK&&!fin&&body[0]==2);
 assert(count(MOQT_ACT_SUBSCRIBED)==1);
 /* A second control stream, a closed control stream and a server PATH are violations. */
 moqt_core_rx(&core,7,b,n,false,0);drain();
 assert(find(MOQT_ACT_CLOSE_SESSION,0)->code==MOQT_S_PROTOCOL_VIOLATION);
 fresh(true);moqt_core_rx(&core,3,NULL,0,true,0);drain();
 assert(find(MOQT_ACT_CLOSE_SESSION,0)->code==MOQT_S_PROTOCOL_VIOLATION);
 moqt_core_init(&core,TRACKS,4,true);n=moqt_setup_write(b,sizeof(b),"/",NULL,NULL,0,0);
 moqt_core_rx(&core,3,b,n,false,0);drain();
 assert(find(MOQT_ACT_CLOSE_SESSION,0)->code==MOQT_S_INVALID_PATH);
 /* The first control message must be SETUP. */
 moqt_core_init(&core,TRACKS,4,true);n=moqt_request_ok_write(b,sizeof(b),NULL,NULL,0);
 /* 0x07 is not a unidirectional stream type (6.4.1). */
 moqt_core_rx(&core,3,b,n,false,0);drain();
 assert(find(MOQT_ACT_CLOSE_SESSION,0)->code==MOQT_S_PROTOCOL_VIOLATION);
 /* Padding streams are discarded; data streams for unknown subscriptions are refused. */
 fresh(true);
 const uint8_t padding[]={0xF0,0x13,0x2B,0x3E,0x28,0,0,0};
 moqt_core_rx(&core,7,padding,sizeof(padding),false,0);moqt_core_rx(&core,7,padding+5,3,true,0);drain();assert(!nacts);
 const uint8_t subgroup[]={0x78,1,0,0,1,'x'};
 moqt_core_rx(&core,11,subgroup,sizeof(subgroup),false,0);drain();
 assert(find(MOQT_ACT_STOP_SENDING,0)->stream==11&&!core.closed);
 /* A request stream may not begin with REQUEST_UPDATE. */
 n=update(b,sizeof(b),1,0,NULL,0);moqt_core_rx(&core,1,b,n,false,0);drain();
 assert(find(MOQT_ACT_CLOSE_SESSION,0)->code==MOQT_S_PROTOCOL_VIOLATION);
 /* Partial delivery of one message byte by byte. */
 fresh(true);uint8_t q[128];n=request(q,sizeof(q),MOQT_MSG_SUBSCRIBE,1,NS,2,"ai/events",0,NULL,0);
 for(size_t i=0;i<n;i++){moqt_core_rx(&core,1,q+i,1,false,0);drain();assert((i+1==n)==(nacts!=0));}
 /* A declared length beyond the endpoint limit ends the session. */
 fresh(true);const uint8_t big[]={0x03,0x07,0xFF};moqt_core_rx(&core,1,big,3,false,0);drain();
 assert(find(MOQT_ACT_CLOSE_SESSION,0)->code==MOQT_S_INTERNAL_ERROR);
}

static void test_request_ids(void){ /* A11 */
 fresh(true);subscribe(1,2,"ai/events",0,NULL,0); /* even ID from the server */
 assert(find(MOQT_ACT_CLOSE_SESSION,0)->code==MOQT_S_INVALID_REQUEST_ID);
 fresh(true);subscribe(1,1,"ai/events",0,NULL,0);assert(!core.closed);
 subscribe(5,1,"preview/jpeg",0,NULL,0); /* duplicate */
 assert(find(MOQT_ACT_CLOSE_SESSION,0)->code==MOQT_S_INVALID_REQUEST_ID);
 fresh(true);subscribe(1,9,"ai/events",0,NULL,0);subscribe(5,3,"preview/jpeg",0,NULL,0); /* out of order is fine */
 assert(!core.closed&&count(MOQT_ACT_SUBSCRIBED)==1);
 subscribe(9,2001,"evidence/jpeg",0,NULL,0);assert(!core.closed);
 subscribe(13,9,"ai/events",0,NULL,0);
 assert(find(MOQT_ACT_CLOSE_SESSION,0)->code==MOQT_S_INVALID_REQUEST_ID); /* outside the window counts as reuse */
}

static void test_subscribe(void){ /* C1-C5, C12, F1-F3, G3, H7 */
 uint64_t type;const uint8_t *body;size_t len;bool fin;moqt_delivery d[4];
 fresh(true);
 subscribe(1,1,"nope",0,NULL,0);assert(error_code(1)==MOQT_R_DOES_NOT_EXIST);
 const char *reserved[]={".session","x"};uint8_t b[256];
 size_t n=request(b,sizeof(b),MOQT_MSG_SUBSCRIBE,3,reserved,2,"t",0,NULL,0);
 moqt_core_rx(&core,5,b,n,false,0);drain();assert(error_code(5)==MOQT_R_DOES_NOT_EXIST);
 /* Nothing published yet: no LARGEST_OBJECT; track properties are present. */
 subscribe(9,5,"preview/jpeg",0,NULL,0);
 assert(sent(9,0,&type,&body,&len,&fin)&&type==MOQT_MSG_SUBSCRIBE_OK);
 moqt_rd r=moqt_rd_of(body,len);assert(moqt_rd_vi(&r)==1&&moqt_rd_vi(&r)==0);
 uint64_t v;
 assert(moqt_props_find_int(r.p,r.n,MOQT_X_DEFAULT_PUBLISHER_PRIORITY,&v)&&v==192);
 assert(moqt_props_find_int(r.p,r.n,MOQT_X_DEFAULT_PUBLISHER_GROUP_ORDER,&v)&&v==MOQT_ORDER_DESCENDING);
 assert(moqt_props_find_int(r.p,r.n,MOQT_X_OBJECT_DELIVERY_TIMEOUT,&v)&&v==2000);
 /* Delivery carries the effective values. */
 assert(offer(0,7,0,0,NULL,0,d)==1&&d[0].sub==0&&d[0].alias==1);
 assert(d[0].subscriber_priority==128&&d[0].publisher_priority==192&&d[0].group_order==MOQT_ORDER_DESCENDING);
 assert(d[0].object_timeout_ms==2000&&!d[0].datagram&&d[0].head_len&&d[0].head[0]==0x78);
 /* A second subscription to the same track with its own parameters: priority 5, ascending,
  * OBJECT_DELIVERY_TIMEOUT 500, INCLUDE_PROPERTIES 0. */
 const uint8_t p2[]={0x02,0x81,0xF4, 0x1E,0x05, 0x02,0x01, 0x13,0x00};
 subscribe(13,7,"preview/jpeg",4,p2,sizeof(p2));
 assert(sent(13,0,&type,&body,&len,&fin)&&type==MOQT_MSG_SUBSCRIBE_OK);
 r=moqt_rd_of(body,len);assert(moqt_rd_vi(&r)==1&&moqt_rd_vi(&r)==1);
 assert(moqt_rd_vi(&r)==MOQT_P_LARGEST_OBJECT&&moqt_rd_vi(&r)==7&&moqt_rd_vi(&r)==0&&!r.n);
 assert(offer(0,8,0,0,NULL,0,d)==2);
 assert(d[1].sub==1&&d[1].subscriber_priority==5&&d[1].group_order==MOQT_ORDER_ASCENDING&&d[1].object_timeout_ms==500);
 moqt_core_stream_opened(&core,0);moqt_core_stream_opened(&core,1);moqt_core_stream_opened(&core,1);
 assert(core.subs[1].streams==2&&moqt_core_active_subs(&core,0)==2);
 /* Forward 0 delivers nothing until an update sets Forward 1. */
 const uint8_t fwd0[]={0x10,0x00};subscribe(17,9,"ai/events",1,fwd0,2);
 assert(offer(1,1,0,0,NULL,0,d)==0&&!moqt_core_any_forwarding(&core,1));
 const uint8_t fwd1[]={0x10,0x01,0x10,0x02};n=update(b,sizeof(b),11,2,fwd1,4);
 moqt_core_rx(&core,17,b,n,false,0);drain();
 assert(sent(17,0,&type,&body,&len,&fin)&&type==MOQT_MSG_REQUEST_OK&&!fin&&count(MOQT_ACT_UPDATED)==1);
 r=moqt_rd_of(body,len);assert(moqt_rd_vi(&r)==1&&moqt_rd_vi(&r)==MOQT_P_LARGEST_OBJECT&&moqt_rd_vi(&r)==1&&moqt_rd_vi(&r)==0&&!r.n);
 assert(offer(1,2,0,0,NULL,0,d)==1&&d[0].subscriber_priority==2&&d[0].publisher_priority==64);
 /* An explicit publisher priority is written into the subgroup header. */
 moqt_offer o={.group=3,.priority=16,.has_priority=true,.payload_len=10};
 assert(moqt_core_publish(&core,1,&o,0,d,4)==1&&!(d[0].head[0]&MOQT_SG_DEFAULT_PRIORITY)&&d[0].head[3]==16);
 /* No free subscription slot. */
 for(unsigned i=0;i<MOQT_CORE_SUBS;i++)subscribe(21+4*i,13+2*i,"evidence/jpeg",0,NULL,0);
 assert(error_code(21+4*(MOQT_CORE_SUBS-1))==MOQT_R_EXCESSIVE_LOAD);
 /* An object-property timeout overrides the track value for that subgroup (G4). */
 fresh(true);subscribe(1,1,"preview/jpeg",0,NULL,0);
 const uint8_t tprop[]={0x02,0x64};
 assert(offer(0,1,0,0,tprop,2,d)==1&&d[0].object_timeout_ms==100&&(d[0].head[0]&MOQT_SG_PROPERTIES));
 /* Invalid object descriptions are refused. */
 const uint8_t bad[]={0x3C,0x01,0x00,0x01};
 assert(offer(0,2,0,0,bad,4,d)==-1);
 o=(moqt_offer){.group=9,.status=MOQT_STATUS_END_OF_GROUP,.payload_len=1};
 assert(moqt_core_publish(&core,0,&o,0,d,4)==-1);
}

static void test_filters(void){ /* D1-D7 applied to delivery */
 moqt_delivery d[4];fresh(true);
 /* Results with a detection count of at least 1 (application property 0x7A). */
 const uint8_t f[]={0x28,0x03,0x00,0x7A,0x01};subscribe(1,1,"ai/events",1,f,sizeof(f));
 const uint8_t none[]={0x7A,0x00},two[]={0x7A,0x02};
 assert(offer(1,1,0,0,none,2,d)==0&&offer(1,2,0,0,two,2,d)==1&&offer(1,3,0,0,NULL,0,d)==0);
 /* Images of subgroup 0 only, groups 5 through 6. */
 const uint8_t g[]={0x21,0x03,0x05,0x00,0x01, 0x04,0x03,0x00,0x00,0x00};subscribe(5,3,"preview/jpeg",2,g,sizeof(g));
 assert(offer(0,4,0,0,NULL,0,d)==0&&offer(0,5,0,0,NULL,0,d)==1&&offer(0,5,1,1,NULL,0,d)==0);
 assert(offer(0,6,0,0,NULL,0,d)==1&&offer(0,7,0,0,NULL,0,d)==0);
 /* A subscription that starts at the Next Group. */
 const uint8_t next[]={0x21,0x01,0x00};subscribe(9,5,"evidence/jpeg",1,next,3);
 assert(offer(2,0,0,0,NULL,0,d)==1);
 /* More ranges than this endpoint advertised. */
 uint8_t many[64];moqt_wr w=moqt_wr_of(many,sizeof(many));moqt_wr_vi(&w,0x26);
 uint8_t body[32];moqt_wr bw=moqt_wr_of(body,sizeof(body));moqt_wr_u8(&bw,0);
 for(int i=0;i<9;i++){moqt_wr_vi(&bw,1);moqt_wr_vi(&bw,0);}
 moqt_wr_vi(&w,bw.n);moqt_wr_bytes(&w,body,bw.n);
 subscribe(13,7,"ai/events",1,many,w.n);assert(error_code(13)==MOQT_R_INVALID_FILTER&&!core.closed);
}

static void test_update_failure_and_done(void){ /* C6, C7, C8, A6 */
 uint64_t type;const uint8_t *body;size_t len;bool fin;uint8_t b[256];
 fresh(true);subscribe(1,1,"ai/events",0,NULL,0);moqt_core_stream_opened(&core,0);
 const uint8_t odd[]={0x28,0x04,0x00,0x79,1,0};size_t n=update(b,sizeof(b),3,1,odd,6);
 moqt_core_rx(&core,1,b,n,false,0);drain();
 assert(sent(1,0,&type,&body,&len,&fin)&&type==MOQT_MSG_REQUEST_ERROR&&!fin);
 const moqt_action *e=find(MOQT_ACT_SUB_ENDING,0);assert(e&&e->index==0&&e->code==MOQT_D_UPDATE_FAILED);
 moqt_delivery d[4];assert(offer(1,1,0,0,NULL,0,d)==0); /* an ending subscription delivers nothing */
 moqt_core_sub_finish(&core,0,"x",1);drain();
 assert(sent(1,0,&type,&body,&len,&fin)&&type==MOQT_MSG_PUBLISH_DONE&&fin);
 const uint8_t done[]={MOQT_D_UPDATE_FAILED,1,1,'x'};assert(len==4&&!memcmp(body,done,4));
 assert(!moqt_core_active_subs(&core,1));
 /* Messages that race with PUBLISH_DONE are absorbed; the entry goes away on FIN. */
 n=update(b,sizeof(b),5,0,NULL,0);moqt_core_rx(&core,1,b,n,true,0);drain();
 assert(nacts==1&&acts[0].kind==MOQT_ACT_REQUEST_CLOSED&&acts[0].stream==1&&!core.closed);
 /* Track end: every subscription ends with TRACK_ENDED and the stream count. */
 fresh(true);subscribe(1,1,"ai/events",0,NULL,0);subscribe(5,3,"ai/events",0,NULL,0);
 moqt_core_stream_opened(&core,1);moqt_core_stream_opened(&core,1);
 moqt_core_track_end(&core,1);drain();assert(count(MOQT_ACT_SUB_ENDING)==2);
 moqt_core_sub_finish(&core,1,NULL,0);drain();
 assert(sent(5,0,&type,&body,&len,&fin)&&fin&&body[0]==MOQT_D_TRACK_ENDED&&body[1]==2&&body[2]==0);
 subscribe(9,5,"ai/events",0,NULL,0);assert(error_code(9)==MOQT_R_DOES_NOT_EXIST);
 /* The subscriber cancels: state is released without PUBLISH_DONE. */
 fresh(true);subscribe(1,1,"ai/events",0,NULL,0);
 moqt_core_stream_cancelled(&core,1);drain();
 assert(find(MOQT_ACT_SUB_CANCELLED,0)->index==0&&!moqt_core_active_subs(&core,1));
 /* The peer closes its half: reported, subscription stays. */
 fresh(true);subscribe(1,1,"ai/events",0,NULL,0);moqt_core_rx(&core,1,NULL,0,true,0);drain();
 assert(count(MOQT_ACT_PEER_FIN)==1&&moqt_core_active_subs(&core,1)==1);
 /* More pipelined updates than MAX_REQUEST_UPDATES. */
 fresh(true);subscribe(1,1,"ai/events",0,NULL,0);
 uint8_t burst[128];size_t bn=0;
 for(unsigned i=0;i<MOQT_CORE_MAX_UPDATES+1;i++)bn+=update(burst+bn,sizeof(burst)-bn,3+2*i,0,NULL,0);
 moqt_core_rx(&core,1,burst,bn,false,0);drain();
 assert(find(MOQT_ACT_CLOSE_SESSION,0)->code==MOQT_S_TOO_MANY_REQUEST_UPDATES);
 /* An unknown parameter in an update closes the session. */
 fresh(true);subscribe(1,1,"ai/events",0,NULL,0);
 const uint8_t unk[]={0x01,0x00};n=update(b,sizeof(b),3,1,unk,2);moqt_core_rx(&core,1,b,n,false,0);drain();
 assert(find(MOQT_ACT_CLOSE_SESSION,0)->code==MOQT_S_PROTOCOL_VIOLATION);
}

typedef struct { uint64_t group,object,subgroup,len; uint8_t priority; bool datagram; size_t props_len; } fo;
/* Independent reader for the fetch object serialization (11.4.1). */
static size_t fetch_object_read(const uint8_t *b,size_t n,bool descending,bool *first,fo *prev,fo *o){
 moqt_rd r=moqt_rd_of(b,n);uint64_t flags=moqt_rd_vi(&r);
 assert(flags<128);
 *o=*prev;o->props_len=0;
 if(flags&MOQT_FO_GROUP_ID){
  uint64_t d=moqt_rd_vi(&r);
  o->group=*first?d:descending?prev->group-d-1:prev->group+d+1;
 }else assert(!*first);
 if(flags&MOQT_FO_DATAGRAM)o->datagram=true;
 else{
  o->datagram=false;unsigned m=flags&MOQT_FO_SUBGROUP_MASK;
  if(m==0)o->subgroup=0;else if(m==2)o->subgroup=prev->subgroup+1;else if(m==3)o->subgroup=moqt_rd_vi(&r);
 }
 if(flags&MOQT_FO_OBJECT_ID){
  uint64_t d=moqt_rd_vi(&r);assert(!*first||(flags&MOQT_FO_GROUP_ID));
  o->object=(flags&MOQT_FO_GROUP_ID)?d:prev->object+d;
 }else o->object=prev->object+1;
 if(flags&MOQT_FO_PRIORITY)o->priority=moqt_rd_u8(&r);else assert(!*first);
 if(flags&MOQT_FO_PROPERTIES){o->props_len=(size_t)moqt_rd_vi(&r);moqt_rd_bytes(&r,o->props_len);}
 o->len=moqt_rd_vi(&r);
 assert(!r.bad);
 *first=false;*prev=*o;
 return n-r.n;
}
static unsigned fetch_all(unsigned k,uint64_t request_id,bool descending,fo *out,void **handles){
 uint8_t head[128];size_t hl,pl;void *payload;unsigned n=0;bool first=true;fo prev={0};
 int rv;bool header=false;
 while((rv=moqt_core_fetch_next(&core,k,head,sizeof(head),&hl,&payload,&pl))==1){
  size_t at=0;
  if(!header){assert(hl>=2&&head[0]==MOQT_STREAM_FETCH_HEADER&&head[1]==request_id);at=2;header=true;}
  if(at==hl){assert(!payload);continue;}
  size_t used=fetch_object_read(head+at,hl-at,descending,&first,&prev,&out[n]);
  assert(used==hl-at&&out[n].len==pl);
  handles[n++]=payload;
 }
 assert(rv==0&&header);
 return n;
}
static void fetch(int64_t stream,uint64_t id,const char *name,unsigned count,const uint8_t *params,size_t plen){
 uint8_t b[256];size_t n=request(b,sizeof(b),MOQT_MSG_FETCH,id,NS,2,name,count,params,plen);
 moqt_core_rx(&core,stream,b,n,false,0);drain();
}
static void test_fetch(void){ /* E1-E7, E9 */
 uint64_t type;const uint8_t *body;size_t len;bool fin;fo o[16];void *h[16];static int tags[16];
 fresh(true);
 fetch(1,1,"evidence/jpeg",0,NULL,0);assert(error_code(1)==MOQT_R_INVALID_RANGE); /* nothing published */
 fetch(5,3,"missing",0,NULL,0);assert(error_code(5)==MOQT_R_DOES_NOT_EXIST);
 const uint8_t det[]={0x7A,0x02};
 cache(2,10,0,0,NULL,0,&tags[0]);cache(2,11,0,0,det,2,&tags[1]);cache(2,11,1,1,NULL,0,&tags[2]);
 cache(2,13,0,0,det,2,&tags[3]);cache(1,10,0,0,NULL,0,&tags[4]);
 assert(moqt_core_cache_count(&core,2)==4&&moqt_core_cache_count(&core,99)==5);
 /* Unfiltered, ascending: FETCH_OK names the Largest Object as the end. */
 fetch(9,5,"evidence/jpeg",0,NULL,0);
 assert(sent(9,0,&type,&body,&len,&fin)&&type==MOQT_MSG_FETCH_OK&&!fin);
 moqt_rd r=moqt_rd_of(body,len);assert(moqt_rd_u8(&r)==0&&moqt_rd_vi(&r)==13&&moqt_rd_vi(&r)==0&&moqt_rd_vi(&r)==0);
 const moqt_action *a=find(MOQT_ACT_FETCH_START,0);assert(a&&a->code==5);
 unsigned k=a->index,n=fetch_all(k,5,false,o,h);
 assert(n==4&&o[0].group==10&&o[1].group==11&&o[1].object==0&&o[1].props_len==2&&o[2].object==1&&o[2].subgroup==1&&o[3].group==13);
 assert(h[0]==&tags[0]&&h[3]==&tags[3]&&o[0].priority==128);
 moqt_core_fetch_closed(&core,k);drain();
 assert(nacts==1&&acts[0].kind==MOQT_ACT_SEND&&acts[0].stream==9&&acts[0].fin&&!acts[0].len);
 /* Descending group order, objects ascending inside a group. */
 const uint8_t desc[]={0x22,0x02};fetch(13,7,"evidence/jpeg",1,desc,2);
 k=find(MOQT_ACT_FETCH_START,0)->index;n=fetch_all(k,7,true,o,h);
 assert(n==4&&o[0].group==13&&o[1].group==11&&o[1].object==0&&o[2].group==11&&o[2].object==1&&o[3].group==10);
 moqt_core_fetch_closed(&core,k);drain();
 /* One observation: group 11 only. */
 const uint8_t one[]={0x21,0x03,0x0B,0x00,0x00};fetch(17,9,"evidence/jpeg",1,one,5);
 assert(sent(17,0,&type,&body,&len,&fin));r=moqt_rd_of(body,len);
 assert(moqt_rd_u8(&r)==0&&moqt_rd_vi(&r)==11);
 k=find(MOQT_ACT_FETCH_START,0)->index;n=fetch_all(k,9,false,o,h);
 assert(n==2&&o[0].group==11&&o[1].group==11&&h[0]==&tags[1]);
 moqt_core_fetch_closed(&core,k);drain();
 /* A range without retained objects: header, then FIN. */
 const uint8_t gap[]={0x21,0x03,0x0C,0x00,0x00};fetch(21,11,"evidence/jpeg",1,gap,5);
 k=find(MOQT_ACT_FETCH_START,0)->index;n=fetch_all(k,11,false,o,h);assert(n==0);
 moqt_core_fetch_closed(&core,k);drain();
 /* A start beyond the Largest Object. */
 const uint8_t far[]={0x21,0x02,0x0E,0x00};fetch(25,13,"evidence/jpeg",1,far,4);assert(error_code(25)==MOQT_R_INVALID_RANGE);
 /* Filters apply to fetches: only objects with detections. */
 const uint8_t pf[]={0x28,0x03,0x00,0x7A,0x01};fetch(29,15,"evidence/jpeg",1,pf,5);
 k=find(MOQT_ACT_FETCH_START,0)->index;n=fetch_all(k,15,false,o,h);
 assert(n==2&&o[0].group==11&&o[1].group==13);
 /* Priority update on a fetch, then cancellation. */
 uint8_t b[64];const uint8_t pr[]={0x20,0x01};size_t m=update(b,sizeof(b),17,1,pr,2);
 moqt_core_rx(&core,29,b,m,false,0);drain();
 assert(sent(29,0,&type,&body,&len,&fin)&&type==MOQT_MSG_REQUEST_OK&&core.fetches[k].priority==1);
 moqt_core_stream_cancelled(&core,29);drain();
 assert(find(MOQT_ACT_FETCH_CANCEL,0)->index==k&&core.fetches[k].state==MOQT_FETCH_FREE);
 /* Fetch limit. */
 for(unsigned i=0;i<MOQT_CORE_FETCHES+1;i++)fetch(33+4*i,19+2*i,"evidence/jpeg",0,NULL,0);
 assert(error_code(33+4*MOQT_CORE_FETCHES)==MOQT_R_EXCESSIVE_LOAD);
 /* End Of Track is reported when the track has ended at the Largest Object. */
 fresh(true);cache(1,4,0,0,NULL,0,&tags[0]);
 moqt_offer end={.group=4,.object=1,.status=MOQT_STATUS_END_OF_TRACK};moqt_delivery d[4];
 assert(moqt_core_publish(&core,1,&end,0,d,4)==0&&core.tracks[1].ended);
 fetch(1,1,"ai/events",0,NULL,0);
 assert(sent(1,0,&type,&body,&len,&fin));r=moqt_rd_of(body,len);
 assert(moqt_rd_u8(&r)==1&&moqt_rd_vi(&r)==4&&moqt_rd_vi(&r)==1);
 /* Cache bounds: the oldest object is displaced; duplicates are refused. */
 fresh(true);
 for(unsigned i=0;i<MOQT_CORE_CACHE;i++)assert(!cache(2,i,0,0,NULL,0,&tags[i%16]));
 assert(cache(2,1000,0,0,NULL,0,&tags[1])==&tags[0]);
 moqt_offer dup={.group=1000,.payload_len=1};void *ev;
 assert(moqt_core_cache_put(&core,2,&dup,&tags[2],0,&ev)==-1);
 assert(moqt_core_cache_evict(&core,2)==&tags[1]&&moqt_core_cache_count(&core,2)==MOQT_CORE_CACHE-1);
 /* MAX_CACHE_DURATION of the evidence track is 60 s. */
 assert(!moqt_core_cache_expire(&core,60000)&&moqt_core_cache_expire(&core,60001));
}

static void test_fill(void){ /* E8 */
 fo o[16];void *h[16];static int tags[4];moqt_delivery d[4];
 fresh(true);
 cache(2,5,0,0,NULL,0,&tags[0]);cache(2,6,0,0,NULL,0,&tags[1]);cache(2,7,0,0,NULL,0,&tags[2]);
 /* Subscribe from the Next Object and fill the two most recent groups. */
 const uint8_t p[]={0x21,0x02,0x00,0x00, 0x02,0x03,0x21,0x01,0x02};
 subscribe(1,1,"evidence/jpeg",2,p,sizeof(p));
 const moqt_action *a=find(MOQT_ACT_FETCH_START,0);assert(a&&a->code==1&&core.fetches[a->index].sub==0);
 assert(core.subs[0].streams==1); /* fill fetch streams count towards Stream Count (9.9) */
 unsigned n=fetch_all(a->index,1,false,o,h);
 assert(n==2&&o[0].group==6&&o[1].group==7);
 moqt_core_fetch_closed(&core,a->index);drain();assert(!nacts); /* no FIN on the request stream */
 assert(offer(2,7,1,0,NULL,0,d)==1&&offer(2,8,0,0,NULL,0,d)==1);
 /* A later fill through REQUEST_UPDATE carries the update's Request ID. */
 uint8_t b[64];const uint8_t up[]={0x23,0x05,0x21,0x03,0x05,0x00,0x00};size_t m=update(b,sizeof(b),3,1,up,7);
 moqt_core_rx(&core,1,b,m,false,0);drain();
 a=find(MOQT_ACT_FETCH_START,0);assert(a&&a->code==3);
 n=fetch_all(a->index,3,false,o,h);assert(n==1&&o[0].group==5&&core.subs[0].streams==2);
 /* Ending the subscription cancels its open fill. */
 moqt_core_sub_end(&core,0,MOQT_D_GOING_AWAY);drain();
 assert(count(MOQT_ACT_FETCH_CANCEL)==1&&count(MOQT_ACT_SUB_ENDING)==1);
 /* Forward 0 opens no fill stream; an empty fill range opens none either. */
 fresh(true);cache(2,5,0,0,NULL,0,&tags[0]);
 const uint8_t q[]={0x10,0x00, 0x13,0x03,0x21,0x01,0x02};subscribe(1,1,"evidence/jpeg",2,q,sizeof(q));
 assert(!count(MOQT_ACT_FETCH_START)&&!core.subs[0].streams);
 const uint8_t e[]={0x23,0x04,0x21,0x02,0x09,0x00};subscribe(5,3,"evidence/jpeg",1,e,sizeof(e));
 assert(!count(MOQT_ACT_FETCH_START)&&count(MOQT_ACT_SUBSCRIBED)==1);
 /* A parameter that may not appear inside FILL_PARAMETERS. */
 const uint8_t bad[]={0x23,0x02,0x10,0x01};subscribe(9,5,"evidence/jpeg",1,bad,4);
 assert(find(MOQT_ACT_CLOSE_SESSION,0)->code==MOQT_S_PROTOCOL_VIOLATION);
 /* No fetch slot left: the failure is signalled on a data stream. */
 fresh(true);cache(2,5,0,0,NULL,0,&tags[0]);
 for(unsigned i=0;i<MOQT_CORE_FETCHES;i++)fetch(1+4*i,1+2*i,"evidence/jpeg",0,NULL,0);
 const uint8_t f[]={0x23,0x03,0x21,0x01,0x05};subscribe(41,41,"evidence/jpeg",1,f,5);
 a=find(MOQT_ACT_FILL_FAILED,0);assert(a&&a->len==2&&a->data[0]==MOQT_STREAM_FETCH_HEADER&&a->data[1]==41);
 assert(a->code==MOQT_RST_EXCESSIVE_LOAD);
}

static void test_status_and_namespaces(void){ /* C11, H1-H7, C9, D9 */
 uint64_t type;const uint8_t *body;size_t len;bool fin;uint8_t b[256];moqt_delivery d[4];
 fresh(true);offer(1,4,2,0,NULL,0,d);
 size_t n=request(b,sizeof(b),MOQT_MSG_TRACK_STATUS,1,NS,2,"ai/events",0,NULL,0);
 moqt_core_rx(&core,1,b,n,false,0);drain();
 assert(sent(1,0,&type,&body,&len,&fin)&&type==MOQT_MSG_REQUEST_OK&&fin);
 moqt_rd r=moqt_rd_of(body,len);
 assert(moqt_rd_vi(&r)==1&&moqt_rd_vi(&r)==MOQT_P_LARGEST_OBJECT&&moqt_rd_vi(&r)==4&&moqt_rd_vi(&r)==2);
 uint64_t v;assert(moqt_props_find_int(r.p,r.n,MOQT_X_DEFAULT_PUBLISHER_PRIORITY,&v)&&v==64);
 assert(!moqt_core_active_subs(&core,1));
 n=request(b,sizeof(b),MOQT_MSG_TRACK_STATUS,3,NS,2,"nope",0,NULL,0);
 moqt_core_rx(&core,5,b,n,false,0);drain();assert(error_code(5)==MOQT_R_DOES_NOT_EXIST);
 /* SUBSCRIBE_NAMESPACE with prefix "aiot": two namespaces, suffixes only. */
 const char *prefix[]={"aiot"};
 n=request(b,sizeof(b),MOQT_MSG_SUBSCRIBE_NAMESPACE,5,prefix,1,NULL,0,NULL,0);
 moqt_core_rx(&core,9,b,n,false,0);drain();
 assert(sent(9,0,&type,&body,&len,&fin)&&type==MOQT_MSG_REQUEST_OK&&!fin&&len==1&&body[0]==0);
 assert(sent(9,1,&type,&body,&len,&fin)&&type==MOQT_MSG_NAMESPACE);
 const uint8_t s1[]={1,5,'e','s','p','3','2'};assert(len==7&&!memcmp(body,s1,7));
 assert(sent(9,2,&type,&body,&len,&fin)&&type==MOQT_MSG_NAMESPACE);
 const uint8_t s2[]={1,6,'s','t','a','t','u','s'};assert(len==8&&!memcmp(body,s2,8));
 assert(!sent(9,3,&type,&body,&len,&fin));
 /* An overlapping prefix is refused; SUBSCRIBE_TRACKS has its own overlap space. */
 n=request(b,sizeof(b),MOQT_MSG_SUBSCRIBE_NAMESPACE,7,NS,2,NULL,0,NULL,0);
 moqt_core_rx(&core,13,b,n,false,0);drain();assert(error_code(13)==MOQT_R_PREFIX_OVERLAP);
 /* NAMESPACE_DONE once every track of a namespace has ended. */
 moqt_core_track_end(&core,3);drain();
 assert(sent(9,0,&type,&body,&len,&fin)&&type==MOQT_MSG_NAMESPACE_DONE&&len==8&&!memcmp(body,s2,8));
 moqt_core_track_end(&core,0);drain();assert(!sent(9,0,&type,&body,&len,&fin)); /* other tracks remain */
 /* SUBSCRIBE_TRACKS for high-priority tracks only (DEFAULT_PUBLISHER_PRIORITY 0..100), Forward 0. */
 const uint8_t tp[]={0x10,0x00, 0x10,0x07, 0x09,0x04,0x00,0x0E,0x00,0x64};
 n=request(b,sizeof(b),MOQT_MSG_SUBSCRIBE_TRACKS,9,NS,2,NULL,3,tp,sizeof(tp));
 moqt_core_rx(&core,17,b,n,false,0);drain();
 assert(sent(17,0,&type,&body,&len,&fin)&&type==MOQT_MSG_REQUEST_OK);
 assert(count(MOQT_ACT_OPEN_BIDI)==1);
 const moqt_action *a=find(MOQT_ACT_OPEN_BIDI,0);size_t used;
 assert(moqt21_message_read(a->data,a->len,&type,&body,&len,&used)==1&&type==MOQT_MSG_PUBLISH);
 r=moqt_rd_of(body,len);moqt_ns ns;
 assert(moqt_rd_vi(&r)==0&&moqt_ns_read(&r,&ns,false)==0&&moqt_rd_vi(&r)==9&&!memcmp(moqt_rd_bytes(&r,9),"ai/events",9));
 assert(moqt_rd_vi(&r)==2); /* track alias */
 assert(moqt_rd_vi(&r)==3&&moqt_rd_vi(&r)==MOQT_P_LARGEST_OBJECT&&moqt_rd_vi(&r)==4&&moqt_rd_vi(&r)==2);
 assert(moqt_rd_vi(&r)==MOQT_P_FORWARD-MOQT_P_LARGEST_OBJECT&&moqt_rd_u8(&r)==0);
 assert(moqt_rd_vi(&r)==MOQT_P_SUBSCRIBER_PRIORITY-MOQT_P_FORWARD&&moqt_rd_u8(&r)==7&&!r.bad);
 unsigned token=a->token;
 moqt_core_bidi_opened(&core,token,0);
 assert(offer(1,5,0,0,NULL,0,d)==0); /* pending and Forward 0 */
 /* PUBLISH_OK sets Forward 1 and a subscriber priority. */
 const uint8_t okp[]={0x01, 0x10,0x01};moqt_wr w=moqt_wr_of(b,sizeof(b));
 size_t mark=moqt_wr_begin_message(&w,MOQT_MSG_REQUEST_OK);moqt_wr_bytes(&w,okp,3);moqt_wr_end_message(&w,mark);
 moqt_core_rx(&core,0,b,w.n,false,0);drain();
 assert(count(MOQT_ACT_SUBSCRIBED)==1&&offer(1,6,0,0,NULL,0,d)==1&&d[0].subscriber_priority==7&&d[0].request_id==0);
 /* The subscriber may update a subscription that PUBLISH established. */
 const uint8_t pr[]={0x20,0x03};n=update(b,sizeof(b),11,1,pr,2);moqt_core_rx(&core,0,b,n,false,0);drain();
 assert(sent(0,0,&type,&body,&len,&fin)&&type==MOQT_MSG_REQUEST_OK&&offer(1,7,0,0,NULL,0,d)==1&&d[0].subscriber_priority==3);
 /* PUBLISH_SKIPPED when no subscription slot is left. */
 fresh(true);
 for(unsigned i=0;i<MOQT_CORE_SUBS;i++)subscribe(1+4*i,1+2*i,"ai/events",0,NULL,0);
 const char *status[]={"aiot","status"};
 n=request(b,sizeof(b),MOQT_MSG_SUBSCRIBE_TRACKS,51,status,2,NULL,0,NULL,0);
 moqt_core_rx(&core,61,b,n,false,0);drain();
 assert(sent(61,1,&type,&body,&len,&fin)&&type==MOQT_MSG_PUBLISH_SKIPPED);
 const uint8_t sk[]={0,6,'h','e','a','l','t','h'};assert(len==8&&!memcmp(body,sk,8));
 /* The subscriber declines a PUBLISH. */
 fresh(true);int k=moqt_core_publish_track(&core,2,true,true);drain();
 assert(k==0&&count(MOQT_ACT_OPEN_BIDI)==1);
 assert(offer(2,1,0,0,NULL,0,d)==0); /* the request stream is not open yet */
 moqt_core_bidi_opened(&core,0,0);
 assert(offer(2,2,0,0,NULL,0,d)==1); /* early delivery before PUBLISH_OK (3.1) */
 n=moqt_request_error_write(b,sizeof(b),MOQT_R_UNINTERESTED,0,"no");moqt_core_rx(&core,0,b,n,true,0);drain();
 assert(find(MOQT_ACT_SUB_CANCELLED,0)->code==MOQT_R_UNINTERESTED&&!moqt_core_active_subs(&core,2));
 /* PUBLISH_NAMESPACE and its response. */
 assert(moqt_core_publish_namespace(&core)==0);drain();
 a=find(MOQT_ACT_OPEN_BIDI,0);assert(a&&a->token==MOQT_CORE_SUBS);
 assert(moqt21_message_read(a->data,a->len,&type,&body,&len,&used)==1&&type==MOQT_MSG_PUBLISH_NAMESPACE&&body[0]==2);
 moqt_core_bidi_opened(&core,a->token,4);
 w=moqt_wr_of(b,sizeof(b));mark=moqt_wr_begin_message(&w,MOQT_MSG_REQUEST_OK);moqt_wr_vi(&w,0);moqt_wr_end_message(&w,mark);
 moqt_core_rx(&core,4,b,w.n,false,0);drain();
 assert(find(MOQT_ACT_NAMESPACE_RESULT,0)->code==0&&core.namespace_state==2);
 assert(moqt_core_publish_namespace(&core)==-1);
 /* A response that is neither REQUEST_OK nor REQUEST_ERROR. */
 fresh(true);moqt_core_publish_namespace(&core);drain();moqt_core_bidi_opened(&core,MOQT_CORE_SUBS,0);
 n=moqt_goaway_write(b,sizeof(b),0);moqt_core_rx(&core,0,b,n,false,0);drain();
 assert(find(MOQT_ACT_CLOSE_SESSION,0)->code==MOQT_S_PROTOCOL_VIOLATION);
 /* The peer offers a track: this endpoint declines. */
 fresh(true);n=moqt_publish_write(b,sizeof(b),1,NS,2,"x",9,NULL,NULL,0);
 moqt_core_rx(&core,1,b,n,false,0);drain();assert(error_code(1)==MOQT_R_UNINTERESTED);
 /* Prefix update on a namespace subscription. */
 fresh(true);n=request(b,sizeof(b),MOQT_MSG_SUBSCRIBE_NAMESPACE,1,status,2,NULL,0,NULL,0);
 moqt_core_rx(&core,1,b,n,false,0);drain();
 assert(sent(1,1,&type,&body,&len,&fin)&&type==MOQT_MSG_NAMESPACE&&len==1&&body[0]==0);
 uint8_t up[32];w=moqt_wr_of(up,sizeof(up));moqt_wr_vi(&w,MOQT_P_TRACK_NAMESPACE_PREFIX);moqt_ns_write(&w,prefix,1);
 n=update(b,sizeof(b),3,1,up,w.n);moqt_core_rx(&core,1,b,n,false,0);drain();
 assert(sent(1,0,&type,&body,&len,&fin)&&type==MOQT_MSG_REQUEST_OK);
 assert(sent(1,1,&type,&body,&len,&fin)&&type==MOQT_MSG_NAMESPACE&&sent(1,2,&type,&body,&len,&fin));
}

static void test_goaway(void){ /* A14 */
 uint8_t b[64];fresh(true);subscribe(1,1,"ai/events",0,NULL,0);
 const uint8_t go[]={0x10,0x00,0x05,0x03,'a','b','c',0x64};
 moqt_core_rx(&core,3,go,sizeof(go),false,0);drain();
 const moqt_action *a=find(MOQT_ACT_GOAWAY,0);
 assert(a&&a->code==100&&a->len==3&&!memcmp(a->data,"abc",3)&&core.going_away);
 assert(moqt_core_active_subs(&core,1)==1); /* GOAWAY does not change subscription state */
 subscribe(5,3,"preview/jpeg",0,NULL,0);assert(error_code(5)==MOQT_R_GOING_AWAY);
 assert(moqt_core_publish_track(&core,0,true,false)==-1);
 moqt_core_rx(&core,3,go,sizeof(go),false,0);drain();
 assert(find(MOQT_ACT_CLOSE_SESSION,0)->code==MOQT_S_PROTOCOL_VIOLATION);
 /* GOAWAY on a request stream ends only that request. */
 fresh(true);subscribe(1,1,"ai/events",0,NULL,0);subscribe(5,3,"preview/jpeg",0,NULL,0);
 size_t n=moqt_goaway_write(b,sizeof(b),0);moqt_core_rx(&core,1,b,n,false,0);drain();
 assert(count(MOQT_ACT_SUB_ENDING)==1&&find(MOQT_ACT_SUB_ENDING,0)->code==MOQT_D_GOING_AWAY&&!core.going_away);
 /* Own GOAWAY has a zero-length URI. */
 assert(moqt_core_goaway(&core,b,sizeof(b),50)==5&&b[3]==0&&b[4]==50);
}

static void test_expiry_and_notify(void){ /* C10, C14 */
 uint64_t type;const uint8_t *body;size_t len;bool fin;uint8_t b[64];
 fresh(true);core.subscription_lifetime_ms=5000;
 uint8_t q[128];size_t n=request(q,sizeof(q),MOQT_MSG_SUBSCRIBE,1,NS,2,"ai/events",0,NULL,0);
 moqt_core_rx(&core,1,q,n,false,1000);drain();
 assert(sent(1,0,&type,&body,&len,&fin)&&type==MOQT_MSG_SUBSCRIBE_OK);
 moqt_rd r=moqt_rd_of(body,len);
 assert(moqt_rd_vi(&r)==2&&moqt_rd_vi(&r)==1&&moqt_rd_vi(&r)==MOQT_P_EXPIRES&&moqt_rd_vi(&r)==5000);
 moqt_core_tick(&core,5999);drain();assert(!nacts);
 /* An update extends the lifetime and reports the new EXPIRES. */
 n=update(b,sizeof(b),3,0,NULL,0);moqt_core_rx(&core,1,b,n,false,5500);drain();
 assert(sent(1,0,&type,&body,&len,&fin)&&type==MOQT_MSG_REQUEST_OK);
 r=moqt_rd_of(body,len);assert(moqt_rd_vi(&r)==1&&moqt_rd_vi(&r)==MOQT_P_EXPIRES&&moqt_rd_vi(&r)==5000&&!r.n);
 moqt_core_tick(&core,6000);drain();assert(!nacts);
 moqt_delivery d[4];assert(offer(1,3,1,0,NULL,0,d)==1);
 assert(moqt_core_state_notify(&core,0)==0);drain();
 assert(sent(1,0,&type,&body,&len,&fin)&&type==MOQT_MSG_PUBLISH_STATE_NOTIFY);
 r=moqt_rd_of(body,len);
 assert(moqt_rd_vi(&r)==2&&moqt_rd_vi(&r)==MOQT_P_LARGEST_OBJECT&&moqt_rd_vi(&r)==3&&moqt_rd_vi(&r)==1);
 assert(moqt_rd_vi(&r)==MOQT_P_FORWARD-MOQT_P_LARGEST_OBJECT&&moqt_rd_u8(&r)==1&&!r.n&&!r.bad);
 assert(moqt_core_state_notify(&core,3)==-1);
 moqt_core_tick(&core,10500);drain();
 assert(count(MOQT_ACT_SUB_ENDING)==1&&find(MOQT_ACT_SUB_ENDING,0)->code==MOQT_D_EXPIRED);
}

/* Extensions proposed for constrained AIoT endpoints (MOQT_AIOT_EXTENSIONS_KO.md). */
static void fresh_ext(const moqt_setup_ext *own,const moqt_setup_ext *peer){
 moqt_core_init(&core,TRACKS_EXT,4,true);
 if(own)moqt_core_extensions(&core,own);
 uint8_t b[160];assert(moqt_core_setup(&core,b,sizeof(b),"/","host:1"));
 size_t n=moqt_setup_write_ext(b,sizeof(b),NULL,NULL,"test-subscriber",4,0,peer);
 moqt_core_rx(&core,3,b,n,false,0);drain();assert(core.peer_setup&&!nacts);
}
static void test_ext_limits(void){ /* X1 */
 uint64_t type;const uint8_t *body;size_t len,used;uint8_t b[1400];moqt_setup_options o;
 moqt_setup_ext own={.has_control_message=true,.max_control_message=200,.has_subscriptions=true,.max_subscriptions=2,
  .has_fetches=true,.max_fetches=1};
 /* Advertised in SETUP; a peer that does not know the options ignores them (section 9.1). */
 moqt_core_init(&core,TRACKS_EXT,4,true);moqt_core_extensions(&core,&own);
 size_t n=moqt_core_setup(&core,b,sizeof(b),"/","host:1");
 assert(moqt21_message_read(b,n,&type,&body,&len,&used)==1&&moqt_setup_read(body,len,false,&o)==0);
 assert(o.ext.has_control_message&&o.ext.max_control_message==200&&o.ext.has_subscriptions&&o.ext.max_subscriptions==2);
 assert(o.ext.has_fetches&&o.ext.max_fetches==1&&!o.ext.source_timeout&&o.max_filter_ranges==8);
 /* Without the extension nothing is advertised. */
 moqt_core_init(&core,TRACKS_EXT,4,true);n=moqt_core_setup(&core,b,sizeof(b),"/","host:1");
 assert(moqt21_message_read(b,n,&type,&body,&len,&used)==1&&moqt_setup_read(body,len,false,&o)==0);
 assert(!o.ext.has_control_message&&!o.ext.has_subscriptions&&!o.ext.has_fetches&&!o.ext.source_timeout);
 /* A value above the capacity of the core is reduced to the capacity. */
 moqt_setup_ext big={.has_control_message=true,.max_control_message=60000,.has_subscriptions=true,.max_subscriptions=99,
  .has_fetches=true,.max_fetches=99};
 moqt_core_init(&core,TRACKS_EXT,4,true);moqt_core_extensions(&core,&big);
 assert(core.ext.max_control_message==MOQT_CORE_RX-16&&core.ext.max_subscriptions==MOQT_CORE_SUBS&&core.ext.max_fetches==MOQT_CORE_FETCHES);
 /* A repeated extension option is a violation like any known option. */
 const uint8_t dup[]={0xC0,0xA1,0xE0,0x10,0x00,0x10};assert(moqt_setup_read(dup,6,true,&o)==MOQT_S_PROTOCOL_VIOLATION);
 /* The stated subscription limit is enforced below the capacity of the core. */
 fresh_ext(&own,NULL);
 subscribe(1,1,"ai/events",0,NULL,0);subscribe(5,3,"preview/jpeg",0,NULL,0);
 assert(count(MOQT_ACT_SUBSCRIBED)==1&&moqt_core_active_subs(&core,1)==1);
 subscribe(9,5,"evidence/jpeg",0,NULL,0);assert(error_code(9)==MOQT_R_EXCESSIVE_LOAD);
 /* Fetch limit 1. */
 static int tag;cache(2,5,0,0,NULL,0,&tag);
 fetch(13,7,"evidence/jpeg",0,NULL,0);assert(count(MOQT_ACT_FETCH_START)==1);
 fetch(17,9,"evidence/jpeg",0,NULL,0);assert(error_code(17)==MOQT_R_EXCESSIVE_LOAD);
 /* A message above the stated size is the peer's violation; one at the limit is served. */
 fresh_ext(&own,NULL);
 uint8_t filler[260];memset(filler,'a',sizeof(filler));
 moqt_wr w=moqt_wr_of(b,sizeof(b));size_t mark=moqt_wr_begin_message(&w,MOQT_MSG_SUBSCRIBE);
 moqt_wr_vi(&w,1);moqt_wr_vi(&w,1);moqt_wr_vi(&w,190);moqt_wr_bytes(&w,filler,190);moqt_wr_vi(&w,1);moqt_wr_u8(&w,'t');moqt_wr_vi(&w,0);
 moqt_wr_end_message(&w,mark);assert(w.n-3==197);
 moqt_core_rx(&core,1,b,w.n,false,0);drain();assert(error_code(1)==MOQT_R_DOES_NOT_EXIST&&!core.closed);
 w=moqt_wr_of(b,sizeof(b));mark=moqt_wr_begin_message(&w,MOQT_MSG_SUBSCRIBE);
 moqt_wr_vi(&w,3);moqt_wr_vi(&w,1);moqt_wr_vi(&w,200);moqt_wr_bytes(&w,filler,200);moqt_wr_vi(&w,1);moqt_wr_u8(&w,'t');moqt_wr_vi(&w,0);
 moqt_wr_end_message(&w,mark);
 moqt_core_rx(&core,5,b,8,false,0);drain(); /* the length field alone decides */
 assert(find(MOQT_ACT_CLOSE_SESSION,0)->code==MOQT_S_PROTOCOL_VIOLATION);
 /* Without the extension the same message only meets the internal buffer limit. */
 fresh_ext(NULL,NULL);moqt_core_rx(&core,5,b,w.n,false,0);drain();
 assert(error_code(5)==MOQT_R_DOES_NOT_EXIST&&!core.closed);
 /* Limits stated by the peer bound what this endpoint requests and sends. */
 moqt_setup_ext peer={.has_subscriptions=true,.max_subscriptions=1,.has_control_message=true,.max_control_message=64};
 fresh_ext(NULL,&peer);
 assert(moqt_core_publish_track(&core,0,true,false)==0&&moqt_core_publish_track(&core,1,true,false)==-1);
 drain();
 subscribe(1,1,"ai/events",0,NULL,0);moqt_core_sub_end(&core,1,MOQT_D_TRACK_ENDED);drain();
 static char reason[400];memset(reason,'r',sizeof(reason));
 moqt_core_sub_finish(&core,1,reason,sizeof(reason));drain();
 bool fin;assert(sent(1,0,&type,&body,&len,&fin)&&type==MOQT_MSG_PUBLISH_DONE&&len==3&&body[2]==0);
}
static void test_ext_source_timeout(void){ /* X2 */
 uint64_t type;const uint8_t *body;size_t len;bool fin;moqt_delivery d[4];uint64_t v;
 moqt_setup_ext own={.source_timeout=true},peer={.source_timeout=true};
 /* Not negotiated: the parameter is unknown and closes the session (section 9.20). */
 fresh_ext(NULL,&peer);
 const uint8_t param[]={0xC0,0xA1,0xEA,0x87,0xD0}; /* SOURCE_DELIVERY_TIMEOUT 2000 */
 subscribe(1,1,"ai/events",1,param,sizeof(param));
 assert(find(MOQT_ACT_CLOSE_SESSION,0)->code==MOQT_S_PROTOCOL_VIOLATION);
 /* Negotiated: the track property is announced and the parameter is accepted. */
 fresh_ext(&own,&peer);
 subscribe(1,1,"evidence/jpeg",0,NULL,0);
 assert(sent(1,0,&type,&body,&len,&fin)&&type==MOQT_MSG_SUBSCRIBE_OK);
 moqt_rd r=moqt_rd_of(body,len);moqt_rd_vi(&r);moqt_rd_vi(&r);
 assert(moqt_props_find_int(r.p,r.n,MOQT_X_SOURCE_DELIVERY_TIMEOUT,&v)&&v==5000);
 subscribe(5,3,"evidence/jpeg",1,param,sizeof(param));assert(!core.closed&&core.subs[1].source_timeout_ms==2000);
 /* Age 1500 ms at the offer: both subscriptions receive it; the smaller timeout applies. */
 const uint8_t app[]={0x78,0x07};
 moqt_offer o={.group=1,.props=app,.props_len=2,.payload_len=10,.end_of_group=true,.has_source_age=true,.source_age_ms=1500};
 assert(moqt_core_publish(&core,2,&o,0,d,4)==2);
 assert(d[0].source_timeout_ms==5000&&d[1].source_timeout_ms==2000&&d[0].source_age_ms==1500);
 /* The header carries SOURCE_AGE after the application properties. */
 const uint8_t expect[]={0x79,0x03,0x01, 0x00, 0x07,0x78,0x07,0xC0,0xA1,0x74,0x85,0xDC, 0x0A};
 assert(d[0].head_len==sizeof(expect)&&!memcmp(d[0].head,expect,sizeof(expect)));
 /* Age 2400 ms: too old for the 2000 ms subscription before it is even offered. */
 o.group=2;o.source_age_ms=2400;
 assert(moqt_core_publish(&core,2,&o,0,d,4)==1&&d[0].sub==0&&core.source_expired==1);
 o.group=3;o.source_age_ms=5000;
 assert(moqt_core_publish(&core,2,&o,0,d,4)==0&&core.source_expired==3);
 /* An object property overrides the track value for that object. */
 const uint8_t tight[]={0xC0,0xA1,0xEE,0x64};
 o=(moqt_offer){.group=4,.props=tight,.props_len=4,.payload_len=10,.has_source_age=true,.source_age_ms=99};
 assert(moqt_core_publish(&core,2,&o,0,d,4)==-1); /* SOURCE_AGE must stay the largest type */
 /* Subscribers can select on the age with OBJECT_PROPERTY_FILTER. */
 fresh_ext(&own,&peer);
 const uint8_t young[]={0x28,0x07,0x00,0xC0,0xA1,0xEC,0x00,0x83,0xE8}; /* SOURCE_AGE in 0..1000 */
 subscribe(1,1,"ai/events",1,young,sizeof(young));
 o=(moqt_offer){.group=1,.payload_len=10,.has_source_age=true,.source_age_ms=400};
 assert(moqt_core_publish(&core,1,&o,0,d,4)==1);
 o.group=2;o.source_age_ms=1001;assert(moqt_core_publish(&core,1,&o,0,d,4)==0);
 /* A fetched object reports the age at the time it is served. */
 fresh_ext(&own,&peer);static int tag;void *ev;
 o=(moqt_offer){.group=9,.props=app,.props_len=2,.payload_len=10,.has_source_age=true,.source_age_ms=300};
 assert(moqt_core_publish(&core,2,&o,1000,d,4)==0&&moqt_core_cache_put(&core,2,&o,&tag,1000,&ev)==0);
 uint8_t q[128];size_t n=request(q,sizeof(q),MOQT_MSG_FETCH,1,NS,2,"evidence/jpeg",0,NULL,0);
 moqt_core_rx(&core,1,q,n,false,4000);drain();
 uint8_t head[128];size_t hl,pl;void *payload;
 assert(moqt_core_fetch_next(&core,find(MOQT_ACT_FETCH_START,0)->index,head,sizeof(head),&hl,&payload,&pl)==1&&payload==&tag);
 /* header 05 01; flags; group 9; object 0; priority 128; properties; length */
 r=moqt_rd_of(head+2,hl-2);
 assert(moqt_rd_vi(&r)==(MOQT_FO_GROUP_ID|MOQT_FO_OBJECT_ID|MOQT_FO_PRIORITY|MOQT_FO_PROPERTIES));
 assert(moqt_rd_vi(&r)==9&&moqt_rd_vi(&r)==0&&moqt_rd_u8(&r)==128);
 size_t pn=(size_t)moqt_rd_vi(&r);const uint8_t *pp=moqt_rd_bytes(&r,pn);
 assert(moqt_props_find_int(pp,pn,MOQT_X_SOURCE_AGE,&v)&&v==3300&&moqt_props_find_int(pp,pn,0x78,&v)&&v==7);
 /* With the extension off the age is neither sent nor enforced. */
 fresh_ext(NULL,NULL);subscribe(1,1,"evidence/jpeg",0,NULL,0);
 o=(moqt_offer){.group=1,.payload_len=10,.end_of_group=true,.has_source_age=true,.source_age_ms=999999};
 assert(moqt_core_publish(&core,2,&o,0,d,4)==1&&d[0].head[0]==0x78&&!d[0].source_timeout_ms&&!core.source_expired);
}

static void test_session_reset(void){ /* M3 */
 static int tags[2];fo o[4];void *h[4];uint64_t type;const uint8_t *body;size_t len;bool fin;
 fresh(true);subscribe(1,1,"ai/events",0,NULL,0);
 cache(1,3,0,0,NULL,0,&tags[0]);cache(1,29,0,0,NULL,0,&tags[1]);
 moqt_core_session_reset(&core);
 assert(!core.peer_setup&&!moqt_core_active_subs(&core,1)&&moqt_core_cache_count(&core,1)==2);
 /* A result produced while no session exists is registered and retained. */
 static int late;moqt_offer off={.group=55,.end_of_group=true,.payload_len=1};moqt_delivery none[1];
 assert(moqt_core_publish(&core,1,&off,0,none,0)==0);
 cache(1,55,0,0,NULL,0,&late);
 /* The next session starts with Request ID 1 again and sees the earlier objects. */
 uint8_t b[64];assert(moqt_core_setup(&core,b,sizeof(b),"/","host:1"));
 size_t n=moqt_setup_write(b,sizeof(b),NULL,NULL,"s",4,0);moqt_core_rx(&core,3,b,n,false,0);drain();
 subscribe(1,1,"ai/events",0,NULL,0);
 assert(sent(1,0,&type,&body,&len,&fin)&&type==MOQT_MSG_SUBSCRIBE_OK);
 moqt_rd r=moqt_rd_of(body,len);
 assert(moqt_rd_vi(&r)==2&&moqt_rd_vi(&r)==1&&moqt_rd_vi(&r)==MOQT_P_LARGEST_OBJECT&&moqt_rd_vi(&r)==55);
 fetch(5,3,"ai/events",0,NULL,0);
 unsigned k=find(MOQT_ACT_FETCH_START,0)->index;
 assert(fetch_all(k,3,false,o,h)==3&&o[0].group==3&&o[1].group==29&&h[1]==&tags[1]&&o[2].group==55&&h[2]==&late);
}

int main(void){
 test_session_reset();
 test_ext_limits();test_ext_source_timeout();
 test_expiry_and_notify();
 test_setup_and_buffering();test_request_ids();test_subscribe();test_filters();test_update_failure_and_done();
 test_fetch();test_fill();test_status_and_namespaces();test_goaway();
 puts("PASS: draft21 publisher core: session, subscriptions, filters, fetch, fill, namespaces, goaway, expiry, extensions");
 return 0;
}
