#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
typedef int ngtcp2_crypto_aead;typedef int ngtcp2_crypto_aead_ctx;typedef int ngtcp2_crypto_cipher;typedef int ngtcp2_crypto_cipher_ctx;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
static uint32_t cpu;static int64_t now;
static uint32_t aiot_task_runtime_now(void){return cpu;}
static int64_t esp_timer_get_time(void){return now;}
static int ngtcp2_crypto_encrypt_cb(uint8_t*d,const int*a,const int*c,const uint8_t*p,size_t n,const uint8_t*v,size_t z,const uint8_t*x,size_t y){(void)d;(void)a;(void)c;(void)p;(void)n;(void)v;(void)z;(void)x;(void)y;cpu+=30;now+=50;return 0;}
static int ngtcp2_crypto_hp_mask_cb(uint8_t*d,const int*a,const int*c,const uint8_t*p){(void)d;(void)a;(void)c;(void)p;cpu+=5;now+=8;return 0;}
#include "../../src/components/transport/quic_pipeline_profile.inc"
int main(void){
 pipe_encrypt(NULL,NULL,NULL,NULL,0,NULL,0,NULL,0);assert(s_pipe.aead_cpu==0);
 s_pipe_scope=true;pipe_encrypt(NULL,NULL,NULL,NULL,0,NULL,0,NULL,0);pipe_hp(NULL,NULL,NULL,NULL);
 assert(s_pipe.aead_cpu==30 && s_pipe.aead_wall==50 && s_pipe.hp_cpu==5 && s_pipe.hp_wall==8);
 uint8_t payload[64];for(int i=0;i<64;i++)payload[i]=i;
 pipe_wifi_track(payload,sizeof(payload));assert(registered);
 uint8_t frame[14+20+8+64]={0};size_t u=34;frame[14]=0x45;frame[23]=17;frame[u+2]=14567>>8;frame[u+3]=14567&255;frame[u+5]=72;memcpy(frame+42,payload,64);
 uint16_t n=sizeof(frame);now+=300;registered(0,frame,&n,true);assert(s_wifi_matched==1 && s_wifi_us==300 && !s_wifi_failed);
 registered(0,frame,&n,true);assert(s_wifi_matched==1); /* duplicate cannot double count */
 pipe_wifi_track(payload,sizeof(payload));frame[42]^=1;registered(0,frame,&n,true);assert(s_wifi_matched==1);
 frame[42]^=1;now+=200;registered(0,frame,&n,false);assert(s_wifi_matched==2 && s_wifi_failed==1);
 for(n=0;n<42;n++)registered(0,frame,&n,true); /* bounded truncated-header parser */
 pipe_report(now);puts("PASS: nested CPU/wall scopes, encrypted packet matching, duplicate/unmatched/truncated frames, TX failure");
}
