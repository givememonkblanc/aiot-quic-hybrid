#include "moqt_core.h"
#include <string.h>

static void emit(moqt_core *c,moqt_action a,const uint8_t *data,size_t len){
 if(c->closed&&a.kind!=MOQT_ACT_CLOSE_SESSION)return;
 bool full=c->act_count==MOQT_CORE_ACTIONS||len>MOQT_CORE_OUT-c->out_len;
 if(full&&a.kind!=MOQT_ACT_CLOSE_SESSION){
  c->closed=true;c->close_code=MOQT_S_INTERNAL_ERROR;
  a=(moqt_action){.kind=MOQT_ACT_CLOSE_SESSION,.code=MOQT_S_INTERNAL_ERROR};data=NULL;len=0;
 }
 if(c->act_count==MOQT_CORE_ACTIONS)c->act_count--; /* the close replaces the newest action */
 if(len){memcpy(c->out+c->out_len,data,len);a.data=c->out+c->out_len;a.len=len;c->out_len+=len;}
 c->actions[(c->act_head+c->act_count)%MOQT_CORE_ACTIONS]=a;c->act_count++;
}
bool moqt_core_next_action(moqt_core *c,moqt_action *a){
 if(!c->act_count){c->out_len=0;c->act_head=0;return false;}
 *a=c->actions[c->act_head];c->act_head=(c->act_head+1)%MOQT_CORE_ACTIONS;c->act_count--;
 return true;
}
static void session_close(moqt_core *c,uint64_t code){
 if(c->closed)return;
 c->closed=true;c->close_code=code;
 emit(c,(moqt_action){.kind=MOQT_ACT_CLOSE_SESSION,.code=code},NULL,0);
}
static void send_on(moqt_core *c,int64_t stream,const uint8_t *data,size_t len,bool fin){
 if(!len&&!fin)return;
 emit(c,(moqt_action){.kind=MOQT_ACT_SEND,.stream=stream,.fin=fin},data,len);
}

static moqt_stream *stream_find(moqt_core *c,int64_t id){
 for(unsigned i=0;i<MOQT_CORE_STREAMS;i++)if(c->streams[i].kind!=MOQT_STREAM_FREE&&c->streams[i].id==id)return &c->streams[i];
 return NULL;
}
static moqt_stream *stream_take(moqt_core *c,int64_t id,uint8_t kind){
 moqt_stream *s=NULL,*oldest=NULL;
 for(unsigned i=0;i<MOQT_CORE_STREAMS;i++){
  moqt_stream *x=&c->streams[i];
  if(x->kind==MOQT_STREAM_FREE){s=x;break;}
  if((x->kind==MOQT_STREAM_CLOSED||x->kind==MOQT_STREAM_IGNORED)&&(!oldest||x->serial<oldest->serial))oldest=x;
 }
 if(!s)s=oldest;
 if(!s)return NULL;
 memset(s,0,sizeof(*s));s->kind=kind;s->id=id;s->serial=++c->stream_serial;
 return s;
}
static void stream_done(moqt_core *c,moqt_stream *s){
 /* Keep the entry until the peer finishes so that late messages are absorbed. */
 if(s->peer_fin){
  s->kind=MOQT_STREAM_FREE;
  emit(c,(moqt_action){.kind=MOQT_ACT_REQUEST_CLOSED,.stream=s->id},NULL,0);
 }else{s->kind=MOQT_STREAM_CLOSED;s->len=0;}
}
static void respond_error(moqt_core *c,moqt_stream *s,uint64_t code,uint64_t retry,const char *reason){
 uint8_t b[160];size_t n=moqt_request_error_write(b,sizeof(b),code,retry,reason);
 send_on(c,s->id,b,n,true);
 stream_done(c,s);
}

static bool peer_request_id_ok(moqt_core *c,uint64_t id){
 /* Section 6.4.2.1: the client uses even IDs, the server odd IDs. */
 if((id&1)!=(c->is_client?1U:0U))return false;
 uint64_t slot=id>>1;
 if(!c->peer_request_any){
  c->peer_request_any=true;c->peer_request_max=slot;memset(c->peer_request_seen,0,sizeof(c->peer_request_seen));
  c->peer_request_seen[0]=1;return true;
 }
 if(slot>c->peer_request_max){
  uint64_t shift=slot-c->peer_request_max;
  if(shift>=256)memset(c->peer_request_seen,0,sizeof(c->peer_request_seen));
  else for(uint64_t k=0;k<shift;k++){
   for(int w=3;w>0;w--)c->peer_request_seen[w]=(c->peer_request_seen[w]<<1)|(c->peer_request_seen[w-1]>>63);
   c->peer_request_seen[0]<<=1;
  }
  c->peer_request_seen[0]|=1;c->peer_request_max=slot;return true;
 }
 uint64_t back=c->peer_request_max-slot;
 if(back>=256)return false;
 uint64_t bit=UINT64_C(1)<<(back&63);
 if(c->peer_request_seen[back>>6]&bit)return false;
 c->peer_request_seen[back>>6]|=bit;return true;
}

static uint8_t track_priority(const moqt_track *t){return t->cfg.priority?t->cfg.priority:128;}
static uint8_t track_order(const moqt_track *t){return t->cfg.group_order?t->cfg.group_order:MOQT_ORDER_ASCENDING;}
static void track_props_build(moqt_track *t,bool with_source){
 moqt_wr w=moqt_wr_of(t->props,sizeof(t->props));uint64_t prev=0;
 if(t->cfg.object_timeout_ms)moqt_kvp_put_int(&w,&prev,MOQT_X_OBJECT_DELIVERY_TIMEOUT,t->cfg.object_timeout_ms);
 if(t->cfg.max_cache_ms)moqt_kvp_put_int(&w,&prev,MOQT_X_MAX_CACHE_DURATION,t->cfg.max_cache_ms);
 if(t->cfg.subgroup_timeout_ms)moqt_kvp_put_int(&w,&prev,MOQT_X_SUBGROUP_DELIVERY_TIMEOUT,t->cfg.subgroup_timeout_ms);
 if(t->cfg.priority&&t->cfg.priority!=128)moqt_kvp_put_int(&w,&prev,MOQT_X_DEFAULT_PUBLISHER_PRIORITY,t->cfg.priority);
 if(t->cfg.group_order==MOQT_ORDER_DESCENDING)moqt_kvp_put_int(&w,&prev,MOQT_X_DEFAULT_PUBLISHER_GROUP_ORDER,MOQT_ORDER_DESCENDING);
 if(t->cfg.dynamic_groups)moqt_kvp_put_int(&w,&prev,MOQT_X_DYNAMIC_GROUPS,1);
 if(with_source&&t->cfg.source_timeout_ms)moqt_kvp_put_int(&w,&prev,MOQT_X_SOURCE_DELIVERY_TIMEOUT,t->cfg.source_timeout_ms);
 t->props_len=w.bad?0:(uint8_t)w.n;
}
void moqt_core_init(moqt_core *c,const moqt_track_cfg *tracks,unsigned count,bool is_client){
 memset(c,0,sizeof(*c));
 c->is_client=is_client;c->next_request_id=is_client?0:1;c->namespace_stream=-1;
 if(count>MOQT_CORE_TRACKS)count=MOQT_CORE_TRACKS;
 c->track_count=count;
 for(unsigned i=0;i<count;i++){c->tracks[i].cfg=tracks[i];track_props_build(&c->tracks[i],false);}
}
void moqt_core_session_reset(moqt_core *c){
 c->setup_sent=c->peer_setup=c->going_away=c->closed=false;c->close_code=0;
 memset(&c->peer,0,sizeof(c->peer));
 memset(c->subs,0,sizeof(c->subs));memset(c->fetches,0,sizeof(c->fetches));
 memset(c->ns_subs,0,sizeof(c->ns_subs));memset(c->streams,0,sizeof(c->streams));
 c->next_request_id=c->is_client?0:1;c->peer_request_any=false;
 c->namespace_stream=-1;c->namespace_state=0;
 c->act_head=c->act_count=0;c->out_len=0;
}
void moqt_core_extensions(moqt_core *c,const moqt_setup_ext *x){
 c->ext=*x;
 const uint64_t body=MOQT_CORE_RX-16;
 if(c->ext.has_control_message&&(!c->ext.max_control_message||c->ext.max_control_message>body))c->ext.max_control_message=body;
 if(c->ext.has_subscriptions&&c->ext.max_subscriptions>MOQT_CORE_SUBS)c->ext.max_subscriptions=MOQT_CORE_SUBS;
 if(c->ext.has_fetches&&c->ext.max_fetches>MOQT_CORE_FETCHES)c->ext.max_fetches=MOQT_CORE_FETCHES;
 for(unsigned i=0;i<c->track_count;i++)track_props_build(&c->tracks[i],c->ext.source_timeout);
}
static unsigned parse_limits(const moqt_core *c){
 return MOQT_MAX_RANGES|(c->ext.source_timeout?MOQT_LIMIT_SOURCE_TIMEOUT:0U);
}
size_t moqt_core_setup(moqt_core *c,uint8_t *dst,size_t cap,const char *path,const char *authority){
 size_t n=moqt_setup_write_ext(dst,cap,c->is_client?path:NULL,c->is_client?authority:NULL,MOQT_CORE_IMPLEMENTATION,
  MOQT_MAX_RANGES,MOQT_CORE_MAX_UPDATES,&c->ext);
 if(n)c->setup_sent=true;
 return n;
}
int moqt_core_goaway(moqt_core *c,uint8_t *dst,size_t cap,uint64_t timeout_ms){
 size_t n=moqt_goaway_write(dst,cap,timeout_ms);
 if(n)c->going_away=true;
 return (int)n;
}

