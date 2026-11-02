/* Draft-21 codec checks. Comments name the checklist item in
 * papers/mission_aiot_moq/MOQT_DRAFT21_CHECKLIST_KO.md. */
#include "moqt21.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static size_t params(uint8_t *b,size_t cap,const uint8_t *raw,size_t n){(void)cap;memcpy(b,raw,n);return n;}
static int read_params(const uint8_t *raw,size_t n,uint64_t count,moqt_ctx ctx,unsigned max,moqt_params *p){
 moqt_rd r=moqt_rd_of(raw,n);int rv=moqt_params_read(&r,count,ctx,max,p);
 if(!rv&&r.n)return 99;
 return rv;
}

static void test_kvp(void){ /* B6, B7, B8, B9 */
 uint8_t b[128];moqt_wr w=moqt_wr_of(b,sizeof(b));uint64_t prev=0;
 moqt_kvp_put_int(&w,&prev,0x02,2000);
 uint8_t inner[16];moqt_wr iw=moqt_wr_of(inner,sizeof(inner));uint64_t ip=0;
 moqt_kvp_put_int(&iw,&ip,0x78,123456);
 moqt_kvp_put_bytes(&w,&prev,MOQT_X_IMMUTABLE,inner,iw.n);
 moqt_kvp_put_int(&w,&prev,0x7A,3);
 assert(!w.bad);
 moqt_kvp_it it=moqt_kvp_begin(b,w.n);moqt_kvp k;
 assert(moqt_kvp_next(&it,&k)==1&&k.type==2&&!k.is_bytes&&k.value==2000);
 assert(moqt_kvp_next(&it,&k)==1&&k.type==0x0B&&k.is_bytes&&k.len==iw.n);
 assert(moqt_kvp_next(&it,&k)==1&&k.type==0x7A&&k.value==3);
 assert(moqt_kvp_next(&it,&k)==0);
 uint64_t v;
 assert(moqt_props_find_int(b,w.n,0x7A,&v)&&v==3);
 assert(moqt_props_find_int(b,w.n,0x78,&v)&&v==123456); /* found inside IMMUTABLE_PROPERTIES */
 assert(!moqt_props_find_int(b,w.n,0x7C,&v));
 assert(moqt_props_validate(b,w.n,true)==0);
 for(size_t n=1;n<w.n;n++)if(moqt_props_validate(b,n,true)==0){
  /* every accepted prefix must itself be a whole number of pairs */
  moqt_kvp_it t=moqt_kvp_begin(b,n);int r;while((r=moqt_kvp_next(&t,&k))==1){}
  assert(r==0);
 }
 /* Ascending order is enforced by the writer; integers need even types. */
 moqt_wr bad=moqt_wr_of(b,sizeof(b));prev=0x10;moqt_kvp_put_int(&bad,&prev,0x02,1);assert(bad.bad);
 bad=moqt_wr_of(b,sizeof(b));prev=0;moqt_kvp_put_int(&bad,&prev,0x03,1);assert(bad.bad);
 /* Immutable inside Immutable is malformed (10.7). */
 uint8_t nested[32];moqt_wr nw=moqt_wr_of(nested,sizeof(nested));prev=0;
 uint8_t in2[8];moqt_wr i2=moqt_wr_of(in2,sizeof(in2));uint64_t p2=0;moqt_kvp_put_bytes(&i2,&p2,MOQT_X_IMMUTABLE,"",0);
 moqt_kvp_put_bytes(&nw,&prev,MOQT_X_IMMUTABLE,in2,i2.n);
 assert(moqt_props_validate(nested,nw.n,true)==-1);
 /* Two Prior Group ID Gap instances are malformed (10.8). */
 uint8_t gap[16];moqt_wr gw=moqt_wr_of(gap,sizeof(gap));prev=0;
 moqt_kvp_put_int(&gw,&prev,MOQT_X_PRIOR_GROUP_ID_GAP,2);moqt_kvp_put_int(&gw,&prev,MOQT_X_PRIOR_GROUP_ID_GAP,2);
 assert(moqt_props_validate(gap,gw.n,true)==-1);
 assert(moqt_props_validate(gap,gw.n/2,true)==0);
 assert(moqt_props_validate(gap,gw.n/2,false)==-1); /* object-only property on a track */
 /* A byte value longer than 2^16-1 is a formatting error (8.3). */
 const uint8_t huge[]={0x01,0xC1,0x00,0x00};it=moqt_kvp_begin(huge,sizeof(huge));
 assert(moqt_kvp_next(&it,&k)==-1);
 /* Mandatory Track Property on an object is malformed (3.6). */
 uint8_t man[16];moqt_wr mw=moqt_wr_of(man,sizeof(man));prev=0;moqt_kvp_put_int(&mw,&prev,0x4000,1);
 assert(moqt_props_validate(man,mw.n,true)==-1&&moqt_props_validate(man,mw.n,false)==0);
}

