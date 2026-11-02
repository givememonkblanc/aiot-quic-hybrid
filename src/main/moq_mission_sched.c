#include "moq_mission_sched.h"
/* rates of the scenarios in thousandths of the mean deviation around the estimate, and
 * their weights in thousandths: a normal distribution at -1.5, -0.75, 0, 0.75, 1.5 sigma,
 * with sigma = 1.25 mean deviations */
static const int32_t mus_offset[MUS_SCENARIOS]={-1875,-938,0,938,1875};
static const int32_t mus_weight[MUS_SCENARIOS]={103,239,316,239,103};
#define MUS_MIDDLE (MUS_SCENARIOS/2)
typedef struct {
 const mus_config *c;const mus_event *ev;
 unsigned order[MUS_EVENTS],n;   /* open events in deadline order */
 uint64_t now,codec_free,usable[MUS_SCENARIOS];
 int32_t weight[MUS_SCENARIOS];
 int keep;                       /* event whose object under way stays in the plan; -1: none */
 int8_t plan[MUS_EVENTS],best[MUS_EVENTS];
 int64_t value;uint64_t bytes;   /* of `best`; the value in millionths of the quality */
 bool found;
} search;
static uint64_t later(uint64_t a,uint64_t b){return a>b?a:b;}
static int64_t gain(const search *s,const mus_event *e,int rep){
 return (int64_t)s->c->quality[rep]-(e->best>=0?(int64_t)s->c->quality[e->best]:0);
}
/* The object of `bytes` follows what the link carries in every scenario. Returns the
 * weight of the scenarios in which it ends before the deadline. */
static int32_t carry(const search *s,const uint64_t *link,uint64_t ready,uint64_t bytes,uint64_t deadline,uint64_t *end){
 int32_t weight=0;
 for(unsigned j=0;j<MUS_SCENARIOS;j++){
  end[j]=later(link[j],ready)+bytes*1000000ULL/s->usable[j];
  if(s->weight[j]&&end[j]+s->c->guard_us<=deadline)weight+=s->weight[j];
 }
 return weight;
}
/* Depth-first over the events in deadline order. `link` holds the time at which the link
 * is free in every scenario, `codec` the time at which the buffer of the codec is free. */
static void descend(search *s,unsigned k,const uint64_t *link,uint64_t codec,int64_t value,uint64_t bytes){
 if(k==s->n){
  if(!s->found||value>s->value||(value==s->value&&bytes<s->bytes)){
   s->found=true;s->value=value;s->bytes=bytes;
   for(unsigned i=0;i<MUS_EVENTS;i++)s->best[i]=s->plan[i];
  }
  return;
 }
 unsigned i=s->order[k];const mus_event *e=&s->ev[i];
 if(s->keep==(int)i){descend(s,k+1,link,codec,value,bytes);return;}   /* counted before the search */
 s->plan[i]=-1;descend(s,k+1,link,codec,value,bytes);
 for(int r=e->best+1;r<MUS_REPS;r++){
  if(r<0||!e->size[r])continue;
  uint64_t ready=s->now,end[MUS_SCENARIOS];
  bool buffer=e->prepared==r||e->converting==r||s->c->prepare_us[r];
  if(e->converting==r)ready=later(s->now,s->codec_free);
  else if(e->prepared!=r&&s->c->prepare_us[r])ready=(s->c->sequential?later(codec,link[MUS_MIDDLE]):codec)+s->c->prepare_us[r];
  int32_t weight=carry(s,link,ready,e->size[r],e->deadline_us,end);
  if(!weight)continue;
  s->plan[i]=(int8_t)r;
  /* the buffer is held until the link takes the object */
  descend(s,k+1,end,buffer?later(codec,later(link[MUS_MIDDLE],ready)):codec,value+weight*gain(s,e,r),bytes+e->size[r]);
 }
 s->plan[i]=-1;
}
/* `occupied`: bytes that the link carries before the first object of the plan. */
static bool solve(search *s,int keep,uint64_t occupied){
 s->keep=keep;s->found=false;s->value=0;s->bytes=0;
 for(unsigned i=0;i<MUS_EVENTS;i++)s->plan[i]=s->best[i]=-1;
 uint64_t link[MUS_SCENARIOS],codec=later(s->now,s->codec_free);
 int64_t value=0;uint64_t bytes=0;
 for(unsigned j=0;j<MUS_SCENARIOS;j++)link[j]=s->now+occupied*1000000ULL/s->usable[j];
 if(keep>=0){
  const mus_event *e=&s->ev[keep];int r=e->active;
  size_t left=e->acked<e->size[r]?e->size[r]-e->acked:0;
  uint64_t end[MUS_SCENARIOS];
  int32_t weight=carry(s,link,s->now,left,e->deadline_us,end);
  if(!weight)return false;
  for(unsigned j=0;j<MUS_SCENARIOS;j++)link[j]=end[j];
  s->plan[keep]=(int8_t)r;value=weight*gain(s,e,r);bytes=left;
 }
 descend(s,0,link,codec,value,bytes);
 return s->found;
}
/* The representation that the codec should produce now: that of the first planned object
 * that needs one, unless a planned object already occupies the buffer. */
