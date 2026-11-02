#include "moqt21.h"
#include <string.h>
size_t moqt21_vi_write(uint8_t *d,size_t cap,uint64_t v) {
 size_t n=1;while(n<9 && (n==8 ? v>UINT64_C(0xffffffffffffff) : v >= (UINT64_C(1)<<(7*n))))n++;
 if(cap<n)return 0;
 uint64_t x=v;for(size_t i=n;i>1;i--){d[i-1]=(uint8_t)x;x>>=8;}
 d[0]=n==9?255:(uint8_t)((0xffU<<(9-n))|x);return n;
}
int moqt21_vi_read(const uint8_t *s,size_t len,uint64_t *v,size_t *used) {
 if(!len)return 0;
 size_t n=1;uint8_t bit=128;while(bit&&(s[0]&bit)){n++;bit>>=1;}
 if(len<n)return 0;
 uint64_t x=n==9?0:(s[0]&(0xffU>>n));for(size_t i=1;i<n;i++)x=(x<<8)|s[i];
 *v=x;*used=n;return 1;
}
size_t moqt21_message(uint8_t *d,size_t cap,uint64_t type,const uint8_t *b,size_t len) {
 if(len>65535)return 0;
 size_t n=moqt21_vi_write(d,cap,type);if(!n||cap-n<2||cap-n-2<len)return 0;
 d[n++]=(uint8_t)(len>>8);d[n++]=(uint8_t)len;if(len)memcpy(d+n,b,len);return n+len;
}
int moqt21_message_read(const uint8_t *s,size_t len,uint64_t *type,const uint8_t **body,size_t *bl,size_t *used) {
 size_t n=0;int r=moqt21_vi_read(s,len,type,&n);if(r!=1)return r;
 if(len-n<2)return 0;
 size_t k=((size_t)s[n]<<8)|s[n+1];if(k>MOQT21_CONTROL_CAP)return -1;
 if(len-n-2<k)return 0;
 *body=s+n+2;*bl=k;*used=n+2+k;return 1;
}
static int take(const uint8_t **p,size_t *n,uint64_t *v) {
 size_t k;int r=moqt21_vi_read(*p,*n,v,&k);if(r!=1)return -1;
 *p+=k;*n-=k;return 1;
}
static int field(const uint8_t **p,size_t *n,const char *expect) {
 uint64_t k;if(take(p,n,&k)<0||k>*n)return -1;
 if(k!=strlen(expect)||memcmp(*p,expect,(size_t)k))return -1;
 *p+=k;*n-=k;return 1;
}
size_t moqt21_setup(uint8_t *d,size_t cap,const char *authority) {
 uint8_t b[256];size_t a=strlen(authority),n=0,k;if(a>200)return 0;
 b[n++]=1;b[n++]=1;b[n++]='/'; /* PATH */
 b[n++]=4; /* delta from PATH1 to AUTHORITY5 */
 k=moqt21_vi_write(b+n,sizeof(b)-n,a);if(!k)return 0;n+=k;memcpy(b+n,authority,a);n+=a;
 return moqt21_message(d,cap,0x2f00,b,n);
}
int moqt21_server_setup(const uint8_t *p,size_t n) {
 uint64_t prev=0,delta,v;while(n){
  if(take(&p,&n,&delta)<0||delta>UINT64_MAX-prev)return -1;
  uint64_t key=prev+delta;if(key==1||key==5)return -1;
  /* Known singleton setup keys may not be duplicated. Unknowns may repeat. */
  if(delta==0 && (key==2||key==4||key==8||key==0x10))return -1;
  prev=key;if(take(&p,&n,&v)<0)return -1;
  if(key&1){if(v>65535||v>n)return -1;p+=v;n-=v;}
 }return 1;
}
int moqt21_subscribe_track(const uint8_t *p,size_t n,uint64_t *request,unsigned *alias) {
 uint64_t fields,params;
 if(take(&p,&n,request)<0||!(*request&1)||take(&p,&n,&fields)<0||fields!=2)return -1;
 if(field(&p,&n,"aiot")<0||field(&p,&n,"esp32")<0)return -1;
 const uint8_t *q=p;size_t z=n;
 if(field(&p,&n,"preview/jpeg")==1)*alias=1;
 else {p=q;n=z;if(field(&p,&n,"ai/events")<0)return -1;*alias=2;}
 if(take(&p,&n,&params)<0||params||n)return -1;
 return 1;
}
size_t moqt21_subscribe_ok_track(uint8_t *d,size_t cap,unsigned alias) {
 if(alias<1||alias>2)return 0;
 const uint8_t b[]={(uint8_t)alias,0}; /* alias1, no parameters/properties; defaults apply */
 return moqt21_message(d,cap,4,b,sizeof(b));
}
size_t moqt21_object_header_track(uint8_t *d,size_t cap,unsigned alias,uint64_t group,size_t len) {
 if(alias<1||alias>2)return 0;
 if(!len||len>MOQT21_MAX_JPEG||cap<4)return 0;
 size_t n=0,k;d[n++]=0x78; /* first object, default priority, end group, subgroup0 */
 d[n++]=(uint8_t)alias;k=moqt21_vi_write(d+n,cap-n,group);if(!k)return 0;n+=k;
 if(n==cap)return 0;
 d[n++]=0;k=moqt21_vi_write(d+n,cap-n,len);if(!k)return 0;return n+k;
}

int moqt21_subscribe(const uint8_t *p,size_t n,uint64_t *request){unsigned alias;return moqt21_subscribe_track(p,n,request,&alias)==1&&alias==1?1:-1;}
size_t moqt21_subscribe_ok(uint8_t *d,size_t cap){return moqt21_subscribe_ok_track(d,cap,1);}
size_t moqt21_object_header(uint8_t *d,size_t cap,uint64_t group,size_t len){return moqt21_object_header_track(d,cap,1,group,len);}