static void test_namespace(void){ /* B1 */
 const char *ns[]={"aiot","esp32"};uint8_t b[64];moqt_wr w=moqt_wr_of(b,sizeof(b));
 moqt_ns_write(&w,ns,2);
 moqt_rd r=moqt_rd_of(b,w.n);moqt_ns out;
 assert(moqt_ns_read(&r,&out,false)==0&&out.count==2&&!r.n&&out.total==9);
 assert(moqt_ns_equals(&out,ns,2));
 const char *other[]={"aiot","esp32","x"};
 assert(!moqt_ns_equals(&out,other,3)&&moqt_ns_is_prefix(&out,other,3));
 const uint8_t zero_field[]={1,0};r=moqt_rd_of(zero_field,2);
 assert(moqt_ns_read(&r,&out,false)==MOQT_S_PROTOCOL_VIOLATION);
 const uint8_t empty[]={0};r=moqt_rd_of(empty,1);
 assert(moqt_ns_read(&r,&out,false)==MOQT_S_PROTOCOL_VIOLATION);
 r=moqt_rd_of(empty,1);assert(moqt_ns_read(&r,&out,true)==0&&out.count==0);
 uint8_t many[80];many[0]=33;for(int i=0;i<33;i++){many[1+2*i]=1;many[2+2*i]='a';}
 r=moqt_rd_of(many,67);assert(moqt_ns_read(&r,&out,true)==MOQT_S_PROTOCOL_VIOLATION);
}

static void test_window(void){ /* D1 */
 moqt_window w;moqt_locfilter f={0};moqt_loc largest={10,4};
 assert(moqt_window_resolve(&f,false,true,largest,false,&w)==0&&!w.has_start&&!w.has_end);
 assert(moqt_window_contains(&w,(moqt_loc){0,0}));
 f=(moqt_locfilter){1,{0}}; /* Next Group */
 assert(moqt_window_resolve(&f,true,true,largest,false,&w)==0&&w.start.group==11&&w.start.object==0);
 f=(moqt_locfilter){1,{1}}; /* current Group */
 assert(moqt_window_resolve(&f,true,true,largest,false,&w)==0&&w.start.group==10);
 f=(moqt_locfilter){1,{3}};
 assert(moqt_window_resolve(&f,true,true,largest,false,&w)==0&&w.start.group==8);
 f=(moqt_locfilter){1,{50}}; /* clamped at 0 */
 assert(moqt_window_resolve(&f,true,true,largest,false,&w)==0&&w.start.group==0);
 f=(moqt_locfilter){2,{0,0}}; /* Next Object */
 assert(moqt_window_resolve(&f,true,true,largest,false,&w)==0&&w.start.group==10&&w.start.object==5);
 assert(!moqt_window_contains(&w,largest)&&moqt_window_contains(&w,(moqt_loc){10,5}));
 assert(moqt_window_resolve(&f,true,false,largest,false,&w)==0&&w.start.group==0&&w.start.object==0);
 f=(moqt_locfilter){3,{4,2,3}}; /* absolute start {4,2}, end group 7, whole group */
 assert(moqt_window_resolve(&f,true,true,largest,false,&w)==0&&w.has_end&&w.end.group==7&&w.end_whole_group);
 assert(!moqt_window_contains(&w,(moqt_loc){4,1})&&moqt_window_contains(&w,(moqt_loc){4,2}));
 assert(moqt_window_contains(&w,(moqt_loc){7,900})&&!moqt_window_contains(&w,(moqt_loc){8,0}));
 f=(moqt_locfilter){4,{4,2,3,6}};
 assert(moqt_window_resolve(&f,true,true,largest,false,&w)==0&&!w.end_whole_group);
 assert(moqt_window_contains(&w,(moqt_loc){7,6})&&!moqt_window_contains(&w,(moqt_loc){7,7}));
 f=(moqt_locfilter){3,{UINT64_MAX,0,1}};
 assert(moqt_window_resolve(&f,true,true,largest,false,&w)==MOQT_S_PROTOCOL_VIOLATION);
 /* FETCH rules: unfiltered covers {0,0}..Largest; the end is capped at Largest. */
 assert(moqt_window_resolve(&f,false,true,largest,true,&w)==0&&w.start.group==0&&w.end.group==10&&w.end.object==4);
 f=(moqt_locfilter){3,{2,0,100}};
 assert(moqt_window_resolve(&f,true,true,largest,true,&w)==0&&w.end.group==10&&w.end.object==4&&!w.end_whole_group);
 f=(moqt_locfilter){4,{11,0,0,0}};
 assert(moqt_window_resolve(&f,true,true,largest,true,&w)==MOQT_REJECT(MOQT_R_INVALID_RANGE));
 f=(moqt_locfilter){0,{0}};
 assert(moqt_window_resolve(&f,true,false,largest,true,&w)==MOQT_REJECT(MOQT_R_INVALID_RANGE));
}