static void prepare(mus_decision *d,const search *s,int active){
 for(unsigned k=0;k<s->n;k++){
  unsigned i=s->order[k];int r=d->plan[i];const mus_event *e=&s->ev[i];
  if(r<0||((int)i==active&&e->active==r)||!s->c->prepare_us[r])continue;
  if(e->prepared!=r&&e->converting!=r){d->prepare_event=(int)i;d->prepare_rep=r;}
  return;
 }
}
mus_decision mus_plan(const mus_config *c,const mus_event *ev,unsigned count,uint64_t now,const mus_link *link,uint64_t last_probe){
 mus_decision d={.action=MUS_IDLE,.event=-1,.rep=-1,.prepare_event=-1,.prepare_rep=-1};
 search s={.c=c,.ev=ev,.now=now,.codec_free=link->codec_free_us};
 if(count>MUS_EVENTS)count=MUS_EVENTS;
 for(unsigned i=0;i<MUS_EVENTS;i++)d.plan[i]=-1;
 for(unsigned j=0;j<MUS_SCENARIOS;j++){
  int64_t rate=(int64_t)link->rate_Bps+(int64_t)link->deviation_Bps*mus_offset[j]/1000;
  if(rate<(int64_t)link->rate_Bps/4)rate=(int64_t)link->rate_Bps/4;
  s.usable[j]=(uint64_t)rate*c->margin_permille/1000;
  if(s.usable[j]<1)s.usable[j]=1;
  s.weight[j]=link->deviation_Bps?mus_weight[j]:j==MUS_MIDDLE?1000:0;
 }
 int active=-1;
 for(unsigned i=0;i<count;i++)if(ev[i].active>=0)active=(int)i;
 /* A probe runs to its end, whatever the plan says. */
 for(unsigned i=0;i<count;i++){
  const mus_event *e=&ev[i];
  if(!e->probe)continue;
  int r=e->active>=0?e->active:e->prepared>=0?e->prepared:e->converting;
  if(r<0)continue;
  d.action=e->active>=0?MUS_CONTINUE:MUS_START;d.event=(int)i;d.rep=r;d.probe=true;
  return d;
 }
 /* open events in deadline order; the search covers the latest MUS_SEARCH and the one under way */
 for(unsigned i=0;i<count;i++){
  if(ev[i].deadline_us<=now&&(int)i!=active)continue;
  unsigned k=s.n++;
  while(k&&ev[s.order[k-1]].deadline_us>ev[i].deadline_us){s.order[k]=s.order[k-1];k--;}
  s.order[k]=i;
 }
 while(s.n>MUS_SEARCH){
  unsigned drop=s.order[0]==(unsigned)active?1:0;
  for(unsigned k=drop;k+1<s.n;k++)s.order[k]=s.order[k+1];
  s.n--;
 }
 if(active>=0){
  /* The object under way stays unless a plan without it gains clearly more. What was
   * sent of it and not acknowledged still occupies the link when it is abandoned. */
  bool with=solve(&s,active,link->drain);
  int64_t kept=s.value;int8_t plan_with[MUS_EVENTS];
  for(unsigned i=0;i<MUS_EVENTS;i++)plan_with[i]=s.best[i];
  solve(&s,-1,(uint64_t)link->drain+ev[active].unacked);
  d.event=active;d.rep=ev[active].active;
  if(with&&kept+(int64_t)c->keep_permille*1000>=s.value){
   for(unsigned i=0;i<MUS_EVENTS;i++)d.plan[i]=plan_with[i];
   d.action=MUS_CONTINUE;
  }else{
   for(unsigned i=0;i<MUS_EVENTS;i++)d.plan[i]=s.best[i];
   d.action=MUS_ABORT;active=-1;
  }
  prepare(&d,&s,active);
  return d;
 }
 solve(&s,-1,link->drain);
 for(unsigned i=0;i<MUS_EVENTS;i++)d.plan[i]=s.best[i];
 for(unsigned k=0;k<s.n;k++){
  unsigned i=s.order[k];
  if(d.plan[i]<0)continue;
  d.action=MUS_START;d.event=(int)i;d.rep=d.plan[i];
  prepare(&d,&s,-1);
  return d;
 }
 /* Nothing fits the estimate, which may be stale after an outage: the smallest
  * representation of the newest event without evidence is sent as a probe. */
 if(!last_probe||now-last_probe>=c->probe_interval_us){
  for(unsigned k=s.n;k-->0;){
   unsigned i=s.order[k];
   if(ev[i].best>=0)continue;
   int r=0;
   while(r<MUS_REPS&&!ev[i].size[r])r++;
   if(r>=MUS_REPS)continue;
   d.action=MUS_START;d.event=(int)i;d.rep=r;d.probe=true;
   if(c->prepare_us[r]&&ev[i].prepared!=r&&ev[i].converting!=r){d.prepare_event=(int)i;d.prepare_rep=r;}
   return d;
  }
 }
 return d;
}
