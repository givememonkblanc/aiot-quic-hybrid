#include "moqt_scheduler.h"
#include <assert.h>
#include <stdio.h>
int main(void){
 moqt_job j[6]={{0}};moqt_sched s={0};size_t n;
 j[3]=(moqt_job){.admitted_us=20,.deadline_us=80,.group_id=7,.remaining=150,.importance=2};
 j[4]=(moqt_job){.admitted_us=10,.deadline_us=100,.group_id=8,.remaining=9000,.importance=0};
 j[5]=(moqt_job){.admitted_us=30,.deadline_us=40,.group_id=9,.remaining=9000,.importance=0};
 assert(moqt_sched_pick(&s,MOQT_FIFO,0x38,j,&n)==4&&n==1024);
 assert(moqt_sched_pick(&s,MOQT_AI_FIRST,0x38,j,&n)==3);
 assert(moqt_sched_pick(&s,MOQT_EDF,0x38,j,&n)==5);
 assert(moqt_sched_pick(&s,MOQT_IMPORTANCE_EDF,0x38,j,&n)==3);
 assert(moqt_sched_pick(&s,MOQT_DRAFT21_PRIORITY,0x38,j,&n)==3); /* subscriber priority */
 assert(moqt_sched_pick(&s,MOQT_DRAFT21_PRIORITY,0x30,j,&n)==5); /* descending group */
 assert(moqt_sched_pick(&s,MOQT_EDF,0x3a,j,&n)==1); /* control always progresses */
 assert(moqt_sched_pick(&s,MOQT_FIFO,0,j,&n)==-1);
 /* Failed writes spend nothing; a partial write only spends accepted bytes. */
 assert(moqt_sched_pick(&s,MOQT_DRR,0x18,j,&n)==3&&n==150);
 moqt_sched_commit(&s,MOQT_DRR,3,0);assert(s.credit[0]==1024);
 moqt_sched_commit(&s,MOQT_DRR,3,50);assert(s.credit[0]==974);
 assert(moqt_sched_pick(&s,MOQT_DRR,0x10,j,&n)==4&&s.credit[0]==974);
 j[3].remaining=0;moqt_sched_pick(&s,MOQT_DRR,0x10,j,&n);assert(s.credit[0]==0);
 /* Both tracks continuously backlogged: 1:1 byte service, unequal object sizes. */
 s=(moqt_sched){0};j[3].remaining=100000;size_t sent[2]={0};
 for(int k=0;k<200;k++){int i=moqt_sched_pick(&s,MOQT_DRR,0x18,j,&n);assert(n>0&&n<=1024);size_t accepted=k%2?n:(n<512?n:512);moqt_sched_commit(&s,MOQT_DRR,i,accepted);sent[i==3?0:1]+=accepted;}
 assert(sent[0]>0&&sent[1]>0);assert(sent[0]>sent[1]?sent[0]-sent[1]<=1024:sent[1]-sent[0]<=1024);
 /* Blocked AI is excluded, so preview does not starve. */
 assert(moqt_sched_pick(&s,MOQT_DRR,0x10,j,&n)==4);
 /* Wire priorities, section 5.1.2 (checklist F4, F5). */
 moqt_wire_job w[8]={
  {.klass=MOQT_CLASS_OBJECT,.subscriber_priority=128,.publisher_priority=192,.group_order=2,.subscription=1,.group=7,.admitted_us=1},
  {.klass=MOQT_CLASS_OBJECT,.subscriber_priority=128,.publisher_priority=192,.group_order=2,.subscription=1,.group=9,.admitted_us=2},
  {.klass=MOQT_CLASS_OBJECT,.subscriber_priority=128,.publisher_priority=64,.group_order=1,.subscription=2,.group=3,.admitted_us=3},
  {.klass=MOQT_CLASS_OBJECT,.subscriber_priority=200,.publisher_priority=0,.group_order=1,.subscription=3,.group=1,.admitted_us=0},
  {.klass=MOQT_CLASS_REQUEST,.subscriber_priority=128,.admitted_us=9},
  {.klass=MOQT_CLASS_CONTROL,.subscriber_priority=255,.admitted_us=10},
  {.klass=MOQT_CLASS_OBJECT,.subscriber_priority=128,.publisher_priority=192,.group_order=2,.subscription=1,.group=9,.subgroup=1,.admitted_us=0},
  {.klass=MOQT_CLASS_OBJECT,.subscriber_priority=128,.publisher_priority=192,.group_order=2,.subscription=1,.group=9,.fill=true,.admitted_us=5},
 };
 assert(moqt_wire_pick(w,8,0xff)==5);  /* control */
 assert(moqt_wire_pick(w,8,0xdf)==4);  /* request streams before Objects */
 assert(moqt_wire_pick(w,8,0xcf)==2);  /* publisher priority 64 before 192 */
 assert(moqt_wire_pick(w,8,0x0b)==1);  /* subscriber priority 128 before 200; descending group 9 */
 assert(moqt_wire_pick(w,8,0x08)==3);
 assert(moqt_wire_pick(w,8,0x42)==1);  /* same group: lower Subgroup ID */
 assert(moqt_wire_pick(w,8,0x82)==7);  /* same group: fill-delivered first */
 w[7].group=7;w[7].fill_order_differs=true;
 assert(moqt_wire_pick(w,8,0x82)==1);  /* a fill with another Group Order yields */
 w[0].group_order=w[1].group_order=1;
 assert(moqt_wire_pick(w,8,0x03)==0);  /* ascending */
 w[0].datagram=true;w[0].group=9;w[0].object=4;w[1].group_order=2;
 assert(moqt_wire_pick(w,8,0x03)==0);  /* datagram before a subgroup of the same group */
 w[2].publisher_priority=192;
 assert(moqt_wire_pick(w,8,0x06)==1);  /* different subscriptions, equal priorities: older first */
 assert(moqt_wire_pick(w,8,0)==-1);
 puts("PASS scheduler ordering, control progress, partial/failed writes, byte fairness, blocked track");
}
