#include "moqt21.h"
#include <string.h>
size_t moqt21_vi_write(uint8_t *d,size_t cap,uint64_t v) {
 size_t n=1;while(n<9 && (n==8 ? v>UINT64_C(0xffffffffffffff) : v >= (UINT64_C(1)<<(7*n))))n++;
 if(cap<n)return 0;
 uint64_t x=v;for(size_t i=n;i>1;i--){d[i-1]=(uint8_t)x;x>>=8;}
 d[0]=n==9?255:(uint8_t)((0xffU<<(9-n))|x);return n;
}
int moqt21_vi_read(const uint8_t *s,size_t len,uint64_t *v,size_t *used) {
 if(!len)return 0;
 size_t n=1;uint8_t bit=128;while(bit&&(s[0]&bit)){n++;bit>>=1;}
 if(len<n)return 0;
 uint64_t x=n==9?0:(s[0]&(0xffU>>n));for(size_t i=1;i<n;i++)x=(x<<8)|s[i];
 *v=x;*used=n;return 1;
}
size_t moqt21_message(uint8_t *d,size_t cap,uint64_t type,const uint8_t *b,size_t len) {
 if(len>65535)return 0;
 size_t n=moqt21_vi_write(d,cap,type);if(!n||cap-n<2||cap-n-2<len)return 0;
 d[n++]=(uint8_t)(len>>8);d[n++]=(uint8_t)len;if(len)memcpy(d+n,b,len);return n+len;
}
int moqt21_message_read(const uint8_t *s,size_t len,uint64_t *type,const uint8_t **body,size_t *bl,size_t *used) {
 size_t n=0;int r=moqt21_vi_read(s,len,type,&n);if(r!=1)return r;
 if(len-n<2)return 0;
 size_t k=((size_t)s[n]<<8)|s[n+1];if(k>MOQT21_CONTROL_CAP)return -1;
 if(len-n-2<k)return 0;
 *body=s+n+2;*bl=k;*used=n+2+k;return 1;
}
static int take(const uint8_t **p,size_t *n,uint64_t *v) {
 size_t k;int r=moqt21_vi_read(*p,*n,v,&k);if(r!=1)return -1;
 *p+=k;*n-=k;return 1;
}
static int field(const uint8_t **p,size_t *n,const char *expect) {
 uint64_t k;if(take(p,n,&k)<0||k>*n)return -1;
 if(k!=strlen(expect)||memcmp(*p,expect,(size_t)k))return -1;
 *p+=k;*n-=k;return 1;
}
size_t moqt21_setup(uint8_t *d,size_t cap,const char *authority) {
 uint8_t b[256];size_t a=strlen(authority),n=0,k;if(a>200)return 0;
 b[n++]=1;b[n++]=1;b[n++]='/'; /* PATH */
 b[n++]=4; /* delta from PATH1 to AUTHORITY5 */
 k=moqt21_vi_write(b+n,sizeof(b)-n,a);if(!k)return 0;n+=k;memcpy(b+n,authority,a);n+=a;
 return moqt21_message(d,cap,0x2f00,b,n);
}
int moqt21_server_setup(const uint8_t *p,size_t n) {
 uint64_t prev=0,delta,v;while(n){
  if(take(&p,&n,&delta)<0||delta>UINT64_MAX-prev)return -1;
  uint64_t key=prev+delta;if(key==1||key==5)return -1;
  /* Known singleton setup keys may not be duplicated. Unknowns may repeat. */
  if(delta==0 && (key==2||key==4||key==8||key==0x10))return -1;
  prev=key;if(take(&p,&n,&v)<0)return -1;
  if(key&1){if(v>65535||v>n)return -1;p+=v;n-=v;}
 }return 1;
}
int moqt21_subscribe_track(const uint8_t *p,size_t n,uint64_t *request,unsigned *alias) {
 uint64_t fields,params;
 if(take(&p,&n,request)<0||!(*request&1)||take(&p,&n,&fields)<0||fields!=2)return -1;
 if(field(&p,&n,"aiot")<0||field(&p,&n,"esp32")<0)return -1;
 const uint8_t *q=p;size_t z=n;
 if(field(&p,&n,"preview/jpeg")==1)*alias=1;
 else {p=q;n=z;if(field(&p,&n,"ai/events")<0)return -1;*alias=2;}
 if(take(&p,&n,&params)<0||params||n)return -1;
 return 1;
}
size_t moqt21_subscribe_ok_track(uint8_t *d,size_t cap,unsigned alias) {
 if(alias<1||alias>2)return 0;
 const uint8_t b[]={(uint8_t)alias,0}; /* alias1, no parameters/properties; defaults apply */
 return moqt21_message(d,cap,4,b,sizeof(b));
}
size_t moqt21_object_header_track(uint8_t *d,size_t cap,unsigned alias,uint64_t group,size_t len) {
 if(alias<1||alias>2)return 0;
 if(!len||len>MOQT21_MAX_JPEG||cap<4)return 0;
 size_t n=0,k;d[n++]=0x78; /* first object, default priority, end group, subgroup0 */
 d[n++]=(uint8_t)alias;k=moqt21_vi_write(d+n,cap-n,group);if(!k)return 0;n+=k;
 if(n==cap)return 0;
 d[n++]=0;k=moqt21_vi_write(d+n,cap-n,len);if(!k)return 0;return n+k;
}

int moqt21_subscribe(const uint8_t *p,size_t n,uint64_t *request){unsigned alias;return moqt21_subscribe_track(p,n,request,&alias)==1&&alias==1?1:-1;}
size_t moqt21_subscribe_ok(uint8_t *d,size_t cap){return moqt21_subscribe_ok_track(d,cap,1);}
size_t moqt21_object_header(uint8_t *d,size_t cap,uint64_t group,size_t len){return moqt21_object_header_track(d,cap,1,group,len);}

/* ---- Full draft-21 codec. ---- */
uint64_t moqt_rd_vi(moqt_rd *r){
 uint64_t v=0;size_t k=0;
 if(r->bad||moqt21_vi_read(r->p,r->n,&v,&k)!=1){r->bad=true;return 0;}
 r->p+=k;r->n-=k;return v;
}
uint8_t moqt_rd_u8(moqt_rd *r){
 if(r->bad||!r->n){r->bad=true;return 0;}
 r->n--;return *r->p++;
}
const uint8_t *moqt_rd_bytes(moqt_rd *r,size_t len){
 if(r->bad||len>r->n){r->bad=true;return NULL;}
 const uint8_t *p=r->p;r->p+=len;r->n-=len;return p;
}
void moqt_wr_vi(moqt_wr *w,uint64_t v){
 if(w->bad)return;
 size_t k=moqt21_vi_write(w->p+w->n,w->cap-w->n,v);
 if(!k)w->bad=true;else w->n+=k;
}
void moqt_wr_u8(moqt_wr *w,uint8_t v){
 if(w->bad||w->n==w->cap){w->bad=true;return;}
 w->p[w->n++]=v;
}
void moqt_wr_bytes(moqt_wr *w,const void *b,size_t len){
 if(w->bad||len>w->cap-w->n){w->bad=true;return;}
 if(len)memcpy(w->p+w->n,b,len);
 w->n+=len;
}
size_t moqt_wr_begin_message(moqt_wr *w,uint64_t type){
 moqt_wr_vi(w,type);size_t mark=w->n;moqt_wr_u8(w,0);moqt_wr_u8(w,0);return mark;
}
void moqt_wr_end_message(moqt_wr *w,size_t mark){
 if(w->bad)return;
 size_t len=w->n-mark-2;
 if(len>65535){w->bad=true;return;}
 w->p[mark]=(uint8_t)(len>>8);w->p[mark+1]=(uint8_t)len;
}

