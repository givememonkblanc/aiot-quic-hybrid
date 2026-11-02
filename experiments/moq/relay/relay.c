/* MOQT draft-21 relay for the alert experiments (RELAY_EXPERIMENT.md).
 *
 * Upstream, the relay is the subscriber of the inspection device: the device connects as
 * QUIC client, the relay subscribes to its result and image tracks and reads the objects.
 * Downstream, every subscriber session is served by the portable publisher core of the
 * firmware (src/components/transport/moqt_core.c), so SUBSCRIBE, FETCH and the object
 * encoding are those of the device. The relay forwards each object whole once it has
 * arrived, with the publisher priority, group, subgroup and properties of the device; it
 * never reads the payload.
 *
 * Egress policies, per subscriber session:
 *   fifo      objects in order of arrival at the relay
 *   prio      draft-21 section 5.1.2 order (moqt_wire_pick of the firmware)
 *   prio-to   prio, and the image track has an object delivery timeout (section 5.2)
 *   alert     prio-to, the image bytes in the network are bounded by the alert budget, and
 *             the connection is probed while a result is unacknowledged
 *
 * --helper-events: a downstream peer whose SETUP names Implementation "aiot-helper/1" (a helper
 * ESP32, src/main/app_moq_helper.inc) is also a publisher: the relay subscribes to its track
 * helper/events on that session and serves the Objects as relay track helper/events (alias 5),
 * logged as up_object with "source":"helper".
 *
 * --roles (with --offload): the relay reads the Object metadata of the shared frame track offload/jpeg (never
 * the payload) and serves its two kinds of subscribers differently:
 *   D1 age: the age of an Object since capture is estimated from its capture time property (0x78) and its
 *      age at publication (41452): per track, arrival - capture is offset by its smallest value, plus the
 *      smallest age at publication (the age is never overestimated);
 *   D2 helper (SETUP "aiot-helper/1"): a frame whose result cannot reach the operator in time (age +
 *      --helper-service-ms > --helper-deadline-ms, defaults 450 and 2000) is not delivered while the helper is
 *      busy (a frame of the track still under way to it, or delivered less than the service time ago); an idle
 *      helper still gets it. A frame queued for a busy helper expires when it can no longer be in time, and a
 *      newer frame supersedes the older frames not yet started for the helper. --helper-max-age-ms N > 0
 *      adds a hard age limit (the first version of D2); --helper-skip-evidence: the evidence frames (the
 *      camera inferred them itself) are not delivered to the helper;
 *   D3 operator (any other subscriber of the track): only the frames with the evidence property (0x7C = 1)
 *      are delivered (--operator-all: every frame), with the image delivery timeout.
 * --emu-up ADDR: a second upstream port for model cameras (host programs that publish offload/jpeg like the device, groups
 * from EMU_GROUP_BASE x camera). Their frames go to the helper only (never stored, cached or served to the other
 * subscribers), their age and supersession are kept per camera, and the helper's results for them are logged
 * (emu_result) and not served, so the run of the real camera is evaluated as before.
 * --helper-dispatch N (with --roles): the relay holds the helper's frames and sends one when the helper has fewer than N
 * frames outstanding (sent, no result yet; a result on helper/events frees it), the frame of the earliest relay group among
 * those still in time, in the order of the relay's groups (one per camera: a newer frame supersedes the older); the helper is fed when it frees, by its result.
 * Every event is written to a JSONL log with CLOCK_MONOTONIC seconds, the clock of the
 * Python receivers (time.monotonic). */
#define _GNU_SOURCE
#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "moqt_core.h"
#include "moqt_feasibility.h"
#include "moqt_scheduler.h"
#include "quic.h"

enum { POL_FIFO, POL_PRIO, POL_PRIO_TO, POL_ALERT };
static const char *policy_names[]={"fifo","prio","prio-to","alert"};
static struct {
 int policy; unsigned budget_ms,probe_ms,image_timeout_ms,handover_us;
 const char *image_track; unsigned images_kept; double seconds;
 bool no_cache; unsigned keepalive_ms; size_t fixed_window; bool window_bdp; bool timeout_all; unsigned rdrive_ms; const char *up_image_track; bool offload; bool helper_events;
 bool roles,operator_all,helper_skip_evidence,helper_free_window; const char *emu_up; unsigned helper_dispatch; bool helper_admit; unsigned helper_max_age_ms,helper_deadline_ms,helper_service_ms;
} cfg={.helper_max_age_ms=0,.helper_deadline_ms=2000,.helper_service_ms=450,.keepalive_ms=1000,.policy=POL_PRIO,.budget_ms=100,.probe_ms=50,.image_timeout_ms=10000,.handover_us=1000,
       .image_track="evidence/jpeg",.images_kept=64,.seconds=0};

/* Track layout of the firmware: alias = index + 1. */
#define TRACKS 5   /* the fifth, helper/events, exists with --helper-events only */
#define TRACKS_BASE 4
#define T_RESULTS 1
static moqt_track_cfg tracks[TRACKS]={
 {.ns={"aiot","esp32"},.ns_count=2,.name="preview/jpeg",.priority=192,.group_order=MOQT_ORDER_DESCENDING},
 {.ns={"aiot","esp32"},.ns_count=2,.name="ai/events",.priority=64},
 {.ns={"aiot","esp32"},.ns_count=2,.name="evidence/jpeg",.priority=128},
 {.ns={"aiot","esp32"},.ns_count=2,.name="status",.priority=224,.dynamic_groups=true},
 {.ns={"aiot","esp32"},.ns_count=2,.name="helper/events",.priority=64},
};
static bool is_image(unsigned track){return track==0||track==2;}
/* --helper-events: track 4 (alias 5), helper/events, holds the results of helper ESP32s (SETUP
 * Implementation "aiot-helper/1"), which the relay subscribes to on their downstream session.
 * Like the results, they never time out at the relay. */
#define T_HELPER 4
#define HELPER_IMPLEMENTATION "aiot-helper/1"
static bool never_expires(unsigned track){return track==T_RESULTS||(cfg.helper_events&&track==T_HELPER);}

/* -- log ---------------------------------------------------------------------------- */
static FILE *logf_;
static void logj(const char *ev,const char *fmt,...){
 if(!logf_)return;
 fprintf(logf_,"{\"t\":%.6f,\"ev\":\"%s\"",quic_now_us()/1e6,ev);
 if(fmt&&*fmt){fputc(',',logf_);va_list a;va_start(a,fmt);vfprintf(logf_,fmt,a);va_end(a);}
 fputs("}\n",logf_);
}

/* -- objects ------------------------------------------------------------------------ */
typedef struct blob { unsigned refs; size_t len; uint8_t data[]; } blob;
static blob *blob_new(const uint8_t *d,size_t n){blob *b=malloc(sizeof(*b)+n);if(!b)abort();b->refs=1;b->len=n;if(n)memcpy(b->data,d,n);return b;}
static void blob_drop(blob *b){if(b&&!--b->refs)free(b);}
static blob *blob_ref(blob *b){if(b)b->refs++;return b;}

typedef struct {
 bool used; unsigned track; uint64_t group,object,subgroup,serial; uint8_t priority,status;
 bool has_priority,end_of_group; uint8_t props[MOQT_CORE_PROPS]; size_t props_len; blob *b; uint64_t up_us;
 bool has_age,evidence; uint64_t age_ms; /* --roles: estimated age since capture at the arrival, evidence property */
 unsigned dev; /* 0 the device, k > 0 the k-th model camera (--emu-up) */
} stored;
#define EMU_GROUP_BASE 1000000000000ULL
#define HG_BASE (1ULL<<50)   /* the relay's groups to the helper (above any camera's, below 2^53 for the JSON log) */
#define EMU_DEVS 8
static unsigned cur_dev;   /* the camera whose subgroup stream is being parsed */
#define STORE 1024
static stored store[STORE];
static uint64_t store_serial;
static moqt_offer offer_of(const stored *s){
 return (moqt_offer){.group=s->group,.object=s->object,.subgroup=s->subgroup,.priority=s->priority,.status=s->status,
  .has_priority=s->has_priority,.end_of_group=s->end_of_group,.props=s->props_len?s->props:NULL,.props_len=s->props_len,
  .payload_len=s->b?s->b->len:0};
}

/* A subgroup stream received from a publisher (the device upstream, a helper downstream). */
typedef struct { int64_t id; bool used,header; uint8_t *buf; size_t len,cap,pos;
 uint64_t alias,group,subgroup,prev_object; bool has_prev,props,end_of_group,first_object_mode,has_priority; uint8_t priority; } upstream_stream;
#define HELPER_STREAMS 8
/* -- downstream sessions ------------------------------------------------------------ */
enum { K_FREE, K_CONTROL, K_REQUEST, K_OBJECT, K_FETCH, K_NOTICE };
#define HEAD_CAP (MOQT_CORE_HEAD+32)
#define CONTROL_CAP 65536
typedef struct {
 uint8_t kind; bool used,fin,fin_sent,reset_requested,reset_when_acked,blocked,opened;
 int64_t id;
 uint8_t *data; size_t cap;                      /* control and request streams */
 uint8_t head[HEAD_CAP]; size_t head_len; blob *b; /* data streams: head, then payload */
 size_t len,sent,acked; uint64_t base;
 uint64_t queued_us,first_us,fin_us,up_us,object_expiry_us,subgroup_timeout_us;
 unsigned alias,sub,fetch,reset_code; uint64_t group,subgroup,object;
 uint8_t subscriber_priority,publisher_priority,group_order; bool fill;
 unsigned dev; bool held; uint64_t cap_us,hg;   /* --helper-dispatch: a helper frame held at the relay, its estimated capture; its relay group */
} tx;
#define TXS 512
typedef struct {
 qconn *q; moqt_core core; bool setup_sent,failed,ending[MOQT_CORE_SUBS];
 tx t[TXS];
 moqt_rate rate; struct {uint64_t start,opened;size_t bytes,total;bool sampled;} win;
 size_t window; uint64_t last_window_log; bool probing;
 unsigned id;
 /* --helper-events: the peer's SETUP, and the subscription to its helper/events track */
 int64_t peer_control; uint8_t peer_ctl[512]; size_t peer_ctl_len; bool setup_checked,helper,helper_ok;
 int64_t helper_req; uint8_t helper_in[512]; size_t helper_in_len; int helper_alias[16];
 upstream_stream *hs;                    /* HELPER_STREAMS, allocated for a helper only */
 unsigned roles_aged,roles_filtered,roles_superseded,roles_delivered,roles_late_idle; /* --roles */
 uint64_t helper_done_us;  /* --roles: the last frame of the shared track the helper acknowledged */
 struct {uint64_t group,at;} out[8]; unsigned nout,dispatched,results,out_expired; /* --helper-dispatch: frames at the helper */
 /* --emu-up: the helper's frames carry the relay's own group, increasing (the helper's publisher refuses a result whose
  * group is below its last; the cameras' groups interleave); the helper's result is mapped back to camera and group */
 uint64_t hg_n; struct {uint64_t hg,group;unsigned dev;} hmap[1024];
 /* --helper-dispatch: the frames held for the helper (one per camera), published to it only when dispatched */
 unsigned held_expired; bool has_node; unsigned node; uint64_t last_dispatch_us;   /* SETUP Implementation "aiot-helper/1;node=K": the helper is camera K too */
 bool roles_logged;
} dsess;

