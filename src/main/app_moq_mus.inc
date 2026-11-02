/* Mission-utility scheduling of evidence (moq_mission_sched.h) in the application.
 *
 * The source images of the latest events are kept until their evidence deadline. The
 * planner chooses the representation of every open event from the service rate of the
 * link, the deadlines, what is still in the network, and the time that the conversion of a
 * representation takes. The codec prepares the next representation while an object is
 * under way. */
#include "moq_mission_sched.h"
#ifndef AIOT_MOQT_MUS_STORE
#define AIOT_MOQT_MUS_STORE 4
#endif
#define MUS_STORE AIOT_MOQT_MUS_STORE
/* as large as a camera frame can be (SOURCE_IMAGE_CAPACITY): at night the sensor gain raises
 * the frame to 170 kB, and a smaller store drops the evidence of the scheduler alone */
#define MUS_SOURCE_CAP (256U*1024U)
#define MUS_TOP (MUS_REPS-1)
/* AIOT_MOQT_MUS_FINE_WINDOW: a result on the wire with its packet, bytes; the time from its
 * production to its hand-over to the transport, us (measured 5-10 ms with binary results);
 * the smallest image window, bytes */
#define MUS_RESULT_WIRE 160U
#define MUS_HANDOVER_US 10000
#define MUS_IMAGE_FLOOR 256U
typedef struct {
 bool used,probe,closed;      /* closed: the transport refused its evidence; it waits for its deadline */
 uint32_t seq;uint64_t capture_us;size_t len;uint8_t *data;
 int8_t best,active,prepared,converting;
 uint8_t objects;             /* evidence objects of this event so far: each is an object of its Group */
} mus_slot;
static mus_slot mus_store[MUS_STORE];
static uint64_t mus_last_probe,mus_last_plan,mus_convert_start;
static unsigned mus_convert_rep;
static struct {bool pending;uint32_t seq;} mus_abandoned;
static uint32_t mus_prepare_us[MUS_REPS]={2400000,2800000,0};
static struct {uint64_t sum,max;unsigned calls,events_max;} mus_cost;   /* time that the planner takes */
static struct {unsigned added,evicted,missing,started[MUS_REPS],delivered[MUS_REPS],aborted,probes,late,delivered_late,failed,
 prepared,discarded,refused;} mus_stats;