int moqt_kvp_next(moqt_kvp_it *it,moqt_kvp *out){
 if(!it->rd.n)return 0;
 uint64_t delta=moqt_rd_vi(&it->rd);
 if(it->rd.bad||delta>UINT64_MAX-it->prev)return -1;
 uint64_t type=it->prev+delta;
 it->prev=type;it->first=false;
 *out=(moqt_kvp){.type=type};
 if(type&1){
  uint64_t len=moqt_rd_vi(&it->rd);
  if(it->rd.bad||len>65535)return -1;
  out->is_bytes=true;out->len=(size_t)len;out->bytes=moqt_rd_bytes(&it->rd,(size_t)len);
 }else out->value=moqt_rd_vi(&it->rd);
 return it->rd.bad?-1:1;
}
void moqt_kvp_put_int(moqt_wr *w,uint64_t *prev,uint64_t type,uint64_t value){
 if((type&1)||type<*prev){w->bad=true;return;}
 moqt_wr_vi(w,type-*prev);moqt_wr_vi(w,value);*prev=type;
}
void moqt_kvp_put_bytes(moqt_wr *w,uint64_t *prev,uint64_t type,const void *b,size_t len){
 if(!(type&1)||type<*prev||len>65535){w->bad=true;return;}
 moqt_wr_vi(w,type-*prev);moqt_wr_vi(w,len);moqt_wr_bytes(w,b,len);*prev=type;
}
static int props_walk(const uint8_t *p,size_t n,bool object_scope,bool nested,unsigned *seen){
 moqt_kvp_it it=moqt_kvp_begin(p,n);moqt_kvp k;int r;
 while((r=moqt_kvp_next(&it,&k))==1){
  if(k.type==MOQT_X_IMMUTABLE){
   if(nested||(*seen&1U))return -1;
   *seen|=1U;
   if(props_walk(k.bytes,k.len,object_scope,true,seen))return -1;
  }else if(k.type==MOQT_X_PRIOR_GROUP_ID_GAP||k.type==MOQT_X_PRIOR_OBJECT_ID_GAP){
   unsigned bit=k.type==MOQT_X_PRIOR_GROUP_ID_GAP?2U:4U;
   if(!object_scope||(*seen&bit))return -1;
   *seen|=bit;
  }else if(object_scope&&k.type>=0x4000&&k.type<=0x7FFF)return -1; /* section 3.6 */
 }
 return r;
}
int moqt_props_validate(const uint8_t *p,size_t n,bool object_scope){
 unsigned seen=0;return props_walk(p,n,object_scope,false,&seen)?-1:0;
}
bool moqt_props_find_int(const uint8_t *p,size_t n,uint64_t type,uint64_t *value){
 moqt_kvp_it it=moqt_kvp_begin(p,n);moqt_kvp k;
 while(moqt_kvp_next(&it,&k)==1){
  if(k.type==type&&!k.is_bytes){*value=k.value;return true;}
  if(k.type==MOQT_X_IMMUTABLE&&type!=MOQT_X_IMMUTABLE){
   moqt_kvp_it in=moqt_kvp_begin(k.bytes,k.len);moqt_kvp j;
   while(moqt_kvp_next(&in,&j)==1)if(j.type==type&&!j.is_bytes){*value=j.value;return true;}
  }
 }
 return false;
}

int moqt_ns_read(moqt_rd *r,moqt_ns *ns,bool allow_empty){
 uint64_t count=moqt_rd_vi(r);
 if(r->bad||count>MOQT_NS_MAX_FIELDS||(!count&&!allow_empty))return MOQT_S_PROTOCOL_VIOLATION;
 ns->count=(uint8_t)count;ns->total=0;
 for(unsigned i=0;i<count;i++){
  uint64_t len=moqt_rd_vi(r);
  if(r->bad||!len||len>4096)return MOQT_S_PROTOCOL_VIOLATION;
  ns->f[i].p=moqt_rd_bytes(r,(size_t)len);ns->f[i].n=(uint16_t)len;ns->total+=(size_t)len;
  if(r->bad||ns->total>4096)return MOQT_S_PROTOCOL_VIOLATION;
 }
 return 0;
}
void moqt_ns_write(moqt_wr *w,const char *const *fields,unsigned count){
 moqt_wr_vi(w,count);
 for(unsigned i=0;i<count;i++){size_t n=strlen(fields[i]);moqt_wr_vi(w,n);moqt_wr_bytes(w,fields[i],n);}
}
static bool ns_field_is(const moqt_ns *ns,unsigned i,const char *text){
 size_t n=strlen(text);return ns->f[i].n==n&&!memcmp(ns->f[i].p,text,n);
}
bool moqt_ns_equals(const moqt_ns *ns,const char *const *fields,unsigned count){
 return ns->count==count&&moqt_ns_is_prefix(ns,fields,count);
}
bool moqt_ns_is_prefix(const moqt_ns *prefix,const char *const *fields,unsigned count){
 if(prefix->count>count)return false;
 for(unsigned i=0;i<prefix->count;i++)if(!ns_field_is(prefix,i,fields[i]))return false;
 return true;
}

static bool in_ranges(const moqt_filters *f,const moqt_rfilter *x,uint64_t v){
 for(unsigned i=0;i<x->count;i++)if(v>=f->r[x->first+i].lo&&v<=f->r[x->first+i].hi)return true;
 return false;
}
bool moqt_filters_pass(const moqt_filters *f,const moqt_filter_subject *s){
 if(!f->count)return true;
 /* Section 3.3.2: parameters sharing a SetID are AND'd; distinct SetIDs are OR'd. */
 for(unsigned i=0;i<f->count;i++){
  bool lead=true;
  for(unsigned j=0;j<i;j++)if(f->f[j].set==f->f[i].set)lead=false;
  if(!lead)continue;
  bool all=true;
  for(unsigned j=i;j<f->count&&all;j++){
   const moqt_rfilter *x=&f->f[j];uint64_t v=0;
   if(x->set!=f->f[i].set)continue;
   if(x->type==MOQT_P_SUBGROUP_FILTER){if(s->datagram)all=false;else v=s->subgroup;}
   else if(x->type==MOQT_P_OBJECTID_FILTER)v=s->object;
   else if(x->type==MOQT_P_PRIORITY_FILTER)v=s->priority;
   else if(x->type==MOQT_P_OBJECT_PROPERTY_FILTER){if(!moqt_props_find_int(s->props,s->props_len,x->prop,&v))all=false;}
   else all=false;
   if(all&&!in_ranges(f,x,v))all=false;
  }
  if(all)return true;
 }
 return false;
}
int moqt_filters_merge(moqt_filters *base,const moqt_filters *update){
 moqt_filters out={0};
 unsigned replaced=update->removed;
 for(unsigned i=0;i<update->count;i++)replaced|=1U<<(update->f[i].type-MOQT_P_SUBGROUP_FILTER);
 for(unsigned pass=0;pass<2;pass++){
  const moqt_filters *src=pass?update:base;
  for(unsigned i=0;i<src->count;i++){
   const moqt_rfilter *x=&src->f[i];
   if(!pass&&((replaced>>(x->type-MOQT_P_SUBGROUP_FILTER))&1U))continue;
   if(out.count==MOQT_MAX_RFILTERS||out.ranges+x->count>MOQT_MAX_RANGES)return MOQT_REJECT(MOQT_R_INVALID_FILTER);
   moqt_rfilter y=*x;y.first=out.ranges;
   for(unsigned k=0;k<x->count;k++)out.r[out.ranges++]=src->r[x->first+k];
   out.f[out.count++]=y;
  }
 }
 *base=out;return 0;
}

