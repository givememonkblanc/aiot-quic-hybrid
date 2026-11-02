#include "moqt_scheduler.h"
#include <assert.h>
static int older(int a,int b,const moqt_job j[6]){return b<0||j[a].admitted_us<j[b].admitted_us||(j[a].admitted_us==j[b].admitted_us&&a<b);}
/* Project-profile adaptation of draft-ietf-moq-transport-21 section 5.1.2:
 * the AI subscription has higher subscriber priority; importance represents
 * publisher priority within a subscription; preview uses descending Group
 * Order for freshness.  Control streams are handled before this comparator. */
static int draft21_before(int a,int b,const moqt_job j[6]){
 if(b<0)return 1;
 int a_ai=a==3,b_ai=b==3;
 if(a_ai!=b_ai)return a_ai;
 if(j[a].importance!=j[b].importance)return j[a].importance>j[b].importance;
 if(j[a].group_id!=j[b].group_id)return j[a].group_id>j[b].group_id;
 return older(a,b,j);
}
const char *moqt_policy_name(moqt_policy p){static const char *n[]={"fifo","ai_first","drr_bytes","edf","importance_edf","moqt21_priority"};return (unsigned)p<MOQT_POLICY_COUNT?n[p]:"invalid";}
int moqt_sched_pick(moqt_sched *s,moqt_policy p,unsigned ready,const moqt_job j[6],size_t *limit){
 *limit=0;if((unsigned)p>=MOQT_POLICY_COUNT)return -1;
 for(int i=0;i<3;i++)if(ready&(1U<<i)){*limit=j[i].remaining;return i;}
 int ai=(ready&8)?3:-1,video=-1,chosen=-1;
 for(int i=4;i<6;i++)if((ready&(1U<<i))&&older(i,video,j))video=i;
 if(p==MOQT_DRR){
  int track[2]={ai,video};
  if(!j[3].remaining)s->credit[0]=0;
  if(!j[4].remaining&&!j[5].remaining)s->credit[1]=0;
  for(unsigned n=0;n<2;n++){
   unsigned t=s->turn;
   if(track[t]>=0){if(!s->credit[t])s->credit[t]=MOQT_SCHED_QUANTUM;chosen=track[t];*limit=s->credit[t];break;}
   s->turn^=1;
  }
 }else if(p==MOQT_AI_FIRST)chosen=ai>=0?ai:video;
 else for(int i=3;i<6;i++)if(ready&(1U<<i)){
  int take=chosen<0;
   if(!take){
   if(p==MOQT_FIFO)take=older(i,chosen,j);
   else if(p==MOQT_DRAFT21_PRIORITY)take=draft21_before(i,chosen,j);
   else if(p==MOQT_IMPORTANCE_EDF&&j[i].importance!=j[chosen].importance)take=j[i].importance>j[chosen].importance;
   else take=j[i].deadline_us<j[chosen].deadline_us||(j[i].deadline_us==j[chosen].deadline_us&&older(i,chosen,j));
  }
  if(take)chosen=i;
 }
 if(chosen>=0){if(p!=MOQT_DRR)*limit=MOQT_SCHED_QUANTUM;if(*limit>j[chosen].remaining)*limit=j[chosen].remaining;}
 return chosen;
}
void moqt_sched_commit(moqt_sched *s,moqt_policy p,unsigned slot,size_t accepted){
 if(p!=MOQT_DRR||slot<3)return;
 unsigned t=slot==3?0:1;assert(accepted<=s->credit[t]);s->credit[t]-=accepted;
 if(!s->credit[t])s->turn=t^1;
}