/* -2 reserved or session-level namespace, -1 unknown, otherwise the track index. */
static int track_find(const moqt_core *c,const moqt_ns *ns,const uint8_t *name,size_t name_len){
 if(ns->count&&ns->f[0].n&&ns->f[0].p[0]=='.')return -2;
 for(unsigned i=0;i<c->track_count;i++){
  const moqt_track_cfg *t=&c->tracks[i].cfg;
  if(moqt_ns_equals(ns,t->ns,t->ns_count)&&strlen(t->name)==name_len&&!memcmp(t->name,name,name_len))return (int)i;
 }
 return -1;
}
static unsigned subs_in_use(const moqt_core *c,bool by_publish){
 unsigned n=0;
 for(unsigned i=0;i<MOQT_CORE_SUBS;i++)n+=c->subs[i].state!=MOQT_SUB_FREE&&c->subs[i].by_publish==by_publish;
 return n;
}
static int sub_alloc(moqt_core *c){
 if(c->ext.has_subscriptions&&subs_in_use(c,false)+subs_in_use(c,true)>=c->ext.max_subscriptions)return -1;
 for(unsigned i=0;i<MOQT_CORE_SUBS;i++)if(c->subs[i].state==MOQT_SUB_FREE){memset(&c->subs[i],0,sizeof(c->subs[i]));return (int)i;}
 return -1;
}
static int fetch_alloc(moqt_core *c){
 if(c->ext.has_fetches){
  unsigned n=0;
  for(unsigned i=0;i<MOQT_CORE_FETCHES;i++)n+=c->fetches[i].state!=MOQT_FETCH_FREE;
  if(n>=c->ext.max_fetches)return -1;
 }
 for(unsigned i=0;i<MOQT_CORE_FETCHES;i++)if(c->fetches[i].state==MOQT_FETCH_FREE){memset(&c->fetches[i],0,sizeof(c->fetches[i]));return (int)i;}
 return -1;
}
static void fills_cancel(moqt_core *c,unsigned sub){
 for(unsigned i=0;i<MOQT_CORE_FETCHES;i++){
  moqt_fetch *f=&c->fetches[i];
  if(f->state!=MOQT_FETCH_FREE&&f->sub==(int)sub){
   emit(c,(moqt_action){.kind=MOQT_ACT_FETCH_CANCEL,.index=i,.code=MOQT_RST_CANCELLED},NULL,0);
   f->state=MOQT_FETCH_FREE;
  }
 }
}
static void strip_track_filters(moqt_filters *f){
 moqt_filters out={0};
 for(unsigned i=0;i<f->count;i++){
  if(f->f[i].type==MOQT_P_TRACK_PROPERTY_FILTER)continue;
  moqt_rfilter x=f->f[i];x.first=out.ranges;
  for(unsigned k=0;k<f->f[i].count;k++)out.r[out.ranges++]=f->r[f->f[i].first+k];
  out.f[out.count++]=x;
 }
 *f=out;
}
static bool track_filter_pass(const moqt_filters *f,const moqt_track *t){
 bool any=false;
 for(unsigned i=0;i<f->count;i++){
  if(f->f[i].type!=MOQT_P_TRACK_PROPERTY_FILTER)continue;
  any=true;
  bool lead=true;
  for(unsigned j=0;j<i;j++)if(f->f[j].type==MOQT_P_TRACK_PROPERTY_FILTER&&f->f[j].set==f->f[i].set)lead=false;
  if(!lead)continue;
  bool all=true;
  for(unsigned j=i;j<f->count&&all;j++){
   const moqt_rfilter *x=&f->f[j];uint64_t v;
   if(x->type!=MOQT_P_TRACK_PROPERTY_FILTER||x->set!=f->f[i].set)continue;
   if(!moqt_props_find_int(t->props,t->props_len,x->prop,&v)){all=false;break;}
   bool in=false;
   for(unsigned k=0;k<x->count;k++)if(v>=f->r[x->first+k].lo&&v<=f->r[x->first+k].hi)in=true;
   all=in;
  }
  if(all)return true;
 }
 return !any;
}

/* Opens a fill fetch stream (section 3.4). Returns a session error or 0. */
static int fill_start(moqt_core *c,unsigned sub_index,uint64_t request_id,const moqt_params *p){
 moqt_sub *s=&c->subs[sub_index];moqt_track *t=&c->tracks[s->track];
 if(!MOQT_HAS(p,MOQT_P_FILL_PARAMETERS)||!s->forward)return 0;
 moqt_rd r=moqt_rd_of(p->fill,p->fill_len);static moqt_params fp;
 int rv=moqt_params_read(&r,0,MOQT_CTX_FILL,parse_limits(c),&fp);
 if(rv>0)return rv;
 moqt_window w;bool own=MOQT_HAS(&fp,MOQT_P_LOCATION_FILTER)&&fp.location.fields;
 int wr=moqt_window_resolve(own?&fp.location:&s->location,own||s->has_location,t->has_largest,t->largest,true,&w);
 if(wr>0)return wr;
 if(rv<0||wr<0)return 0; /* an empty or unsatisfiable fill range opens no stream */
 s->streams++;
 int k=fetch_alloc(c);
 if(k<0){
  uint8_t h[24];size_t n=moqt_fetch_header_write(h,sizeof(h),request_id);
  emit(c,(moqt_action){.kind=MOQT_ACT_FILL_FAILED,.index=sub_index,.code=MOQT_RST_EXCESSIVE_LOAD},h,n);
  return 0;
 }
 moqt_fetch *f=&c->fetches[k];
 f->state=MOQT_FETCH_READY;f->track=s->track;f->sub=(int)sub_index;f->stream=-1;f->request_id=request_id;
 f->priority=MOQT_HAS(&fp,MOQT_P_SUBSCRIBER_PRIORITY)?fp.subscriber_priority:s->subscriber_priority;
 f->descending=(MOQT_HAS(&fp,MOQT_P_GROUP_ORDER)?fp.group_order:s->group_order)==MOQT_ORDER_DESCENDING;
 f->window=w;f->filters=fp.filters.count?fp.filters:s->filters;
 emit(c,(moqt_action){.kind=MOQT_ACT_FETCH_START,.index=(unsigned)k,.track=s->track,.code=request_id},NULL,0);
 return 0;
}
static void new_group_check(moqt_core *c,unsigned track,const moqt_params *p){
 moqt_track *t=&c->tracks[track];
 if(!MOQT_HAS(p,MOQT_P_NEW_GROUP_REQUEST)||!t->cfg.dynamic_groups)return;
 if(!p->new_group||!t->has_largest||p->new_group>t->largest.group)
  emit(c,(moqt_action){.kind=MOQT_ACT_NEW_GROUP,.track=track,.code=p->new_group},NULL,0);
}
static void ok_with_track(moqt_core *c,moqt_stream *s,const moqt_track *t,bool subscribe,uint64_t alias,bool props,bool fin){
 uint8_t b[160];moqt_ok_params p={.has_largest=t->has_largest,.largest=t->largest,
  .has_expires=subscribe&&c->subscription_lifetime_ms,.expires=c->subscription_lifetime_ms};
 size_t n=subscribe?moqt_subscribe_ok_write(b,sizeof(b),alias,&p,t->props,props?t->props_len:0)
                   :moqt_request_ok_write(b,sizeof(b),&p,t->props,props?t->props_len:0);
 send_on(c,s->id,b,n,fin);
}

