#include "moq_passage.h"
#include <string.h>
void moq_passage_init(moq_passage_counter *c,const moq_passage_cfg *cfg){
 memset(c,0,sizeof(*c));c->cfg=*cfg;
}
static int axial(const moq_passage_counter *c,int x,int y){return c->cfg.vertical_axis?y:x;}
static int lateral(const moq_passage_counter *c,int x,int y){return c->cfg.vertical_axis?x:y;}
static int magnitude(int v){return v<0?-v:v;}
static int side(const moq_passage_counter *c,uint8_t x,uint8_t y){
 return axial(c,x,y)>=c->cfg.line?1:-1;
}
/* Cost of giving detection p to track t, or -1 when p is outside the gate of t. */
static int cost(const moq_passage_counter *c,unsigned t,const moq_passage_point *p){
 int ta=axial(c,c->tracks[t].x,c->tracks[t].y),tl=lateral(c,c->tracks[t].x,c->tracks[t].y);
 int pa=axial(c,p->x,p->y),pl=lateral(c,p->x,p->y);
 int steps=c->tracks[t].missed+1,da=pa-ta,dl=magnitude(pl-tl);
 if(!c->cfg.predict||!c->cfg.direction){
  int d=magnitude(da)>dl?magnitude(da):dl; /* Chebyshev distance: cheap and adequate for a gate */
  return d<=c->cfg.max_step?d:-1;
 }
 /* One direction of flow: the object is ahead of where it was seen, at most max_step per
  * result, and close to where its velocity puts it. */
 int forward=da*c->cfg.direction,tolerance=c->cfg.max_step/4;
 if(forward<-tolerance||forward>c->cfg.max_step*steps||dl>c->cfg.max_step/2)return -1;
 bool known=c->tracks[t].moving||c->has_flow;
 int v=c->tracks[t].moving?c->tracks[t].v:c->flow;
 int off=known?magnitude(forward-v*steps):0;
 if(known&&off>c->cfg.max_step/2+tolerance*(steps-1))return -1;
 return off+dl;
}
static void count(moq_passage_counter *c,unsigned t,const moq_passage_point *p,int8_t direction,
                  uint32_t source_sequence,moq_passage_event *events,unsigned *produced){
 c->total++;c->tracks[t].counted=true;
 if(events&&*produced<MOQ_PASSAGE_EVENTS)
  events[*produced]=(moq_passage_event){c->total,source_sequence,p->x,p->y,direction};
 (*produced)++;
}
unsigned moq_passage_update(moq_passage_counter *c,const moq_passage_point *points,unsigned count_in,
                            uint32_t source_sequence,moq_passage_event *events){
 bool track_taken[MOQ_PASSAGE_TRACKS]={false},point_taken[64]={false};
 unsigned produced=0,count_points=count_in>64?64:count_in;
 /* Greedy association: repeatedly take the cheapest unmatched track and detection pair. */
 for(;;){
  int best=-1,bt=-1,bp=-1;
  for(unsigned t=0;t<MOQ_PASSAGE_TRACKS;t++){
   if(!c->tracks[t].used||track_taken[t])continue;
   for(unsigned p=0;p<count_points;p++){
    if(point_taken[p])continue;
    int d=cost(c,t,&points[p]);
    if(d>=0&&(best<0||d<best)){best=d;bt=(int)t;bp=(int)p;}
   }
  }
  if(bt<0)break;
  track_taken[bt]=point_taken[bp]=true;
  int before=side(c,c->tracks[bt].x,c->tracks[bt].y),after=side(c,points[bp].x,points[bp].y);
  if(before!=after){
   int8_t direction=(int8_t)after;
   if(!c->cfg.direction||c->cfg.direction==direction){
    /* A track counts once per crossing in the counted direction. */
    if(!c->tracks[bt].counted)count(c,(unsigned)bt,&points[bp],direction,source_sequence,events,&produced);
   }else c->tracks[bt].counted=false; /* it went back: a later crossing counts again */
  }
  if(c->cfg.predict&&c->cfg.direction){
   int steps=c->tracks[bt].missed+1;
   int v=(axial(c,points[bp].x,points[bp].y)-axial(c,c->tracks[bt].x,c->tracks[bt].y))*c->cfg.direction/steps;
   c->tracks[bt].v=(int16_t)(c->tracks[bt].moving?(c->tracks[bt].v+v)/2:v);c->tracks[bt].moving=true;
   c->flow=(int16_t)(c->has_flow?(3*c->flow+v)/4:v);c->has_flow=true;
  }
  c->tracks[bt].x=points[bp].x;c->tracks[bt].y=points[bp].y;c->tracks[bt].missed=0;
 }
 for(unsigned t=0;t<MOQ_PASSAGE_TRACKS;t++){
  if(!c->tracks[t].used||track_taken[t])continue;
  if(++c->tracks[t].missed>c->cfg.max_missed)c->tracks[t].used=false;
 }
 for(unsigned p=0;p<count_points;p++){
  if(point_taken[p])continue;
  int free_track=-1;
  for(unsigned t=0;t<MOQ_PASSAGE_TRACKS;t++)if(!c->tracks[t].used){free_track=(int)t;break;}
  if(free_track<0){c->dropped_detections++;continue;}
  memset(&c->tracks[free_track],0,sizeof(c->tracks[free_track]));
  c->tracks[free_track].used=true;
  c->tracks[free_track].x=points[p].x;c->tracks[free_track].y=points[p].y;
  if(c->cfg.count_unseen&&c->cfg.direction&&(c->has_flow||!c->cfg.settled)){
   /* Less than one step past the line and unknown before: it crossed between two results. */
   int past=(axial(c,points[p].x,points[p].y)-c->cfg.line)*c->cfg.direction;
   int step=c->has_flow?c->flow:c->cfg.max_step;
   if(past>=0&&past<step)count(c,(unsigned)free_track,&points[p],c->cfg.direction,source_sequence,events,&produced);
  }
 }
 return produced;
}

