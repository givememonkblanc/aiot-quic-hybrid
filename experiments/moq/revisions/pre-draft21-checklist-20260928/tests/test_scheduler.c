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
 puts("PASS scheduler ordering, control progress, partial/failed writes, byte fairness, blocked track");
}