static void on_subscribe(moqt_core *c,moqt_stream *s,const moqt_request *q,int parse){
 if(parse<0){respond_error(c,s,MOQT_REJECT_CODE(parse),0,"invalid request");return;}
 if(c->going_away){respond_error(c,s,MOQT_R_GOING_AWAY,0,"going away");return;}
 int t=track_find(c,&q->ns,q->name,q->name_len);
 if(t<0||c->tracks[t].ended){respond_error(c,s,MOQT_R_DOES_NOT_EXIST,0,"unknown track");return;}
 int k=sub_alloc(c);
 if(k<0){respond_error(c,s,MOQT_R_EXCESSIVE_LOAD,1001,"subscription limit");return;}
 moqt_sub *u=&c->subs[k];moqt_track *tr=&c->tracks[t];const moqt_params *p=&q->params;
 moqt_window w;
 int wr=moqt_window_resolve(&p->location,MOQT_HAS(p,MOQT_P_LOCATION_FILTER),tr->has_largest,tr->largest,false,&w);
 if(wr){
  if(wr>0)session_close(c,(uint64_t)wr);else respond_error(c,s,MOQT_REJECT_CODE(wr),0,"invalid range");
  return;
 }
 u->state=MOQT_SUB_ESTABLISHED;u->track=(uint8_t)t;u->stream=s->id;u->request_id=q->request_id;
 u->forward=p->forward;u->subscriber_priority=p->subscriber_priority;
 u->group_order=MOQT_HAS(p,MOQT_P_GROUP_ORDER)?p->group_order:track_order(tr);
 u->object_timeout_ms=p->object_timeout;u->subgroup_timeout_ms=p->subgroup_timeout;
 u->source_timeout_ms=p->source_timeout;
 u->location=p->location;u->has_location=MOQT_HAS(p,MOQT_P_LOCATION_FILTER)&&p->location.fields;
 u->window=w;u->filters=p->filters;
 u->expires_at_ms=c->subscription_lifetime_ms?c->now_ms+c->subscription_lifetime_ms:0;
 s->kind=MOQT_STREAM_SUB;s->index=(uint8_t)k;
 ok_with_track(c,s,tr,true,(uint64_t)t+1,p->include_properties,false);
 emit(c,(moqt_action){.kind=MOQT_ACT_SUBSCRIBED,.index=(unsigned)k,.track=(unsigned)t,.stream=s->id},NULL,0);
 int fr=fill_start(c,(unsigned)k,q->request_id,p);
 if(fr>0){session_close(c,(uint64_t)fr);return;}
 new_group_check(c,(unsigned)t,p);
}
static void on_track_status(moqt_core *c,moqt_stream *s,const moqt_request *q,int parse){
 if(parse<0){respond_error(c,s,MOQT_REJECT_CODE(parse),0,"invalid request");return;}
 int t=track_find(c,&q->ns,q->name,q->name_len);
 if(t<0){respond_error(c,s,MOQT_R_DOES_NOT_EXIST,0,"unknown track");return;}
 ok_with_track(c,s,&c->tracks[t],false,0,q->params.include_properties,true);
 stream_done(c,s);
}
static void on_fetch(moqt_core *c,moqt_stream *s,const moqt_request *q,int parse){
 if(parse<0){respond_error(c,s,MOQT_REJECT_CODE(parse),0,"invalid request");return;}
 if(c->going_away){respond_error(c,s,MOQT_R_GOING_AWAY,0,"going away");return;}
 int t=track_find(c,&q->ns,q->name,q->name_len);
 if(t<0){respond_error(c,s,MOQT_R_DOES_NOT_EXIST,0,"unknown track");return;}
 moqt_track *tr=&c->tracks[t];const moqt_params *p=&q->params;moqt_window w;
 int wr=moqt_window_resolve(&p->location,MOQT_HAS(p,MOQT_P_LOCATION_FILTER),tr->has_largest,tr->largest,true,&w);
 if(wr>0){session_close(c,(uint64_t)wr);return;}
 if(wr<0){respond_error(c,s,MOQT_REJECT_CODE(wr),0,"invalid range");return;}
 int k=fetch_alloc(c);
 if(k<0){respond_error(c,s,MOQT_R_EXCESSIVE_LOAD,1001,"fetch limit");return;}
 moqt_fetch *f=&c->fetches[k];
 f->state=MOQT_FETCH_READY;f->track=(uint8_t)t;f->sub=-1;f->stream=s->id;f->request_id=q->request_id;
 f->priority=p->subscriber_priority;
 f->descending=MOQT_HAS(p,MOQT_P_GROUP_ORDER)&&p->group_order==MOQT_ORDER_DESCENDING;
 f->window=w;f->filters=p->filters;
 s->kind=MOQT_STREAM_FETCH;s->index=(uint8_t)k;
 moqt_loc end=w.end;
 if(w.end_whole_group)end=tr->largest.group==w.end.group?tr->largest:(moqt_loc){w.end.group,UINT64_MAX};
 uint8_t b[160];
 bool final=tr->ended&&tr->has_largest&&!moqt_loc_cmp(end,tr->largest);
 size_t n=moqt_fetch_ok_write(b,sizeof(b),final,end,tr->props,p->include_properties?tr->props_len:0);
 send_on(c,s->id,b,n,false);
 emit(c,(moqt_action){.kind=MOQT_ACT_FETCH_START,.index=(unsigned)k,.track=(unsigned)t,.code=q->request_id},NULL,0);
}

static bool prefix_store(moqt_ns_sub *n,const moqt_ns *p){
 if(p->count>MOQT_CORE_PREFIX_FIELDS)return false;
 n->count=p->count;
 for(unsigned i=0;i<p->count;i++){
  if(p->f[i].n>MOQT_CORE_PREFIX_FIELD)return false;
  n->len[i]=(uint8_t)p->f[i].n;memcpy(n->field[i],p->f[i].p,p->f[i].n);
 }
 return true;
}
static bool prefix_overlaps(const moqt_ns_sub *a,const moqt_ns *p){
 unsigned m=a->count<p->count?a->count:p->count;
 for(unsigned i=0;i<m;i++)if(a->len[i]!=p->f[i].n||memcmp(a->field[i],p->f[i].p,a->len[i]))return false;
 return true;
}
static bool prefix_matches(const moqt_ns_sub *n,const moqt_track_cfg *t){
 if(n->count>t->ns_count)return false;
 for(unsigned i=0;i<n->count;i++)if(strlen(t->ns[i])!=n->len[i]||memcmp(t->ns[i],n->field[i],n->len[i]))return false;
 return true;
}
static bool same_namespace(const moqt_track_cfg *a,const moqt_track_cfg *b){
 if(a->ns_count!=b->ns_count)return false;
 for(unsigned i=0;i<a->ns_count;i++)if(strcmp(a->ns[i],b->ns[i]))return false;
 return true;
}
static void namespaces_announce(moqt_core *c,moqt_ns_sub *n){
 for(unsigned i=0;i<c->track_count;i++){
  const moqt_track_cfg *t=&c->tracks[i].cfg;
  if(c->tracks[i].ended||!prefix_matches(n,t))continue;
  bool repeated=false;
  for(unsigned j=0;j<i;j++)if(((n->announced>>j)&1U)&&same_namespace(&c->tracks[j].cfg,t))repeated=true;
  n->announced|=(uint8_t)(1U<<i);
  if(repeated)continue;
  uint8_t b[200];size_t k=moqt_namespace_write(b,sizeof(b),MOQT_MSG_NAMESPACE,t->ns,t->ns_count,n->count);
  send_on(c,n->stream,b,k,false);
 }
}
static int publish_start(moqt_core *c,unsigned track,const moqt_ns_sub *from,bool forward,bool early);
static void tracks_publish(moqt_core *c,moqt_ns_sub *n){
 for(unsigned i=0;i<c->track_count;i++){
  moqt_track *t=&c->tracks[i];
  if(t->ended||((n->announced>>i)&1U)||!prefix_matches(n,&t->cfg)||!track_filter_pass(&n->filters,t))continue;
  n->announced|=(uint8_t)(1U<<i);
  if(publish_start(c,i,n,n->forward,false)<0){
   uint8_t b[240];size_t k=moqt_publish_skipped_write(b,sizeof(b),t->cfg.ns,t->cfg.ns_count,n->count,t->cfg.name);
   send_on(c,n->stream,b,k,false);
  }
 }
}
static void on_namespace_request(moqt_core *c,moqt_stream *s,const moqt_request *q,int parse,bool tracks){
 if(parse<0){respond_error(c,s,MOQT_REJECT_CODE(parse),0,"invalid request");return;}
 if(c->going_away){respond_error(c,s,MOQT_R_GOING_AWAY,0,"going away");return;}
 moqt_ns_sub *n=NULL;
 for(unsigned i=0;i<MOQT_CORE_NS_SUBS;i++){
  moqt_ns_sub *x=&c->ns_subs[i];
  if(x->used&&x->tracks==tracks&&prefix_overlaps(x,&q->ns)){respond_error(c,s,MOQT_R_PREFIX_OVERLAP,0,"prefix overlap");return;}
  if(!x->used&&!n)n=x;
 }
 if(!n){respond_error(c,s,MOQT_R_EXCESSIVE_LOAD,1001,"namespace subscription limit");return;}
 memset(n,0,sizeof(*n));
 if(!prefix_store(n,&q->ns)){respond_error(c,s,MOQT_R_INTERNAL_ERROR,0,"prefix exceeds endpoint limit");return;}
 const moqt_params *p=&q->params;
 n->used=true;n->tracks=tracks;n->stream=s->id;n->request_id=q->request_id;
 n->forward=p->forward;n->subscriber_priority=p->subscriber_priority;
 n->group_order=MOQT_HAS(p,MOQT_P_GROUP_ORDER)?p->group_order:0;
 n->object_timeout_ms=p->object_timeout;n->subgroup_timeout_ms=p->subgroup_timeout;
 n->source_timeout_ms=p->source_timeout;
 n->location=p->location;n->has_location=MOQT_HAS(p,MOQT_P_LOCATION_FILTER)&&p->location.fields;
 n->filters=p->filters;
 s->kind=tracks?MOQT_STREAM_TRACKS:MOQT_STREAM_NAMESPACES;s->index=(uint8_t)(n-c->ns_subs);
 uint8_t b[16];size_t k=moqt_request_ok_write(b,sizeof(b),NULL,NULL,0);
 send_on(c,s->id,b,k,false);
 if(tracks)tracks_publish(c,n);else namespaces_announce(c,n);
}