/* ---- assoc 1: minimum-cost assignment, 2-D velocity per second, coasting ----
 * The reference is experiments/moq/passage_assoc.py; both use the same integer arithmetic and
 * give the same events. */
#define MOQ_PASSAGE_POINTS 24
#define MOQ_BIG (1<<20)
static int32_t cdiv(int64_t a,int64_t b){return (int32_t)(a/b);} /* C division truncates toward zero */
static int32_t absi(int32_t v){return v<0?-v:v;}
/* Residual of p from where track t is predicted at now, or -1 outside the gate. */
static int32_t cost_at(const moq_passage_counter *c,unsigned t,const moq_passage_point *p,uint32_t now){
 int32_t dt=(int32_t)(now-c->tracks[t].ms);
 int ta=axial(c,c->tracks[t].x,c->tracks[t].y),tl=lateral(c,c->tracks[t].x,c->tracks[t].y);
 int pa=axial(c,p->x,p->y),pl=lateral(c,p->x,p->y),tol=c->cfg.max_step/4;
 if(!c->cfg.predict||!c->cfg.direction){
  int d=magnitude(pa-ta)>magnitude(pl-tl)?magnitude(pa-ta):magnitude(pl-tl);
  return d<=c->cfg.max_step?d:-1;
 }
 int forward=(pa-ta)*c->cfg.direction;
 int steps=(c->tracks[t].missed<c->cfg.max_missed?c->tracks[t].missed:c->cfg.max_missed)+1;
 if(forward<-tol||forward>c->cfg.max_step*steps)return -1;
 int32_t va,vl;
 if(c->tracks[t].moving){va=c->tracks[t].va;vl=c->tracks[t].vl;}
 else if(c->has_flow){va=c->flow_a;vl=c->flow_l;}
 else{int dl=magnitude(pl-tl);return dl<=c->cfg.max_step/2?dl:-1;}
 int32_t ea=absi(pa-(ta+cdiv((int64_t)va*dt,16000)*c->cfg.direction));
 int32_t el=absi(pl-(tl+cdiv((int64_t)vl*dt,16000)));
 int32_t gate=c->cfg.max_step/2+tol*(steps-1);
 if(ea>gate||el>gate)return -1;
 return ea+el;
}
/* Minimum-cost assignment of n rows to m columns, n <= m <= MOQ_PASSAGE_POINTS (Hungarian
 * method with potentials, O(n^2 m)). row[i] is the column of row i. */