static int64_t slot_index(dsess *s,int64_t id){for(unsigned i=0;i<TXS;i++)if(s->t[i].used&&s->t[i].opened&&s->t[i].id==id)return i;return -1;}
static tx *slot_of(dsess *s,int64_t id){int64_t i=slot_index(s,id);return i<0?NULL:&s->t[i];}
static tx *slot_new(dsess *s){for(unsigned i=0;i<TXS;i++)if(!s->t[i].used){memset(&s->t[i],0,sizeof(tx));s->t[i].used=true;s->t[i].sub=MOQT_CORE_SUBS;return &s->t[i];}return NULL;}
static void slot_free(tx *x){free(x->data);blob_drop(x->b);memset(x,0,sizeof(*x));}
static size_t remaining(const tx *x){return x->len-x->sent;}

static void fail(dsess *s,uint64_t code){if(!s->failed){s->failed=true;logj("session_fail","\"sess\":%u,\"code\":%llu",s->id,(unsigned long long)code);quic_close(s->q,code);}}

static int append(tx *x,const uint8_t *d,size_t n,bool fin){
 if(x->fin)return -1;
 if(n){
  if(x->len&&x->acked==x->len){x->base+=x->len;x->len=x->sent=x->acked=0;}
  /* ngtcp2 reads unacknowledged bytes from this buffer until they are acknowledged, so the
   * buffer never moves: one allocation, and the bytes start over once all are acknowledged. */
  if(!x->data){x->cap=CONTROL_CAP;x->data=malloc(x->cap);if(!x->data)abort();}
  if(x->len+n>x->cap)return -1;
  memcpy(x->data+x->len,d,n);x->len+=n;
 }
 if(fin)x->fin=true;
 if(!x->queued_us)x->queued_us=quic_now_us();
 return 0;
}
static tx *request_slot(dsess *s,int64_t id){
 tx *x=slot_of(s,id);
 if(x)return x->kind==K_REQUEST||x->kind==K_CONTROL?x:NULL;
 x=slot_new(s);if(!x)return NULL;
 x->kind=K_REQUEST;x->id=id;x->opened=true;x->subscriber_priority=128;return x;
}
static bool result_waiting(dsess *s){
 for(unsigned i=0;i<TXS;i++){const tx *x=&s->t[i];if(x->used&&x->kind==K_OBJECT&&x->alias==T_RESULTS+1)return true;}
 return false;
}
/* An idle session is kept open; only an outage of the link ends it. */
static ngtcp2_duration keepalive(void){return cfg.keepalive_ms?(ngtcp2_duration)cfg.keepalive_ms*NGTCP2_MILLISECONDS:UINT64_MAX;}
/* Result probe: while a result is unacknowledged, a PING after probe_ms of idle link. */
static void probe_update(dsess *s){
 if(cfg.policy!=POL_ALERT||!cfg.probe_ms||!s->q->conn)return;
 bool w=result_waiting(s);
 if(w==s->probing)return;
 s->probing=w;
 ngtcp2_conn_set_keep_alive_timeout(s->q->conn,w?(ngtcp2_duration)cfg.probe_ms*NGTCP2_MILLISECONDS:keepalive());
}
static void reset_slot(dsess *s,tx *x,unsigned code){
 if(!x->used||x->reset_requested)return;
 if(!x->opened){slot_free(x);return;}
 if(ngtcp2_conn_shutdown_stream_write(s->q->conn,0,x->id,code)==0)x->reset_requested=true;
}

/* The next object of a fetch stream once the previous one is acknowledged: ngtcp2 may still
 * read the head and the payload of an unacknowledged object to send it again. */
static void fetch_advance(dsess *s,tx *x){
 if(x->kind!=K_FETCH||x->fin||x->reset_requested||x->acked<x->len)return;
 x->base+=x->len;x->len=x->sent=x->acked=0;x->head_len=0;blob_drop(x->b);x->b=NULL;
 void *payload;size_t hl=0,pl=0;
 int rv=moqt_core_fetch_next(&s->core,x->fetch,x->head,sizeof(x->head),&hl,&payload,&pl);
 if(rv==1){x->head_len=hl;x->len=hl+pl;x->b=blob_ref(payload);return;}
 x->fin=true;
 if(rv<0){ngtcp2_conn_shutdown_stream_write(s->q->conn,0,x->id,MOQT_RST_INTERNAL_ERROR);x->reset_requested=true;}
}

static void sub_maybe_finish(dsess *s,unsigned sub){
 if(sub>=MOQT_CORE_SUBS||!s->ending[sub])return;
 for(unsigned i=0;i<TXS;i++)if(s->t[i].used&&s->t[i].sub==sub&&s->t[i].kind!=K_REQUEST&&s->t[i].kind!=K_CONTROL)return;
 s->ending[sub]=false;
 moqt_core_sub_finish(&s->core,sub,"",0);
}

static void drain(dsess *s);
static void handle(dsess *s,const moqt_action *a){
 ngtcp2_conn *conn=s->q->conn;
 switch(a->kind){
 case MOQT_ACT_SEND:{
  tx *x=request_slot(s,a->stream);
  if(!x||append(x,a->data,a->len,a->fin))fail(s,MOQT_S_INTERNAL_ERROR);
  break;}
 case MOQT_ACT_OPEN_BIDI:{
  int64_t id;
  if(ngtcp2_conn_open_bidi_stream(conn,&id,NULL)!=0)return;
  tx *x=request_slot(s,id);
  if(!x||append(x,a->data,a->len,false)){fail(s,MOQT_S_INTERNAL_ERROR);return;}
  moqt_core_bidi_opened(&s->core,a->token,id);
  break;}
 case MOQT_ACT_FETCH_START:{
  int64_t id;tx *x=slot_new(s);
  if(!x||ngtcp2_conn_open_uni_stream(conn,&id,NULL)!=0){
   if(x)slot_free(x);
   if(s->core.fetches[a->index].sub>=0)moqt_core_fetch_closed(&s->core,a->index);
   else moqt_core_stream_cancelled(&s->core,s->core.fetches[a->index].stream);
   return;
  }
  x->kind=K_FETCH;x->id=id;x->opened=true;x->fetch=a->index;x->alias=a->track+1;x->queued_us=quic_now_us();
  x->subscriber_priority=s->core.fetches[a->index].priority;x->publisher_priority=128;x->group_order=1;
  x->fill=s->core.fetches[a->index].sub>=0;x->sub=x->fill?(unsigned)s->core.fetches[a->index].sub:MOQT_CORE_SUBS;
  logj("fetch","\"sess\":%u,\"alias\":%u,\"stream\":%lld,\"fill\":%u",s->id,x->alias,(long long)id,(unsigned)x->fill);
  fetch_advance(s,x);
  break;}
 case MOQT_ACT_FETCH_CANCEL:
  for(unsigned i=0;i<TXS;i++)if(s->t[i].used&&s->t[i].kind==K_FETCH&&s->t[i].fetch==a->index)reset_slot(s,&s->t[i],(unsigned)a->code);
  break;
 case MOQT_ACT_FILL_FAILED:{
  int64_t id;tx *x=slot_new(s);
  if(!x)return;
  if(a->len>sizeof(x->head)||ngtcp2_conn_open_uni_stream(conn,&id,NULL)!=0){slot_free(x);return;}
  x->kind=K_NOTICE;x->id=id;x->opened=true;memcpy(x->head,a->data,a->len);x->head_len=x->len=a->len;
  x->reset_when_acked=true;x->reset_code=(unsigned)a->code;x->queued_us=quic_now_us();x->sub=a->index;
  break;}
 case MOQT_ACT_SUB_ENDING:if(a->index<MOQT_CORE_SUBS){s->ending[a->index]=true;sub_maybe_finish(s,a->index);}break;
 case MOQT_ACT_SUB_CANCELLED:{
  for(unsigned i=0;i<TXS;i++)
   if(s->t[i].used&&s->t[i].sub==a->index&&s->t[i].kind!=K_FETCH&&s->t[i].kind!=K_REQUEST&&s->t[i].kind!=K_CONTROL)reset_slot(s,&s->t[i],MOQT_RST_CANCELLED);
  tx *x=slot_of(s,a->stream);
  if(x&&x->kind==K_REQUEST)reset_slot(s,x,MOQT_RST_CANCELLED);
  else ngtcp2_conn_shutdown_stream_write(conn,0,a->stream,MOQT_RST_CANCELLED);
  if(a->index<MOQT_CORE_SUBS)s->ending[a->index]=false;
  logj("unsubscribed","\"sess\":%u,\"alias\":%u",s->id,a->track+1);
  break;}
 case MOQT_ACT_RESET_STREAM:{
  tx *x=slot_of(s,a->stream);
  if(x)reset_slot(s,x,(unsigned)a->code);else ngtcp2_conn_shutdown_stream_write(conn,0,a->stream,a->code);
  break;}
 case MOQT_ACT_STOP_SENDING:ngtcp2_conn_shutdown_stream_read(conn,0,a->stream,a->code);break;
 case MOQT_ACT_CLOSE_SESSION:fail(s,a->code);break;
 case MOQT_ACT_SUBSCRIBED:{
  const moqt_sub *u=&s->core.subs[a->index];
  logj("subscribed","\"sess\":%u,\"alias\":%u,\"sub\":%u,\"subscriber_priority\":%u,\"forward\":%u",s->id,a->track+1,a->index,
       (unsigned)u->subscriber_priority,(unsigned)u->forward);
  break;}
 default:break;
 }
}
static void drain(dsess *s){moqt_action a;while(moqt_core_next_action(&s->core,&a))handle(s,&a);}

/* Admits a stored object to one session: one data stream per delivery, opened at once
 * or, when the peer's stream limit is reached, by tick. */
static bool helper_busy(const dsess *s,uint64_t now){
 if(cfg.helper_dispatch)return s->nout>=cfg.helper_dispatch;
 for(unsigned i=0;i<TXS;i++){const tx *x=&s->t[i];if(x->used&&x->kind==K_OBJECT&&x->alias==1&&!x->reset_requested)return true;}
 return s->helper_done_us&&now-s->helper_done_us<(uint64_t)cfg.helper_service_ms*1000;
}
/* --helper-dispatch: while the helper has fewer than N frames outstanding, the held frame of the earliest capture that
 * can still be in time is opened for it; a frame whose result has not come back within the deadline counts no longer. */
static const qhandler down_handler;
static bool dispatching;   /* deliver() called by the dispatcher: publish now, with the relay's group */
static void deliver(dsess *s,unsigned track,const stored *st);
/* --helper-dispatch N: the relay is the broker of the helpers' compute. A frame for the helpers is held once at the relay (one per
 * camera: a newer frame supersedes the older) and given to one helper only, when a helper has fewer than N frames outstanding
 * (sent, no result yet; its result frees it): the least loaded helper takes the held frame of the earliest capture (EDF, the
 * same deadline from capture for all) that is not its own camera's (a helper that is camera K too, SETUP "aiot-helper/1;node=K"),
 * with the next relay group of that helper. --helper-admit: only a frame that can end in time there (its age, the helper's
 * queue of outstanding frames and one service time within the deadline). A held frame that can no longer be in time is dropped;
 * an outstanding frame whose result has not come back within the deadline counts no longer. */