static int publish_start(moqt_core *c,unsigned track,const moqt_ns_sub *from,bool forward,bool early){
 if(track>=c->track_count||c->tracks[track].ended||c->going_away)return -1;
 /* The peer stated how many subscriptions it accepts from this endpoint. */
 if(c->peer.ext.has_subscriptions&&subs_in_use(c,true)>=c->peer.ext.max_subscriptions)return -1;
 int k=sub_alloc(c);
 if(k<0)return -1;
 moqt_sub *u=&c->subs[k];moqt_track *t=&c->tracks[track];
 u->state=MOQT_SUB_PENDING;u->track=(uint8_t)track;u->by_publish=true;u->early=early;u->stream=-1;
 u->request_id=c->next_request_id;c->next_request_id+=2;
 u->forward=forward;u->subscriber_priority=128;u->group_order=track_order(t);
 moqt_publish_params p={.has_forward=true,.forward=forward,.has_largest=t->has_largest,.largest=t->largest};
 if(from){
  u->subscriber_priority=from->subscriber_priority;
  if(from->group_order)u->group_order=from->group_order;
  u->object_timeout_ms=from->object_timeout_ms;u->subgroup_timeout_ms=from->subgroup_timeout_ms;
  u->source_timeout_ms=from->source_timeout_ms;
  p.has_source_timeout=u->source_timeout_ms!=0&&c->peer.ext.source_timeout;p.source_timeout=u->source_timeout_ms;
  u->location=from->location;u->has_location=from->has_location;
  u->filters=from->filters;strip_track_filters(&u->filters);
  p.has_priority=true;p.priority=u->subscriber_priority;
  p.has_order=from->group_order!=0;p.order=u->group_order;
  p.has_object_timeout=u->object_timeout_ms!=0;p.object_timeout=u->object_timeout_ms;
  p.has_subgroup_timeout=u->subgroup_timeout_ms!=0;p.subgroup_timeout=u->subgroup_timeout_ms;
  p.has_location=u->has_location;p.location=u->location;
 }
 if(moqt_window_resolve(&u->location,u->has_location,t->has_largest,t->largest,false,&u->window)){u->state=MOQT_SUB_FREE;return -1;}
 uint8_t b[320];
 size_t n=moqt_publish_write(b,sizeof(b),u->request_id,t->cfg.ns,t->cfg.ns_count,t->cfg.name,(uint64_t)track+1,&p,t->props,t->props_len);
 if(!n){u->state=MOQT_SUB_FREE;return -1;}
 emit(c,(moqt_action){.kind=MOQT_ACT_OPEN_BIDI,.token=(unsigned)k,.track=track},b,n);
 return k;
}
int moqt_core_publish_track(moqt_core *c,unsigned track,bool forward,bool early){
 return c->closed?-1:publish_start(c,track,NULL,forward,early);
}
int moqt_core_publish_namespace(moqt_core *c){
 if(c->closed||c->going_away||c->namespace_state==1||c->namespace_state==2||!c->track_count)return -1;
 const moqt_track_cfg *t=&c->tracks[0].cfg;uint8_t b[200];
 size_t n=moqt_publish_namespace_write(b,sizeof(b),c->next_request_id,t->ns,t->ns_count);
 if(!n)return -1;
 c->next_request_id+=2;c->namespace_state=1;c->namespace_stream=-1;
 emit(c,(moqt_action){.kind=MOQT_ACT_OPEN_BIDI,.token=MOQT_CORE_SUBS},b,n);
 return 0;
}
void moqt_core_bidi_opened(moqt_core *c,unsigned token,int64_t stream){
 if(c->closed)return;
 if(token==MOQT_CORE_SUBS){
  moqt_stream *s=stream_take(c,stream,MOQT_STREAM_OWN_NAMESPACE);
  if(!s){session_close(c,MOQT_S_INTERNAL_ERROR);return;}
  c->namespace_stream=stream;return;
 }
 if(token>=MOQT_CORE_SUBS||c->subs[token].state==MOQT_SUB_FREE||!c->subs[token].by_publish)return;
 moqt_stream *s=stream_take(c,stream,MOQT_STREAM_OWN_PUBLISH);
 if(!s){session_close(c,MOQT_S_INTERNAL_ERROR);return;}
 s->index=(uint8_t)token;c->subs[token].stream=stream;
}

static void sub_apply(moqt_core *c,moqt_sub *u,const moqt_params *p,bool update){
 moqt_track *t=&c->tracks[u->track];
 if(MOQT_HAS(p,MOQT_P_OBJECT_DELIVERY_TIMEOUT))u->object_timeout_ms=p->object_timeout;
 if(MOQT_HAS(p,MOQT_P_SUBGROUP_DELIVERY_TIMEOUT))u->subgroup_timeout_ms=p->subgroup_timeout;
 if(MOQT_HAS(p,MOQT_P_SOURCE_DELIVERY_TIMEOUT))u->source_timeout_ms=p->source_timeout;
 if(MOQT_HAS(p,MOQT_P_FORWARD))u->forward=p->forward;
 if(MOQT_HAS(p,MOQT_P_SUBSCRIBER_PRIORITY))u->subscriber_priority=p->subscriber_priority;
 if(!update&&MOQT_HAS(p,MOQT_P_GROUP_ORDER))u->group_order=p->group_order;
 if(MOQT_HAS(p,MOQT_P_LOCATION_FILTER)){
  u->location=p->location;u->has_location=p->location.fields!=0;
  moqt_window_resolve(&u->location,u->has_location,t->has_largest,t->largest,false,&u->window);
 }
}
static void on_sub_update(moqt_core *c,moqt_stream *s,const uint8_t *body,size_t len){
 moqt_sub *u=&c->subs[s->index];uint64_t id;static moqt_params p;
 int rv=moqt_update_read(body,len,MOQT_CTX_UPDATE_SUBSCRIPTION,parse_limits(c),&id,&p);
 if(rv>0){session_close(c,(uint64_t)rv);return;}
 if(!peer_request_id_ok(c,id)){session_close(c,MOQT_S_INVALID_REQUEST_ID);return;}
 moqt_filters merged=u->filters;
 if(!rv)rv=moqt_filters_merge(&merged,&p.filters);
 if(rv<0){
  /* Section 9.5.1: a failed update also terminates the subscription. */
  uint8_t b[160];size_t n=moqt_request_error_write(b,sizeof(b),MOQT_REJECT_CODE(rv),0,"update rejected");
  send_on(c,s->id,b,n,false);
  moqt_core_sub_end(c,s->index,MOQT_D_UPDATE_FAILED);
  return;
 }
 sub_apply(c,u,&p,true);u->filters=merged;
 moqt_track *t=&c->tracks[u->track];
 /* An accepted update extends the subscription (section 9.20.17). */
 if(u->expires_at_ms)u->expires_at_ms=c->now_ms+c->subscription_lifetime_ms;
 uint8_t b[64];moqt_ok_params ok={.has_largest=t->has_largest,.largest=t->largest,
  .has_expires=u->expires_at_ms!=0,.expires=c->subscription_lifetime_ms};
 size_t n=moqt_request_ok_write(b,sizeof(b),&ok,NULL,0);
 send_on(c,s->id,b,n,false);
 emit(c,(moqt_action){.kind=MOQT_ACT_UPDATED,.index=s->index,.track=u->track,.stream=s->id},NULL,0);
 int fr=fill_start(c,s->index,id,&p);
 if(fr>0){session_close(c,(uint64_t)fr);return;}
 new_group_check(c,u->track,&p);
}
static void on_fetch_update(moqt_core *c,moqt_stream *s,const uint8_t *body,size_t len){
 uint64_t id;static moqt_params p;
 int rv=moqt_update_read(body,len,MOQT_CTX_UPDATE_FETCH,parse_limits(c),&id,&p);
 if(rv>0){session_close(c,(uint64_t)rv);return;}
 if(!peer_request_id_ok(c,id)){session_close(c,MOQT_S_INVALID_REQUEST_ID);return;}
 moqt_fetch *f=&c->fetches[s->index];
 if(rv<0){
  uint8_t b[160];size_t n=moqt_request_error_write(b,sizeof(b),MOQT_REJECT_CODE(rv),0,"update rejected");
  send_on(c,s->id,b,n,true);
  emit(c,(moqt_action){.kind=MOQT_ACT_FETCH_CANCEL,.index=s->index,.code=MOQT_RST_CANCELLED},NULL,0);
  f->state=MOQT_FETCH_FREE;stream_done(c,s);return;
 }
 if(MOQT_HAS(&p,MOQT_P_SUBSCRIBER_PRIORITY))f->priority=p.subscriber_priority;
 uint8_t b[16];size_t n=moqt_request_ok_write(b,sizeof(b),NULL,NULL,0);
 send_on(c,s->id,b,n,false);
}
static void on_namespace_update(moqt_core *c,moqt_stream *s,const uint8_t *body,size_t len){
 moqt_ns_sub *n=&c->ns_subs[s->index];uint64_t id;static moqt_params p;
 int rv=moqt_update_read(body,len,n->tracks?MOQT_CTX_UPDATE_TRACKS:MOQT_CTX_UPDATE_NAMESPACE,parse_limits(c),&id,&p);
 if(rv>0){session_close(c,(uint64_t)rv);return;}
 if(!peer_request_id_ok(c,id)){session_close(c,MOQT_S_INVALID_REQUEST_ID);return;}
 uint64_t error=rv<0?MOQT_REJECT_CODE(rv):0;bool failed=rv<0;
 if(!failed&&MOQT_HAS(&p,MOQT_P_TRACK_NAMESPACE_PREFIX)){
  for(unsigned i=0;i<MOQT_CORE_NS_SUBS;i++){
   moqt_ns_sub *x=&c->ns_subs[i];
   if(x!=n&&x->used&&x->tracks==n->tracks&&prefix_overlaps(x,&p.prefix)){failed=true;error=MOQT_R_PREFIX_OVERLAP;}
  }
  static moqt_ns_sub probe;probe=*n;
  if(!failed&&!prefix_store(&probe,&p.prefix)){failed=true;error=MOQT_R_INTERNAL_ERROR;}
  if(!failed){*n=probe;if(!n->tracks)n->announced=0;}
 }
 if(failed){
  /* Section 9.5.1: the responder closes the bidi stream. */
  n->used=false;respond_error(c,s,error,0,"update rejected");return;
 }
 if(MOQT_HAS(&p,MOQT_P_FORWARD))n->forward=p.forward;
 uint8_t b[16];size_t k=moqt_request_ok_write(b,sizeof(b),NULL,NULL,0);
 send_on(c,s->id,b,k,false);
 if(n->tracks)tracks_publish(c,n);else namespaces_announce(c,n);
}