static void hungarian(const int32_t cost[][MOQ_PASSAGE_POINTS],int n,int m,int row[]){
 const int32_t inf=1<<30;
 int32_t u[MOQ_PASSAGE_POINTS+1]={0},v[MOQ_PASSAGE_POINTS+1]={0},minv[MOQ_PASSAGE_POINTS+1];
 int p[MOQ_PASSAGE_POINTS+1]={0},way[MOQ_PASSAGE_POINTS+1]={0};bool used[MOQ_PASSAGE_POINTS+1];
 for(int i=1;i<=n;i++){
  p[0]=i;int j0=0;
  for(int j=0;j<=m;j++){minv[j]=inf;used[j]=false;}
  do{
   used[j0]=true;int i0=p[j0],j1=0;int32_t delta=inf;
   for(int j=1;j<=m;j++)if(!used[j]){
    int32_t cur=cost[i0-1][j-1]-u[i0]-v[j];
    if(cur<minv[j]){minv[j]=cur;way[j]=j0;}
    if(minv[j]<delta){delta=minv[j];j1=j;}
   }
   for(int j=0;j<=m;j++){if(used[j]){u[p[j]]+=delta;v[j]-=delta;}else minv[j]-=delta;}
   j0=j1;
  }while(p[j0]!=0);
  do{int j1=way[j0];p[j0]=p[j1];j0=j1;}while(j0);
 }
 for(int i=0;i<n;i++)row[i]=-1;
 for(int j=1;j<=m;j++)if(p[j])row[p[j]-1]=j-1;
}
/* Time since the object of track t, counted at p, crossed the line (ms): its travel past the line
 * over its speed along the axis; 0 when the speed is not known. */