#define GQ 64
static struct {bool used;stored st;uint64_t cap_us;} gq[GQ];
static unsigned gq_superseded,gq_expired;
static uint64_t frame_age(const stored *e,uint64_t now){return e->has_age?e->age_ms+(now-e->up_us)/1000:0;}
static void gq_drop(unsigned i){blob_drop(gq[i].st.b);gq[i].used=false;}
static void gq_put(const stored *st){
 int k=-1;
 for(unsigned j=0;j<GQ;j++){
  if(gq[j].used&&gq[j].st.dev==st->dev&&gq[j].st.group==st->group)return;   /* already held (fan-out to several helpers) */
  if(gq[j].used&&gq[j].st.dev==st->dev){
   gq_superseded++;logj("held_superseded","\"group\":%llu,\"by\":%llu,\"dev\":%u",(unsigned long long)gq[j].st.group,(unsigned long long)st->group,st->dev);
   gq_drop(j);
  }
  if(!gq[j].used&&k<0)k=(int)j;
 }
 if(k<0){logj("dropped","\"alias\":1,\"group\":%llu,\"why\":\"held\"",(unsigned long long)st->group);return;}
 gq[k].used=true;gq[k].st=*st;gq[k].st.b=blob_ref(st->b);
 gq[k].cap_us=st->has_age&&st->up_us>st->age_ms*1000?st->up_us-st->age_ms*1000:st->up_us;
}
/* the time a frame given to this helper now would wait behind its outstanding ones (one service time each, the oldest begun) */
static uint64_t helper_wait_ms(const dsess *s,uint64_t now){
 if(!s->nout)return 0;
 uint64_t oldest=s->out[0].at;for(unsigned i=1;i<s->nout;i++)if(s->out[i].at<oldest)oldest=s->out[i].at;
 uint64_t w=(uint64_t)s->nout*cfg.helper_service_ms,el=(now-oldest)/1000;
 return w>el?w-el:0;
}
static void helper_dispatch(dsess *unused,uint64_t now){
 (void)unused;
 if(!cfg.helper_dispatch||dispatching)return;
 for(qconn *c=quic_conns();c;c=c->next)
  if(c->h==&down_handler&&c->app){dsess *s=c->app;
   for(unsigned i=0;i<s->nout;)if(now-s->out[i].at>(uint64_t)cfg.helper_deadline_ms*1000){s->out[i]=s->out[--s->nout];s->out_expired++;}else i++;}
 for(unsigned j=0;j<GQ;j++)
  if(gq[j].used&&gq[j].st.has_age&&frame_age(&gq[j].st,now)+cfg.helper_service_ms>cfg.helper_deadline_ms){
   gq_expired++;logj("held_expired","\"group\":%llu,\"dev\":%u,\"age_ms\":%llu",(unsigned long long)gq[j].st.group,gq[j].st.dev,(unsigned long long)frame_age(&gq[j].st,now));
   gq_drop(j);
  }
 for(;;){
  /* the least loaded helper with a free slot that has a frame it may take */
  dsess *best=NULL;int bf=-1;
  for(qconn *c=quic_conns();c;c=c->next){
   if(c->h!=&down_handler||!c->app||c->closing||c->draining)continue;
   dsess *s=c->app;
   if(!s->helper||s->failed||!s->core.peer_setup||s->nout>=cfg.helper_dispatch||s->nout>=8)continue;
   if(!moqt_core_any_forwarding(&s->core,0))continue;   /* no subscription to the frames: it lends no compute now */
   /* the least loaded; among equals the one that got a frame longest ago (round robin) */
   if(best&&(s->nout>best->nout||(s->nout==best->nout&&s->last_dispatch_us>=best->last_dispatch_us)))continue;
   uint64_t wait=helper_wait_ms(s,now);int f=-1;
   for(unsigned j=0;j<GQ;j++){
    if(!gq[j].used)continue;
    if(s->has_node&&gq[j].st.dev==s->node)continue;   /* not its own camera's frame */
    if(cfg.helper_admit&&gq[j].st.has_age&&frame_age(&gq[j].st,now)+wait+cfg.helper_service_ms>cfg.helper_deadline_ms)continue;
    if(f<0||gq[j].cap_us<gq[f].cap_us)f=(int)j;
   }
   if(f>=0){best=s;bf=f;}
  }
  if(!best)break;
  stored e=gq[bf].st;uint64_t cap=gq[bf].cap_us;gq[bf].used=false;
  uint64_t before=best->hg_n;
  dispatching=true;deliver(best,0,&e);dispatching=false;
  if(best->hg_n!=before){
   best->out[best->nout].group=HG_BASE+best->hg_n;best->out[best->nout].at=now;best->nout++;best->dispatched++;best->last_dispatch_us=now;
   logj("dispatch","\"sess\":%u,\"group\":%llu,\"dev\":%u,\"age_ms\":%llu,\"out\":%u",best->id,(unsigned long long)e.group,e.dev,
        (unsigned long long)(now>cap?(now-cap)/1000:0),best->nout);
   drain(best);
  }
  blob_drop(e.b);
 }
}
static void deliver(dsess *s,unsigned track,const stored *st){
 if(s->failed||!s->core.peer_setup)return;
 moqt_offer o=offer_of(st);moqt_delivery d[MOQT_CORE_SUBS];
 uint64_t now=quic_now_us();
 /* --roles on the shared frame track: the helper gets fresh frames only, the operator the evidence only */
 bool role=cfg.roles&&cfg.offload&&track==0;
 uint64_t age_now=st->has_age?st->age_ms+(now-st->up_us)/1000:0;
 bool in_time=!st->has_age||age_now+cfg.helper_service_ms<=cfg.helper_deadline_ms;
 bool busy=role&&s->helper&&helper_busy(s,now);
 if(role&&s->helper&&st->has_age&&((cfg.helper_max_age_ms&&age_now>=cfg.helper_max_age_ms)||(!in_time&&busy))){
  s->roles_aged++;
  logj("roles_aged","\"sess\":%u,\"group\":%llu,\"age_ms\":%llu,\"busy\":%u",s->id,(unsigned long long)st->group,(unsigned long long)age_now,(unsigned)busy);
  return;
 }
 if(role&&s->helper&&!in_time&&!dispatching)s->roles_late_idle++;   /* late, but the helper is idle: it gets the frame */
 if(role&&!s->helper&&!cfg.operator_all&&!st->evidence){s->roles_filtered++;return;}
 /* --helper-skip-evidence: the evidence frames are frames the camera inferred itself; the helper does not get them */
 if(role&&s->helper&&cfg.helper_skip_evidence&&st->evidence){s->roles_filtered++;return;}
 if(role&&s->helper&&cfg.helper_dispatch&&!dispatching)return;   /* held at the relay once (store_object), given to one helper */
 uint64_t hg=0;
 if(role&&s->helper&&(cfg.emu_up||cfg.helper_dispatch)){hg=HG_BASE+ ++s->hg_n;o.group=hg;}
 int n=moqt_core_publish(&s->core,track,&o,now/1000,d,MOQT_CORE_SUBS);
 if(hg&&n>0){unsigned k=(unsigned)(s->hg_n%1024);s->hmap[k].hg=hg;s->hmap[k].group=st->group;s->hmap[k].dev=st->dev;}
 for(int i=0;i<n;i++){
  if(d[i].datagram||d[i].head_len>HEAD_CAP)continue;
  tx *x=slot_new(s);
  if(!x){logj("dropped","\"sess\":%u,\"alias\":%u,\"group\":%llu,\"why\":\"slots\"",s->id,track+1,(unsigned long long)st->group);continue;}
  x->kind=K_OBJECT;x->fin=true;memcpy(x->head,d[i].head,d[i].head_len);x->head_len=d[i].head_len;
  x->b=blob_ref(st->b);x->len=x->head_len+(x->b?x->b->len:0);
  x->queued_us=now;x->up_us=st->up_us;x->group=st->group;x->subgroup=st->subgroup;x->object=st->object;x->alias=track+1;x->sub=d[i].sub;
  x->subscriber_priority=d[i].subscriber_priority;x->publisher_priority=d[i].publisher_priority;x->group_order=d[i].group_order;
  x->object_expiry_us=d[i].object_timeout_ms?now+d[i].object_timeout_ms*1000:0;
  x->subgroup_timeout_us=d[i].subgroup_timeout_ms*1000;
  if(never_expires(track))x->object_expiry_us=x->subgroup_timeout_us=0; /* results never time out */
  /* --timeout-all (base reproduction, implicit adaptation of Freeman et al.): every track,
   * results included, has the delivery timeout, counted from the arrival of the Object at the
   * relay (the creation time of the device clock is not known here; this errs in its favour) */
  if(cfg.timeout_all&&cfg.image_timeout_ms&&!(cfg.helper_events&&track==T_HELPER)){x->object_expiry_us=st->up_us+(uint64_t)cfg.image_timeout_ms*1000;x->subgroup_timeout_us=0;}
  if(role){
   if(!dispatching)s->roles_delivered++;
   if(s->helper){
    /* expires for the helper when it reaches the helper age; supersedes the older frames not yet started */
    /* queued behind another frame: expires when its result can no longer be in time */
    if(st->has_age&&in_time&&(busy||cfg.helper_dispatch)){uint64_t e=now+(cfg.helper_deadline_ms-cfg.helper_service_ms-age_now)*1000;if(!x->object_expiry_us||e<x->object_expiry_us)x->object_expiry_us=e;}
    x->dev=st->dev;x->hg=hg?hg:st->group;x->cap_us=st->has_age&&st->up_us>st->age_ms*1000?st->up_us-st->age_ms*1000:st->up_us;

    for(unsigned j=0;j<TXS&&!cfg.helper_dispatch;j++){   /* with dispatch, supersession is done on the held frames */
     tx *y=&s->t[j];
     if(y==x||!y->used||y->kind!=K_OBJECT||y->alias!=1||y->dev!=st->dev||y->group>=st->group||y->sent||y->reset_requested)continue;
     s->roles_superseded++;
     logj("roles_superseded","\"sess\":%u,\"group\":%llu,\"by\":%llu",s->id,(unsigned long long)y->group,(unsigned long long)st->group);
     if(!y->opened){unsigned sub=y->sub;slot_free(y);sub_maybe_finish(s,sub);}
     else reset_slot(s,y,MOQT_RST_DELIVERY_TIMEOUT);
    }
   }
  }
  int64_t id;
  if(!x->held&&ngtcp2_conn_open_uni_stream(s->q->conn,&id,NULL)==0){x->id=id;x->opened=true;moqt_core_stream_opened(&s->core,x->sub);}
  if(is_image(track)&&!s->win.start){s->win.start=s->win.opened=now;s->win.bytes=s->win.total=0;s->win.sampled=false;}
  logj("queued","\"sess\":%u,\"alias\":%u,\"group\":%llu,\"bytes\":%zu,\"opened\":%u",s->id,track+1,(unsigned long long)st->group,x->len,(unsigned)x->opened);
 }
 probe_update(s);
 helper_dispatch(s,now);
 drain(s);
}

static void cache_put(dsess *s,unsigned track,const stored *st){
 if(cfg.no_cache||st->status!=MOQT_STATUS_NORMAL)return;
 moqt_offer o=offer_of(st);void *evicted=NULL;
 while(moqt_core_cache_count(&s->core,MOQT_CORE_TRACKS)>=MOQT_CORE_CACHE-1){
  /* images go first; results only when nothing else is left */
  void *old=moqt_core_cache_evict(&s->core,moqt_core_cache_count(&s->core,2)?2U:moqt_core_cache_count(&s->core,0)?0U:MOQT_CORE_TRACKS);
  if(!old)break;
  blob_drop(old);
 }
 if(moqt_core_cache_put(&s->core,track,&o,st->b,st->up_us/1000,&evicted)==0)blob_ref(st->b);
 blob_drop(evicted);
}