static bool mus_init(void){
 for(unsigned i=0;i<MUS_STORE;i++){
  mus_store[i]=(mus_slot){.best=-1,.active=-1,.prepared=-1,.converting=-1};
  mus_store[i].data=heap_caps_malloc(MUS_SOURCE_CAP,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  if(!mus_store[i].data)return false;
 }
 #if AIOT_MOQT_MUS_PREDICT
 quic_client_moq_alert_clock(source_manager_alert_safe_us);
 #endif
 return true;
}
static void mus_free(void){for(unsigned i=0;i<MUS_STORE;i++){free(mus_store[i].data);mus_store[i].data=NULL;}}
static mus_slot *mus_find(uint32_t seq){
 for(unsigned i=0;i<MUS_STORE;i++)if(mus_store[i].used&&mus_store[i].seq==seq)return &mus_store[i];
 return NULL;
}
/* Called in the inference task when a result reports a passage event. */
static void mus_add(uint32_t seq,uint64_t capture_us,const uint8_t *image,size_t len){
 if(!image||!len||len>MUS_SOURCE_CAP){mus_stats.missing++;printf("[MOQ_EVIDENCE_MISSING] group=%lu\n",(unsigned long)seq);return;}
 xSemaphoreTake(moq_image_lock,portMAX_DELAY);
 mus_slot *slot=NULL;
 for(unsigned i=0;i<MUS_STORE&&!slot;i++)if(!mus_store[i].used)slot=&mus_store[i];
 if(!slot){
  /* the oldest source that is not in use gives way */
  for(unsigned i=0;i<MUS_STORE;i++){
   mus_slot *s=&mus_store[i];
   if(s->active>=0||s->converting>=0)continue;
   if(!slot||s->capture_us<slot->capture_us)slot=s;
  }
  if(slot){
   if(slot->best<0)mus_stats.evicted++;
   if(slot->prepared>=0)moq_encoding.ready=false;
   printf("[MOQ_MUS_EVICT] {\"group\":%lu,\"best\":%d}\n",(unsigned long)slot->seq,(int)slot->best);
  }
 }
 if(slot){
  memcpy(slot->data,image,len);
  slot->used=true;slot->probe=slot->closed=false;slot->seq=seq;slot->capture_us=capture_us;slot->len=len;
  slot->best=slot->active=slot->prepared=slot->converting=-1;slot->objects=0;mus_stats.added++;
 }else mus_stats.missing++;
 xSemaphoreGive(moq_image_lock);
 printf("[MOQ_EVIDENCE] {\"group\":%lu,\"bytes\":%u,\"stored\":%u}\n",(unsigned long)seq,(unsigned)len,(unsigned)(slot!=NULL));
}
static uint64_t mus_deadline(const mus_slot *s){return s->capture_us+(uint64_t)AIOT_MOQT_AI_DEADLINE_MS*1000;}
static bool mus_busy(uint64_t now){
 bool busy=mus_abandoned.pending;
 xSemaphoreTake(moq_image_lock,portMAX_DELAY);
 for(unsigned i=0;i<MUS_STORE;i++){
  const mus_slot *s=&mus_store[i];
  if(!s->used)continue;
  if(s->active>=0||s->converting>=0||s->prepared>=0||(!s->closed&&s->best<MUS_TOP&&mus_deadline(s)>now))busy=true;
 }
 xSemaphoreGive(moq_image_lock);
 return busy;
}
/* The evidence needs the link now: an object under way or being prepared, or an event of
 * which no representation has arrived yet. Unlike mus_busy, an event whose evidence has
 * arrived does not count while the planner could still send a better representation. */
static bool mus_needs_link(uint64_t now){
 bool need=mus_abandoned.pending;
 xSemaphoreTake(moq_image_lock,portMAX_DELAY);
 for(unsigned i=0;i<MUS_STORE;i++){
  const mus_slot *s=&mus_store[i];
  if(!s->used)continue;
  if(s->active>=0||s->converting>=0||s->prepared>=0||(!s->closed&&s->best<0&&mus_deadline(s)>now))need=true;
 }
 xSemaphoreGive(moq_image_lock);
 return need;
}
static void mus_log(const char *what,const mus_slot *s,int rep,const mus_decision *d,const mus_link *link,unsigned count){
 printf("[MOQ_MUS_PLAN] {\"do\":\"%s\",\"group\":%lu,\"rep\":%d,\"probe\":%u,\"rate_Bps\":%llu,\"deviation_Bps\":%llu,\"drain\":%u,\"events\":%u,\"plan\":[%d,%d,%d,%d]}\n",
  what,(unsigned long)s->seq,rep,(unsigned)s->probe,(unsigned long long)link->rate_Bps,(unsigned long long)link->deviation_Bps,(unsigned)link->drain,count,
  (int)d->plan[0],(int)d->plan[1],(int)d->plan[2],(int)d->plan[3]);
}
static void mus_service(uint64_t now){
 static const mus_config config={.quality={AIOT_MOQT_MUS_QUALITY_LOW,AIOT_MOQT_MUS_QUALITY_MEDIUM,1000},
  .margin_permille=AIOT_MOQT_MUS_MARGIN,.guard_us=100000,.probe_interval_us=2000000,.keep_permille=AIOT_MOQT_MUS_KEEP,
  .sequential=!AIOT_MOQT_MUS_AHEAD};
 uint64_t group,held;size_t bytes,sent,acked;bool delivered,abandoned;
 xSemaphoreTake(moq_image_lock,portMAX_DELAY);
 /* outcome of the objects that left the transport */
 while(quic_client_moq_image_result(&group,&bytes,&held,&delivered)){
  mus_slot *s=mus_find((uint32_t)group);int rep=-1;
  if(mus_abandoned.pending&&mus_abandoned.seq==(uint32_t)group&&!delivered)mus_abandoned.pending=false;
  else if(s&&s->active>=0){
   rep=s->active;
   if(delivered){
    mus_stats.delivered[rep]++;
    if(now>mus_deadline(s))mus_stats.delivered_late++;
    else if(rep>s->best)s->best=(int8_t)rep;
   }
   s->active=-1;s->probe=false;
  }
  printf("[MOQ_MUS_DONE] {\"group\":%llu,\"rep\":%d,\"delivered\":%u,\"bytes\":%u,\"held_us\":%llu}\n",
   (unsigned long long)group,rep,(unsigned)delivered,(unsigned)bytes,(unsigned long long)held);
 }
 /* a representation that the codec finished */
 moq_codec_result converted;
 if(moq_codec_worker_take(&converted)){
  moq_observe_codec(converted,moq_encoding.level);
  mus_slot *s=mus_find(moq_encoding.seq);
  unsigned rep=2U-moq_encoding.level;
  if(s)s->converting=-1;
  if(converted.ok){
   mus_prepare_us[rep]=(uint32_t)(converted.metrics.decode_us+converted.metrics.encode_us);
   moq_encoding.len=converted.metrics.bytes;
   if(s){s->prepared=(int8_t)rep;moq_encoding.ready=true;mus_stats.prepared++;}
  }else{mus_stats.failed++;if(s)s->probe=false;}
 }
 if(now-mus_last_plan<100000){xSemaphoreGive(moq_image_lock);return;}
 mus_last_plan=now;
 bool flying=quic_client_moq_image_flight(&group,&bytes,&sent,&acked,&abandoned);
 bool codec_busy=moq_codec_worker_busy();
 mus_link link={.rate_Bps=quic_client_moq_service_rate(),.codec_free_us=now};
#if AIOT_MOQT_MUS_FIXED_WINDOW
 /* Ablation: a constant limit of the evidence in the network, as TCP_NOTSENT_LOWAT, in place
  * of the limit derived from the delay and the service rate. */
 quic_client_moq_image_window(AIOT_MOQT_MUS_FIXED_WINDOW);
#elif AIOT_MOQT_MUS_STATE_DELAY_MS && AIOT_MOQT_MUS_DELAY_WINDOW
 /* A result waits behind the bytes queued at the bottleneck, which the queueing delay (smoothed
  * less smallest round trip) measures directly. The window of the evidence grows while that delay
  * stays under half the bound and shrinks above it, once a round trip: the window derived from the
  * rate the evidence itself achieved stays where that rate keeps it (the rate is measured under the
  * window), and leaves the link idle. */
 {static size_t w;static uint64_t at;
  if(!w)w=(size_t)(8192ULL*AIOT_MOQT_MUS_STATE_DELAY_MS/1000ULL);
  uint64_t srtt=quic_client_moq_srtt_us(),mrtt=quic_client_moq_min_rtt_us();
  if(srtt&&mrtt&&now-at>=srtt){
   at=now;
   uint64_t qd=srtt>mrtt?srtt-mrtt:0,bound=(uint64_t)AIOT_MOQT_MUS_STATE_DELAY_MS*1000ULL;
   if(qd<bound/2)w+=w/8+1200;
   else if(qd>bound)w=w*3/4;
   if(w<1200)w=1200;
   if(w>65536)w=65536;
  }
  quic_client_moq_image_window(w);}
#elif AIOT_MOQT_MUS_STATE_DELAY_MS && AIOT_MOQT_MUS_FINE_WINDOW
 /* A state result waits behind the evidence that is in the network, then is sent itself and
  * crosses the link. The evidence may hold the part of the delay that is left: the delay
  * less the time to send a result at the service rate, half the smallest round trip and the
  * time the device takes to hand a result over. The image data is cut to that window, so a
  * packet of evidence is smaller than a full packet when the link is slow: a full packet of
  * 1200 bytes alone takes 75 ms at 128 kbit/s. */
 {uint64_t rate=quic_client_moq_service_initialized()?link.rate_Bps:8192;
  int64_t left_us=(int64_t)AIOT_MOQT_MUS_STATE_DELAY_MS*1000-(int64_t)(MUS_RESULT_WIRE*1000000ULL/(rate?rate:1))
                  -(int64_t)(quic_client_moq_min_rtt_us()/2)-MUS_HANDOVER_US;
  size_t window=left_us>0?(size_t)(rate*(uint64_t)left_us/1000000ULL):0;
  /* a few hundred bytes still move the evidence */
  quic_client_moq_image_window(window<MUS_IMAGE_FLOOR?MUS_IMAGE_FLOOR:window);}
#elif AIOT_MOQT_MUS_STATE_DELAY_MS
 /* A state result waits behind the evidence that is in the network: at the service rate
  * that is at most the configured delay. With AIOT_MOQT_MUS_WINDOW_BDP the window also holds
  * the bytes on the wire, the service rate times the smallest round trip: they are in flight
  * but not ahead of a result in the queue of the bottleneck, and without them a round trip
  * longer than the delay caps the evidence at window/rtt. */
 {uint64_t wait_us=(uint64_t)AIOT_MOQT_MUS_STATE_DELAY_MS*1000+(AIOT_MOQT_MUS_WINDOW_BDP?quic_client_moq_min_rtt_us():0);
  size_t window=(size_t)(link.rate_Bps*wait_us/1000000ULL);
  /* before the first sample the rate is not known: little is committed to the network */
  if(!quic_client_moq_service_initialized())window=(size_t)(8192ULL*wait_us/1000000ULL);
  /* at least one packet, or the evidence would not move at all */
  quic_client_moq_image_window(window<1200?1200:window);}
#endif
#if AIOT_MOQT_MUS_PREDICT
 {static uint64_t last_us;
  if(now-last_us>=1000000){uint64_t max,mean;quic_client_moq_predict_stats(&max,&mean);last_us=now;
   printf("[MOQ_MUS_PREDICT] {\"safe_us\":%lu,\"extra_max\":%llu,\"extra_mean\":%llu}\n",
    (unsigned long)source_manager_alert_safe_us(),(unsigned long long)max,(unsigned long long)mean);}}
#endif
#if AIOT_MOQT_MUS_RISK
 /* the scatter of the samples, and at least a twentieth of the rate */
 link.deviation_Bps=quic_client_moq_service_deviation();
 if(link.deviation_Bps<link.rate_Bps/20)link.deviation_Bps=link.rate_Bps/20;
#endif
 if(codec_busy){
  uint64_t end=mus_convert_start+mus_prepare_us[mus_convert_rep];
  link.codec_free_us=end>now+100000?end:now+100000;
 }
 mus_event events[MUS_STORE];mus_slot *slots[MUS_STORE];unsigned count=0;
 for(unsigned i=0;i<MUS_STORE;i++){
  mus_slot *s=&mus_store[i];
  if(!s->used)continue;
  bool mine=flying&&group==s->seq;
  if(s->active>=0&&mine&&abandoned){
   /* the transport gave the object up at its deadline */
   s->active=-1;s->probe=false;mus_abandoned.pending=true;mus_abandoned.seq=s->seq;
  }
  if(s->active>=0&&!mine){xSemaphoreGive(moq_image_lock);return;}   /* its outcome is on the way */
  if(s->active<0&&s->converting<0&&mus_deadline(s)<=now){
   if(s->prepared>=0)moq_encoding.ready=false;
   if(s->best<0)mus_stats.late++;
   s->used=false;continue;
  }
  if(s->closed)continue;
  size_t medium=last_medium_bytes?last_medium_bytes:s->len/4,low=last_low_bytes?last_low_bytes:s->len/12;
  if(s->prepared==1)medium=moq_encoding.len;
  if(s->prepared==0)low=moq_encoding.len;
  events[count]=(mus_event){.seq=s->seq,.deadline_us=mus_deadline(s),.size={low,medium,s->len},.best=s->best,
   .active=s->active,.prepared=s->prepared,.converting=s->converting,.probe=s->probe};
  if(s->active>=0){events[count].acked=acked;events[count].unacked=sent-acked;}
  slots[count++]=s;
 }
 if(flying&&(abandoned||(mus_abandoned.pending&&group==mus_abandoned.seq)))link.drain=sent-acked;
 mus_config c=config;
 for(unsigned r=0;r<MUS_REPS;r++)c.prepare_us[r]=mus_prepare_us[r];
 uint64_t began=(uint64_t)esp_timer_get_time();
 mus_decision d=mus_plan(&c,events,count,now,&link,mus_last_probe);
 uint64_t took=(uint64_t)esp_timer_get_time()-began;
 mus_cost.sum+=took;mus_cost.calls++;
 if(took>mus_cost.max)mus_cost.max=took;
 if(count>mus_cost.events_max)mus_cost.events_max=count;
 mus_slot *s=d.event>=0?slots[d.event]:NULL;
 if(d.action==MUS_ABORT&&s){
  mus_stats.aborted++;
  mus_log("abort",s,d.rep,&d,&link,count);
  /* without a stream the object has ended and its outcome is reported */
  if(quic_client_moq_image_abort(s->seq)){s->active=-1;mus_abandoned.pending=true;mus_abandoned.seq=s->seq;}
 }
 if(d.action==MUS_START&&s){
  if(d.probe&&!s->probe){s->probe=true;mus_last_probe=now;mus_stats.probes++;}
  bool source=d.rep==MUS_TOP;
  if((source||(s->prepared==d.rep&&moq_encoding.ready))&&!flying&&quic_client_moq_image_slot_available()){
   mus_log("send",s,d.rep,&d,&link,count);
   /* the transport copies the object, so the source and the buffer may be reused afterwards */
   uint64_t deadline=s->probe?UINT64_MAX:mus_deadline(s);
   moq_media_object=s->objects++;
   if(source)moq_publish_media(s->data,s->len,s->seq,s->capture_us,deadline,0);
   else{
    moq_publish_media(moq_converted,moq_encoding.len,s->seq,s->capture_us,deadline,2U-(unsigned)d.rep);
    s->prepared=-1;moq_encoding.ready=false;
   }
   if(quic_client_moq_image_flight(&group,&bytes,&sent,&acked,&abandoned)&&group==s->seq){
    s->active=(int8_t)d.rep;mus_stats.started[d.rep]++;
   }else{
    mus_stats.refused++;s->probe=false;s->closed=true;
    printf("[MOQ_MUS_REFUSED] {\"group\":%lu,\"rep\":%d}\n",(unsigned long)s->seq,d.rep);
   }
  }
 }
 if(!AIOT_MOQT_MUS_AHEAD&&(flying||d.action!=MUS_START||d.prepare_event!=d.event))d.prepare_event=-1;
 if(d.prepare_event>=0&&!codec_busy){
  mus_slot *t=slots[d.prepare_event];
  /* the buffer holds a representation that the plan no longer contains */
  for(unsigned i=0;i<count;i++){
   mus_slot *o=slots[i];
   if(o->prepared<0||o->probe||d.plan[i]==o->prepared)continue;
   o->prepared=-1;moq_encoding.ready=false;mus_stats.discarded++;
  }
  if(!moq_encoding.ready){
   moq_codec_request q={.input=t->data,.length=t->len,.level=2U-(unsigned)d.prepare_rep,.output=moq_converted,.capacity=MOQ_IMAGE_CAP};
   moq_encoding.seq=t->seq;moq_encoding.capture=t->capture_us;moq_encoding.deadline=mus_deadline(t);moq_encoding.level=q.level;
   if(moq_codec_worker_submit(&q)){
    t->converting=(int8_t)d.prepare_rep;mus_convert_start=now;mus_convert_rep=(unsigned)d.prepare_rep;
    mus_log("prepare",t,d.prepare_rep,&d,&link,count);
   }else{t->probe=false;printf("[MOQ_CODEC_ERROR] submit\n");}
  }
 }
 xSemaphoreGive(moq_image_lock);
}
static void mus_report(void){
 printf("[MOQ_MUS] {\"added\":%u,\"evicted\":%u,\"missing\":%u,\"started\":[%u,%u,%u],\"delivered\":[%u,%u,%u],\"delivered_late\":%u,"
  "\"aborted\":%u,\"probes\":%u,\"late\":%u,\"codec_failed\":%u,\"prepared\":%u,\"discarded\":%u,\"refused\":%u,\"prepare_us\":[%lu,%lu,%lu],"
  "\"margin\":%u,\"keep\":%u,\"risk\":%u,\"ahead\":%u,\"state_delay_ms\":%u,\"plans\":%u,\"plan_us_mean\":%llu,\"plan_us_max\":%llu,\"plan_events_max\":%u}\n",
  mus_stats.added,mus_stats.evicted,mus_stats.missing,mus_stats.started[0],mus_stats.started[1],mus_stats.started[2],
  mus_stats.delivered[0],mus_stats.delivered[1],mus_stats.delivered[2],mus_stats.delivered_late,mus_stats.aborted,mus_stats.probes,
  mus_stats.late,mus_stats.failed,mus_stats.prepared,mus_stats.discarded,mus_stats.refused,
  (unsigned long)mus_prepare_us[0],(unsigned long)mus_prepare_us[1],(unsigned long)mus_prepare_us[2],
  (unsigned)AIOT_MOQT_MUS_MARGIN,(unsigned)AIOT_MOQT_MUS_KEEP,(unsigned)AIOT_MOQT_MUS_RISK,(unsigned)AIOT_MOQT_MUS_AHEAD,(unsigned)AIOT_MOQT_MUS_STATE_DELAY_MS,
  mus_cost.calls,(unsigned long long)(mus_cost.calls?mus_cost.sum/mus_cost.calls:0),(unsigned long long)mus_cost.max,mus_cost.events_max);
}