static int32_t crossed_ago(const moq_passage_counter *c,unsigned t,const moq_passage_point *p){
 int32_t past=absi(axial(c,p->x,p->y)-c->cfg.line);
 int32_t v=absi(c->tracks[t].moving?c->tracks[t].va:c->flow_a);
 return v>0?cdiv((int64_t)past*16000,v):0;
}
/* The events of one update in the order of crossing, earliest first (stable), IDs in that order. */
static void order_events(moq_passage_event *events,const int32_t *ago,unsigned produced){
 unsigned n=produced<MOQ_PASSAGE_EVENTS?produced:MOQ_PASSAGE_EVENTS;
 if(n<2)return;
 uint32_t base=events[0].id;
 moq_passage_event e[MOQ_PASSAGE_EVENTS];int32_t k[MOQ_PASSAGE_EVENTS];
 for(unsigned i=0;i<n;i++){
  unsigned j=i;
  while(j>0&&k[j-1]<ago[i]){e[j]=e[j-1];k[j]=k[j-1];j--;}
  e[j]=events[i];k[j]=ago[i];
 }
 for(unsigned i=0;i<n;i++){events[i]=e[i];events[i].id=base+i;}
}
static unsigned update_assoc(moq_passage_counter *c,const moq_passage_point *points,unsigned count_in,
                             uint32_t source_sequence,uint32_t now,moq_passage_event *events){
 static int32_t cost[MOQ_PASSAGE_POINTS][MOQ_PASSAGE_POINTS];
 int rows[MOQ_PASSAGE_TRACKS],assign[MOQ_PASSAGE_POINTS],match[MOQ_PASSAGE_TRACKS];
 bool taken[MOQ_PASSAGE_POINTS]={false};
 int32_t ago[MOQ_PASSAGE_EVENTS]={0};
 unsigned produced=0,m=count_in>MOQ_PASSAGE_POINTS?MOQ_PASSAGE_POINTS:count_in,n=0;
 for(unsigned t=0;t<MOQ_PASSAGE_TRACKS;t++){match[t]=-1;if(c->tracks[t].used)rows[n++]=(int)t;}
 if(n&&m){
  if(n<=m){
   for(unsigned r=0;r<n;r++)for(unsigned q=0;q<m;q++){int32_t d=cost_at(c,(unsigned)rows[r],&points[q],now);cost[r][q]=d>=0?d:MOQ_BIG;}
   hungarian((const int32_t (*)[MOQ_PASSAGE_POINTS])cost,(int)n,(int)m,assign);
   for(unsigned r=0;r<n;r++)if(assign[r]>=0&&cost[r][assign[r]]<MOQ_BIG)match[rows[r]]=assign[r];
  }else{
   for(unsigned q=0;q<m;q++)for(unsigned r=0;r<n;r++){int32_t d=cost_at(c,(unsigned)rows[r],&points[q],now);cost[q][r]=d>=0?d:MOQ_BIG;}
   hungarian((const int32_t (*)[MOQ_PASSAGE_POINTS])cost,(int)m,(int)n,assign);
   for(unsigned q=0;q<m;q++)if(assign[q]>=0&&cost[q][assign[q]]<MOQ_BIG)match[rows[assign[q]]]=(int)q;
  }
 }
 for(unsigned t=0;t<MOQ_PASSAGE_TRACKS;t++){
  if(match[t]<0)continue;
  const moq_passage_point *p=&points[match[t]];taken[match[t]]=true;
  int before=side(c,c->tracks[t].x,c->tracks[t].y),after=side(c,p->x,p->y);
  if(before!=after){
   int8_t direction=(int8_t)after;
   if(!c->cfg.direction||c->cfg.direction==direction){
    if(!c->tracks[t].counted){
     count(c,t,p,direction,source_sequence,events,&produced);
     if(produced<=MOQ_PASSAGE_EVENTS)ago[produced-1]=crossed_ago(c,t,p);
    }
   }else c->tracks[t].counted=false;
  }
  int32_t dt=(int32_t)(now-c->tracks[t].ms);
  if(c->cfg.predict&&c->cfg.direction&&dt>0){
   int32_t va=cdiv((int64_t)(axial(c,p->x,p->y)-axial(c,c->tracks[t].x,c->tracks[t].y))*c->cfg.direction*16000,dt);
   int32_t vl=cdiv((int64_t)(lateral(c,p->x,p->y)-lateral(c,c->tracks[t].x,c->tracks[t].y))*16000,dt);
   if(c->tracks[t].moving){c->tracks[t].va=cdiv((int64_t)c->tracks[t].va+va,2);c->tracks[t].vl=cdiv((int64_t)c->tracks[t].vl+vl,2);}
   else{c->tracks[t].va=va;c->tracks[t].vl=vl;}
   c->tracks[t].moving=true;c->tracks[t].v=(int16_t)(c->tracks[t].va/16);
   if(c->has_flow){c->flow_a=cdiv(3*(int64_t)c->flow_a+va,4);c->flow_l=cdiv(3*(int64_t)c->flow_l+vl,4);}
   else{c->flow_a=va;c->flow_l=vl;}
   c->has_flow=true;c->flow=(int16_t)(c->flow_a/16);
  }
  c->tracks[t].x=p->x;c->tracks[t].y=p->y;c->tracks[t].missed=0;c->tracks[t].ms=now;
 }
 for(unsigned t=0;t<MOQ_PASSAGE_TRACKS;t++){
  if(!c->tracks[t].used||match[t]>=0)continue;
  if(c->tracks[t].missed<255)c->tracks[t].missed++;
  if(c->tracks[t].missed>c->cfg.max_missed){
   /* coasting: kept while its predicted travel since it was seen is within one gate */
   int32_t va=c->tracks[t].moving?c->tracks[t].va:c->flow_a;
   int32_t travel=absi(cdiv((int64_t)va*(int32_t)(now-c->tracks[t].ms),16000));
   if(!(c->tracks[t].moving||c->has_flow)||travel>c->cfg.max_step)c->tracks[t].used=false;
  }
 }
 for(unsigned q=0;q<m;q++){
  if(taken[q])continue;
  int free_track=-1;
  for(unsigned t=0;t<MOQ_PASSAGE_TRACKS;t++)if(!c->tracks[t].used){free_track=(int)t;break;}
  if(free_track<0){c->dropped_detections++;continue;}
  memset(&c->tracks[free_track],0,sizeof(c->tracks[free_track]));
  c->tracks[free_track].used=true;c->tracks[free_track].ms=now;
  c->tracks[free_track].x=points[q].x;c->tracks[free_track].y=points[q].y;
  if(c->cfg.count_unseen&&c->cfg.direction&&(c->has_flow||!c->cfg.settled)){
   /* less than the travel of the flow since the previous result past the line: it crossed unseen */
   int past=(axial(c,points[q].x,points[q].y)-c->cfg.line)*c->cfg.direction;
   int32_t step=(c->has_flow&&c->has_last)?cdiv((int64_t)c->flow_a*(int32_t)(now-c->last_ms),16000):c->cfg.max_step;
   if(past>=0&&past<step){
    count(c,(unsigned)free_track,&points[q],c->cfg.direction,source_sequence,events,&produced);
    if(produced<=MOQ_PASSAGE_EVENTS)ago[produced-1]=crossed_ago(c,(unsigned)free_track,&points[q]);
   }
  }
 }
 c->last_ms=now;c->has_last=true;
 if(events)order_events(events,ago,produced);
 return produced;
}
unsigned moq_passage_update_at(moq_passage_counter *c,const moq_passage_point *points,unsigned count_in,
                               uint32_t source_sequence,uint32_t capture_ms,moq_passage_event *events){
 if(c->cfg.assoc==1)return update_assoc(c,points,count_in,source_sequence,capture_ms,events);
 return moq_passage_update(c,points,count_in,source_sequence,events);
}