/* -- downstream QUIC handler -------------------------------------------------------- */
static unsigned dsess_serial;
static void d_accepted(qconn *q){
 dsess *s=calloc(1,sizeof(*s));if(!s)abort();
 s->q=q;q->app=s;s->id=++dsess_serial;
 moqt_track_cfg tc[TRACKS];memcpy(tc,tracks,sizeof(tc));
 tc[2].name=cfg.image_track;
 if(cfg.policy==POL_PRIO_TO||cfg.policy==POL_ALERT)tc[2].object_timeout_ms=tc[0].object_timeout_ms=cfg.image_timeout_ms;
 moqt_core_init(&s->core,tc,cfg.helper_events?TRACKS:TRACKS_BASE,false);
 /* A subscriber that joins late or returns finds everything the relay retains. */
 const stored *order[STORE];unsigned n=0;
 for(unsigned i=0;i<STORE;i++)if(store[i].used)order[n++]=&store[i];
 for(unsigned i=1;i<n;i++)for(unsigned j=i;j>0&&order[j]->serial<order[j-1]->serial;j--){const stored *t=order[j];order[j]=order[j-1];order[j-1]=t;}
 for(unsigned i=0;i<n;i++){
  moqt_offer o=offer_of(order[i]);moqt_delivery d[1];
  moqt_core_publish(&s->core,order[i]->track,&o,order[i]->up_us/1000,d,0); /* Largest Object only */
  cache_put(s,order[i]->track,order[i]);
 }
 s->peer_control=s->helper_req=-1;for(unsigned i=0;i<16;i++)s->helper_alias[i]=-1;
 logj("session","\"sess\":%u,\"conn\":%u,\"role\":\"subscriber\",\"policy\":\"%s\",\"retained\":%u",s->id,q->id,policy_names[cfg.policy],n);
}
static void d_opened(qconn *q){
 dsess *s=q->app;uint8_t b[256];int64_t id;
 size_t n=moqt_core_setup(&s->core,b,sizeof(b),NULL,NULL);
 if(!n||ngtcp2_conn_open_uni_stream(q->conn,&id,NULL)!=0){fail(s,MOQT_S_INTERNAL_ERROR);return;}
 tx *x=slot_new(s);x->kind=K_CONTROL;x->id=id;x->opened=true;append(x,b,n,false);
 s->setup_sent=true;
 ngtcp2_conn_set_keep_alive_timeout(q->conn,keepalive());
}
/* -- --helper-events: a helper ESP32 on a downstream session ----------------------- */
static void subgroup_rx(upstream_stream *s,const int *alias_track,const uint8_t *d,size_t n,bool fin,unsigned helper_sess);
static void ustream_free(upstream_stream *s);
/* SUBSCRIBE helper/events on a new request stream of the session (the relay is the server: odd
 * Request IDs, taken from the core so that they never collide with its own requests). */