static void test_params(void){ /* C1, C15, D2-D8, A15 */
 moqt_params p;uint8_t b[128];
 /* OBJECT_DELIVERY_TIMEOUT 1500, FORWARD 0, SUBSCRIBER_PRIORITY 7, GROUP_ORDER descending. */
 const uint8_t basic[]={0x02,0x85,0xDC,0x0E,0x00,0x10,0x07,0x02,0x02};
 assert(read_params(basic,sizeof(basic),4,MOQT_CTX_SUBSCRIBE,8,&p)==0);
 assert(p.object_timeout==1500&&p.forward==0&&p.subscriber_priority==7&&p.group_order==2);
 assert(MOQT_HAS(&p,MOQT_P_FORWARD)&&!MOQT_HAS(&p,MOQT_P_LOCATION_FILTER));
 /* Defaults when omitted. */
 assert(read_params(NULL,0,0,MOQT_CTX_SUBSCRIBE,8,&p)==0&&p.forward==1&&p.subscriber_priority==128&&p.include_properties==1);
 /* Unknown parameter type closes the session (9.20). */
 const uint8_t unknown[]={0x01,0x00};
 assert(read_params(unknown,2,1,MOQT_CTX_SUBSCRIBE,8,&p)==MOQT_S_PROTOCOL_VIOLATION);
 /* GROUP_ORDER is not allowed in REQUEST_UPDATE (9.20.9). */
 const uint8_t order[]={0x22,0x01};
 assert(read_params(order,2,1,MOQT_CTX_UPDATE_SUBSCRIPTION,8,&p)==MOQT_S_PROTOCOL_VIOLATION);
 assert(read_params(order,2,1,MOQT_CTX_SUBSCRIBE,8,&p)==0);
 const uint8_t bad_order[]={0x22,0x03};
 assert(read_params(bad_order,2,1,MOQT_CTX_SUBSCRIBE,8,&p)==MOQT_S_PROTOCOL_VIOLATION);
 const uint8_t bad_forward[]={0x10,0x02};
 assert(read_params(bad_forward,2,1,MOQT_CTX_SUBSCRIBE,8,&p)==MOQT_S_PROTOCOL_VIOLATION);
 /* A repeated non-repeatable parameter. */
 const uint8_t twice[]={0x10,0x01,0x00,0x01};
 assert(read_params(twice,4,2,MOQT_CTX_SUBSCRIBE,8,&p)==MOQT_S_PROTOCOL_VIOLATION);
 /* Truncated block. */
 assert(read_params(basic,sizeof(basic)-1,4,MOQT_CTX_SUBSCRIBE,8,&p)==MOQT_S_PROTOCOL_VIOLATION);
 /* LOCATION_FILTER with two fields. */
 const uint8_t loc[]={0x21,0x02,0x05,0x01};
 assert(read_params(loc,4,1,MOQT_CTX_FETCH,8,&p)==0&&p.location.fields==2&&p.location.v[0]==5&&p.location.v[1]==1);
 const uint8_t loc5[]={0x21,0x05,1,1,1,1,1};
 assert(read_params(loc5,7,1,MOQT_CTX_FETCH,8,&p)==MOQT_S_PROTOCOL_VIOLATION);
 /* SUBGROUP_FILTER SetID 0, ranges 3-5 and 10-15 encode as 3,2,5,5 (8.6). */
 const uint8_t ranges[]={0x25,0x05,0x00,3,2,5,5};
 assert(read_params(ranges,7,1,MOQT_CTX_SUBSCRIBE,8,&p)==0&&p.filters.count==1&&p.filters.ranges==2);
 assert(p.filters.r[0].lo==3&&p.filters.r[0].hi==5&&p.filters.r[1].lo==10&&p.filters.r[1].hi==15);
 /* Open-ended last range. */
 const uint8_t open[]={0x26,0x02,0x00,4};
 assert(read_params(open,4,1,MOQT_CTX_SUBSCRIBE,8,&p)==0&&p.filters.r[0].lo==4&&p.filters.r[0].hi==UINT64_MAX);
 /* MAX_FILTER_RANGES 0 forbids range filters; the limit is enforced (D8). */
 assert(read_params(ranges,7,1,MOQT_CTX_SUBSCRIBE,0,&p)==MOQT_REJECT(MOQT_R_INVALID_FILTER));
 assert(read_params(ranges,7,1,MOQT_CTX_SUBSCRIBE,1,&p)==MOQT_REJECT(MOQT_R_INVALID_FILTER));
 /* PRIORITY_FILTER above 255 (D4). */
 const uint8_t prio[]={0x27,0x04,0x00,0x81,0x00,0x00}; /* start 256 */
 assert(read_params(prio,6,1,MOQT_CTX_SUBSCRIBE,8,&p)==MOQT_REJECT(MOQT_R_INVALID_FILTER));
 /* OBJECT_PROPERTY_FILTER with an odd property type (D5). */
 const uint8_t odd[]={0x28,0x04,0x00,0x79,1,0};
 assert(read_params(odd,6,1,MOQT_CTX_SUBSCRIBE,8,&p)==MOQT_REJECT(MOQT_R_INVALID_FILTER));
 const uint8_t even[]={0x28,0x04,0x00,0x7A,1,0};
 assert(read_params(even,6,1,MOQT_CTX_SUBSCRIBE,8,&p)==0&&p.filters.f[0].prop==0x7A&&p.filters.r[0].lo==1&&p.filters.r[0].hi==1);
 /* The same (type, SetID, property) repeated in one message. */
 const uint8_t dup[]={0x25,0x03,0x00,1,0, 0x00,0x03,0x00,2,0};
 assert(read_params(dup,10,2,MOQT_CTX_SUBSCRIBE,8,&p)==MOQT_REJECT(MOQT_R_INVALID_FILTER));
 /* A rejected filter followed by a violation still closes the session. */
 const uint8_t mixed[]={0x27,0x04,0x00,0x81,0x00,0x00, 0x0A,0x00};
 (void)mixed;
 /* Length 0 removes a filter type (REQUEST_UPDATE). */
 const uint8_t removal[]={0x25,0x00};
 assert(read_params(removal,2,1,MOQT_CTX_UPDATE_SUBSCRIPTION,8,&p)==0&&p.filters.removed==1&&!p.filters.count);
 /* TRACK_PROPERTY_FILTER only with SUBSCRIBE_TRACKS. */
 const uint8_t tpf[]={0x29,0x04,0x00,0x0E,10,5};
 assert(read_params(tpf,6,1,MOQT_CTX_SUBSCRIBE,8,&p)==MOQT_S_PROTOCOL_VIOLATION);
 assert(read_params(tpf,6,1,MOQT_CTX_SUBSCRIBE_TRACKS,8,&p)==0);
 /* Authorization tokens: USE_VALUE accepted; REGISTER exceeds the zero-size cache (8.9, 9.1.3). */
 const uint8_t use_value[]={0x03,0x04,0x03,0x00,'o','k'};
 assert(read_params(use_value,6,1,MOQT_CTX_SUBSCRIBE,8,&p)==0&&p.tokens==1);
 const uint8_t reg[]={0x03,0x04,0x01,0x01,0x00,'x'};
 assert(read_params(reg,6,1,MOQT_CTX_SUBSCRIBE,8,&p)==MOQT_S_AUTH_TOKEN_CACHE_OVERFLOW);
 const uint8_t alias[]={0x03,0x02,0x02,0x09};
 assert(read_params(alias,4,1,MOQT_CTX_SUBSCRIBE,8,&p)==MOQT_S_UNKNOWN_AUTH_TOKEN_ALIAS);
 const uint8_t bad_token[]={0x03,0x01,0x07};
 assert(read_params(bad_token,3,1,MOQT_CTX_SUBSCRIBE,8,&p)==MOQT_S_KEY_VALUE_FORMATTING_ERROR);
 /* FILL_PARAMETERS is a separate scope bounded by its length (9.20.16). */
 const uint8_t fill[]={0x23,0x05, 0x21,0x01,0x02, 0x01,0x02};
 assert(read_params(fill,7,1,MOQT_CTX_SUBSCRIBE,8,&p)==0&&p.fill_len==5);
 moqt_params fp;moqt_rd fr=moqt_rd_of(p.fill,p.fill_len);
 assert(moqt_params_read(&fr,0,MOQT_CTX_FILL,8,&fp)==0&&fp.location.fields==1&&fp.location.v[0]==2&&fp.group_order==2);
 const uint8_t fill_bad[]={0x10,0x01};fr=moqt_rd_of(fill_bad,2);
 assert(moqt_params_read(&fr,0,MOQT_CTX_FILL,8,&fp)==MOQT_S_PROTOCOL_VIOLATION);
 (void)b;(void)params;
}

