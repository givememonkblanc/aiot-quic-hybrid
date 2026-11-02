#include "moq_representation.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "img_converters.h"
#include "jpeg_decoder.h"
#include <stdlib.h>
#include <string.h>

typedef struct {uint8_t *data;size_t cap,used;bool overflow;} sink;
static size_t emit(void *context,size_t index,const void *data,size_t length) {
    sink *s=context;
    /* The upstream encoder does not propagate short callback writes. Track
     * overflow explicitly and reject the complete result after encoding. */
    if(s->overflow||index!=s->used||index>s->cap||length>s->cap-index){s->overflow=true;return 0;}
    if(length&&!data){s->overflow=true;return 0;}
    if(length)memcpy(s->data+index,data,length);
    s->used+=length;return length;
}
static bool transcode(const uint8_t *jpeg,size_t length,esp_jpeg_image_scale_t scale,int quality,
                      uint8_t *output,size_t capacity,moq_representation_metrics *metrics) {
    if(!metrics)return false;
    memset(metrics,0,sizeof(*metrics));
    if(!jpeg||!output||!length||length>262144||capacity<4)return false;
    esp_jpeg_image_cfg_t cfg={.indata=(uint8_t*)jpeg,.indata_size=length,
        .out_format=JPEG_IMAGE_FORMAT_RGB565,.out_scale=scale,
        .flags.swap_color_bytes=1};
    esp_jpeg_image_output_t info={0};
    uint64_t start=(uint64_t)esp_timer_get_time();
    if(esp_jpeg_get_image_info(&cfg,&info)!=ESP_OK||!info.output_len||
       info.output_len>1024U*1024U||info.width>1600||info.height>1200)return false;
    size_t scratch=info.output_len;
    uint8_t *rgb=heap_caps_malloc(scratch,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!rgb)return false;
    cfg.outbuf=rgb;cfg.outbuf_size=scratch;
    bool ok=esp_jpeg_decode(&cfg,&info)==ESP_OK;
    uint64_t decoded=(uint64_t)esp_timer_get_time();
    sink s={.data=output,.cap=capacity};
    if(ok){
        /* Decoder emits big-endian RGB565, the encoder's default. No global
         * byte-order setting is changed while other application tasks run. */
        ok=fmt2jpg_cb(rgb,info.output_len,info.width,info.height,PIXFORMAT_RGB565,
                      quality,emit,&s);
    }
    uint64_t done=(uint64_t)esp_timer_get_time();
    free(rgb);
    metrics->decode_us=decoded-start;metrics->encode_us=done-decoded;
    metrics->scratch_bytes=scratch;metrics->width=info.width;metrics->height=info.height;
    if(!ok||s.overflow||s.used<4||output[0]!=0xff||output[1]!=0xd8||
       output[s.used-2]!=0xff||output[s.used-1]!=0xd9)return false;
    metrics->bytes=s.used;return true;
}
bool moq_transcode(const uint8_t *jpeg,size_t length,unsigned level,
                   uint8_t *output,size_t capacity,moq_representation_metrics *metrics) {
    if(level<1||level>2){if(metrics)memset(metrics,0,sizeof(*metrics));return false;}
    return transcode(jpeg,length,level==1?JPEG_IMAGE_SCALE_1_2:JPEG_IMAGE_SCALE_1_4,level==1?75:65,
                     output,capacity,metrics);
}
bool moq_encode_rgb565(const uint8_t *rgb,unsigned width,unsigned height,int quality,
                       uint8_t *output,size_t capacity,moq_representation_metrics *metrics) {
    if(!metrics)return false;
    memset(metrics,0,sizeof(*metrics));
    if(!rgb||!output||!width||!height||capacity<4)return false;
    sink s={.data=output,.cap=capacity};
    uint64_t start=(uint64_t)esp_timer_get_time();
    bool ok=fmt2jpg_cb((uint8_t*)rgb,(size_t)width*height*2U,width,height,PIXFORMAT_RGB565,quality,emit,&s);
    metrics->encode_us=(uint64_t)esp_timer_get_time()-start;metrics->width=width;metrics->height=height;
    if(!ok||s.overflow||s.used<4||output[0]!=0xff||output[1]!=0xd8||
       output[s.used-2]!=0xff||output[s.used-1]!=0xd9)return false;
    metrics->bytes=s.used;return true;
}
bool moq_transcode_eighth(const uint8_t *jpeg,size_t length,int quality,
                          uint8_t *output,size_t capacity,moq_representation_metrics *metrics) {
    return transcode(jpeg,length,JPEG_IMAGE_SCALE_1_8,quality,output,capacity,metrics);
}