static void helper_subscribe(dsess *s){
 int64_t id;
 if(ngtcp2_conn_open_bidi_stream(s->q->conn,&id,NULL)!=0){logj("helper_subscribe_failed","\"sess\":%u",s->id);return;}
 tx *x=request_slot(s,id);
 uint8_t b[256];moqt_wr w=moqt_wr_of(b,sizeof(b));
 uint64_t rid=s->core.next_request_id;s->core.next_request_id+=2;
 size_t mark=moqt_wr_begin_message(&w,MOQT_MSG_SUBSCRIBE);
 moqt_wr_vi(&w,rid);
 moqt_ns_write(&w,tracks[T_HELPER].ns,tracks[T_HELPER].ns_count);
 moqt_wr_vi(&w,strlen(tracks[T_HELPER].name));moqt_wr_bytes(&w,tracks[T_HELPER].name,strlen(tracks[T_HELPER].name));
 moqt_wr_vi(&w,1);moqt_wr_vi(&w,MOQT_P_SUBSCRIBER_PRIORITY);moqt_wr_u8(&w,0);
 moqt_wr_end_message(&w,mark);
 if(!x||w.bad||append(x,b,w.n,false)){fail(s,MOQT_S_INTERNAL_ERROR);return;}
 s->helper_req=id;
 if(!s->hs&&!(s->hs=calloc(HELPER_STREAMS,sizeof(*s->hs))))abort();
 logj("helper_subscribe","\"sess\":%u,\"stream\":%lld,\"request_id\":%llu",s->id,(long long)id,(unsigned long long)rid);
}
/* The peer's SETUP: a helper names itself in the Implementation option (section 9.1). */
static void helper_check_setup(dsess *s,const uint8_t *d,size_t n){
 if(s->setup_checked)return;
 if(n>sizeof(s->peer_ctl)-s->peer_ctl_len)n=sizeof(s->peer_ctl)-s->peer_ctl_len;
 memcpy(s->peer_ctl+s->peer_ctl_len,d,n);s->peer_ctl_len+=n;
 uint64_t t;const uint8_t *body;size_t bl,used;
 if(moqt21_message_read(s->peer_ctl,s->peer_ctl_len,&t,&body,&bl,&used)!=1){if(s->peer_ctl_len==sizeof(s->peer_ctl))s->setup_checked=true;return;}
 s->setup_checked=true;
 moqt_setup_options o;memset(&o,0,sizeof(o));
 if(t!=MOQT_MSG_SETUP||moqt_setup_read(body,bl,false,&o)!=0)return;
 size_t k=strlen(HELPER_IMPLEMENTATION);
 if(o.implementation_len<k||memcmp(o.implementation,HELPER_IMPLEMENTATION,k))return;
 if(o.implementation_len>k){   /* "aiot-helper/1;node=K": a peer, camera K and helper */
  char t[32];size_t m=o.implementation_len-k<sizeof(t)-1?o.implementation_len-k:sizeof(t)-1;memcpy(t,o.implementation+k,m);t[m]=0;
  if(strncmp(t,";node=",6))return;
  s->has_node=true;s->node=(unsigned)atoi(t+6);
 }
 s->helper=true;
 logj("helper","\"sess\":%u,\"conn\":%u,\"node\":%d",s->id,s->q->id,s->has_node?(int)s->node:-1);
 helper_subscribe(s);
}
/* SUBSCRIBE_OK, REQUEST_ERROR and PUBLISH_DONE of the helper/events subscription. */
static void helper_request_rx(dsess *s,const uint8_t *d,size_t n){
 if(n>sizeof(s->helper_in)-s->helper_in_len){logj("helper_request_overflow","\"sess\":%u",s->id);return;}
 memcpy(s->helper_in+s->helper_in_len,d,n);s->helper_in_len+=n;
 for(;;){
  uint64_t t;const uint8_t *body;size_t bl,used;
  if(moqt21_message_read(s->helper_in,s->helper_in_len,&t,&body,&bl,&used)!=1)break;
  if(t==MOQT_MSG_SUBSCRIBE_OK){
   moqt_rd r=moqt_rd_of(body,bl);uint64_t alias=moqt_rd_vi(&r);
   if(!r.bad&&alias<16){s->helper_alias[alias]=T_HELPER;s->helper_ok=true;}
   logj("helper_subscribed","\"sess\":%u,\"alias\":%llu",s->id,(unsigned long long)alias);
  }else if(t==MOQT_MSG_REQUEST_ERROR)logj("helper_request_error","\"sess\":%u",s->id);
  else if(t==MOQT_MSG_PUBLISH_DONE)logj("helper_publish_done","\"sess\":%u",s->id);
  memmove(s->helper_in,s->helper_in+used,s->helper_in_len-used);s->helper_in_len-=used;
 }
}
static upstream_stream *helper_stream(dsess *s,int64_t id){
 if(s->hs)for(unsigned i=0;i<HELPER_STREAMS;i++)if(s->hs[i].used&&s->hs[i].id==id)return &s->hs[i];
 return NULL;
}
/* true: the bytes belonged to the helper side and the core does not see them */
static bool helper_rx(dsess *s,int64_t id,const uint8_t *d,size_t n,bool fin){
 if(s->helper&&id==s->helper_req){helper_request_rx(s,d,n);return true;}
 if((id&3)!=2)return false;                       /* client-initiated unidirectional streams only */
 upstream_stream *u=helper_stream(s,id);
 if(u){subgroup_rx(u,s->helper_alias,d,n,fin,s->id);return true;}
 bool subgroup=n&&d[0]<0x80&&(d[0]&MOQT_SG_ALWAYS);
 if(id==s->peer_control||(s->peer_control<0&&n&&!subgroup)){
  s->peer_control=id;helper_check_setup(s,d,n);return false;   /* the core reads it too */
 }
 if(!s->helper||!subgroup)return false;
 for(unsigned i=0;i<HELPER_STREAMS;i++)if(!s->hs[i].used){
  u=&s->hs[i];memset(u,0,sizeof(*u));u->used=true;u->id=id;
  subgroup_rx(u,s->helper_alias,d,n,fin,s->id);return true;
 }
 logj("helper_streams_exhausted","\"sess\":%u",s->id);
 return false;
}
static int d_data(qconn *q,int64_t id,const uint8_t *d,size_t n,bool fin){
 dsess *s=q->app;
 if(cfg.helper_events&&helper_rx(s,id,d,n,fin))return 0;
 moqt_core_rx(&s->core,id,d,n,fin,quic_now_us()/1000);
 drain(s);
 return 0;
}
static void d_acked(qconn *q,int64_t id,uint64_t off,uint64_t n){
 dsess *s=q->app;tx *x=slot_of(s,id);(void)off;
 if(!x)return;
 x->acked+=(size_t)n;
 if(x->kind==K_FETCH){fetch_advance(s,x);return;}
 if(x->kind==K_OBJECT&&is_image(x->alias-1)&&s->win.start){
  uint64_t now=quic_now_us();s->win.bytes+=n;s->win.total+=n;
  if(now-s->win.start>=1000000){moqt_rate_observe_window(&s->rate,s->win.bytes,now-s->win.start);s->win.start=now;s->win.bytes=0;s->win.sampled=true;}
 }
}
static bool images_in_flight(dsess *s){for(unsigned i=0;i<TXS;i++)if(s->t[i].used&&s->t[i].kind==K_OBJECT&&is_image(s->t[i].alias-1))return true;return false;}
static void d_closed(qconn *q,int64_t id,bool app_error,uint64_t code){
 dsess *s=q->app;tx *x=slot_of(s,id);
 if(!x)return;
 uint64_t now=quic_now_us();
 if(x->kind==K_OBJECT){
  logj("done","\"sess\":%u,\"alias\":%u,\"group\":%llu,\"bytes\":%zu,\"acked\":%zu,\"ok\":%u,\"code\":%llu,\"wait_us\":%llu,\"send_us\":%llu,\"ack_us\":%llu,\"relay_us\":%llu",
       s->id,x->alias,(unsigned long long)x->group,x->len,x->acked,(unsigned)(!app_error&&!x->reset_requested&&x->acked==x->len),
       (unsigned long long)(app_error?code:0),
       (unsigned long long)(x->first_us?x->first_us-x->queued_us:0),(unsigned long long)(x->fin_us&&x->first_us?x->fin_us-x->first_us:0),
       (unsigned long long)(x->fin_us?now-x->fin_us:0),(unsigned long long)(x->queued_us-x->up_us));
  if(cfg.roles&&s->helper&&x->alias==1&&!app_error&&x->acked==x->len)s->helper_done_us=now;
  if(is_image(x->alias-1)){
   unsigned keep=x->sub;slot_free(x);x=NULL;
   /* the rate window closes with the last image under way (estimator 2 of the firmware) */
   if(!images_in_flight(s)&&s->win.start){
    uint64_t el=now-s->win.start;
    if(el>=500000&&s->win.bytes)moqt_rate_observe_window(&s->rate,s->win.bytes,el);
    else if(!s->win.sampled&&s->win.total&&now-s->win.opened>=200000)moqt_rate_observe_window(&s->rate,s->win.total,now-s->win.opened);
    memset(&s->win,0,sizeof(s->win));
   }
   probe_update(s);sub_maybe_finish(s,keep);drain(s);return;
  }
 }
 uint8_t kind=x->kind;unsigned sub=x->sub,fetch=x->fetch;
 slot_free(x);
 probe_update(s);
 if(kind==K_FETCH){moqt_core_fetch_closed(&s->core,fetch);drain(s);}
 if(kind!=K_REQUEST&&kind!=K_CONTROL)sub_maybe_finish(s,sub);
 drain(s);
}
static void d_reset(qconn *q,int64_t id,uint64_t code,bool stop){
 dsess *s=q->app;tx *x=slot_of(s,id);(void)code;
 if(cfg.helper_events){
  upstream_stream *u=helper_stream(s,id);
  if(u){if(!stop){logj("helper_reset","\"sess\":%u,\"group\":%llu,\"code\":%llu",s->id,(unsigned long long)u->group,(unsigned long long)code);ustream_free(u);}return;}
 }
 if(x&&x->kind!=K_REQUEST&&x->kind!=K_CONTROL){if(stop)x->reset_requested=true;return;}
 moqt_core_stream_cancelled(&s->core,id);drain(s);
}
static void d_tick(qconn *q,uint64_t now){
 dsess *s=q->app;
 for(unsigned i=0;i<TXS;i++)s->t[i].blocked=false;
 helper_dispatch(s,now);
 /* streams the peer's limit did not allow yet, in the order the policy serves them */
 for(;;){
  moqt_wire_job jobs[64];unsigned map[64],m=0;uint64_t ready=0;
  for(unsigned i=0;i<TXS&&m<64;i++){
   tx *x=&s->t[i];
   if(!x->used||x->opened||x->held)continue;
   jobs[m]=(moqt_wire_job){.klass=MOQT_CLASS_OBJECT,.subscriber_priority=x->subscriber_priority,.publisher_priority=x->publisher_priority,
    .group_order=x->group_order,.subscription=x->sub,.group=x->group,.subgroup=x->subgroup,.object=x->object,.admitted_us=x->queued_us};
   if(cfg.policy==POL_FIFO)jobs[m].publisher_priority=jobs[m].subscriber_priority=0;
   map[m]=i;ready|=1ULL<<m;m++;
  }
  if(!m)break;
  int k=moqt_wire_pick(jobs,m,ready);int64_t id;
  if(k<0||ngtcp2_conn_open_uni_stream(q->conn,&id,NULL)!=0)break;
  tx *x=&s->t[map[k]];x->id=id;x->opened=true;moqt_core_stream_opened(&s->core,x->sub);
 }
 for(unsigned i=0;i<TXS;i++){
  tx *x=&s->t[i];
  if(!x->used||x->reset_requested)continue;
  if(x->reset_when_acked){if(x->len&&x->acked==x->len)reset_slot(s,x,x->reset_code);continue;}
  if(x->kind==K_FETCH){fetch_advance(s,x);continue;}
  if(x->kind!=K_OBJECT)continue;
  bool expired=(x->object_expiry_us&&x->sent<x->len&&now>=x->object_expiry_us)||
               (x->subgroup_timeout_us&&x->fin_sent&&x->acked<x->len&&now-x->fin_us>=x->subgroup_timeout_us);
  if(!expired)continue;
  logj("timeout_reset","\"sess\":%u,\"alias\":%u,\"group\":%llu,\"sent\":%zu,\"acked\":%zu,\"len\":%zu",s->id,x->alias,(unsigned long long)x->group,x->sent,x->acked,x->len);
  if(!x->opened){unsigned sub=x->sub;slot_free(x);sub_maybe_finish(s,sub);continue;}
  reset_slot(s,x,MOQT_RST_DELIVERY_TIMEOUT);
 }
 moqt_core_tick(&s->core,now/1000);drain(s);
 if(cfg.policy==POL_ALERT){
  /* The alert budget of the firmware (app_moq_mus.inc, fine window): a result waits behind
   * the image bytes in the network, is sent, and crosses half the smallest round trip. */
  uint64_t rate=s->rate.samples?moqt_rate_value(&s->rate):8192;
  ngtcp2_conn_info ci;ngtcp2_conn_get_conn_info(q->conn,&ci);
  uint64_t min_rtt=ci.min_rtt==UINT64_MAX?0:ci.min_rtt/1000;
  int64_t left=(int64_t)cfg.budget_ms*1000-(int64_t)(160ULL*1000000ULL/(rate?rate:1))-(int64_t)(min_rtt/2)-(int64_t)cfg.handover_us;
  size_t w=left>0?(size_t)(rate*(uint64_t)left/1000000ULL):0;
  /* --window-bdp: the window also holds the bytes on the wire, rate x smallest round trip */
  if(cfg.window_bdp&&left>0)w+=(size_t)(rate*min_rtt/1000000ULL);
  s->window=cfg.fixed_window?cfg.fixed_window:(w<256?256:w);   /* fixed: ablation, as TCP_NOTSENT_LOWAT */
  if(now-s->last_window_log>=1000000){
   s->last_window_log=now;
   logj("window","\"sess\":%u,\"rate_Bps\":%llu,\"samples\":%u,\"min_rtt_us\":%llu,\"window\":%zu,\"cwnd\":%llu,\"in_flight\":%llu",s->id,
        (unsigned long long)rate,s->rate.samples,(unsigned long long)min_rtt,s->window,(unsigned long long)ci.cwnd,(unsigned long long)ci.bytes_in_flight);
  }
 }
}
static int d_pull(qconn *q,int64_t *id,ngtcp2_vec *v,bool *fin){
 dsess *s=q->app;
 size_t image_out=0;
 for(unsigned i=0;i<TXS;i++){const tx *x=&s->t[i];if(x->used&&x->kind==K_OBJECT&&is_image(x->alias-1))image_out+=x->sent-x->acked;}
 size_t allow=SIZE_MAX;
 /* --helper-free-window: the mission window keeps a result from waiting behind image bytes; no result goes to the helper,
  * whose session sends frames only (on the LAN its rate estimate stays at the floor and the window cut each frame into
  * round trips), so its frames go at the pace of the congestion controller */
 if(cfg.policy==POL_ALERT&&!(cfg.helper_free_window&&s->helper))allow=image_out>=s->window?0:s->window-image_out;
 int best=-1;bool best_first=false;moqt_wire_job bj={0};
 for(unsigned i=0;i<TXS;i++){
  tx *x=&s->t[i];
  if(!x->used||!x->opened||x->reset_requested||x->blocked)continue;
  if(!(x->sent<x->len||(x->fin&&!x->fin_sent)))continue;
  bool first=x->kind==K_CONTROL||x->kind==K_REQUEST;
  if(!first&&x->kind==K_OBJECT&&is_image(x->alias-1)&&!allow)continue;
  moqt_wire_job j={.klass=(uint8_t)(x->kind==K_CONTROL?MOQT_CLASS_CONTROL:first?MOQT_CLASS_REQUEST:MOQT_CLASS_OBJECT),
   .subscriber_priority=x->subscriber_priority,.publisher_priority=x->publisher_priority,.group_order=x->group_order,.fill=x->fill,
   .subscription=x->sub,.group=x->group,.subgroup=x->subgroup,.object=x->object,.admitted_us=x->queued_us};
  if(cfg.policy==POL_FIFO&&!first)j.subscriber_priority=j.publisher_priority=0,j.subscription=0,j.group=j.subgroup=0;
  bool better;
  if(best<0)better=true;
  else if(first!=best_first)better=first;
  else better=moqt_wire_before(&j,&bj);
  if(better){best=(int)i;best_first=first;bj=j;}
 }
 if(best<0)return 0;
 tx *x=&s->t[best];
 size_t left=remaining(x);
 *id=x->id;
 if(x->data){v->base=x->data+x->sent;v->len=left;}
 else if(x->sent<x->head_len){v->base=x->head+x->sent;v->len=x->head_len-x->sent;}
 else{v->base=x->b?x->b->data+(x->sent-x->head_len):NULL;v->len=left;}
 if(x->kind==K_OBJECT&&is_image(x->alias-1)&&v->len>allow)v->len=allow;
 /* one packet at a time, so that a result is chosen as soon as it arrives */
 if(cfg.policy!=POL_FIFO&&x->kind!=K_CONTROL&&x->kind!=K_REQUEST&&v->len>1200)v->len=1200;
 *fin=x->fin&&v->len==left;
 return 1;
}
static void d_pushed(qconn *q,int64_t id,size_t n,bool fin){
 dsess *s=q->app;tx *x=slot_of(s,id);
 if(!x)return;
 uint64_t now=quic_now_us();
 if(!x->first_us&&n)x->first_us=now;
 x->sent+=n;
 if(fin&&!x->fin_sent){x->fin_sent=true;x->fin_us=now;}
}
static void d_blocked(qconn *q,int64_t id){dsess *s=q->app;tx *x=slot_of(s,id);if(x)x->blocked=true;}
static void d_gone(qconn *q){
 dsess *s=q->app;
 logj("session_end","\"sess\":%u",s->id);
 if(cfg.roles&&!s->roles_logged){s->roles_logged=true;
  logj("roles","\"sess\":%u,\"helper\":%u,\"delivered\":%u,\"aged\":%u,\"late_idle\":%u,\"superseded\":%u,\"filtered\":%u,\"dispatched\":%u,\"results\":%u,\"out_expired\":%u,\"held_expired\":%u",s->id,(unsigned)s->helper,
       s->roles_delivered,s->roles_aged,s->roles_late_idle,s->roles_superseded,s->roles_filtered,s->dispatched,s->results,s->out_expired,s->held_expired);}
 for(unsigned i=0;i<TXS;i++)if(s->t[i].used)slot_free(&s->t[i]);
 void *old;while((old=moqt_core_cache_evict(&s->core,MOQT_CORE_TRACKS)))blob_drop(old);
 if(s->hs){for(unsigned i=0;i<HELPER_STREAMS;i++)if(s->hs[i].used)ustream_free(&s->hs[i]);free(s->hs);}
 free(s);q->app=NULL;
}
static const qhandler down_handler={.opened=d_opened,.data=d_data,.acked=d_acked,.closed=d_closed,.reset=d_reset,
 .pull=d_pull,.pushed=d_pushed,.blocked=d_blocked,.tick=d_tick,.gone=d_gone};