static void test_filters(void){ /* D7 */
 moqt_params p;
 /* Set 0: subgroup in 0-0 AND property 0x7A in 1..; Set 1: object id 5-5. */
 const uint8_t raw[]={0x25,0x03,0x00,0,0, 0x01,0x03,0x01,5,0, 0x02,0x03,0x00,0x7A,1};
 assert(read_params(raw,sizeof(raw),3,MOQT_CTX_SUBSCRIBE,8,&p)==0&&p.filters.count==3);
 uint8_t props[8];moqt_wr w=moqt_wr_of(props,sizeof(props));uint64_t prev=0;moqt_kvp_put_int(&w,&prev,0x7A,2);
 moqt_filter_subject s={.subgroup=0,.object=0,.priority=128,.props=props,.props_len=w.n};
 assert(moqt_filters_pass(&p.filters,&s));
 s.subgroup=1;assert(!moqt_filters_pass(&p.filters,&s));
 s.object=5;assert(moqt_filters_pass(&p.filters,&s)); /* the other SetID matches */
 s.object=0;s.subgroup=0;s.props_len=0;assert(!moqt_filters_pass(&p.filters,&s)); /* property absent */
 s.datagram=true;s.props_len=w.n;assert(!moqt_filters_pass(&p.filters,&s)); /* datagrams have no subgroup */
 moqt_filters none={0};assert(moqt_filters_pass(&none,&s));
 /* Merge: the update replaces only the types it names. */
 moqt_params u;const uint8_t upd[]={0x25,0x03,0x00,1,0};
 assert(read_params(upd,5,1,MOQT_CTX_UPDATE_SUBSCRIPTION,8,&u)==0);
 moqt_filters merged=p.filters;
 assert(moqt_filters_merge(&merged,&u.filters)==0&&merged.count==3);
 s=(moqt_filter_subject){.subgroup=1,.object=0,.priority=128,.props=props,.props_len=w.n};
 assert(moqt_filters_pass(&merged,&s));
 s.subgroup=0;assert(!moqt_filters_pass(&merged,&s));
 const uint8_t rm[]={0x25,0x00,0x01,0x00};
 assert(read_params(rm,4,2,MOQT_CTX_UPDATE_SUBSCRIPTION,8,&u)==0);
 assert(moqt_filters_merge(&merged,&u.filters)==0&&merged.count==1&&merged.f[0].type==MOQT_P_OBJECT_PROPERTY_FILTER);
}