int moqt_window_resolve(const moqt_locfilter *f,bool present,bool has_largest,moqt_loc largest,bool for_fetch,moqt_window *w){
 *w=(moqt_window){0};
 moqt_loc next=has_largest?(moqt_loc){largest.group,largest.object+1}:(moqt_loc){0,0};
 unsigned fields=present?f->fields:0;
 if(fields==1){
  /* Relative to the Next Group: {Largest.Group + 1 - StartGroup, 0}, clamped at 0. */
  uint64_t next_group=has_largest?largest.group+1:0;
  w->has_start=true;w->start=(moqt_loc){f->v[0]>next_group?0:next_group-f->v[0],0};
 }else if(fields==2&&!f->v[0]&&!f->v[1]){
  w->has_start=true;w->start=next;
 }else if(fields>=2){
  w->has_start=true;w->start=(moqt_loc){f->v[0],f->v[1]};
  if(fields>=3){
   if(f->v[2]>UINT64_MAX-f->v[0])return MOQT_S_PROTOCOL_VIOLATION;
   w->has_end=true;w->end.group=f->v[0]+f->v[2];
   if(fields==4)w->end.object=f->v[3];else w->end_whole_group=true;
  }
 }
 if(for_fetch){
  if(!w->has_start){w->has_start=true;w->start=(moqt_loc){0,0};}
  if(!has_largest||moqt_loc_cmp(w->start,largest)>0)return MOQT_REJECT(MOQT_R_INVALID_RANGE);
  moqt_loc end=w->end;
  if(w->has_end&&w->end_whole_group)end.object=UINT64_MAX;
  if(!w->has_end||moqt_loc_cmp(end,largest)>0){w->end=largest;w->end_whole_group=false;}
  w->has_end=true;
  if(moqt_loc_cmp(w->end,w->start)<0&&!w->end_whole_group)return MOQT_REJECT(MOQT_R_INVALID_RANGE);
 }
 return 0;
}
bool moqt_window_contains(const moqt_window *w,moqt_loc at){
 if(w->has_start&&moqt_loc_cmp(at,w->start)<0)return false;
 if(!w->has_end)return true;
 if(at.group!=w->end.group)return at.group<w->end.group;
 return w->end_whole_group||at.object<=w->end.object;
}