/* -- upstream: the device ---------------------------------------------------------- */
#define UP_STREAMS 64
typedef struct {
 qconn *q; bool setup_sent,peer_setup; int64_t control; uint8_t ctl_out[1024]; size_t ctl_len,ctl_sent;
 struct { int64_t id; uint8_t out[512]; size_t len,sent; uint8_t in[2048]; size_t in_len; int track; } req[TRACKS];
 unsigned nreq; int alias_track[16];
 unsigned next_rid; int img_req; bool img_off; uint64_t calm_since,ping_at,img_ok_at; unsigned switches;
 upstream_stream s[UP_STREAMS];
 uint8_t peer_ctl[2048]; size_t peer_ctl_len; int64_t peer_control;
 bool emu; unsigned dev;   /* --emu-up: a model camera, the dev-th */
} usess;

static void store_object(unsigned track,const upstream_stream *u,uint64_t object,uint8_t status,const uint8_t *props,size_t props_len,
                         const uint8_t *payload,size_t len,unsigned helper_sess){
 uint64_t now=quic_now_us();
 if(helper_sess||cur_dev){
  uint64_t cap_ms=0,pub_age=0;bool has_cap=false,has_pub=false;
  moqt_kvp_it it=moqt_kvp_begin(props,props_len);moqt_kvp k;
  while(props_len&&moqt_kvp_next(&it,&k)==1)if(!k.is_bytes){if(k.type==0x78){has_cap=true;cap_ms=k.value;}else if(k.type==41452){has_pub=true;pub_age=k.value;}}
  static upstream_stream mapped;unsigned mdev=0;
  if(helper_sess)
   for(qconn *c=quic_conns();c;c=c->next)
    if(c->h==&down_handler&&c->app&&((dsess*)c->app)->id==helper_sess){
     dsess *d=c->app;
     if(cfg.helper_dispatch){   /* --helper-dispatch: the helper's result frees its slot */
      for(unsigned i=0;i<d->nout;i++)if(d->out[i].group==u->group){d->out[i]=d->out[--d->nout];d->results++;break;}
      helper_dispatch(d,now);
     }
     if((cfg.emu_up||cfg.helper_dispatch)&&u->group>=HG_BASE){   /* back to the camera and its group */
      unsigned k=(unsigned)((u->group-HG_BASE)%1024);
      if(d->hmap[k].hg!=u->group){logj("helper_unmapped","\"sess\":%u,\"hg\":%llu",helper_sess,(unsigned long long)u->group);return;}
      mapped=*u;mapped.group=d->hmap[k].group;mdev=d->hmap[k].dev;
      logj("helper_map","\"hg\":%llu,\"group\":%llu,\"dev\":%u",(unsigned long long)u->group,(unsigned long long)mapped.group,mdev);
      u=&mapped;
     }
    }
  if(helper_sess&&(mdev||u->group>=EMU_GROUP_BASE)){   /* the helper's result for a model camera: logged, not served */
   logj("emu_result","\"dev\":%llu,\"group\":%llu,\"capture_ms\":%llu,\"bytes\":%zu,\"sess\":%u",(unsigned long long)(u->group/EMU_GROUP_BASE),
        (unsigned long long)u->group,(unsigned long long)cap_ms,len,helper_sess);
   return;
  }
  if(!helper_sess&&cur_dev){   /* a frame of a model camera: to the helpers only */
   if(track!=0||cur_dev>=EMU_DEVS)return;
   stored st={.used=true,.track=track,.group=u->group,.object=object,.subgroup=u->subgroup,.serial=0,.priority=u->priority,.status=status,
    .has_priority=u->has_priority,.end_of_group=u->end_of_group,.props_len=props_len<=MOQT_CORE_PROPS?props_len:0,.b=blob_new(payload,len),.up_us=now,
    .dev=u->group>=EMU_GROUP_BASE?(unsigned)(u->group/EMU_GROUP_BASE):cur_dev};   /* the camera's own number, in its groups */
   if(st.props_len)memcpy(st.props,props,st.props_len);
   static struct {bool have,have_pub;int64_t off;uint64_t pub;} em[EMU_DEVS];
   if(cfg.roles&&has_cap){
    int64_t dlt=(int64_t)(now/1000)-(int64_t)cap_ms;
    if(!em[cur_dev].have||dlt<em[cur_dev].off){em[cur_dev].off=dlt;em[cur_dev].have=true;}
    if(has_pub&&(!em[cur_dev].have_pub||pub_age<em[cur_dev].pub)){em[cur_dev].pub=pub_age;em[cur_dev].have_pub=true;}
    st.has_age=true;st.age_ms=(uint64_t)(dlt-em[cur_dev].off)+(em[cur_dev].have_pub?em[cur_dev].pub:0);
   }
   logj("emu_object","\"dev\":%u,\"group\":%llu,\"bytes\":%zu,\"capture_ms\":%llu,\"age_ms\":%llu",cur_dev,(unsigned long long)u->group,len,
        (unsigned long long)cap_ms,(unsigned long long)st.age_ms);
   if(cfg.helper_dispatch&&cfg.roles&&cfg.offload){gq_put(&st);helper_dispatch(NULL,now);}
   else for(qconn *c=quic_conns();c;c=c->next)
    if(c->h==&down_handler&&c->app&&!c->closing&&!c->draining&&((dsess*)c->app)->helper)deliver(c->app,track,&st);
   blob_drop(st.b);
   return;
  }
 }
 stored *slot=NULL,*oldest_image=NULL;unsigned images=0;
 for(unsigned i=0;i<STORE;i++){
  stored *e=&store[i];
  if(!e->used){if(!slot)slot=e;continue;}
  if(is_image(e->track)){images++;if(!oldest_image||e->serial<oldest_image->serial)oldest_image=e;}
 }
 if(is_image(track)&&images>=cfg.images_kept&&oldest_image){blob_drop(oldest_image->b);memset(oldest_image,0,sizeof(*oldest_image));slot=oldest_image;}
 if(!slot){fprintf(stderr,"store full\n");return;}
 if(props_len>MOQT_CORE_PROPS)props_len=0;
 *slot=(stored){.used=true,.track=track,.group=u->group,.object=object,.subgroup=u->subgroup,.serial=++store_serial,.priority=u->priority,
  .status=status,.has_priority=u->has_priority,.end_of_group=u->end_of_group,.props_len=props_len,.b=blob_new(payload,len),.up_us=now};
 if(props_len)memcpy(slot->props,props,props_len);
 /* integer properties, as the receivers record them: {"type": value} in decimal */
 char pj[512];size_t pn=0;pj[0]=0;
 moqt_kvp_it it=moqt_kvp_begin(props,props_len);moqt_kvp k;
 bool has_cap=false,has_pub=false;uint64_t cap_ms=0,pub_age=0;
 while(props_len&&moqt_kvp_next(&it,&k)==1&&pn<sizeof(pj)-48)
  if(!k.is_bytes){
   pn+=(size_t)snprintf(pj+pn,sizeof(pj)-pn,"%s\"%llu\":%llu",pn?",":"",(unsigned long long)k.type,(unsigned long long)k.value);
   if(k.type==0x78){has_cap=true;cap_ms=k.value;}
   else if(k.type==41452){has_pub=true;pub_age=k.value;}
   else if(k.type==0x7C)slot->evidence=k.value==1;
  }
 /* D1 (--roles): the age since capture at the arrival. The clocks of the device and the relay differ by an unknown
  * offset; per track, arrival - capture is taken from its smallest value, and the smallest age at publication is
  * added back, so the estimate is the age of the fastest Object plus the extra delay of this one (a lower bound). */
 if(cfg.roles&&has_cap&&!helper_sess&&track<TRACKS){
  static struct {bool have,have_pub;int64_t off;uint64_t pub;} am[TRACKS];
  int64_t dlt=(int64_t)(slot->up_us/1000)-(int64_t)cap_ms;
  if(!am[track].have||dlt<am[track].off){am[track].off=dlt;am[track].have=true;}
  if(has_pub&&(!am[track].have_pub||pub_age<am[track].pub)){am[track].pub=pub_age;am[track].have_pub=true;}
  slot->has_age=true;slot->age_ms=(uint64_t)(dlt-am[track].off)+(am[track].have_pub?am[track].pub:0);
 }
 if(helper_sess)   /* the source is a helper (--helper-events) */
  logj("up_object","\"alias\":%u,\"group\":%llu,\"object\":%llu,\"bytes\":%zu,\"properties\":{%s},\"source\":\"helper\",\"sess\":%u",track+1,
       (unsigned long long)u->group,(unsigned long long)object,len,pj,helper_sess);
 else logj("up_object","\"alias\":%u,\"group\":%llu,\"object\":%llu,\"bytes\":%zu,\"properties\":{%s}",track+1,(unsigned long long)u->group,
      (unsigned long long)object,len,pj);
 if(cfg.roles&&slot->has_age&&track==0)logj("roles_age","\"group\":%llu,\"age_ms\":%llu,\"evidence\":%u",(unsigned long long)u->group,
      (unsigned long long)slot->age_ms,(unsigned)slot->evidence);
 /* --helper-dispatch: the frame is held once for the helpers (the helper sessions are skipped in deliver) */
 if(cfg.helper_dispatch&&cfg.roles&&cfg.offload&&track==0&&!helper_sess&&!(cfg.helper_skip_evidence&&slot->evidence)){gq_put(slot);helper_dispatch(NULL,now);}
 for(qconn *c=quic_conns();c;c=c->next)
  if(c->h==&down_handler&&c->app&&!c->closing&&!c->draining){
   dsess *d=c->app;
   deliver(d,track,slot);
   cache_put(d,track,slot);
  }
}

/* Parses the subgroup stream as far as complete objects reach. `alias_track` maps the
 * publisher's Track Aliases to relay tracks; helper_sess is 0 for the device. */
static int parse_subgroup(const int *alias_track,upstream_stream *u,unsigned helper_sess){
 for(;;){
  moqt_rd r=moqt_rd_of(u->buf+u->pos,u->len-u->pos);
  if(!u->header){
   uint64_t flags=moqt_rd_vi(&r);
   if(r.bad)return 0;
   if(!(flags<128&&(flags&0x10)))return -1;
   unsigned mode=(unsigned)((flags&0x06)>>1);
   if(mode==3)return -1;
   u->alias=moqt_rd_vi(&r);u->group=moqt_rd_vi(&r);
   u->subgroup=0;u->first_object_mode=mode==1;
   if(mode==2)u->subgroup=moqt_rd_vi(&r);
   u->has_priority=!(flags&0x20);
   if(u->has_priority)u->priority=moqt_rd_u8(&r);
   if(r.bad)return 0;
   u->props=flags&0x01;u->end_of_group=flags&0x08;
   u->header=true;u->pos=u->len-r.n;continue;
  }
  if(!r.n)return 0;
  uint64_t delta=moqt_rd_vi(&r);
  const uint8_t *props=NULL;size_t props_len=0;
  if(u->props){props_len=(size_t)moqt_rd_vi(&r);props=moqt_rd_bytes(&r,props_len);}
  uint64_t size=moqt_rd_vi(&r);uint8_t status=MOQT_STATUS_NORMAL;const uint8_t *payload=NULL;
  if(size)payload=moqt_rd_bytes(&r,(size_t)size);
  else status=(uint8_t)moqt_rd_vi(&r);
  if(r.bad)return 0;
  uint64_t object=u->has_prev?u->prev_object+delta+1:delta;
  u->has_prev=true;u->prev_object=object;
  if(u->first_object_mode){u->subgroup=object;u->first_object_mode=false;}
  u->pos=u->len-r.n;
  int track=u->alias<16?alias_track[u->alias]:-1;
  if(track<0){fprintf(stderr,"unknown alias %llu\n",(unsigned long long)u->alias);continue;}
  if(!u->has_priority)u->priority=tracks[track].priority;
  store_object((unsigned)track,u,object,status,props,props_len,payload,(size_t)size,helper_sess);
 }
}

