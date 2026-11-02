#include "moq_representation.h"
#include "jpeg_decoder.h"
#include "img_converters.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
static int mode,decode_calls,allocations;
void *heap_caps_malloc(size_t size,unsigned flags){(void)flags;allocations++;return mode==2?NULL:malloc(size);}
int64_t esp_timer_get_time(void){static int64_t t=100;return t+=100;}
int esp_jpeg_get_image_info(esp_jpeg_image_cfg_t *c,esp_jpeg_image_output_t *o){
 assert(c->flags.swap_color_bytes==1);
 *o=(esp_jpeg_image_output_t){16,16,128};
 if(mode==1)o->output_len=2*1024*1024;
 return 0;
}
int esp_jpeg_decode(esp_jpeg_image_cfg_t *c,esp_jpeg_image_output_t *o){
 decode_calls++;assert(c->outbuf_size==128);*o=(esp_jpeg_image_output_t){8,8,128};return mode==3?-1:0;
}
bool fmt2jpg_cb(uint8_t *rgb,size_t n,uint16_t w,uint16_t h,int fmt,uint8_t q,jpg_out_cb cb,void *arg){
 (void)rgb;(void)fmt;(void)q;assert(n==128&&w==8&&h==8);
 const uint8_t jpeg[]={0xff,0xd8,0x00,0xff,0xd9};
 cb(arg,mode==4?1:0,jpeg,sizeof(jpeg));return true;
}
int main(void){
 const uint8_t in[]={0xff,0xd8,0xff,0xd9};uint8_t out[8]={0};moq_representation_metrics m;
 assert(moq_transcode(in,4,1,out,8,&m));assert(m.bytes==5&&m.width==8&&m.scratch_bytes==128);
 out[4]=0xa5;assert(!moq_transcode(in,4,2,out,4,&m));assert(out[4]==0xa5&&m.bytes==0);
 mode=1;int prior=allocations;assert(!moq_transcode(in,4,1,out,8,&m));assert(prior==allocations);
 mode=2;assert(!moq_transcode(in,4,1,out,8,&m));
 mode=3;assert(!moq_transcode(in,4,1,out,8,&m));
 mode=4;assert(!moq_transcode(in,4,1,out,8,&m));
 assert(!moq_transcode(in,4,0,out,8,&m));assert(!moq_transcode(in,4,3,out,8,&m));
 assert(!moq_transcode(NULL,4,1,out,8,&m));assert(!moq_transcode(in,4,1,out,8,NULL));
 puts("PASS: bounded codec allocation/output, overflow, malformed callback and failure cleanup");
}