static void on_publish_response(moqt_core *c,moqt_stream *s,uint64_t type,const uint8_t *body,size_t len){
 moqt_sub *u=&c->subs[s->index];
 if(type==MOQT_MSG_REQUEST_OK){
  static moqt_params p;int rv=moqt_request_ok_read(body,len,MOQT_CTX_PUBLISH_OK,parse_limits(c),&p);
  if(rv>0){session_close(c,(uint64_t)rv);return;}
  if(rv<0){moqt_core_sub_end(c,s->index,MOQT_D_INTERNAL_ERROR);s->typed=true;return;}
  sub_apply(c,u,&p,false);
  if(p.filters.count||p.filters.removed)moqt_filters_merge(&u->filters,&p.filters);
  if(u->state==MOQT_SUB_PENDING)u->state=MOQT_SUB_ESTABLISHED;
  s->typed=true;
  emit(c,(moqt_action){.kind=MOQT_ACT_SUBSCRIBED,.index=s->index,.track=u->track,.stream=s->id},NULL,0);
  new_group_check(c,u->track,&p);
  return;
 }
 moqt_error e;int rv=moqt_request_error_read(body,len,&e);
 if(rv){session_close(c,(uint64_t)rv);return;}
 fills_cancel(c,s->index);
 emit(c,(moqt_action){.kind=MOQT_ACT_SUB_CANCELLED,.index=s->index,.track=u->track,.stream=s->id,.code=e.code},NULL,0);
 u->state=MOQT_SUB_FREE;stream_done(c,s);
}
static void on_namespace_response(moqt_core *c,moqt_stream *s,uint64_t type,const uint8_t *body,size_t len){
 if(type==MOQT_MSG_REQUEST_OK){
  static moqt_params p;int rv=moqt_request_ok_read(body,len,MOQT_CTX_NAMESPACE_OK,0,&p);
  if(rv){session_close(c,rv>0?(uint64_t)rv:MOQT_S_PROTOCOL_VIOLATION);return;}
  c->namespace_state=2;s->typed=true;
  emit(c,(moqt_action){.kind=MOQT_ACT_NAMESPACE_RESULT,.stream=s->id,.code=0},NULL,0);
  return;
 }
 moqt_error e;int rv=moqt_request_error_read(body,len,&e);
 if(rv){session_close(c,(uint64_t)rv);return;}
 c->namespace_state=3;
 emit(c,(moqt_action){.kind=MOQT_ACT_NAMESPACE_RESULT,.stream=s->id,.code=e.code+1},NULL,0);
 stream_done(c,s);
}
static void on_goaway(moqt_core *c,moqt_stream *s,const uint8_t *body,size_t len){
 moqt_goaway g;int rv=moqt_goaway_read(body,len,!c->is_client,&g);
 if(rv||s->goaway){session_close(c,MOQT_S_PROTOCOL_VIOLATION);return;}
 s->goaway=true;
 if(s->kind==MOQT_STREAM_CONTROL){
  c->going_away=true;
  emit(c,(moqt_action){.kind=MOQT_ACT_GOAWAY,.stream=s->id,.code=g.timeout},g.uri,g.uri_len);
  return;
 }
 /* Request-level migration (section 9.2): end this request only. */
 emit(c,(moqt_action){.kind=MOQT_ACT_GOAWAY,.stream=s->id,.code=g.timeout,.index=s->index+1U},g.uri,g.uri_len);
 if(s->kind==MOQT_STREAM_SUB||s->kind==MOQT_STREAM_OWN_PUBLISH)moqt_core_sub_end(c,s->index,MOQT_D_GOING_AWAY);
}

