#include "moqt21.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void){
 uint64_t vals[]={0,37,127,128,16383,16384,2097151,2097152,UINT64_C(0xffffffffffffff),UINT64_C(0x100000000000000),UINT64_MAX};
 uint8_t b[256];size_t n,k;uint64_t v;
 for(size_t i=0;i<sizeof(vals)/sizeof(vals[0]);i++){
  n=moqt21_vi_write(b,sizeof(b),vals[i]);assert(n&&moqt21_vi_read(b,n,&v,&k)==1&&v==vals[i]&&k==n);
  for(size_t j=0;j<n;j++)assert(moqt21_vi_read(b,j,&v,&k)==0);
  assert(!moqt21_vi_write(b,n-1,vals[i]));
 }
 const uint8_t example[]={0xed,0x7f,0x3e,0x7d};assert(moqt21_vi_read(example,4,&v,&k)==1&&v==226442877);
 n=moqt21_setup(b,sizeof(b),"localhost:14568");uint64_t type;const uint8_t *body;size_t bl,used;
 for(size_t j=0;j<n;j++)assert(moqt21_message_read(b,j,&type,&body,&bl,&used)==0);
 assert(moqt21_message_read(b,n,&type,&body,&bl,&used)==1&&type==0x2f00&&used==n);
 assert(moqt21_server_setup(body,bl)==-1);assert(moqt21_server_setup(NULL,0)==1);
 uint8_t bad[]={3,255,255};assert(moqt21_message_read(bad,3,&type,&body,&bl,&used)==-1);
 const uint8_t sub[]={1,2,4,'a','i','o','t',5,'e','s','p','3','2',12,'p','r','e','v','i','e','w','/','j','p','e','g',0};
 assert(moqt21_subscribe(sub,sizeof(sub),&v)==1&&v==1);
 for(size_t j=0;j<sizeof(sub);j++)assert(moqt21_subscribe(sub,j,&v)==-1);
 assert(moqt21_object_header(b,sizeof(b),0,123179)>0&&b[0]==0x78);
 assert(!moqt21_object_header(b,sizeof(b),0,MOQT21_MAX_JPEG+1));
 const uint8_t ai_sub[]={3,2,4,'a','i','o','t',5,'e','s','p','3','2',9,'a','i','/','e','v','e','n','t','s',0};
 unsigned alias=0;assert(moqt21_subscribe_track(ai_sub,sizeof(ai_sub),&v,&alias)==1&&v==3&&alias==2);
 assert(moqt21_subscribe(ai_sub,sizeof(ai_sub),&v)==-1);
 for(size_t j=0;j<sizeof(ai_sub);j++)assert(moqt21_subscribe_track(ai_sub,j,&v,&alias)==-1);
 n=moqt21_object_header_track(b,sizeof(b),2,128,200);assert(n>0&&b[1]==2);
 assert(!moqt21_object_header_track(b,sizeof(b),3,0,10));
 n=moqt21_subscribe_ok_track(b,sizeof(b),2);
 assert(moqt21_message_read(b,n,&type,&body,&bl,&used)==1&&type==4&&bl==2&&body[0]==2);
 puts("PASS: draft21 vi64 vectors, truncation, bounds, setup roles, subscription, object header");return 0;
}