/* Bit position of each known parameter in moqt_params.present. */
unsigned moqt_param_bit(uint64_t type){
 static const uint16_t known[]={MOQT_P_OBJECT_DELIVERY_TIMEOUT,MOQT_P_AUTHORIZATION_TOKEN,MOQT_P_RENDEZVOUS_TIMEOUT,
  MOQT_P_SUBGROUP_DELIVERY_TIMEOUT,MOQT_P_EXPIRES,MOQT_P_LARGEST_OBJECT,MOQT_P_FILL_TIMEOUT,MOQT_P_FORWARD,
  MOQT_P_SUBSCRIBER_PRIORITY,MOQT_P_LOCATION_FILTER,MOQT_P_GROUP_ORDER,MOQT_P_FILL_PARAMETERS,
  MOQT_P_SUBGROUP_FILTER,MOQT_P_OBJECTID_FILTER,MOQT_P_PRIORITY_FILTER,MOQT_P_OBJECT_PROPERTY_FILTER,
  MOQT_P_TRACK_PROPERTY_FILTER,MOQT_P_NEW_GROUP_REQUEST,MOQT_P_TRACK_NAMESPACE_PREFIX,MOQT_P_INCLUDE_PROPERTIES,
  MOQT_P_SOURCE_DELIVERY_TIMEOUT};
 for(unsigned i=0;i<sizeof(known)/sizeof(known[0]);i++)if(known[i]==type)return i;
 return 31;
}
#define PB(type) (1U<<moqt_param_bit(type))
static uint32_t allowed(moqt_ctx c){
 const uint32_t range=PB(MOQT_P_SUBGROUP_FILTER)|PB(MOQT_P_OBJECTID_FILTER)|PB(MOQT_P_PRIORITY_FILTER)|PB(MOQT_P_OBJECT_PROPERTY_FILTER);
 const uint32_t token=PB(MOQT_P_AUTHORIZATION_TOKEN);
 /* SOURCE_DELIVERY_TIMEOUT travels wherever the delivery timeouts do, once negotiated. */
 const uint32_t delivery=PB(MOQT_P_OBJECT_DELIVERY_TIMEOUT)|PB(MOQT_P_SUBGROUP_DELIVERY_TIMEOUT)|PB(MOQT_P_FORWARD)|
  PB(MOQT_P_SUBSCRIBER_PRIORITY)|PB(MOQT_P_LOCATION_FILTER)|range|PB(MOQT_P_SOURCE_DELIVERY_TIMEOUT);
 switch(c){
 case MOQT_CTX_SUBSCRIBE:case MOQT_CTX_TRACK_STATUS:
  return token|delivery|PB(MOQT_P_RENDEZVOUS_TIMEOUT)|PB(MOQT_P_GROUP_ORDER)|PB(MOQT_P_FILL_PARAMETERS)|
   PB(MOQT_P_NEW_GROUP_REQUEST)|PB(MOQT_P_INCLUDE_PROPERTIES);
 case MOQT_CTX_UPDATE_SUBSCRIPTION:
  return token|delivery|PB(MOQT_P_FILL_PARAMETERS)|PB(MOQT_P_NEW_GROUP_REQUEST);
 case MOQT_CTX_UPDATE_FETCH:return token|PB(MOQT_P_SUBSCRIBER_PRIORITY);
 case MOQT_CTX_UPDATE_NAMESPACE:return token|PB(MOQT_P_TRACK_NAMESPACE_PREFIX);
 case MOQT_CTX_UPDATE_TRACKS:return token|PB(MOQT_P_TRACK_NAMESPACE_PREFIX)|PB(MOQT_P_FORWARD);
 case MOQT_CTX_FETCH:
  return token|PB(MOQT_P_FILL_TIMEOUT)|PB(MOQT_P_SUBSCRIBER_PRIORITY)|PB(MOQT_P_LOCATION_FILTER)|
   PB(MOQT_P_GROUP_ORDER)|range|PB(MOQT_P_INCLUDE_PROPERTIES);
 case MOQT_CTX_SUBSCRIBE_NAMESPACE:return token;
 case MOQT_CTX_SUBSCRIBE_TRACKS:
  return token|delivery|PB(MOQT_P_GROUP_ORDER)|PB(MOQT_P_FILL_PARAMETERS)|PB(MOQT_P_NEW_GROUP_REQUEST)|
   PB(MOQT_P_INCLUDE_PROPERTIES)|PB(MOQT_P_TRACK_PROPERTY_FILTER);
 case MOQT_CTX_PUBLISH_OK:return delivery|PB(MOQT_P_GROUP_ORDER)|PB(MOQT_P_EXPIRES)|PB(MOQT_P_NEW_GROUP_REQUEST);
 case MOQT_CTX_NAMESPACE_OK:return PB(MOQT_P_EXPIRES);
 case MOQT_CTX_FILL:
  return PB(MOQT_P_FILL_TIMEOUT)|PB(MOQT_P_SUBSCRIBER_PRIORITY)|PB(MOQT_P_LOCATION_FILTER)|PB(MOQT_P_GROUP_ORDER)|range;
 }
 return 0;
}
static int token_read(moqt_rd *r){
 uint64_t len=moqt_rd_vi(r);const uint8_t *p=moqt_rd_bytes(r,(size_t)len);
 if(r->bad||len>65535)return MOQT_S_PROTOCOL_VIOLATION;
 moqt_rd t=moqt_rd_of(p,(size_t)len);uint64_t alias_type=moqt_rd_vi(&t);
 if(t.bad||alias_type>3)return MOQT_S_KEY_VALUE_FORMATTING_ERROR;
 if(alias_type==1){moqt_rd_vi(&t);moqt_rd_vi(&t);return t.bad?MOQT_S_KEY_VALUE_FORMATTING_ERROR:MOQT_S_AUTH_TOKEN_CACHE_OVERFLOW;}
 if(alias_type!=3){moqt_rd_vi(&t);return t.bad||t.n?MOQT_S_KEY_VALUE_FORMATTING_ERROR:MOQT_S_UNKNOWN_AUTH_TOKEN_ALIAS;}
 moqt_rd_vi(&t);
 return t.bad?MOQT_S_KEY_VALUE_FORMATTING_ERROR:0;
}
static int range_filter_read(moqt_rd *r,uint64_t type,unsigned max_ranges,moqt_filters *f){
 uint64_t len=moqt_rd_vi(r);const uint8_t *p=moqt_rd_bytes(r,(size_t)len);
 if(r->bad)return MOQT_S_PROTOCOL_VIOLATION;
 if(!len){f->removed|=(uint8_t)(1U<<(type-MOQT_P_SUBGROUP_FILTER));return 0;}
 if(!max_ranges)return MOQT_REJECT(MOQT_R_INVALID_FILTER);
 moqt_rd b=moqt_rd_of(p,(size_t)len);
 moqt_rfilter x={.type=(uint8_t)type,.set=moqt_rd_u8(&b),.first=f->ranges};
 if(type==MOQT_P_OBJECT_PROPERTY_FILTER||type==MOQT_P_TRACK_PROPERTY_FILTER){
  x.prop=moqt_rd_vi(&b);
  if(b.bad)return MOQT_S_PROTOCOL_VIOLATION;
  if(x.prop&1)return MOQT_REJECT(MOQT_R_INVALID_FILTER);
 }
 if(b.bad)return MOQT_S_PROTOCOL_VIOLATION;
 for(unsigned i=0;i<f->count;i++)if(f->f[i].type==x.type&&f->f[i].set==x.set&&f->f[i].prop==x.prop)
  return MOQT_REJECT(MOQT_R_INVALID_FILTER);
 uint64_t prior=0;
 while(b.n){
  uint64_t start=moqt_rd_vi(&b);
  if(b.bad)return MOQT_S_PROTOCOL_VIOLATION;
  if(start>UINT64_MAX-prior)return MOQT_REJECT(MOQT_R_INVALID_FILTER);
  moqt_range g={prior+start,UINT64_MAX};
  bool open=!b.n;
  if(!open){
   uint64_t end=moqt_rd_vi(&b);
   if(b.bad)return MOQT_S_PROTOCOL_VIOLATION;
   if(end>UINT64_MAX-g.lo)return MOQT_REJECT(MOQT_R_INVALID_FILTER);
   g.hi=g.lo+end;
  }
  if(type==MOQT_P_PRIORITY_FILTER&&(g.lo>255||(!open&&g.hi>255)))return MOQT_REJECT(MOQT_R_INVALID_FILTER);
  if(f->ranges>=max_ranges||f->ranges>=MOQT_MAX_RANGES)return MOQT_REJECT(MOQT_R_INVALID_FILTER);
  f->r[f->ranges++]=g;x.count++;prior=g.hi;
 }
 if(!x.count||f->count==MOQT_MAX_RFILTERS)return MOQT_REJECT(MOQT_R_INVALID_FILTER);
 f->f[f->count++]=x;
 return 0;
}
int moqt_params_read(moqt_rd *r,uint64_t count,moqt_ctx ctx,unsigned limits,moqt_params *out){
 memset(out,0,sizeof(*out));
 out->forward=1;out->subscriber_priority=128;out->include_properties=1;
 unsigned max_ranges=MOQT_LIMIT_RANGES(limits);
 uint32_t ok=allowed(ctx);uint64_t prev=0;int reject=0;
 /* Without negotiation the extension parameter is an unknown parameter (section 9.20). */
 if(!(limits&MOQT_LIMIT_SOURCE_TIMEOUT))ok&=~PB(MOQT_P_SOURCE_DELIVERY_TIMEOUT);
 for(uint64_t i=0;ctx==MOQT_CTX_FILL?r->n>0:i<count;i++){
  uint64_t delta=moqt_rd_vi(r);
  if(r->bad||delta>UINT64_MAX-prev)return MOQT_S_PROTOCOL_VIOLATION;
  uint64_t type=prev+delta;unsigned bit=moqt_param_bit(type);
  bool repeatable=type==MOQT_P_AUTHORIZATION_TOKEN||(type>=MOQT_P_SUBGROUP_FILTER&&type<=MOQT_P_TRACK_PROPERTY_FILTER);
  if(bit==31||!((ok>>bit)&1U))return MOQT_S_PROTOCOL_VIOLATION;
  if(i&&!delta&&!repeatable)return MOQT_S_PROTOCOL_VIOLATION;
  prev=type;out->present|=1U<<bit;
  int rv=0;
  switch(type){
  case MOQT_P_OBJECT_DELIVERY_TIMEOUT:out->object_timeout=moqt_rd_vi(r);break;
  case MOQT_P_SUBGROUP_DELIVERY_TIMEOUT:out->subgroup_timeout=moqt_rd_vi(r);break;
  case MOQT_P_RENDEZVOUS_TIMEOUT:out->rendezvous_timeout=moqt_rd_vi(r);break;
  case MOQT_P_FILL_TIMEOUT:out->fill_timeout=moqt_rd_vi(r);break;
  case MOQT_P_EXPIRES:out->expires=moqt_rd_vi(r);break;
  case MOQT_P_NEW_GROUP_REQUEST:out->new_group=moqt_rd_vi(r);break;
  case MOQT_P_SOURCE_DELIVERY_TIMEOUT:out->source_timeout=moqt_rd_vi(r);break;
  case MOQT_P_AUTHORIZATION_TOKEN:rv=token_read(r);out->tokens++;break;
  case MOQT_P_FORWARD:out->forward=moqt_rd_u8(r);if(out->forward>1)return MOQT_S_PROTOCOL_VIOLATION;break;
  case MOQT_P_INCLUDE_PROPERTIES:out->include_properties=moqt_rd_u8(r);if(out->include_properties>1)return MOQT_S_PROTOCOL_VIOLATION;break;
  case MOQT_P_SUBSCRIBER_PRIORITY:out->subscriber_priority=moqt_rd_u8(r);break;
  case MOQT_P_GROUP_ORDER:
   out->group_order=moqt_rd_u8(r);
   if(out->group_order!=MOQT_ORDER_ASCENDING&&out->group_order!=MOQT_ORDER_DESCENDING)return MOQT_S_PROTOCOL_VIOLATION;
   break;
  case MOQT_P_LOCATION_FILTER:{
   uint64_t len=moqt_rd_vi(r);const uint8_t *p=moqt_rd_bytes(r,(size_t)len);
   if(r->bad)return MOQT_S_PROTOCOL_VIOLATION;
   moqt_rd b=moqt_rd_of(p,(size_t)len);out->location.fields=0;
   while(b.n){
    if(out->location.fields==4)return MOQT_S_PROTOCOL_VIOLATION;
    out->location.v[out->location.fields++]=moqt_rd_vi(&b);
    if(b.bad)return MOQT_S_PROTOCOL_VIOLATION;
   }
   if(out->location.fields>=3&&out->location.v[2]>UINT64_MAX-out->location.v[0])return MOQT_S_PROTOCOL_VIOLATION;
   break;}
  case MOQT_P_FILL_PARAMETERS:{
   uint64_t len=moqt_rd_vi(r);out->fill=moqt_rd_bytes(r,(size_t)len);out->fill_len=(size_t)len;
   break;}
  case MOQT_P_TRACK_NAMESPACE_PREFIX:rv=moqt_ns_read(r,&out->prefix,true);break;
  default:rv=range_filter_read(r,type,max_ranges,&out->filters);break;
  }
  if(r->bad)return MOQT_S_PROTOCOL_VIOLATION;
  if(rv>0)return rv;
  if(rv<0&&!reject)reject=rv; /* keep parsing: a later violation still closes the session */
 }
 return reject;
}