static void on_first_request(moqt_core *c,moqt_stream *s,uint64_t type,const uint8_t *body,size_t len){
 if(type==MOQT_MSG_PUBLISH||type==MOQT_MSG_PUBLISH_NAMESPACE){
  /* This endpoint publishes only; it declines tracks offered by the peer. */
  moqt_rd r=moqt_rd_of(body,len);uint64_t id=moqt_rd_vi(&r);
  if(r.bad){session_close(c,MOQT_S_PROTOCOL_VIOLATION);return;}
  if(!peer_request_id_ok(c,id)){session_close(c,MOQT_S_INVALID_REQUEST_ID);return;}
  respond_error(c,s,MOQT_R_UNINTERESTED,0,"publisher-only endpoint");return;
 }
 if(type!=MOQT_MSG_SUBSCRIBE&&type!=MOQT_MSG_FETCH&&type!=MOQT_MSG_TRACK_STATUS&&
    type!=MOQT_MSG_SUBSCRIBE_NAMESPACE&&type!=MOQT_MSG_SUBSCRIBE_TRACKS){
  session_close(c,MOQT_S_PROTOCOL_VIOLATION);return;
 }
 static moqt_request q;int rv=moqt_request_read(type,body,len,parse_limits(c),&q);
 if(rv>0){session_close(c,(uint64_t)rv);return;}
 if(!peer_request_id_ok(c,q.request_id)){session_close(c,MOQT_S_INVALID_REQUEST_ID);return;}
 s->typed=true;
 if(type==MOQT_MSG_SUBSCRIBE)on_subscribe(c,s,&q,rv);
 else if(type==MOQT_MSG_FETCH)on_fetch(c,s,&q,rv);
 else if(type==MOQT_MSG_TRACK_STATUS)on_track_status(c,s,&q,rv);
 else on_namespace_request(c,s,&q,rv,type==MOQT_MSG_SUBSCRIBE_TRACKS);
}
static void on_message(moqt_core *c,moqt_stream *s,uint64_t type,const uint8_t *body,size_t len,unsigned *updates){
 switch(s->kind){
 case MOQT_STREAM_CONTROL:
  if(!c->peer_setup){
   if(type!=MOQT_MSG_SETUP){session_close(c,MOQT_S_PROTOCOL_VIOLATION);return;}
   int rv=moqt_setup_read(body,len,c->is_client,&c->peer);
   if(rv){session_close(c,(uint64_t)rv);return;}
   c->peer_setup=true;return;
  }
  if(type==MOQT_MSG_GOAWAY)on_goaway(c,s,body,len);else session_close(c,MOQT_S_PROTOCOL_VIOLATION);
  return;
 case MOQT_STREAM_REQUEST:on_first_request(c,s,type,body,len);return;
 case MOQT_STREAM_CLOSED:
  if(type!=MOQT_MSG_REQUEST_UPDATE&&type!=MOQT_MSG_GOAWAY)session_close(c,MOQT_S_PROTOCOL_VIOLATION);
  return;
 case MOQT_STREAM_OWN_PUBLISH:
  if(!s->typed){
   if(type!=MOQT_MSG_REQUEST_OK&&type!=MOQT_MSG_REQUEST_ERROR){session_close(c,MOQT_S_PROTOCOL_VIOLATION);return;}
   on_publish_response(c,s,type,body,len);return;
  }
  break;
 case MOQT_STREAM_OWN_NAMESPACE:
  if(!s->typed){
   if(type!=MOQT_MSG_REQUEST_OK&&type!=MOQT_MSG_REQUEST_ERROR){session_close(c,MOQT_S_PROTOCOL_VIOLATION);return;}
   on_namespace_response(c,s,type,body,len);return;
  }
  if(type==MOQT_MSG_GOAWAY)on_goaway(c,s,body,len);else session_close(c,MOQT_S_PROTOCOL_VIOLATION);
  return;
 default:break;
 }
 if(type==MOQT_MSG_GOAWAY){on_goaway(c,s,body,len);return;}
 if(type!=MOQT_MSG_REQUEST_UPDATE){session_close(c,MOQT_S_PROTOCOL_VIOLATION);return;}
 /* Section 9.1.7: pipelined updates beyond the advertised limit end the session. */
 if(++*updates>MOQT_CORE_MAX_UPDATES){session_close(c,MOQT_S_TOO_MANY_REQUEST_UPDATES);return;}
 if(s->kind==MOQT_STREAM_SUB||s->kind==MOQT_STREAM_OWN_PUBLISH){
  if(c->subs[s->index].state==MOQT_SUB_ENDING)return;
  on_sub_update(c,s,body,len);
 }else if(s->kind==MOQT_STREAM_FETCH)on_fetch_update(c,s,body,len);
 else on_namespace_update(c,s,body,len);
}
static void stream_process(moqt_core *c,moqt_stream *s){
 unsigned updates=0;
 while(!c->closed&&s->len&&s->kind!=MOQT_STREAM_FREE&&s->kind!=MOQT_STREAM_IGNORED){
  if(s->kind!=MOQT_STREAM_CONTROL&&!c->peer_setup)return; /* section 6.3: buffer until setup completes */
  uint64_t type;const uint8_t *body;size_t bl,used;
  int r=moqt21_message_read(s->buf,s->len,&type,&body,&bl,&used);
  if(r<0){session_close(c,MOQT_S_INTERNAL_ERROR);return;}
  if(!r){
   /* A declared length that can never fit ends the session instead of stalling. */
   uint64_t t2;size_t k;
   if(moqt21_vi_read(s->buf,s->len,&t2,&k)==1&&s->len>=k+2){
    size_t body_len=((size_t)s->buf[k]<<8)|s->buf[k+1];
    /* With the limit advertised, exceeding it is the peer's violation. */
    if(c->ext.has_control_message&&s->kind!=MOQT_STREAM_CONTROL&&body_len>c->ext.max_control_message){
     session_close(c,MOQT_S_PROTOCOL_VIOLATION);return;
    }
    if(k+2+body_len>MOQT_CORE_RX){session_close(c,MOQT_S_INTERNAL_ERROR);return;}
   }
   break;
  }
  if(c->ext.has_control_message&&s->kind!=MOQT_STREAM_CONTROL&&bl>c->ext.max_control_message){session_close(c,MOQT_S_PROTOCOL_VIOLATION);return;}
  /* Large scratch objects are static: the core is single-threaded and MCU stacks are small. */
  static uint8_t copy[MOQT_CORE_RX];memcpy(copy,body,bl);
  int64_t id=s->id;
  memmove(s->buf,s->buf+used,s->len-used);s->len=(uint16_t)(s->len-used);
  on_message(c,s,type,copy,bl,&updates);
  if(s->kind==MOQT_STREAM_FREE||s->id!=id)return;
 }
}
void moqt_core_rx(moqt_core *c,int64_t id,const uint8_t *data,size_t len,bool fin,uint64_t now_ms){
 c->now_ms=now_ms;
 if(c->closed)return;
 unsigned low=(unsigned)(id&3);
 bool own_bidi=low==(c->is_client?0U:1U),peer_bidi=low==(c->is_client?1U:0U),peer_uni=low==(c->is_client?3U:2U);
 moqt_stream *s=stream_find(c,id);
 if(!s){
  if(own_bidi||(!peer_bidi&&!peer_uni))return;
  if(!len)return; /* a bare FIN on a stream that is no longer tracked */
  s=stream_take(c,id,peer_uni?MOQT_STREAM_CONTROL:MOQT_STREAM_REQUEST);
  if(!s){
   if(peer_uni)session_close(c,MOQT_S_INTERNAL_ERROR);
   else emit(c,(moqt_action){.kind=MOQT_ACT_RESET_STREAM,.stream=id,.code=MOQT_RST_EXCESSIVE_LOAD},NULL,0);
   return;
  }
  s->typed=!peer_uni;
 }
 if(s->kind==MOQT_STREAM_IGNORED){if(fin)s->kind=MOQT_STREAM_FREE;return;}
 if(len>sizeof(s->buf)-s->len){session_close(c,MOQT_S_INTERNAL_ERROR);return;}
 if(len)memcpy(s->buf+s->len,data,len);
 s->len=(uint16_t)(s->len+len);
 if(fin)s->peer_fin=true;
 if(s->kind==MOQT_STREAM_CONTROL&&!s->typed){
  /* Section 6.4.1: classify the unidirectional stream by its type. */
  uint64_t type;size_t k;
  if(moqt21_vi_read(s->buf,s->len,&type,&k)!=1){if(fin)s->kind=MOQT_STREAM_FREE;return;}
  if(type==MOQT_MSG_SETUP){
   for(unsigned i=0;i<MOQT_CORE_STREAMS;i++)
    if(&c->streams[i]!=s&&c->streams[i].kind==MOQT_STREAM_CONTROL&&c->streams[i].typed){session_close(c,MOQT_S_PROTOCOL_VIOLATION);return;}
   s->typed=true;
  }else if(type==MOQT_STREAM_PADDING){
   s->kind=fin?MOQT_STREAM_FREE:MOQT_STREAM_IGNORED;return;
  }else if(type==MOQT_STREAM_FETCH_HEADER||(type<128&&(type&MOQT_SG_ALWAYS))){
   /* Objects for a subscription this endpoint never made (section 11.3.1). */
   emit(c,(moqt_action){.kind=MOQT_ACT_STOP_SENDING,.stream=id,.code=MOQT_RST_CANCELLED},NULL,0);
   s->kind=fin?MOQT_STREAM_FREE:MOQT_STREAM_IGNORED;return;
  }else{session_close(c,MOQT_S_PROTOCOL_VIOLATION);return;}
 }
 if(s->kind==MOQT_STREAM_CONTROL&&fin){session_close(c,MOQT_S_PROTOCOL_VIOLATION);return;}
 bool was_setup=c->peer_setup;
 stream_process(c,s);
 if(c->closed)return;
 if(!was_setup&&c->peer_setup){
  for(unsigned i=0;i<MOQT_CORE_STREAMS&&!c->closed;i++){
   moqt_stream *x=&c->streams[i];
   if(x!=s&&x->kind!=MOQT_STREAM_FREE&&x->kind!=MOQT_STREAM_CONTROL&&x->len)stream_process(c,x);
  }
  if(c->closed)return;
  s=stream_find(c,id);
 }
 if(!s||!s->peer_fin)return;
 if(s->kind==MOQT_STREAM_SUB||s->kind==MOQT_STREAM_OWN_PUBLISH)
  emit(c,(moqt_action){.kind=MOQT_ACT_PEER_FIN,.index=s->index,.track=c->subs[s->index].track,.stream=id},NULL,0);
 else if(s->kind==MOQT_STREAM_CLOSED){
  s->kind=MOQT_STREAM_FREE;
  emit(c,(moqt_action){.kind=MOQT_ACT_REQUEST_CLOSED,.stream=id},NULL,0);
 }else if(s->kind==MOQT_STREAM_REQUEST)s->kind=MOQT_STREAM_FREE;
}
void moqt_core_stream_cancelled(moqt_core *c,int64_t id){
 moqt_stream *s=stream_find(c,id);
 if(!s||c->closed)return;
 if(s->kind==MOQT_STREAM_CONTROL){session_close(c,MOQT_S_PROTOCOL_VIOLATION);return;}
 if(s->kind==MOQT_STREAM_SUB||s->kind==MOQT_STREAM_OWN_PUBLISH){
  moqt_sub *u=&c->subs[s->index];
  fills_cancel(c,s->index);
  emit(c,(moqt_action){.kind=MOQT_ACT_SUB_CANCELLED,.index=s->index,.track=u->track,.stream=id,.code=MOQT_RST_CANCELLED},NULL,0);
  u->state=MOQT_SUB_FREE;
 }else if(s->kind==MOQT_STREAM_FETCH){
  emit(c,(moqt_action){.kind=MOQT_ACT_FETCH_CANCEL,.index=s->index,.code=MOQT_RST_CANCELLED},NULL,0);
  emit(c,(moqt_action){.kind=MOQT_ACT_RESET_STREAM,.stream=id,.code=MOQT_RST_CANCELLED},NULL,0);
  c->fetches[s->index].state=MOQT_FETCH_FREE;
 }else if(s->kind==MOQT_STREAM_NAMESPACES||s->kind==MOQT_STREAM_TRACKS){
  c->ns_subs[s->index].used=false;
  emit(c,(moqt_action){.kind=MOQT_ACT_RESET_STREAM,.stream=id,.code=MOQT_RST_CANCELLED},NULL,0);
 }else if(s->kind==MOQT_STREAM_OWN_NAMESPACE){
  c->namespace_state=3;
  emit(c,(moqt_action){.kind=MOQT_ACT_NAMESPACE_RESULT,.stream=id,.code=MOQT_R_UNINTERESTED+1},NULL,0);
 }
 s->kind=MOQT_STREAM_FREE;
}