static void u_accepted(qconn *q){
 usess *u=calloc(1,sizeof(*u));if(!u)abort();
 u->q=q;q->app=u;u->control=-1;u->peer_control=-1;
 for(unsigned i=0;i<16;i++)u->alias_track[i]=-1;
 logj("session","\"conn\":%u,\"role\":\"device\"",q->id);
}
static unsigned emu_count;
static void u_accepted_emu(qconn *q){
 u_accepted(q);usess *u=q->app;u->emu=true;u->dev=++emu_count;
 logj("emu_session","\"conn\":%u,\"dev\":%u",q->id,u->dev);
}
/* subscriber priority of each upstream track; --offload-priority sets the one of offload/jpeg (track 0):
 * 0 (default) puts the helper's frames first, above 128 they yield to the evidence (mission first) */
static uint8_t sub_prio[TRACKS]={0,64,128,0};
/* SUBSCRIBE for track t on a new request stream, in request slot k */
static bool u_sub_one(usess *u,unsigned t,unsigned k){
 int64_t id;
 if(ngtcp2_conn_open_bidi_stream(u->q->conn,&id,NULL)!=0)return false;
 uint8_t *o=u->req[k].out;moqt_wr w=moqt_wr_of(o,sizeof(u->req[0].out));
 size_t mark=moqt_wr_begin_message(&w,MOQT_MSG_SUBSCRIBE);
 moqt_wr_vi(&w,1+2*u->next_rid++);                     /* the server uses odd Request IDs */
 moqt_ns_write(&w,tracks[t].ns,tracks[t].ns_count);
 /* --up-image-track: the device publishes its frames under another name (video firmware) */
 const char *name=t==2?(cfg.up_image_track?cfg.up_image_track:cfg.image_track):tracks[t].name;
 moqt_wr_vi(&w,strlen(name));moqt_wr_bytes(&w,name,strlen(name));
 moqt_wr_vi(&w,1);moqt_wr_vi(&w,MOQT_P_SUBSCRIBER_PRIORITY);moqt_wr_u8(&w,sub_prio[t]);
 moqt_wr_end_message(&w,mark);
 if(w.bad)abort();
 u->req[k].id=id;u->req[k].len=w.n;u->req[k].sent=0;u->req[k].in_len=0;u->req[k].track=(int)t;
 return true;
}
static void u_subscribe(usess *u){
 if(u->emu){if(u_sub_one(u,0,u->nreq))u->nreq++;return;}   /* a model camera: its helper frames only */
 /* --offload: also the camera frames for a helper (track 0, renamed offload/jpeg) */
 unsigned want[3]={T_RESULTS,2,0},nwant=cfg.offload?3:2;
 for(unsigned k=0;k<nwant;k++){
  if(!u_sub_one(u,want[k],u->nreq))return;
  if(want[k]==2)u->img_req=(int)u->nreq;
  u->nreq++;
 }
}
/* --rdrive-ms (base reproduction, Hamara et al., IEEE MCNA 2025: receiver-driven adaptation of
 * MoQ by subscribing to and unsubscribing from tracks to hold a latency target). The relay is the
 * receiver of the device: it probes the first hop every 50 ms (PING), takes the queueing delay as
 * the latest round trip less the smallest, unsubscribes from the image track when it exceeds the
 * target and subscribes again when it has stayed below half the target for 300 ms. It starts 2 s
 * after the device has accepted the first image subscription: during the handshake and the first
 * round trips the smallest round trip is not yet known, the queue estimate is the handshake itself
 * (~250 ms at 128 kbit/s), and an unsubscription before SUBSCRIBE_OK left the session idle until
 * the device's idle timeout (22 s), connection after connection. */
static void u_tick(qconn *q,uint64_t now){
 usess *u=q->app;
 if(!cfg.rdrive_ms||!u||!u->peer_setup||u->nreq<2||!q->conn||!u->img_ok_at||now-u->img_ok_at<2000000)return;
 if(now-u->ping_at>=50000){u->ping_at=now;ngtcp2_conn_set_keep_alive_timeout(q->conn,50*NGTCP2_MILLISECONDS);}
 ngtcp2_conn_info ci;ngtcp2_conn_get_conn_info(q->conn,&ci);
 if(ci.min_rtt==UINT64_MAX||!ci.latest_rtt)return;
 uint64_t queue_us=(ci.latest_rtt-ci.min_rtt)/NGTCP2_MICROSECONDS,target_us=(uint64_t)cfg.rdrive_ms*1000;
 if(!u->img_off&&queue_us>target_us){
  ngtcp2_conn_shutdown_stream(q->conn,0,u->req[u->img_req].id,MOQT_RST_CANCELLED);
  u->img_off=true;u->calm_since=0;u->switches++;
  logj("rd_unsubscribe","\"queue_us\":%llu,\"switches\":%u",(unsigned long long)queue_us,u->switches);
 }else if(u->img_off){
  if(queue_us*2>=target_us){u->calm_since=0;return;}
  if(!u->calm_since){u->calm_since=now;return;}
  if(now-u->calm_since<300000)return;
  if(u_sub_one(u,2,(unsigned)u->img_req)){
   u->img_off=false;u->switches++;
   logj("rd_subscribe","\"queue_us\":%llu,\"switches\":%u",(unsigned long long)queue_us,u->switches);
  }
 }
}
static void u_opened(qconn *q){
 usess *u=q->app;int64_t id;
 size_t n=moqt_setup_write_ext(u->ctl_out,sizeof(u->ctl_out),NULL,NULL,"aiot-relay/1",8,0,NULL);
 if(!n||ngtcp2_conn_open_uni_stream(q->conn,&id,NULL)!=0){quic_close(q,MOQT_S_INTERNAL_ERROR);return;}
 u->control=id;u->ctl_len=n;u->setup_sent=true;
 logj("device_handshake","\"conn\":%u",q->id);
}
static upstream_stream *ustream(usess *u,int64_t id,bool create){
 for(unsigned i=0;i<UP_STREAMS;i++)if(u->s[i].used&&u->s[i].id==id)return &u->s[i];
 if(!create)return NULL;
 for(unsigned i=0;i<UP_STREAMS;i++)if(!u->s[i].used){memset(&u->s[i],0,sizeof(u->s[i]));u->s[i].used=true;u->s[i].id=id;return &u->s[i];}
 return NULL;
}
static void ustream_free(upstream_stream *s){free(s->buf);memset(s,0,sizeof(*s));}
/* Bytes of one subgroup stream: appended, parsed, and the stream freed at its end. */
static void subgroup_rx(upstream_stream *s,const int *alias_track,const uint8_t *d,size_t n,bool fin,unsigned helper_sess){
 if(s->len+n>s->cap){size_t c=s->cap?s->cap*2:16384;while(c<s->len+n)c*=2;s->buf=realloc(s->buf,c);if(!s->buf)abort();s->cap=c;}
 if(n)memcpy(s->buf+s->len,d,n);
 s->len+=n;
 if(parse_subgroup(alias_track,s,helper_sess)<0){fprintf(stderr,"bad subgroup stream\n");ustream_free(s);return;}
 if(s->pos>65536){memmove(s->buf,s->buf+s->pos,s->len-s->pos);s->len-=s->pos;s->pos=0;}
 if(fin)ustream_free(s);
}
static void u_end_tracks(int track){
 for(qconn *c=quic_conns();c;c=c->next)
  if(c->h==&down_handler&&c->app){dsess *d=c->app;if(!d->core.tracks[track].ended){moqt_core_track_end(&d->core,(unsigned)track);drain(d);}}
}
static int u_data(qconn *q,int64_t id,const uint8_t *d,size_t n,bool fin){
 usess *u=q->app;
 bool uni=!ngtcp2_is_bidi_stream(id);
 if(uni&&(id==u->peer_control||u->peer_control<0)&&!ustream(u,id,false)){
  /* The first unidirectional stream that starts with SETUP is the control stream. */
  if(u->peer_control<0){
   uint64_t t;size_t used;
   if(moqt21_vi_read(d,n,&t,&used)==1&&t==MOQT_MSG_SETUP)u->peer_control=id;
  }
  if(id==u->peer_control){
   if(u->peer_ctl_len+n<=sizeof(u->peer_ctl)){memcpy(u->peer_ctl+u->peer_ctl_len,d,n);u->peer_ctl_len+=n;}
   for(;;){
    uint64_t t;const uint8_t *body;size_t bl,used;
    if(moqt21_message_read(u->peer_ctl,u->peer_ctl_len,&t,&body,&bl,&used)!=1)break;
    if(t==MOQT_MSG_SETUP&&!u->peer_setup){u->peer_setup=true;logj(u->emu?"emu_setup":"device_setup","\"conn\":%u",q->id);u_subscribe(u);}
    else if(t==MOQT_MSG_GOAWAY)logj("device_goaway","");
    memmove(u->peer_ctl,u->peer_ctl+used,u->peer_ctl_len-used);u->peer_ctl_len-=used;
   }
   return 0;
  }
 }
 if(!uni){
  for(unsigned i=0;i<u->nreq;i++)if(u->req[i].id==id){
   if(u->req[i].in_len+n<=sizeof(u->req[i].in)){memcpy(u->req[i].in+u->req[i].in_len,d,n);u->req[i].in_len+=n;}
   for(;;){
    uint64_t t;const uint8_t *body;size_t bl,used;
    if(moqt21_message_read(u->req[i].in,u->req[i].in_len,&t,&body,&bl,&used)!=1)break;
    if(t==MOQT_MSG_SUBSCRIBE_OK){
     moqt_rd r=moqt_rd_of(body,bl);uint64_t alias=moqt_rd_vi(&r);
     if(!r.bad&&alias<16)u->alias_track[alias]=u->req[i].track;
     if((int)i==u->img_req&&!u->img_ok_at)u->img_ok_at=quic_now_us();
     logj(u->emu?"emu_subscribed":"device_subscribed","\"track\":%d,\"alias\":%llu",u->req[i].track,(unsigned long long)alias);
    }else if(t==MOQT_MSG_PUBLISH_DONE&&u->emu){
     logj("emu_publish_done","\"dev\":%u",u->dev);
    }else if(t==MOQT_MSG_PUBLISH_DONE){
     logj("device_publish_done","\"track\":%d",u->req[i].track);
     u_end_tracks(u->req[i].track);
     /* --rdrive-ms: the image subscription may be cancelled, or renewed too late to be ended, when
      * the device ends its run; the end of the results is the end of the run for the image too */
     if(cfg.rdrive_ms&&u->req[i].track==T_RESULTS)u_end_tracks(2);
    }else if(t==MOQT_MSG_REQUEST_ERROR)logj("device_request_error","\"track\":%d",u->req[i].track);
    memmove(u->req[i].in,u->req[i].in+used,u->req[i].in_len-used);u->req[i].in_len-=used;
   }
   return 0;
  }
  return 0;
 }
 upstream_stream *s=ustream(u,id,true);
 if(!s){fprintf(stderr,"upstream streams exhausted\n");return 0;}
 cur_dev=u->emu?u->dev:0;
 subgroup_rx(s,u->alias_track,d,n,fin,0);
 cur_dev=0;
 return 0;
}
static void u_closed(qconn *q,int64_t id,bool app_error,uint64_t code){
 (void)app_error;(void)code;usess *u=q->app;upstream_stream *s=ustream(u,id,false);if(s)ustream_free(s);
}
static void u_reset(qconn *q,int64_t id,uint64_t code,bool stop){
 usess *u=q->app;upstream_stream *s=ustream(u,id,false);
 if(s&&!stop){logj("device_reset","\"alias\":%llu,\"group\":%llu,\"code\":%llu",(unsigned long long)s->alias,(unsigned long long)s->group,(unsigned long long)code);ustream_free(s);}
}
static int u_pull(qconn *q,int64_t *id,ngtcp2_vec *v,bool *fin){
 usess *u=q->app;
 if(u->control>=0&&u->ctl_sent<u->ctl_len){*id=u->control;v->base=u->ctl_out+u->ctl_sent;v->len=u->ctl_len-u->ctl_sent;*fin=false;return 1;}
 for(unsigned i=0;i<u->nreq;i++)if(u->req[i].sent<u->req[i].len){*id=u->req[i].id;v->base=u->req[i].out+u->req[i].sent;v->len=u->req[i].len-u->req[i].sent;*fin=false;return 1;}
 return 0;
}
static void u_pushed(qconn *q,int64_t id,size_t n,bool fin){
 (void)fin;usess *u=q->app;
 if(id==u->control){u->ctl_sent+=n;return;}
 for(unsigned i=0;i<u->nreq;i++)if(u->req[i].id==id)u->req[i].sent+=n;
}
static void u_gone(qconn *q){
 usess *u=q->app;logj("session_end","\"conn\":%u,\"role\":\"device\"",q->id);
 for(unsigned i=0;i<UP_STREAMS;i++)if(u->s[i].used)ustream_free(&u->s[i]);
 free(u);q->app=NULL;
}
static const qhandler up_handler={.opened=u_opened,.data=u_data,.closed=u_closed,.reset=u_reset,.pull=u_pull,.pushed=u_pushed,.tick=u_tick,.gone=u_gone};