int moqt_request_read(uint64_t type,const uint8_t *body,size_t len,unsigned limits,moqt_request *q){
 moqt_rd r=moqt_rd_of(body,len);
 memset(q,0,sizeof(*q));q->type=type;
 q->request_id=moqt_rd_vi(&r);
 if(r.bad)return MOQT_S_PROTOCOL_VIOLATION;
 bool by_prefix=type==MOQT_MSG_SUBSCRIBE_NAMESPACE||type==MOQT_MSG_SUBSCRIBE_TRACKS;
 int rv=moqt_ns_read(&r,&q->ns,by_prefix);
 if(rv)return rv;
 if(!by_prefix){
  uint64_t n=moqt_rd_vi(&r);q->name=moqt_rd_bytes(&r,(size_t)n);q->name_len=(size_t)n;
  if(r.bad||q->ns.total+n>4096)return MOQT_S_PROTOCOL_VIOLATION;
 }
 uint64_t count=moqt_rd_vi(&r);
 if(r.bad)return MOQT_S_PROTOCOL_VIOLATION;
 moqt_ctx ctx=type==MOQT_MSG_SUBSCRIBE?MOQT_CTX_SUBSCRIBE:type==MOQT_MSG_FETCH?MOQT_CTX_FETCH:
  type==MOQT_MSG_TRACK_STATUS?MOQT_CTX_TRACK_STATUS:type==MOQT_MSG_SUBSCRIBE_NAMESPACE?MOQT_CTX_SUBSCRIBE_NAMESPACE:MOQT_CTX_SUBSCRIBE_TRACKS;
 rv=moqt_params_read(&r,count,ctx,limits,&q->params);
 if(rv>0)return rv;
 if(r.n)return MOQT_S_PROTOCOL_VIOLATION;
 return rv;
}
int moqt_update_read(const uint8_t *body,size_t len,moqt_ctx ctx,unsigned limits,uint64_t *request_id,moqt_params *p){
 moqt_rd r=moqt_rd_of(body,len);
 *request_id=moqt_rd_vi(&r);uint64_t count=moqt_rd_vi(&r);
 if(r.bad)return MOQT_S_PROTOCOL_VIOLATION;
 int rv=moqt_params_read(&r,count,ctx,limits,p);
 if(rv>0)return rv;
 if(r.n)return MOQT_S_PROTOCOL_VIOLATION;
 return rv;
}
int moqt_request_ok_read(const uint8_t *body,size_t len,moqt_ctx ctx,unsigned limits,moqt_params *p){
 moqt_rd r=moqt_rd_of(body,len);uint64_t count=moqt_rd_vi(&r);
 if(r.bad)return MOQT_S_PROTOCOL_VIOLATION;
 int rv=moqt_params_read(&r,count,ctx,limits,p);
 if(rv>0)return rv;
 /* Track Properties are empty in PUBLISH_OK and PUBLISH_NAMESPACE_OK (section 9.3). */
 if(r.n)return MOQT_S_PROTOCOL_VIOLATION;
 return rv;
}
int moqt_request_error_read(const uint8_t *body,size_t len,moqt_error *e){
 moqt_rd r=moqt_rd_of(body,len);
 e->code=moqt_rd_vi(&r);e->retry=moqt_rd_vi(&r);
 uint64_t n=moqt_rd_vi(&r);
 if(r.bad||n>1024)return MOQT_S_PROTOCOL_VIOLATION;
 e->reason=moqt_rd_bytes(&r,(size_t)n);e->reason_len=(size_t)n;
 if(r.bad)return MOQT_S_PROTOCOL_VIOLATION;
 if(e->code==MOQT_R_REDIRECT){
  uint64_t uri=moqt_rd_vi(&r);moqt_rd_bytes(&r,(size_t)uri);moqt_ns ns;
  if(r.bad||moqt_ns_read(&r,&ns,true))return MOQT_S_PROTOCOL_VIOLATION;
  uint64_t name=moqt_rd_vi(&r);moqt_rd_bytes(&r,(size_t)name);
 }
 return r.bad||r.n?MOQT_S_PROTOCOL_VIOLATION:0;
}
int moqt_goaway_read(const uint8_t *body,size_t len,bool receiver_is_server,moqt_goaway *g){
 moqt_rd r=moqt_rd_of(body,len);uint64_t n=moqt_rd_vi(&r);
 if(r.bad||n>8192||(receiver_is_server&&n))return MOQT_S_PROTOCOL_VIOLATION;
 g->uri=moqt_rd_bytes(&r,(size_t)n);g->uri_len=(size_t)n;g->timeout=moqt_rd_vi(&r);
 return r.bad||r.n?MOQT_S_PROTOCOL_VIOLATION:0;
}
int moqt_setup_read(const uint8_t *body,size_t len,bool from_server,moqt_setup_options *o){
 memset(o,0,sizeof(*o));
 moqt_kvp_it it=moqt_kvp_begin(body,len);moqt_kvp k;int r;uint32_t seen=0;
 while((r=moqt_kvp_next(&it,&k))==1){
  if(k.type<=MOQT_OPT_MAX_REQUEST_UPDATES&&k.type!=MOQT_OPT_AUTHORIZATION_TOKEN&&k.type!=0&&k.type!=2){
   if((seen>>k.type)&1U)return MOQT_S_PROTOCOL_VIOLATION;
   seen|=1U<<k.type;
  }
  if(k.type>=MOQT_OPT_MAX_CONTROL_MESSAGE_SIZE&&k.type<=MOQT_OPT_SOURCE_TIMEOUT){
   unsigned bit=16U+(unsigned)((k.type-MOQT_OPT_MAX_CONTROL_MESSAGE_SIZE)>>1);
   if((seen>>bit)&1U)return MOQT_S_PROTOCOL_VIOLATION;
   seen|=1U<<bit;
  }
  if(k.type==MOQT_OPT_MAX_CONTROL_MESSAGE_SIZE){o->ext.has_control_message=true;o->ext.max_control_message=k.value;}
  else if(k.type==MOQT_OPT_MAX_SUBSCRIPTIONS){o->ext.has_subscriptions=true;o->ext.max_subscriptions=k.value;}
  else if(k.type==MOQT_OPT_MAX_FETCHES){o->ext.has_fetches=true;o->ext.max_fetches=k.value;}
  else if(k.type==MOQT_OPT_SOURCE_TIMEOUT)o->ext.source_timeout=k.value==1;
  if(k.type==MOQT_OPT_PATH&&from_server)return MOQT_S_INVALID_PATH;
  if(k.type==MOQT_OPT_AUTHORITY&&from_server)return MOQT_S_INVALID_AUTHORITY;
  if(k.type==MOQT_OPT_MAX_FILTER_RANGES)o->max_filter_ranges=k.value;
  else if(k.type==MOQT_OPT_MAX_REQUEST_UPDATES)o->max_request_updates=k.value;
  else if(k.type==MOQT_OPT_MAX_AUTH_TOKEN_CACHE_SIZE)o->max_token_cache=k.value;
  else if(k.type==MOQT_OPT_IMPLEMENTATION){o->implementation=k.bytes;o->implementation_len=k.len;}
  else if(k.type==MOQT_OPT_AUTHORIZATION_TOKEN){
   moqt_rd t=moqt_rd_of(k.bytes,k.len);uint64_t alias_type=moqt_rd_vi(&t);
   if(t.bad||alias_type>3)return MOQT_S_KEY_VALUE_FORMATTING_ERROR;
   if(!from_server&&(alias_type==0||alias_type==2))return MOQT_S_PROTOCOL_VIOLATION;
  }
 }
 return r?MOQT_S_KEY_VALUE_FORMATTING_ERROR:0;
}
size_t moqt_setup_write(uint8_t *d,size_t cap,const char *path,const char *authority,const char *implementation,uint64_t max_filter_ranges,uint64_t max_request_updates){
 return moqt_setup_write_ext(d,cap,path,authority,implementation,max_filter_ranges,max_request_updates,NULL);
}
size_t moqt_setup_write_ext(uint8_t *d,size_t cap,const char *path,const char *authority,const char *implementation,uint64_t max_filter_ranges,uint64_t max_request_updates,const moqt_setup_ext *x){
 moqt_wr w=moqt_wr_of(d,cap);size_t mark=moqt_wr_begin_message(&w,MOQT_MSG_SETUP);uint64_t prev=0;
 if(path)moqt_kvp_put_bytes(&w,&prev,MOQT_OPT_PATH,path,strlen(path));
 if(authority)moqt_kvp_put_bytes(&w,&prev,MOQT_OPT_AUTHORITY,authority,strlen(authority));
 if(max_filter_ranges)moqt_kvp_put_int(&w,&prev,MOQT_OPT_MAX_FILTER_RANGES,max_filter_ranges);
 if(implementation)moqt_kvp_put_bytes(&w,&prev,MOQT_OPT_IMPLEMENTATION,implementation,strlen(implementation));
 if(max_request_updates)moqt_kvp_put_int(&w,&prev,MOQT_OPT_MAX_REQUEST_UPDATES,max_request_updates);
 if(x){
  if(x->has_control_message)moqt_kvp_put_int(&w,&prev,MOQT_OPT_MAX_CONTROL_MESSAGE_SIZE,x->max_control_message);
  if(x->has_subscriptions)moqt_kvp_put_int(&w,&prev,MOQT_OPT_MAX_SUBSCRIPTIONS,x->max_subscriptions);
  if(x->has_fetches)moqt_kvp_put_int(&w,&prev,MOQT_OPT_MAX_FETCHES,x->max_fetches);
  if(x->source_timeout)moqt_kvp_put_int(&w,&prev,MOQT_OPT_SOURCE_TIMEOUT,1);
 }
 moqt_wr_end_message(&w,mark);
 return w.bad?0:w.n;
}