static bool sub_delivers(const moqt_sub *u){
 return u->forward&&(u->state==MOQT_SUB_ESTABLISHED||(u->state==MOQT_SUB_PENDING&&u->early&&u->stream>=0));
}
static uint64_t offer_timeout(const moqt_offer *o,uint64_t type,uint64_t track_value){
 uint64_t v;
 return moqt_props_find_int(o->props,o->props_len,type,&v)?v:track_value;
}
/* Appends SOURCE_AGE to a property block. Returns the new length, 0 when it cannot be added. */
static size_t props_with_age(const uint8_t *props,size_t len,uint64_t age,uint8_t *out,size_t cap){
 moqt_kvp_it it=moqt_kvp_begin(props,len);moqt_kvp k;uint64_t last=0;int r;
 while((r=moqt_kvp_next(&it,&k))==1)last=k.type;
 if(r||last>=MOQT_X_SOURCE_AGE||len>cap)return 0;
 if(len)memcpy(out,props,len);
 moqt_wr w=moqt_wr_of(out+len,cap-len);uint64_t prev=last;
 moqt_kvp_put_int(&w,&prev,MOQT_X_SOURCE_AGE,age);
 return w.bad?0:len+w.n;
}
int moqt_core_publish(moqt_core *c,unsigned track,const moqt_offer *o,uint64_t now_ms,moqt_delivery *out,unsigned cap){
 c->now_ms=now_ms;
 if(c->closed||track>=c->track_count||c->tracks[track].ended)return -1;
 if(o->props_len&&moqt_props_validate(o->props,o->props_len,true))return -1;
 if(o->status!=MOQT_STATUS_NORMAL&&(o->payload_len||o->props_len))return -1;
 if(o->status!=MOQT_STATUS_NORMAL&&o->status!=MOQT_STATUS_END_OF_GROUP&&o->status!=MOQT_STATUS_END_OF_TRACK)return -1;
 moqt_track *t=&c->tracks[track];moqt_loc at={o->group,o->object};
 uint8_t priority=o->has_priority?o->priority:track_priority(t);
 static uint8_t merged[MOQT_CORE_PROPS+16];
 const uint8_t *props=o->props;size_t props_len=o->props_len;
 bool aged=c->ext.source_timeout&&o->has_source_age&&o->status==MOQT_STATUS_NORMAL;
 if(aged){
  props_len=props_with_age(o->props,o->props_len,o->source_age_ms,merged,sizeof(merged));
  if(!props_len)return -1;
  props=merged;
 }
 if(!t->has_largest||moqt_loc_cmp(at,t->largest)>0){t->has_largest=true;t->largest=at;}
 if(o->status==MOQT_STATUS_END_OF_TRACK)t->ended=true;
 moqt_filter_subject subject={o->subgroup,o->object,priority,o->datagram,props,props_len};
 moqt_object wire={.alias=(uint64_t)track+1,.group=o->group,.subgroup=o->subgroup,.object=o->object,.priority=priority,
  .status=o->status,.has_priority=priority!=track_priority(t),.end_of_group=o->end_of_group,.first_object=true,
  .datagram=o->datagram,.props=props,.props_len=props_len,.payload_len=o->payload_len};
 unsigned count=0;
 for(unsigned i=0;i<MOQT_CORE_SUBS&&count<cap;i++){
  moqt_sub *u=&c->subs[i];
  if(u->state==MOQT_SUB_FREE||u->track!=track||!sub_delivers(u))continue;
  if(!moqt_window_contains(&u->window,at)||!moqt_filters_pass(&u->filters,&subject))continue;
  uint64_t source_timeout=aged?moqt_timeout_effective(offer_timeout(o,MOQT_X_SOURCE_DELIVERY_TIMEOUT,t->cfg.source_timeout_ms),u->source_timeout_ms):0;
  /* Older than the subscription tolerates before it was even offered: never sent. */
  if(source_timeout&&o->source_age_ms>=source_timeout){c->source_expired++;continue;}
  moqt_delivery *d=&out[count];
  memset(d,0,sizeof(*d));
  d->sub=i;d->alias=wire.alias;d->request_id=u->request_id;d->datagram=o->datagram;
  d->subscriber_priority=u->subscriber_priority;d->publisher_priority=priority;d->group_order=u->group_order;
  d->object_timeout_ms=moqt_timeout_effective(offer_timeout(o,MOQT_X_OBJECT_DELIVERY_TIMEOUT,t->cfg.object_timeout_ms),u->object_timeout_ms);
  d->subgroup_timeout_ms=moqt_timeout_effective(offer_timeout(o,MOQT_X_SUBGROUP_DELIVERY_TIMEOUT,t->cfg.subgroup_timeout_ms),u->subgroup_timeout_ms);
  d->source_timeout_ms=source_timeout;d->source_age_ms=aged?o->source_age_ms:0;
  if(o->datagram)d->head_len=moqt_datagram_header_write(d->head,sizeof(d->head),&wire);
  else{
   size_t n=moqt_subgroup_header_write(d->head,sizeof(d->head),&wire,props_len!=0);
   size_t m=n?moqt_subgroup_object_write(d->head+n,sizeof(d->head)-n,&wire,props_len!=0,true,0):0;
   d->head_len=n&&m?n+m:0;
  }
  if(!d->head_len)return -1;
  count++;
 }
 return (int)count;
}
void moqt_core_stream_opened(moqt_core *c,unsigned sub){
 if(sub<MOQT_CORE_SUBS&&c->subs[sub].state!=MOQT_SUB_FREE)c->subs[sub].streams++;
}

static moqt_cached *cache_oldest(moqt_core *c,unsigned track){
 moqt_cached *o=NULL;
 for(unsigned i=0;i<MOQT_CORE_CACHE;i++){
  moqt_cached *e=&c->cache[i];
  if(e->used&&(track>=c->track_count||e->track==track)&&(!o||e->serial<o->serial))o=e;
 }
 return o;
}
int moqt_core_cache_put(moqt_core *c,unsigned track,const moqt_offer *o,void *payload,uint64_t now_ms,void **evicted){
 *evicted=NULL;
 if(track>=c->track_count||o->props_len>MOQT_CORE_PROPS||o->status!=MOQT_STATUS_NORMAL)return -1;
 moqt_cached *e=NULL;
 for(unsigned i=0;i<MOQT_CORE_CACHE;i++){
  moqt_cached *x=&c->cache[i];
  if(x->used&&x->track==track&&x->group==o->group&&x->object==o->object)return -1; /* objects are immutable */
  if(!x->used&&!e)e=x;
 }
 if(!e){e=cache_oldest(c,MOQT_CORE_TRACKS);*evicted=e->payload;}
 *e=(moqt_cached){.used=true,.datagram=o->datagram,.track=(uint8_t)track,
  .priority=o->has_priority?o->priority:track_priority(&c->tracks[track]),
  .group=o->group,.object=o->object,.subgroup=o->subgroup,.stored_ms=now_ms,.serial=++c->cache_serial,
  .props_len=(uint16_t)o->props_len,.payload=payload,.len=o->payload_len,
  .has_source_age=c->ext.source_timeout&&o->has_source_age,.source_age_ms=o->source_age_ms};
 if(o->props_len)memcpy(e->props,o->props,o->props_len);
 return 0;
}
void *moqt_core_cache_evict(moqt_core *c,unsigned track){
 moqt_cached *e=cache_oldest(c,track);
 if(!e)return NULL;
 e->used=false;return e->payload;
}
unsigned moqt_core_cache_count(const moqt_core *c,unsigned track){
 unsigned n=0;
 for(unsigned i=0;i<MOQT_CORE_CACHE;i++)if(c->cache[i].used&&(track>=c->track_count||c->cache[i].track==track))n++;
 return n;
}
void *moqt_core_cache_expire(moqt_core *c,uint64_t now_ms){
 for(unsigned i=0;i<MOQT_CORE_CACHE;i++){
  moqt_cached *e=&c->cache[i];
  if(!e->used)continue;
  uint32_t limit=c->tracks[e->track].cfg.max_cache_ms;
  if(limit&&now_ms-e->stored_ms>limit){e->used=false;return e->payload;}
 }
 return NULL;
}