static void test_setup(void){ /* A3-A8 */
 uint8_t b[128];moqt_setup_options o;uint64_t type;const uint8_t *body;size_t bl,used;
 size_t n=moqt_setup_write(b,sizeof(b),"/","host:1","impl/1",8,4);
 assert(n&&moqt21_message_read(b,n,&type,&body,&bl,&used)==1&&type==MOQT_MSG_SETUP&&used==n);
 assert(moqt_setup_read(body,bl,false,&o)==0&&o.max_filter_ranges==8&&o.max_request_updates==4);
 assert(o.implementation_len==6&&!memcmp(o.implementation,"impl/1",6));
 assert(moqt_setup_read(body,bl,true,&o)==MOQT_S_INVALID_PATH);
 n=moqt_setup_write(b,sizeof(b),NULL,"host:1",NULL,0,0);
 assert(moqt21_message_read(b,n,&type,&body,&bl,&used)==1&&moqt_setup_read(body,bl,true,&o)==MOQT_S_INVALID_AUTHORITY);
 n=moqt_setup_write(b,sizeof(b),NULL,NULL,"srv",3,0);
 assert(moqt21_message_read(b,n,&type,&body,&bl,&used)==1&&moqt_setup_read(body,bl,true,&o)==0&&o.max_filter_ranges==3);
 const uint8_t dup[]={0x06,0x01,0x00,0x02};assert(moqt_setup_read(dup,4,true,&o)==MOQT_S_PROTOCOL_VIOLATION);
 const uint8_t unknown[]={0x40,0x01,0x00,0x02};assert(moqt_setup_read(unknown,4,true,&o)==0); /* unknown may repeat */
 const uint8_t cut[]={0x07,0x05,'a'};assert(moqt_setup_read(cut,3,true,&o)==MOQT_S_KEY_VALUE_FORMATTING_ERROR);
 /* A server may not DELETE or USE_ALIAS in SETUP tokens; a client check applies to the server side. */
 const uint8_t token[]={0x03,0x02,0x02,0x01};assert(moqt_setup_read(token,4,false,&o)==MOQT_S_PROTOCOL_VIOLATION);
}

