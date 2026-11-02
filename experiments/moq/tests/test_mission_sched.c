#include "moq_mission_sched.h"
#include <assert.h>
#include <stdio.h>
#define S 1000000ULL
static const mus_config cfg={.quality={400,700,1000},.prepare_us={0,0,0},.margin_permille=800,.guard_us=100000,
                             .probe_interval_us=2000000,.keep_permille=50};
static mus_event event(uint32_t seq,uint64_t deadline){
 return (mus_event){.seq=seq,.deadline_us=deadline,.size={6754,21253,87454},.best=-1,.active=-1,.prepared=-1,.converting=-1};
}
static mus_decision plan(const mus_config *c,const mus_event *e,unsigned n,uint64_t now,uint64_t rate,uint64_t probe){
 mus_link link={.rate_Bps=rate,.codec_free_us=now};
 return mus_plan(c,e,n,now,&link,probe);
}
int main(void){
 mus_event e[4];mus_decision d;
 /* A wide link: the highest representation is planned and started. */
 e[0]=event(1,10*S);
 d=plan(&cfg,e,1,0,64000,0);
 assert(d.action==MUS_START&&d.event==0&&d.rep==2&&!d.probe&&d.plan[0]==2&&d.prepare_event<0);
 /* 12 kB/s usable for 10 s carries 96 kB with the margin: high fits. At 8 kB/s it does not. */
 d=plan(&cfg,e,1,0,15000,0);assert(d.rep==2);
 d=plan(&cfg,e,1,0,8000,0);assert(d.action==MUS_START&&d.rep==1);
 d=plan(&cfg,e,1,0,1500,0);assert(d.action==MUS_START&&d.rep==0);
 /* Nothing fits: a probe with the smallest representation, then none until the interval passed. */
 d=plan(&cfg,e,1,0,300,0);assert(d.action==MUS_START&&d.rep==0&&d.probe);
 d=plan(&cfg,e,1,1*S,300,1);assert(d.action==MUS_IDLE);
 d=plan(&cfg,e,1,3*S,300,1*S);assert(d.action==MUS_START&&d.probe);
 /* Two events share the link: the plan with the largest sum of quality is chosen. */
 e[0]=event(1,6*S);e[1]=event(2,9*S);
 d=plan(&cfg,e,2,0,8000,0);
 assert(d.plan[0]==1&&d.plan[1]==1&&d.action==MUS_START&&d.event==0);  /* 21 kB and 21 kB at 6.4 kB/s: 0.7+0.7 */
 /* When the smallest representation is worth little, one event is served well and the
  * other receives nothing: 1.0 exceeds 0.1+0.5 and 0.5+0.5 does not fit. */
 mus_config steep=cfg;steep.quality[0]=100;steep.quality[1]=500;
 e[0]=event(1,9*S);e[1]=event(2,10*S);
 d=plan(&steep,e,2,0,13000,0);
 assert(d.action==MUS_START&&((d.plan[0]==2&&d.plan[1]<=0)||(d.plan[1]==2&&d.plan[0]<=0)));
 /* The earlier deadline is served first whatever the order of the array. */
 e[0]=event(2,9*S);e[1]=event(1,6*S);
 d=plan(&cfg,e,2,0,8000,0);assert(d.event==1);
 /* An object under way continues while it can finish, and its acknowledged bytes count. */
 e[0]=event(1,10*S);e[0].active=2;e[0].acked=60000;
 d=plan(&cfg,e,1,6*S,10000,0);assert(d.action==MUS_CONTINUE&&d.rep==2);
 /* It is abandoned when it cannot finish; the next call starts a smaller one. */
 e[0].acked=10000;
 d=plan(&cfg,e,1,6*S,10000,0);assert(d.action==MUS_ABORT&&d.event==0&&d.plan[0]==1);
 /* What is in the network of an abandoned object delays its replacement: with 16 kB
  * unacknowledged the medium representation no longer fits and the low one is planned. */
 e[0].unacked=16000;
 d=plan(&cfg,e,1,6*S,10000,0);assert(d.action==MUS_ABORT&&d.plan[0]==0);
 e[0]=event(1,10*S);
 {mus_link busy={.rate_Bps=10000,.drain=16000,.codec_free_us=6*S};
  d=mus_plan(&cfg,e,1,6*S,&busy,0);assert(d.action==MUS_START&&d.rep==0);
  busy.drain=0;d=mus_plan(&cfg,e,1,6*S,&busy,0);assert(d.rep==1);}
 /* It is not abandoned for a small gain of another plan. */
 e[0]=event(1,10*S);e[0].active=1;e[0].acked=1000;
 d=plan(&cfg,e,1,0,64000,0);assert(d.action==MUS_ABORT);      /* high fits and gains 0.3 */
 mus_config sticky=cfg;sticky.keep_permille=400;
 d=plan(&sticky,e,1,0,64000,0);assert(d.action==MUS_CONTINUE);
 e[0].active=-1;e[0].acked=0;
 d=plan(&cfg,e,1,6*S,10000,0);assert(d.action==MUS_START&&d.rep==1);
 /* A probe is never abandoned, and a probe that is being prepared is the next object. */
 e[0]=event(1,10*S);e[0].active=0;e[0].probe=true;
 d=plan(&cfg,e,1,9*S,100,0);assert(d.action==MUS_CONTINUE&&d.probe);
 e[0].active=-1;e[0].converting=0;
 d=plan(&cfg,e,1,9*S,100,0);assert(d.action==MUS_START&&d.probe&&d.rep==0&&d.prepare_event<0);
 /* Delivered evidence is upgraded only with a better representation. */
 e[0]=event(1,10*S);e[0].best=1;
 d=plan(&cfg,e,1,0,64000,0);assert(d.action==MUS_START&&d.rep==2);
 d=plan(&cfg,e,1,0,8000,0);assert(d.action==MUS_IDLE);
 e[0].best=2;d=plan(&cfg,e,1,0,64000,0);assert(d.action==MUS_IDLE);
 /* Events past their deadline are left alone, and the time to prepare a representation counts. */
 e[0]=event(1,5*S);
 d=plan(&cfg,e,1,6*S,64000,0);assert(d.action==MUS_IDLE);
 mus_config slow=cfg;slow.prepare_us[1]=3000000;slow.prepare_us[0]=3000000;
 e[0]=event(1,4*S);
 d=plan(&slow,e,1,0,64000,0);assert(d.action==MUS_START&&d.rep==2); /* the source itself needs no preparation */
 e[0].size[2]=0;
 d=plan(&slow,e,1,0,64000,0);assert(d.action==MUS_START&&d.rep==1&&d.prepare_event==0&&d.prepare_rep==1);
 d=plan(&slow,e,1,2*S,64000,0);assert(d.action==MUS_START&&d.probe&&d.prepare_event==0); /* 3 s of preparation no longer fit */
 /* A prepared representation needs no preparation any more. */
 e[0].prepared=1;
 d=plan(&slow,e,1,2*S,64000,0);assert(d.action==MUS_START&&!d.probe&&d.rep==1&&d.prepare_event<0);
 /* The next representation is prepared while an object is under way. The medium one of
  * the second event is ready when the link becomes free; prepared one after the other,
  * the two would not fit. */
 e[0]=event(1,6*S);e[0].active=1;e[0].acked=0;e[1]=event(2,9*S);e[1].size[2]=0;
 d=plan(&slow,e,2,0,8000,0);   /* 21 kB at 6.4 kB/s: 3.3 s each; conversion 3 s */
 assert(d.action==MUS_CONTINUE&&d.plan[1]==1&&d.prepare_event==1&&d.prepare_rep==1);
 e[1].converting=1;
 {mus_link l={.rate_Bps=8000,.codec_free_us=3*S};
  d=mus_plan(&slow,e,2,1*S,&l,0);assert(d.action==MUS_CONTINUE&&d.plan[1]==1&&d.prepare_event<0);}
 /* One buffer: the representation of a third event waits until the link took the second. */
 e[1].converting=-1;e[1].prepared=1;e[2]=event(3,20*S);e[2].size[2]=0;
 d=plan(&slow,e,3,3*S,8000,0);assert(d.plan[1]==1&&d.plan[2]==1&&d.prepare_event<0);
 /* The deviation of the rate: at 10 kB/s the high representation needs 8.7 s of 9.9 s.
  * With a small deviation it is planned. With a large one it arrives in three of five
  * scenarios, 0.66 of its quality, which exceeds the certain 0.5 of the medium one; when
  * the medium one is worth 0.7 it is preferred. */
 {mus_config one=cfg;one.margin_permille=1000;one.quality[0]=100;one.quality[1]=500;
  mus_link l={.rate_Bps=10000,.deviation_Bps=300,.codec_free_us=0};
  e[0]=event(1,10*S);
  d=mus_plan(&one,e,1,0,&l,0);assert(d.action==MUS_START&&d.rep==2);
  l.deviation_Bps=2000;
  d=mus_plan(&one,e,1,0,&l,0);assert(d.rep==2);
  one.quality[1]=700;
  d=mus_plan(&one,e,1,0,&l,0);assert(d.rep==1);
  /* An object under way that arrives in few scenarios only is abandoned for a certain one. */
  e[0].active=2;e[0].acked=20000;e[0].unacked=8000;
  l.rate_Bps=8000;l.deviation_Bps=800;
  d=mus_plan(&one,e,1,2*S,&l,0);assert(d.action==MUS_ABORT&&d.plan[0]==1);
  e[0].acked=30000;
  d=mus_plan(&one,e,1,2*S,&l,0);assert(d.action==MUS_CONTINUE);}
 /* Unavailable representations are skipped. */
 e[0]=event(1,10*S);e[0].size[0]=0;
 d=plan(&cfg,e,1,0,1500,0);assert(d.action==MUS_START&&d.probe&&d.rep==1);
 puts("PASS: mission scheduler: representation by deadline and rate, upgrades, order, abort and its cost, probe, preparation ahead, deviation of the rate");
 return 0;
}
