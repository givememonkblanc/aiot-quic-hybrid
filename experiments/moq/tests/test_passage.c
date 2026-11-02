#include "moq_passage.h"
#include <assert.h>
#include <stdio.h>
int main(void){
 moq_passage_counter c;moq_passage_event e[MOQ_PASSAGE_EVENTS];
 moq_passage_cfg cfg={.line=128,.direction=1,.max_step=40,.max_missed=1};
 moq_passage_init(&c,&cfg);
 /* One object moving right across the line is counted once, however often it is seen. */
 uint8_t xs[]={60,90,120,150,180,210};
 unsigned total=0;
 for(unsigned i=0;i<6;i++){
  moq_passage_point p={xs[i],100};
  unsigned n=moq_passage_update(&c,&p,1,i,e);total+=n;
  assert(n==(i==3));
  if(n)assert(e[0].id==1&&e[0].source_sequence==3&&e[0].x==150&&e[0].direction==1);
 }
 assert(total==1&&c.total==1);
 /* Two objects in the same result are kept apart by position. */
 moq_passage_init(&c,&cfg);
 moq_passage_point two[][2]={{{100,50},{110,200}},{{135,52},{120,198}},{{160,55},{140,196}}};
 assert(moq_passage_update(&c,two[0],2,0,e)==0);
 assert(moq_passage_update(&c,two[1],2,1,e)==1&&e[0].y==52);
 assert(moq_passage_update(&c,two[2],2,2,e)==1&&e[0].y==196&&c.total==2);
 /* The opposite direction is not counted when a direction is configured. */
 moq_passage_init(&c,&cfg);
 moq_passage_point back[]={{150,10},{120,10}};
 moq_passage_update(&c,&back[0],1,0,e);assert(moq_passage_update(&c,&back[1],1,1,e)==0&&c.total==0);
 cfg.direction=0;moq_passage_init(&c,&cfg);
 moq_passage_update(&c,&back[0],1,0,e);assert(moq_passage_update(&c,&back[1],1,1,e)==1&&e[0].direction==-1);
 /* A missed detection is tolerated once; the object is still counted once. */
 cfg.direction=1;moq_passage_init(&c,&cfg);
 moq_passage_point a={110,80},b={140,80};
 moq_passage_update(&c,&a,1,0,e);
 assert(moq_passage_update(&c,NULL,0,1,e)==0);
 assert(moq_passage_update(&c,&b,1,2,e)==1&&c.total==1);
 /* After two misses the track is gone: the same object reappearing past the line is a
  * new track and is not counted. This is a known miss of the method. */
 moq_passage_init(&c,&cfg);
 moq_passage_update(&c,&a,1,0,e);moq_passage_update(&c,NULL,0,1,e);moq_passage_update(&c,NULL,0,2,e);
 assert(moq_passage_update(&c,&b,1,3,e)==0&&c.total==0);
 /* A jump larger than max_step is treated as another object. */
 moq_passage_init(&c,&cfg);
 moq_passage_point far1={100,80},far2={170,80};
 moq_passage_update(&c,&far1,1,0,e);assert(moq_passage_update(&c,&far2,1,1,e)==0);
 /* An object that returns and crosses again is a second passage. */
 moq_passage_init(&c,&cfg);
 uint8_t loop[]={110,140,110,140};unsigned n=0;
 for(unsigned i=0;i<4;i++){moq_passage_point p={loop[i],9};n+=moq_passage_update(&c,&p,1,i,e);}
 assert(n==2&&c.total==2);
 /* More detections than tracks are reported, not silently merged. */
 moq_passage_init(&c,&cfg);
 moq_passage_point many[30];
 for(unsigned i=0;i<30;i++)many[i]=(moq_passage_point){(uint8_t)(i*3),(uint8_t)(i*8)};
 moq_passage_update(&c,many,30,0,e);assert(c.dropped_detections==30-MOQ_PASSAGE_TRACKS);
 /* Vertical counting line. */
 cfg=(moq_passage_cfg){.line=100,.vertical_axis=true,.direction=-1,.max_step=50,.max_missed=1};
 moq_passage_init(&c,&cfg);
 moq_passage_point v1={10,120},v2={12,90};
 moq_passage_update(&c,&v1,1,0,e);assert(moq_passage_update(&c,&v2,1,1,e)==1);
 /* More than MOQ_PASSAGE_EVENTS crossings in one result are all counted. */
 cfg=(moq_passage_cfg){.line=128,.direction=1,.max_step=40,.max_missed=1};moq_passage_init(&c,&cfg);
 moq_passage_point l[12],r[12];
 for(unsigned i=0;i<12;i++){l[i]=(moq_passage_point){110,(uint8_t)(i*20)};r[i]=(moq_passage_point){140,(uint8_t)(i*20)};}
 moq_passage_update(&c,l,12,0,e);assert(moq_passage_update(&c,r,12,1,e)==12&&c.total==12&&e[7].id==8);
 /* One direction of flow: objects 64 apart that move 32 per result stay apart. Nearest
  * association would hand each detection to the track of the object behind it. */
 cfg=(moq_passage_cfg){.line=128,.direction=1,.max_step=48,.max_missed=2,.predict=true};moq_passage_init(&c,&cfg);
 unsigned counted=0;
 for(unsigned t=0;t<18;t++){
  moq_passage_point q[8];unsigned used=0;
  for(unsigned k=0;k<6;k++){int x=(int)t*32-(int)k*64;if(x>=0&&x<=255)q[used++]=(moq_passage_point){(uint8_t)x,100};}
  counted+=moq_passage_update(&c,q,used,t,e);
 }
 assert(counted==6&&c.total==6);
 /* With prediction a track is found again after two missed results, 96 units ahead. */
 moq_passage_init(&c,&cfg);
 moq_passage_point s0={40,60},s1={72,60},s4={168,60};
 moq_passage_update(&c,&s0,1,0,e);moq_passage_update(&c,&s1,1,1,e);
 moq_passage_update(&c,NULL,0,2,e);moq_passage_update(&c,NULL,0,3,e);
 assert(moq_passage_update(&c,&s4,1,4,e)==1&&c.total==1);
 /* It does not take a detection behind the track. */
 moq_passage_init(&c,&cfg);
 moq_passage_point f0={100,60},f1={132,60},behind={90,60};
 moq_passage_update(&c,&f0,1,0,e);assert(moq_passage_update(&c,&f1,1,1,e)==1);
 assert(moq_passage_update(&c,&behind,1,2,e)==0&&c.total==1);
 /* An object first seen less than one step past the line crossed it unseen. */
 cfg.count_unseen=true;moq_passage_init(&c,&cfg);
 moq_passage_point u0={140,30},u1={172,30},late={250,200};
 assert(moq_passage_update(&c,&u0,1,0,e)==1&&e[0].x==140);
 assert(moq_passage_update(&c,&u1,1,1,e)==0&&c.total==1);
 assert(moq_passage_update(&c,&late,1,2,e)==0&&c.total==1); /* far past the line: not counted */
 /* Settled: at the start the flow is not known and an object just past the line is not
  * counted; once an object has moved, a new one just past the line is. */
 cfg.settled=true;moq_passage_init(&c,&cfg);
 moq_passage_point g0={140,30},m0={60,120},m1={80,120},g1={136,200};
 assert(moq_passage_update(&c,&g0,1,0,e)==0&&c.total==0);
 moq_passage_update(&c,&m0,1,1,e);moq_passage_update(&c,&m1,1,2,e);
 assert(moq_passage_update(&c,&g1,1,3,e)==1&&c.total==1);
 puts("PASS: passage events: single count per object, separation, direction, misses, re-entry, capacity, flow prediction, unseen crossing, settled start");
 return 0;
}