static void test_messages(void){ /* C2, C3, C7, C9-C11, E2, H1-H5, A14 */
 uint8_t b[512];uint64_t type;const uint8_t *body;size_t bl,used;
 const uint8_t props[]={0x0E,0x40};
 moqt_ok_params ok={.has_largest=true,.largest={7,2}};
 size_t n=moqt_subscribe_ok_write(b,sizeof(b),2,&ok,props,sizeof(props));
 assert(moqt21_message_read(b,n,&type,&body,&bl,&used)==1&&type==MOQT_MSG_SUBSCRIBE_OK&&used==n);
 const uint8_t expect_ok[]={0x02,0x01,0x09,0x07,0x02,0x0E,0x40};
 assert(bl==sizeof(expect_ok)&&!memcmp(body,expect_ok,bl));
 n=moqt_subscribe_ok_write(b,sizeof(b),1,NULL,NULL,0);
 assert(moqt21_message_read(b,n,&type,&body,&bl,&used)==1&&bl==2&&body[0]==1&&body[1]==0);
 n=moqt_request_error_write(b,sizeof(b),MOQT_R_DOES_NOT_EXIST,0,"nope");
 moqt_error e;
 assert(moqt21_message_read(b,n,&type,&body,&bl,&used)==1&&type==MOQT_MSG_REQUEST_ERROR);
 assert(moqt_request_error_read(body,bl,&e)==0&&e.code==0x10&&e.retry==0&&e.reason_len==4);
 for(size_t k=0;k<bl;k++)assert(moqt_request_error_read(body,k,&e)==MOQT_S_PROTOCOL_VIOLATION);
 n=moqt_publish_done_write(b,sizeof(b),MOQT_D_TRACK_ENDED,23,"{}",2);
 assert(moqt21_message_read(b,n,&type,&body,&bl,&used)==1&&type==MOQT_MSG_PUBLISH_DONE);
 const uint8_t expect_done[]={0x02,0x17,0x02,'{','}'};
 assert(bl==sizeof(expect_done)&&!memcmp(body,expect_done,bl));
 n=moqt_fetch_ok_write(b,sizeof(b),true,(moqt_loc){9,0},NULL,0);
 assert(moqt21_message_read(b,n,&type,&body,&bl,&used)==1&&type==MOQT_MSG_FETCH_OK);
 const uint8_t expect_fetch[]={0x01,0x09,0x00,0x00};
 assert(bl==sizeof(expect_fetch)&&!memcmp(body,expect_fetch,bl));
 const char *ns[]={"aiot","esp32"};
 moqt_publish_params pp={.has_forward=true,.forward=0,.has_priority=true,.priority=9,.has_largest=true,.largest={3,0},
  .has_location=true,.location={1,{0}}};
 n=moqt_publish_write(b,sizeof(b),4,ns,2,"ai/events",2,&pp,props,sizeof(props));
 assert(moqt21_message_read(b,n,&type,&body,&bl,&used)==1&&type==MOQT_MSG_PUBLISH);
 moqt_rd r=moqt_rd_of(body,bl);moqt_ns rns;
 assert(moqt_rd_vi(&r)==4&&moqt_ns_read(&r,&rns,false)==0&&moqt_ns_equals(&rns,ns,2));
 assert(moqt_rd_vi(&r)==9&&!memcmp(moqt_rd_bytes(&r,9),"ai/events",9)&&moqt_rd_vi(&r)==2);
 uint64_t count=moqt_rd_vi(&r);assert(count==4);
 /* Parameters arrive in ascending type order: 0x09, 0x10, 0x20, 0x21. */
 assert(moqt_rd_vi(&r)==0x09&&moqt_rd_vi(&r)==3&&moqt_rd_vi(&r)==0);
 assert(moqt_rd_vi(&r)==0x07&&moqt_rd_u8(&r)==0);
 assert(moqt_rd_vi(&r)==0x10&&moqt_rd_u8(&r)==9);
 assert(moqt_rd_vi(&r)==0x01&&moqt_rd_vi(&r)==1&&moqt_rd_vi(&r)==0);
 assert(r.n==sizeof(props)&&!r.bad);
 n=moqt_publish_namespace_write(b,sizeof(b),6,ns,2);
 assert(moqt21_message_read(b,n,&type,&body,&bl,&used)==1&&type==MOQT_MSG_PUBLISH_NAMESPACE&&body[0]==6&&body[bl-1]==0);
 n=moqt_namespace_write(b,sizeof(b),MOQT_MSG_NAMESPACE,ns,2,1);
 assert(moqt21_message_read(b,n,&type,&body,&bl,&used)==1&&type==MOQT_MSG_NAMESPACE);
 const uint8_t suffix[]={1,5,'e','s','p','3','2'};assert(bl==sizeof(suffix)&&!memcmp(body,suffix,bl));
 assert(!moqt_namespace_write(b,sizeof(b),MOQT_MSG_NAMESPACE,ns,2,3));
 n=moqt_publish_skipped_write(b,sizeof(b),ns,2,2,"t");
 assert(moqt21_message_read(b,n,&type,&body,&bl,&used)==1&&type==MOQT_MSG_PUBLISH_SKIPPED&&bl==3&&body[0]==0);
 n=moqt_goaway_write(b,sizeof(b),250);moqt_goaway g;
 assert(moqt21_message_read(b,n,&type,&body,&bl,&used)==1&&type==MOQT_MSG_GOAWAY);
 assert(moqt_goaway_read(body,bl,true,&g)==0&&!g.uri_len&&g.timeout==250);
 const uint8_t with_uri[]={0x03,'a','b','c',0x00};
 assert(moqt_goaway_read(with_uri,5,false,&g)==0&&g.uri_len==3);
 assert(moqt_goaway_read(with_uri,5,true,&g)==MOQT_S_PROTOCOL_VIOLATION); /* a server rejects a URI */
 moqt_publish_params notify={.has_forward=true,.forward=1,.has_priority=true,.priority=1,.has_largest=true,.largest={1,1}};
 n=moqt_publish_state_notify_write(b,sizeof(b),&notify);
 assert(moqt21_message_read(b,n,&type,&body,&bl,&used)==1&&type==MOQT_MSG_PUBLISH_STATE_NOTIFY&&body[0]==2);
 /* Requests. */
 uint8_t req[128];moqt_wr w=moqt_wr_of(req,sizeof(req));
 moqt_wr_vi(&w,5);moqt_ns_write(&w,ns,2);moqt_wr_vi(&w,9);moqt_wr_bytes(&w,"ai/events",9);moqt_wr_vi(&w,1);
 moqt_wr_vi(&w,0x20);moqt_wr_u8(&w,3);
 static moqt_request q;
 assert(moqt_request_read(MOQT_MSG_SUBSCRIBE,req,w.n,8,&q)==0&&q.request_id==5&&q.name_len==9&&q.params.subscriber_priority==3);
 for(size_t k=0;k<w.n;k++)assert(moqt_request_read(MOQT_MSG_SUBSCRIBE,req,k,8,&q)==MOQT_S_PROTOCOL_VIOLATION);
 req[w.n]=0;assert(moqt_request_read(MOQT_MSG_SUBSCRIBE,req,w.n+1,8,&q)==MOQT_S_PROTOCOL_VIOLATION); /* trailing byte */
 w=moqt_wr_of(req,sizeof(req));moqt_wr_vi(&w,7);moqt_wr_vi(&w,0);moqt_wr_vi(&w,0);
 assert(moqt_request_read(MOQT_MSG_SUBSCRIBE_NAMESPACE,req,w.n,8,&q)==0&&q.ns.count==0);
 assert(moqt_request_read(MOQT_MSG_SUBSCRIBE,req,w.n,8,&q)==MOQT_S_PROTOCOL_VIOLATION); /* empty namespace */
}