/* -- main --------------------------------------------------------------------------- */
static volatile bool stop;
static uint64_t deadline_us;
static void on_signal(int sig){(void)sig;stop=true;}
static void idle(uint64_t now){if(deadline_us&&now>=deadline_us)stop=true;if(logf_)fflush(logf_);}

static int split_addr(const char *s,char *ip,size_t cap,int *port){
 const char *c=strrchr(s,':');if(!c||(size_t)(c-s)>=cap)return -1;
 memcpy(ip,s,(size_t)(c-s));ip[c-s]=0;*port=atoi(c+1);return *port>0?0:-1;
}
int main(int argc,char **argv){
 const char *up="0.0.0.0:14568",*down="0.0.0.0:14600",*cert=NULL,*key=NULL,*logp=NULL;unsigned idle_ms=20000;
 static struct option o[]={{"up",1,0,'u'},{"down",1,0,'d'},{"cert",1,0,'c'},{"key",1,0,'k'},{"log",1,0,'l'},
  {"policy",1,0,'p'},{"budget-ms",1,0,'b'},{"probe-ms",1,0,'P'},{"image-timeout-ms",1,0,'T'},{"timeout-all",0,0,'A'},{"rdrive-ms",1,0,'R'},{"up-image-track",1,0,'U'},{"offload",0,0,'O'},{"helper-events",0,0,'H'},{"offload-priority",1,0,'Q'},{"image-track",1,0,'i'},
  {"roles",0,0,'r'},{"helper-max-age-ms",1,0,'M'},{"helper-deadline-ms",1,0,'D'},{"helper-service-ms",1,0,'S'},{"operator-all",0,0,'a'},{"helper-skip-evidence",0,0,'E'},{"helper-free-window",0,0,'J'},{"emu-up",1,0,'V'},{"helper-dispatch",1,0,'W'},{"helper-admit",0,0,'X'},{"seconds",1,0,'s'},{"keepalive-ms",1,0,'K'},{"idle-ms",1,0,'I'},{"no-cache",0,0,'N'},{"cc",1,0,'C'},{"fixed-window",1,0,'F'},{"window-bdp",0,0,'B'},{0,0,0,0}};
 for(int ch;(ch=getopt_long(argc,argv,"",o,NULL))!=-1;){
  switch(ch){
  case 'u':up=optarg;break; case 'd':down=optarg;break; case 'c':cert=optarg;break; case 'k':key=optarg;break;
  case 'l':logp=optarg;break; case 'b':cfg.budget_ms=(unsigned)atoi(optarg);break; case 'P':cfg.probe_ms=(unsigned)atoi(optarg);break;
  case 'T':cfg.image_timeout_ms=(unsigned)atoi(optarg);break; case 'i':cfg.image_track=optarg;break;
  case 's':cfg.seconds=atof(optarg);break; case 'I':idle_ms=(unsigned)atoi(optarg);break; case 'N':cfg.no_cache=true;break; case 'K':cfg.keepalive_ms=(unsigned)atoi(optarg);break;
  case 'F':cfg.fixed_window=(size_t)atol(optarg);break;
  case 'B':cfg.window_bdp=true;break; case 'A':cfg.timeout_all=true;break; case 'R':cfg.rdrive_ms=(unsigned)atoi(optarg);break; case 'U':cfg.up_image_track=optarg;break; case 'O':cfg.offload=true;tracks[0].name="offload/jpeg";break;
  case 'r':cfg.roles=true;break; case 'M':cfg.helper_max_age_ms=(unsigned)atoi(optarg);break;
  case 'D':cfg.helper_deadline_ms=(unsigned)atoi(optarg);break; case 'S':cfg.helper_service_ms=(unsigned)atoi(optarg);break; case 'a':cfg.operator_all=true;break; case 'E':cfg.helper_skip_evidence=true;break; case 'J':cfg.helper_free_window=true;break; case 'V':cfg.emu_up=optarg;break; case 'W':cfg.helper_dispatch=(unsigned)atoi(optarg);break; case 'X':cfg.helper_admit=true;break;
  case 'H':cfg.helper_events=true;break; case 'Q':sub_prio[0]=(uint8_t)atoi(optarg);break;
  case 'C':if(!strcmp(optarg,"bbr"))quic_cc_algo=NGTCP2_CC_ALGO_BBR;
   else if(strcmp(optarg,"cubic")){fprintf(stderr,"cc: cubic, bbr\n");return 2;}break;
  case 'p':cfg.policy=-1;for(int i=0;i<4;i++)if(!strcmp(optarg,policy_names[i]))cfg.policy=i;
   if(cfg.policy<0){fprintf(stderr,"policy: fifo, prio, prio-to, alert\n");return 2;}break;
  default:return 2;
  }
 }
 if(!cert||!key){fprintf(stderr,"usage: moq_relay --cert C --key K [--up ip:port] [--down ip:port] [--policy P] [--log F]\n");return 2;}
 if(logp&&!(logf_=fopen(logp,"w"))){perror(logp);return 1;}
 char ip[64];int port;
 if(quic_init(cert,key,"moqt-21",idle_ms)){fprintf(stderr,"TLS setup failed\n");return 1;}
 if(split_addr(up,ip,sizeof(ip),&port)||quic_listen(ip,port,&up_handler,u_accepted)){fprintf(stderr,"listen %s failed\n",up);return 1;}
 if(cfg.emu_up&&(split_addr(cfg.emu_up,ip,sizeof(ip),&port)||quic_listen(ip,port,&up_handler,u_accepted_emu))){fprintf(stderr,"listen %s failed\n",cfg.emu_up);return 1;}
 if(cfg.emu_up||cfg.helper_dispatch)logj("emu_config","\"emu_up\":\"%s\",\"helper_dispatch\":%u,\"helper_admit\":%u",cfg.emu_up?cfg.emu_up:"",cfg.helper_dispatch,(unsigned)cfg.helper_admit);
 if(split_addr(down,ip,sizeof(ip),&port)||quic_listen(ip,port,&down_handler,d_accepted)){fprintf(stderr,"listen %s failed\n",down);return 1;}
 signal(SIGINT,on_signal);signal(SIGTERM,on_signal);
 if(cfg.seconds>0)deadline_us=quic_now_us()+(uint64_t)(cfg.seconds*1e6);
 logj("start","\"policy\":\"%s\",\"budget_ms\":%u,\"probe_ms\":%u,\"image_timeout_ms\":%u,\"up\":\"%s\",\"down\":\"%s\",\"cache\":%u,\"cc\":\"%s\",\"fixed_window\":%zu,\"window_bdp\":%u,\"timeout_all\":%u,\"rdrive_ms\":%u,\"offload_priority\":%u",
      policy_names[cfg.policy],cfg.budget_ms,cfg.probe_ms,cfg.image_timeout_ms,up,down,(unsigned)!cfg.no_cache,quic_cc_algo==NGTCP2_CC_ALGO_BBR?"bbr":"cubic",cfg.fixed_window,(unsigned)cfg.window_bdp,(unsigned)cfg.timeout_all,cfg.rdrive_ms,(unsigned)sub_prio[0]);
 if(cfg.helper_free_window)logj("helper_window","\"free\":1");
 if(cfg.roles)logj("roles_config","\"helper_max_age_ms\":%u,\"helper_deadline_ms\":%u,\"helper_service_ms\":%u,\"operator_all\":%u,\"helper_skip_evidence\":%u,\"helper_free_window\":%u,\"offload\":%u",cfg.helper_max_age_ms,cfg.helper_deadline_ms,cfg.helper_service_ms,(unsigned)cfg.operator_all,(unsigned)cfg.helper_skip_evidence,(unsigned)cfg.helper_free_window,(unsigned)cfg.offload);
 if(cfg.roles&&!cfg.offload)fprintf(stderr,"--roles acts on the offload/jpeg track: give --offload\n");
 if(cfg.helper_events)logj("helper_events","\"track\":\"%s\",\"alias\":%u,\"implementation\":\"%s\"",tracks[T_HELPER].name,T_HELPER+1,HELPER_IMPLEMENTATION);
 fprintf(stderr,"relay: policy %s, device %s, subscribers %s\n",policy_names[cfg.policy],up,down);
 quic_run(&stop,idle);
 if(cfg.roles)for(qconn *c=quic_conns();c;c=c->next)
  if(c->h==&down_handler&&c->app){dsess *s=c->app;if(!s->roles_logged){s->roles_logged=true;
   logj("roles","\"sess\":%u,\"helper\":%u,\"delivered\":%u,\"aged\":%u,\"late_idle\":%u,\"superseded\":%u,\"filtered\":%u,\"dispatched\":%u,\"results\":%u,\"out_expired\":%u,\"held_expired\":%u,\"at\":\"stop\"",s->id,(unsigned)s->helper,
        s->roles_delivered,s->roles_aged,s->roles_late_idle,s->roles_superseded,s->roles_filtered,s->dispatched,s->results,s->out_expired,s->held_expired);}}
 if(cfg.helper_dispatch)logj("broker","\"superseded\":%u,\"expired\":%u",gq_superseded,gq_expired);
 logj("stop","");
 if(logf_)fclose(logf_);
 return 0;
}