/* Fetch order: groups in the requested order, objects ascending within a group. */
static bool fetch_before(const moqt_fetch *f,const moqt_cached *a,const moqt_cached *b){
 if(a->group!=b->group)return f->descending?a->group>b->group:a->group<b->group;
 return a->object<b->object;
}
static bool fetch_after_cursor(const moqt_fetch *f,const moqt_cached *e){
 if(!f->started)return true;
 if(e->group!=f->cursor.group)return f->descending?e->group<f->cursor.group:e->group>f->cursor.group;
 return e->object>f->cursor.object;
}
int moqt_core_fetch_next(moqt_core *c,unsigned k,uint8_t *head,size_t cap,size_t *head_len,void **payload,size_t *payload_len){
 if(k>=MOQT_CORE_FETCHES)return -1;
 moqt_fetch *f=&c->fetches[k];
 if(f->state!=MOQT_FETCH_READY&&f->state!=MOQT_FETCH_STREAMING)return f->state==MOQT_FETCH_DRAINED?0:-1;
 *head_len=0;*payload=NULL;*payload_len=0;
 size_t n=0;
 if(!f->header_sent){
  n=moqt_fetch_header_write(head,cap,f->request_id);
  if(!n)return -1;
  f->header_sent=true;f->state=MOQT_FETCH_STREAMING;
 }
 moqt_cached *best=NULL;
 for(unsigned i=0;i<MOQT_CORE_CACHE;i++){
  moqt_cached *e=&c->cache[i];
  if(!e->used||e->track!=f->track||!moqt_window_contains(&f->window,(moqt_loc){e->group,e->object}))continue;
  if(!fetch_after_cursor(f,e))continue;
  static uint8_t aged[MOQT_CORE_PROPS+16];
  const uint8_t *props=e->props;size_t props_len=e->props_len;
  if(e->has_source_age){
   /* The age keeps growing while the object waits in the cache. */
   props_len=props_with_age(e->props,e->props_len,e->source_age_ms+(c->now_ms-e->stored_ms),aged,sizeof(aged));
   props=aged;
  }
  moqt_filter_subject s={e->subgroup,e->object,e->priority,e->datagram,props,props_len};
  if(!moqt_filters_pass(&f->filters,&s))continue;
  if(!best||fetch_before(f,e,best))best=e;
 }
 if(!best){
  f->state=MOQT_FETCH_DRAINED;
  *head_len=n;
  return n?1:0;
 }
 static uint8_t served[MOQT_CORE_PROPS+16];
 const uint8_t *best_props=best->props;size_t best_len=best->props_len;
 if(best->has_source_age){
  best_len=props_with_age(best->props,best->props_len,best->source_age_ms+(c->now_ms-best->stored_ms),served,sizeof(served));
  best_props=served;
 }
 moqt_object o={.group=best->group,.subgroup=best->subgroup,.object=best->object,.priority=best->priority,
  .datagram=best->datagram,.props=best_props,.props_len=best_len,.payload_len=best->len};
 size_t m=moqt_fetch_object_write(head+n,cap-n,&o,f->descending,&f->prev);
 if(!m)return -1;
 f->started=true;f->cursor=(moqt_loc){best->group,best->object};
 *head_len=n+m;*payload=best->payload;*payload_len=best->len;
 return 1;
}
void moqt_core_fetch_closed(moqt_core *c,unsigned k){
 if(k>=MOQT_CORE_FETCHES||c->fetches[k].state==MOQT_FETCH_FREE)return;
 moqt_fetch *f=&c->fetches[k];
 if(f->sub<0){
  moqt_stream *s=stream_find(c,f->stream);
  if(s&&s->kind==MOQT_STREAM_FETCH){send_on(c,s->id,NULL,0,true);stream_done(c,s);}
 }
 f->state=MOQT_FETCH_FREE;
}

void moqt_core_sub_end(moqt_core *c,unsigned k,uint64_t status){
 if(k>=MOQT_CORE_SUBS)return;
 moqt_sub *u=&c->subs[k];
 if(u->state==MOQT_SUB_FREE||u->state==MOQT_SUB_ENDING)return;
 u->state=MOQT_SUB_ENDING;u->done_status=status;
 fills_cancel(c,k);
 emit(c,(moqt_action){.kind=MOQT_ACT_SUB_ENDING,.index=k,.track=u->track,.stream=u->stream,.code=status},NULL,0);
}
void moqt_core_sub_finish(moqt_core *c,unsigned k,const char *reason,size_t reason_len){
 if(k>=MOQT_CORE_SUBS||c->subs[k].state!=MOQT_SUB_ENDING)return;
 moqt_sub *u=&c->subs[k];
 static uint8_t b[1100];
 /* The reason phrase is diagnostic; it yields to the size limit the peer stated. */
 if(c->peer.ext.has_control_message&&reason_len+24>c->peer.ext.max_control_message)reason_len=0;
 size_t n=moqt_publish_done_write(b,sizeof(b),u->done_status,u->streams,reason,reason_len);
 moqt_stream *s=stream_find(c,u->stream);
 if(s&&n){send_on(c,s->id,b,n,true);stream_done(c,s);}
 u->state=MOQT_SUB_FREE;
}
void moqt_core_track_end(moqt_core *c,unsigned track){
 if(track>=c->track_count)return;
 c->tracks[track].ended=true;
 for(unsigned i=0;i<MOQT_CORE_SUBS;i++)
  if(c->subs[i].state!=MOQT_SUB_FREE&&c->subs[i].track==track)moqt_core_sub_end(c,i,MOQT_D_TRACK_ENDED);
 const moqt_track_cfg *t=&c->tracks[track].cfg;
 for(unsigned i=0;i<c->track_count;i++)if(!c->tracks[i].ended&&same_namespace(&c->tracks[i].cfg,t))return;
 for(unsigned i=0;i<MOQT_CORE_NS_SUBS;i++){
  moqt_ns_sub *n=&c->ns_subs[i];
  if(!n->used||n->tracks||!prefix_matches(n,t))continue;
  bool announced=false;
  for(unsigned j=0;j<c->track_count;j++)if(((n->announced>>j)&1U)&&same_namespace(&c->tracks[j].cfg,t))announced=true;
  if(!announced)continue; /* NAMESPACE_DONE never precedes NAMESPACE (section 9.15) */
  uint8_t b[200];size_t k=moqt_namespace_write(b,sizeof(b),MOQT_MSG_NAMESPACE_DONE,t->ns,t->ns_count,n->count);
  send_on(c,n->stream,b,k,false);
 }
}
void moqt_core_tick(moqt_core *c,uint64_t now_ms){
 c->now_ms=now_ms;
 for(unsigned i=0;i<MOQT_CORE_SUBS;i++){
  moqt_sub *u=&c->subs[i];
  if(u->state==MOQT_SUB_ESTABLISHED&&u->expires_at_ms&&now_ms>=u->expires_at_ms)moqt_core_sub_end(c,i,MOQT_D_EXPIRED);
 }
}
int moqt_core_state_notify(moqt_core *c,unsigned k){
 if(k>=MOQT_CORE_SUBS||c->subs[k].state!=MOQT_SUB_ESTABLISHED)return -1;
 moqt_sub *u=&c->subs[k];moqt_track *t=&c->tracks[u->track];
 moqt_publish_params p={.has_forward=true,.forward=u->forward,.has_largest=t->has_largest,.largest=t->largest,
  .has_location=u->has_location,.location=u->location};
 uint8_t b[96];size_t n=moqt_publish_state_notify_write(b,sizeof(b),&p);
 if(!n)return -1;
 send_on(c,u->stream,b,n,false);
 return 0;
}
unsigned moqt_core_active_subs(const moqt_core *c,unsigned track){
 unsigned n=0;
 for(unsigned i=0;i<MOQT_CORE_SUBS;i++)if(c->subs[i].state!=MOQT_SUB_FREE&&c->subs[i].track==track)n++;
 return n;
}
bool moqt_core_any_forwarding(const moqt_core *c,unsigned track){
 for(unsigned i=0;i<MOQT_CORE_SUBS;i++)if(c->subs[i].state!=MOQT_SUB_FREE&&c->subs[i].track==track&&sub_delivers(&c->subs[i]))return true;
 return false;
}