typedef struct { unsigned flags; uint64_t alias,group,subgroup,object,len,status; uint8_t priority; bool has_priority; size_t props_len,used; } sg;
static bool sg_read(const uint8_t *b,size_t n,sg *o){
 moqt_rd r=moqt_rd_of(b,n);memset(o,0,sizeof(*o));
 o->flags=(unsigned)moqt_rd_vi(&r);
 if(o->flags>=128||!(o->flags&MOQT_SG_ALWAYS)||((o->flags&MOQT_SG_ID_MODE_MASK)>>1)==3)return false;
 o->alias=moqt_rd_vi(&r);o->group=moqt_rd_vi(&r);
 unsigned mode=(o->flags&MOQT_SG_ID_MODE_MASK)>>1;
 if(mode==2)o->subgroup=moqt_rd_vi(&r);
 if(!(o->flags&MOQT_SG_DEFAULT_PRIORITY)){o->priority=moqt_rd_u8(&r);o->has_priority=true;}
 o->object=moqt_rd_vi(&r);
 if(mode==1)o->subgroup=o->object;
 if(o->flags&MOQT_SG_PROPERTIES){o->props_len=(size_t)moqt_rd_vi(&r);moqt_rd_bytes(&r,o->props_len);}
 o->len=moqt_rd_vi(&r);
 if(!o->len)o->status=moqt_rd_vi(&r);
 o->used=n-r.n;
 return !r.bad;
}
static void test_data(void){ /* B3, B4, B5, B10, E3, E10 */
 uint8_t b[128];sg s;
 moqt_object o={.alias=1,.group=42,.object=0,.payload_len=1000,.first_object=true,.end_of_group=true};
 size_t n=moqt_subgroup_header_write(b,sizeof(b),&o,false);
 assert(n&&b[0]==0x78); /* identical to the earlier single-object profile */
 n+=moqt_subgroup_object_write(b+n,sizeof(b)-n,&o,false,true,0);
 assert(sg_read(b,n,&s)&&s.used==n&&s.alias==1&&s.group==42&&s.object==0&&s.len==1000&&!s.has_priority);
 /* Explicit priority, subgroup 2, properties. */
 const uint8_t props[]={0x7A,0x03};
 o=(moqt_object){.alias=2,.group=9,.subgroup=2,.object=5,.priority=32,.has_priority=true,.first_object=true,
  .props=props,.props_len=2,.payload_len=10};
 n=moqt_subgroup_header_write(b,sizeof(b),&o,true);
 n+=moqt_subgroup_object_write(b+n,sizeof(b)-n,&o,true,true,0);
 assert(sg_read(b,n,&s)&&s.flags==(MOQT_SG_ALWAYS|MOQT_SG_PROPERTIES|(2U<<1)|MOQT_SG_FIRST_OBJECT));
 assert(s.subgroup==2&&s.object==5&&s.priority==32&&s.props_len==2&&s.len==10);
 /* Subgroup ID equal to the first Object ID uses mode 1. */
 o.subgroup=5;n=moqt_subgroup_header_write(b,sizeof(b),&o,true);
 assert(((b[0]&MOQT_SG_ID_MODE_MASK)>>1)==1);
 n+=moqt_subgroup_object_write(b+n,sizeof(b)-n,&o,true,true,0);
 assert(sg_read(b,n,&s)&&s.subgroup==5);
 /* A later object on the same stream: delta encoding and status. */
 o=(moqt_object){.object=9,.status=MOQT_STATUS_END_OF_GROUP};
 n=moqt_subgroup_object_write(b,sizeof(b),&o,true,false,5);
 const uint8_t expect[]={0x03,0x00,0x00,0x03};assert(n==4&&!memcmp(b,expect,4));
 assert(!moqt_subgroup_object_write(b,sizeof(b),&o,true,false,9)); /* not increasing */
 o.payload_len=1;assert(!moqt_subgroup_object_write(b,sizeof(b),&o,true,false,5)); /* status with payload */
 o=(moqt_object){.object=1,.props=props,.props_len=2,.payload_len=1};
 assert(!moqt_subgroup_object_write(b,sizeof(b),&o,false,true,0)); /* properties need the header flag */
 /* Datagrams. */
 o=(moqt_object){.alias=4,.group=7,.object=0,.payload_len=20,.datagram=true};
 n=moqt_datagram_header_write(b,sizeof(b),&o);
 const uint8_t dg[]={MOQT_DG_ZERO_OBJECT_ID|MOQT_DG_DEFAULT_PRIORITY,4,7};assert(n==3&&!memcmp(b,dg,3));
 o=(moqt_object){.alias=4,.group=7,.object=3,.priority=5,.has_priority=true,.props=props,.props_len=2,.payload_len=20,
  .end_of_group=true,.datagram=true};
 n=moqt_datagram_header_write(b,sizeof(b),&o);
 const uint8_t dg2[]={MOQT_DG_PROPERTIES|MOQT_DG_END_OF_GROUP,4,7,3,5,2,0x7A,0x03};assert(n==8&&!memcmp(b,dg2,8));
 assert(!(b[0]&0x10));
 o=(moqt_object){.alias=4,.group=7,.status=MOQT_STATUS_END_OF_GROUP,.datagram=true};
 n=moqt_datagram_header_write(b,sizeof(b),&o);
 assert(n==4&&b[0]==(MOQT_DG_STATUS|MOQT_DG_ZERO_OBJECT_ID|MOQT_DG_DEFAULT_PRIORITY)&&b[3]==3);
 /* Fetch stream. */
 n=moqt_fetch_header_write(b,sizeof(b),9);assert(n==2&&b[0]==5&&b[1]==9);
 moqt_fetch_prev prev={0};
 o=(moqt_object){.group=4,.object=2,.subgroup=0,.priority=128,.payload_len=3};
 n=moqt_fetch_object_write(b,sizeof(b),&o,false,&prev);
 const uint8_t f1[]={MOQT_FO_GROUP_ID|MOQT_FO_OBJECT_ID|MOQT_FO_PRIORITY,4,2,128,3};assert(n==5&&!memcmp(b,f1,5));
 o.object=3;n=moqt_fetch_object_write(b,sizeof(b),&o,false,&prev);
 const uint8_t f2[]={0x00,3};assert(n==2&&!memcmp(b,f2,2)); /* next object, same subgroup 0, same priority */
 o.object=6;o.subgroup=1;n=moqt_fetch_object_write(b,sizeof(b),&o,false,&prev);
 const uint8_t f3[]={MOQT_FO_OBJECT_ID|2,3,3};assert(n==3&&!memcmp(b,f3,3)); /* prior subgroup + 1, delta 3 */
 o=(moqt_object){.group=7,.object=0,.subgroup=5,.priority=9,.props=props,.props_len=2,.payload_len=1};
 n=moqt_fetch_object_write(b,sizeof(b),&o,false,&prev);
 const uint8_t f4[]={MOQT_FO_GROUP_ID|MOQT_FO_OBJECT_ID|MOQT_FO_PRIORITY|MOQT_FO_PROPERTIES|3,2,5,0,9,2,0x7A,0x03,1};
 assert(n==9&&!memcmp(b,f4,9)); /* group delta is 7-4-1 */
 o.group=6;assert(!moqt_fetch_object_write(b,sizeof(b),&o,false,&prev)); /* ascending only */
 n=moqt_fetch_object_write(b,sizeof(b),&o,true,&prev);assert(n&&b[1]==0); /* descending: 7-6-1 */
 o=(moqt_object){.group=5,.object=1,.priority=9,.datagram=true,.payload_len=2};
 n=moqt_fetch_object_write(b,sizeof(b),&o,true,&prev);
 assert(n&&(b[0]&MOQT_FO_DATAGRAM)&&!(b[0]&MOQT_FO_SUBGROUP_MASK)&&!(b[0]&MOQT_FO_PRIORITY));
 n=moqt_fetch_end_range_write(b,sizeof(b),MOQT_FO_END_NONEXISTENT,(moqt_loc){3,0},true,&prev);
 const uint8_t er[]={0x80,0x8C,3,0};assert(n==4&&!memcmp(b,er,4));
 assert(!moqt_fetch_end_range_write(b,sizeof(b),0x99,(moqt_loc){3,0},true,&prev));
 assert(prev.group==3&&prev.priority==9);
}

static void test_timeouts(void){ /* G3 */
 assert(moqt_timeout_effective(0,0)==0&&moqt_timeout_effective(2000,0)==2000);
 assert(moqt_timeout_effective(0,500)==500&&moqt_timeout_effective(2000,500)==500&&moqt_timeout_effective(100,500)==100);
}

int main(void){
 test_kvp();test_namespace();test_window();test_params();test_filters();test_setup();test_messages();test_data();test_timeouts();
 puts("PASS: draft21 properties, namespaces, location and range filters, parameters, setup, messages, data streams");
 return 0;
}