static void ok_params_write(moqt_wr *w,const moqt_ok_params *p){
 unsigned count=(p&&p->has_expires)+(p&&p->has_largest);
 moqt_wr_vi(w,count);uint64_t prev=0;
 if(p&&p->has_expires){moqt_wr_vi(w,MOQT_P_EXPIRES-prev);moqt_wr_vi(w,p->expires);prev=MOQT_P_EXPIRES;}
 if(p&&p->has_largest){moqt_wr_vi(w,MOQT_P_LARGEST_OBJECT-prev);moqt_wr_vi(w,p->largest.group);moqt_wr_vi(w,p->largest.object);}
}
size_t moqt_subscribe_ok_write(uint8_t *d,size_t cap,uint64_t alias,const moqt_ok_params *p,const uint8_t *props,size_t props_len){
 moqt_wr w=moqt_wr_of(d,cap);size_t mark=moqt_wr_begin_message(&w,MOQT_MSG_SUBSCRIBE_OK);
 moqt_wr_vi(&w,alias);ok_params_write(&w,p);moqt_wr_bytes(&w,props,props_len);
 moqt_wr_end_message(&w,mark);return w.bad?0:w.n;
}
size_t moqt_request_ok_write(uint8_t *d,size_t cap,const moqt_ok_params *p,const uint8_t *props,size_t props_len){
 moqt_wr w=moqt_wr_of(d,cap);size_t mark=moqt_wr_begin_message(&w,MOQT_MSG_REQUEST_OK);
 ok_params_write(&w,p);moqt_wr_bytes(&w,props,props_len);
 moqt_wr_end_message(&w,mark);return w.bad?0:w.n;
}
static void reason_write(moqt_wr *w,const char *reason,size_t len){
 if(len>1024)len=1024;
 moqt_wr_vi(w,len);moqt_wr_bytes(w,reason,len);
}
size_t moqt_request_error_write(uint8_t *d,size_t cap,uint64_t code,uint64_t retry,const char *reason){
 moqt_wr w=moqt_wr_of(d,cap);size_t mark=moqt_wr_begin_message(&w,MOQT_MSG_REQUEST_ERROR);
 moqt_wr_vi(&w,code);moqt_wr_vi(&w,retry);reason_write(&w,reason,reason?strlen(reason):0);
 moqt_wr_end_message(&w,mark);return w.bad?0:w.n;
}
size_t moqt_publish_done_write(uint8_t *d,size_t cap,uint64_t status,uint64_t streams,const char *reason,size_t reason_len){
 moqt_wr w=moqt_wr_of(d,cap);size_t mark=moqt_wr_begin_message(&w,MOQT_MSG_PUBLISH_DONE);
 moqt_wr_vi(&w,status);moqt_wr_vi(&w,streams);reason_write(&w,reason,reason_len);
 moqt_wr_end_message(&w,mark);return w.bad?0:w.n;
}
size_t moqt_fetch_ok_write(uint8_t *d,size_t cap,bool end_of_track,moqt_loc end,const uint8_t *props,size_t props_len){
 moqt_wr w=moqt_wr_of(d,cap);size_t mark=moqt_wr_begin_message(&w,MOQT_MSG_FETCH_OK);
 moqt_wr_u8(&w,end_of_track);moqt_wr_vi(&w,end.group);moqt_wr_vi(&w,end.object);moqt_wr_vi(&w,0);
 moqt_wr_bytes(&w,props,props_len);
 moqt_wr_end_message(&w,mark);return w.bad?0:w.n;
}
static void publish_params_write(moqt_wr *w,const moqt_publish_params *p){
 unsigned count=0;
 if(p)count=p->has_object_timeout+p->has_subgroup_timeout+p->has_largest+p->has_forward+p->has_priority+p->has_order+p->has_location+p->has_source_timeout;
 moqt_wr_vi(w,count);
 if(!count)return;
 uint64_t prev=0;
#define PARAM(flag,type,body) if(p->flag){moqt_wr_vi(w,(type)-prev);prev=(type);body;}
 PARAM(has_object_timeout,MOQT_P_OBJECT_DELIVERY_TIMEOUT,moqt_wr_vi(w,p->object_timeout))
 PARAM(has_subgroup_timeout,MOQT_P_SUBGROUP_DELIVERY_TIMEOUT,moqt_wr_vi(w,p->subgroup_timeout))
 PARAM(has_largest,MOQT_P_LARGEST_OBJECT,(moqt_wr_vi(w,p->largest.group),moqt_wr_vi(w,p->largest.object)))
 PARAM(has_forward,MOQT_P_FORWARD,moqt_wr_u8(w,p->forward))
 PARAM(has_priority,MOQT_P_SUBSCRIBER_PRIORITY,moqt_wr_u8(w,p->priority))
 if(p->has_location){
  uint8_t b[40];moqt_wr f=moqt_wr_of(b,sizeof(b));
  for(unsigned i=0;i<p->location.fields&&i<4;i++)moqt_wr_vi(&f,p->location.v[i]);
  moqt_wr_vi(w,MOQT_P_LOCATION_FILTER-prev);prev=MOQT_P_LOCATION_FILTER;
  moqt_wr_vi(w,f.n);moqt_wr_bytes(w,b,f.n);
 }
 PARAM(has_order,MOQT_P_GROUP_ORDER,moqt_wr_u8(w,p->order))
 PARAM(has_source_timeout,MOQT_P_SOURCE_DELIVERY_TIMEOUT,moqt_wr_vi(w,p->source_timeout))
#undef PARAM
}
size_t moqt_publish_write(uint8_t *d,size_t cap,uint64_t request_id,const char *const *ns,unsigned ns_count,const char *name,uint64_t alias,const moqt_publish_params *p,const uint8_t *props,size_t props_len){
 moqt_wr w=moqt_wr_of(d,cap);size_t mark=moqt_wr_begin_message(&w,MOQT_MSG_PUBLISH);
 moqt_wr_vi(&w,request_id);moqt_ns_write(&w,ns,ns_count);
 moqt_wr_vi(&w,strlen(name));moqt_wr_bytes(&w,name,strlen(name));
 moqt_wr_vi(&w,alias);publish_params_write(&w,p);moqt_wr_bytes(&w,props,props_len);
 moqt_wr_end_message(&w,mark);return w.bad?0:w.n;
}
size_t moqt_publish_state_notify_write(uint8_t *d,size_t cap,const moqt_publish_params *p){
 /* Section 9.10: only LARGEST_OBJECT, FORWARD and LOCATION_FILTER may appear. */
 moqt_publish_params q=*p;q.has_priority=q.has_order=q.has_object_timeout=q.has_subgroup_timeout=q.has_source_timeout=false;
 moqt_wr w=moqt_wr_of(d,cap);size_t mark=moqt_wr_begin_message(&w,MOQT_MSG_PUBLISH_STATE_NOTIFY);
 publish_params_write(&w,&q);
 moqt_wr_end_message(&w,mark);return w.bad?0:w.n;
}
size_t moqt_publish_namespace_write(uint8_t *d,size_t cap,uint64_t request_id,const char *const *ns,unsigned ns_count){
 moqt_wr w=moqt_wr_of(d,cap);size_t mark=moqt_wr_begin_message(&w,MOQT_MSG_PUBLISH_NAMESPACE);
 moqt_wr_vi(&w,request_id);moqt_ns_write(&w,ns,ns_count);moqt_wr_vi(&w,0);
 moqt_wr_end_message(&w,mark);return w.bad?0:w.n;
}
size_t moqt_namespace_write(uint8_t *d,size_t cap,uint64_t type,const char *const *ns,unsigned ns_count,unsigned skip){
 moqt_wr w=moqt_wr_of(d,cap);size_t mark=moqt_wr_begin_message(&w,type);
 if(skip>ns_count)w.bad=true;else moqt_ns_write(&w,ns+skip,ns_count-skip);
 moqt_wr_end_message(&w,mark);return w.bad?0:w.n;
}
size_t moqt_publish_skipped_write(uint8_t *d,size_t cap,const char *const *ns,unsigned ns_count,unsigned skip,const char *name){
 moqt_wr w=moqt_wr_of(d,cap);size_t mark=moqt_wr_begin_message(&w,MOQT_MSG_PUBLISH_SKIPPED);
 if(skip>ns_count)w.bad=true;else moqt_ns_write(&w,ns+skip,ns_count-skip);
 moqt_wr_vi(&w,strlen(name));moqt_wr_bytes(&w,name,strlen(name));
 moqt_wr_end_message(&w,mark);return w.bad?0:w.n;
}
size_t moqt_goaway_write(uint8_t *d,size_t cap,uint64_t timeout){
 moqt_wr w=moqt_wr_of(d,cap);size_t mark=moqt_wr_begin_message(&w,MOQT_MSG_GOAWAY);
 moqt_wr_vi(&w,0);moqt_wr_vi(&w,timeout);
 moqt_wr_end_message(&w,mark);return w.bad?0:w.n;
}

size_t moqt_subgroup_header_write(uint8_t *d,size_t cap,const moqt_object *o,bool with_props){
 moqt_wr w=moqt_wr_of(d,cap);
 /* Subgroup ID mode: 0 absent and zero, 1 absent and equal to the first Object ID, 2 present. */
 unsigned mode=!o->subgroup?0:o->subgroup==o->object&&o->first_object?1:2;
 unsigned flags=MOQT_SG_ALWAYS|(mode<<1)|(with_props?MOQT_SG_PROPERTIES:0)|(o->end_of_group?MOQT_SG_END_OF_GROUP:0)|
  (o->has_priority?0:MOQT_SG_DEFAULT_PRIORITY)|(o->first_object?MOQT_SG_FIRST_OBJECT:0);
 moqt_wr_vi(&w,flags);moqt_wr_vi(&w,o->alias);moqt_wr_vi(&w,o->group);
 if(mode==2)moqt_wr_vi(&w,o->subgroup);
 if(o->has_priority)moqt_wr_u8(&w,o->priority);
 return w.bad?0:w.n;
}
size_t moqt_subgroup_object_write(uint8_t *d,size_t cap,const moqt_object *o,bool with_props,bool first,uint64_t prev_object){
 moqt_wr w=moqt_wr_of(d,cap);
 if(!first&&o->object<=prev_object)return 0;
 if(o->props_len&&(!with_props||o->status!=MOQT_STATUS_NORMAL))return 0;
 if(o->status!=MOQT_STATUS_NORMAL&&o->payload_len)return 0;
 moqt_wr_vi(&w,first?o->object:o->object-prev_object-1);
 if(with_props){moqt_wr_vi(&w,o->props_len);moqt_wr_bytes(&w,o->props,o->props_len);}
 moqt_wr_vi(&w,o->payload_len);
 if(!o->payload_len)moqt_wr_vi(&w,o->status);
 return w.bad?0:w.n;
}
size_t moqt_datagram_header_write(uint8_t *d,size_t cap,const moqt_object *o){
 moqt_wr w=moqt_wr_of(d,cap);
 bool status=o->status!=MOQT_STATUS_NORMAL||!o->payload_len;
 if(o->props_len&&o->status!=MOQT_STATUS_NORMAL)return 0;
 if(status&&o->payload_len)return 0;
 /* END_OF_GROUP and STATUS are mutually exclusive (section 11.2.1). */
 if(status&&o->end_of_group&&o->status==MOQT_STATUS_NORMAL)return 0;
 unsigned flags=(o->props_len?MOQT_DG_PROPERTIES:0)|(o->end_of_group&&!status?MOQT_DG_END_OF_GROUP:0)|
  (!o->object?MOQT_DG_ZERO_OBJECT_ID:0)|(o->has_priority?0:MOQT_DG_DEFAULT_PRIORITY)|(status?MOQT_DG_STATUS:0);
 moqt_wr_vi(&w,flags);moqt_wr_vi(&w,o->alias);moqt_wr_vi(&w,o->group);
 if(o->object)moqt_wr_vi(&w,o->object);
 if(o->has_priority)moqt_wr_u8(&w,o->priority);
 if(o->props_len){moqt_wr_vi(&w,o->props_len);moqt_wr_bytes(&w,o->props,o->props_len);}
 if(status)moqt_wr_vi(&w,o->status);
 return w.bad?0:w.n;
}
size_t moqt_fetch_header_write(uint8_t *d,size_t cap,uint64_t request_id){
 moqt_wr w=moqt_wr_of(d,cap);moqt_wr_vi(&w,MOQT_STREAM_FETCH_HEADER);moqt_wr_vi(&w,request_id);
 return w.bad?0:w.n;
}
static bool fetch_group_delta(bool descending,const moqt_fetch_prev *prev,uint64_t group,uint64_t *delta){
 if(!prev->valid){*delta=group;return true;}
 if(descending){if(group>=prev->group)return false;*delta=prev->group-group-1;}
 else{if(group<=prev->group)return false;*delta=group-prev->group-1;}
 return true;
}
size_t moqt_fetch_object_write(uint8_t *d,size_t cap,const moqt_object *o,bool descending,moqt_fetch_prev *prev){
 moqt_wr w=moqt_wr_of(d,cap);unsigned flags=0;uint64_t group_delta=0,object_delta=0;
 bool new_group=!prev->valid||o->group!=prev->group;
 if(new_group){
  if(!fetch_group_delta(descending,prev,o->group,&group_delta))return 0;
  flags|=MOQT_FO_GROUP_ID|MOQT_FO_OBJECT_ID;object_delta=o->object;
 }else{
  if(o->object<=prev->object)return 0;
  if(o->object!=prev->object+1){flags|=MOQT_FO_OBJECT_ID;object_delta=o->object-prev->object;}
 }
 if(o->datagram)flags|=MOQT_FO_DATAGRAM;
 else if(!o->subgroup)flags|=0;
 else if(prev->has_subgroup&&o->subgroup==prev->subgroup)flags|=1;
 else if(prev->has_subgroup&&o->subgroup==prev->subgroup+1)flags|=2;
 else flags|=3;
 if(!prev->has_priority||o->priority!=prev->priority)flags|=MOQT_FO_PRIORITY;
 if(o->props_len)flags|=MOQT_FO_PROPERTIES;
 moqt_wr_vi(&w,flags);
 if(flags&MOQT_FO_GROUP_ID)moqt_wr_vi(&w,group_delta);
 if(!o->datagram&&(flags&MOQT_FO_SUBGROUP_MASK)==3)moqt_wr_vi(&w,o->subgroup);
 if(flags&MOQT_FO_OBJECT_ID)moqt_wr_vi(&w,object_delta);
 if(flags&MOQT_FO_PRIORITY)moqt_wr_u8(&w,o->priority);
 if(flags&MOQT_FO_PROPERTIES){moqt_wr_vi(&w,o->props_len);moqt_wr_bytes(&w,o->props,o->props_len);}
 moqt_wr_vi(&w,o->payload_len);
 if(w.bad)return 0;
 prev->valid=true;prev->group=o->group;prev->object=o->object;prev->priority=o->priority;prev->has_priority=true;
 if(!o->datagram){prev->subgroup=o->subgroup;prev->has_subgroup=true;}
 return w.n;
}
size_t moqt_fetch_end_range_write(uint8_t *d,size_t cap,uint64_t kind,moqt_loc at,bool descending,moqt_fetch_prev *prev){
 moqt_wr w=moqt_wr_of(d,cap);
 if(kind!=MOQT_FO_END_NONEXISTENT&&kind!=MOQT_FO_END_UNKNOWN&&kind!=MOQT_FO_END_TIMED_OUT)return 0;
 (void)descending;
 /* Section 11.4.1.2: the Group ID and Object ID fields are present and absolute. */
 moqt_wr_vi(&w,kind);moqt_wr_vi(&w,at.group);moqt_wr_vi(&w,at.object);
 if(w.bad)return 0;
 prev->valid=true;prev->group=at.group;prev->object=at.object;
 return w.n;
}
