extern "C" {
#include "ai_inference.h"
#include "moq_audit.h"
#include "mobility_scheduler.h"
}
#include "cat_detect.hpp"
#include "dl_image_jpeg.hpp"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#if AIOT_QUIC_BATCH_EXPERIMENT || AIOT_MOQT_CONCURRENT
#include "ai_batch_decode.inc"
#endif
static CatDetect *detector;
static dl::image::jpeg_img_t roi{};
extern "C" void ai_inference_init(ai_inference_t *s) {
    if(s) {memset(s,0,sizeof(*s));s->initialized=true;s->selected_backend=AI_INFERENCE_BACKEND_ESPDL;}
}
extern "C" void ai_inference_get_capabilities(ai_inference_capabilities_t *c) {
    if(c) {*c={};c->backend=AI_INFERENCE_BACKEND_ESPDL;c->model_available=true;c->model_input_width=224;c->model_input_height=224;}
}
extern "C" size_t ai_inference_copy_roi(uint8_t *dst,size_t capacity) {
    if(!roi.data||roi.data_len>capacity) return 0;
    memcpy(dst,roi.data,roi.data_len);return roi.data_len;
}
static uint8_t normalized(int n,int bound) {return (uint8_t)std::clamp(n*255/std::max(1,bound),0,255);}
extern "C" bool ai_inference_run(ai_inference_t *state,const uint8_t *jpeg,uint32_t len,uint32_t seq,aiot_ai_result_t *out) {
    if(!state||!jpeg||!out||len<4) return false;
    aiot_ai_result_init(out,AIOT_AI_BACKEND_ESPDL,seq);
    heap_caps_free(roi.data);roi={};
    if(!detector) {detector=new CatDetect(CatDetect::ESPDET_PICO_224_224_CAT,false);state->model_ready=true;}
    int64_t t0=esp_timer_get_time();
    dl::image::jpeg_img_t input{(void*)jpeg,len};
#if AIOT_QUIC_BATCH_EXPERIMENT || AIOT_MOQT_CONCURRENT
    auto img=batch_decode_jpeg(input);
#else
    auto img=dl::image::sw_decode_jpeg(input,dl::image::DL_IMAGE_PIX_TYPE_RGB888);
#endif
    int64_t t1=esp_timer_get_time();
    if(!img.data) {state->decode_failures++;return false;}
    auto &found=detector->run(img);int64_t t2=esp_timer_get_time();
    out->source_width=img.width;out->source_height=img.height;out->input_width=224;out->input_height=224;
    const dl::detect::result_t *best=nullptr;
    for(const auto &r:found) {
        if(!best||r.score>best->score) best=&r;
        uint8_t score=(uint8_t)std::clamp((int)(r.score*255),0,255);out->raw_score=std::max(out->raw_score,score);
        if(out->detection_count>=AIOT_AI_RESULT_MAX_DETECTIONS) continue;
        auto &d=out->detections[out->detection_count++];d.class_id=r.category+1;d.confidence=score;
        d.x=normalized((r.box[0]+r.box[2])/2,img.width);d.y=normalized((r.box[1]+r.box[3])/2,img.height);
        d.width=normalized(r.box[2]-r.box[0],img.width);d.height=normalized(r.box[3]-r.box[1],img.height);
    }
    int p=mobility_profile_get();
    if(best&&(p==4||p>=7)) {
        int x0=std::clamp(best->box[0],0,(int)img.width),y0=std::clamp(best->box[1],0,(int)img.height);
        int x1=std::clamp(best->box[2],0,(int)img.width),y1=std::clamp(best->box[3],0,(int)img.height);
        dl::image::img_t crop{};crop.width=std::max(0,x1-x0);crop.height=std::max(0,y1-y0);crop.pix_type=img.pix_type;
        // Bound allocation and preserve headroom for transport. A skipped ROI is logged, never substituted.
        if(crop.width&&crop.height&&heap_caps_get_free_size(MALLOC_CAP_SPIRAM)>crop.bytes()*2+262144) {
            crop.data=heap_caps_aligned_alloc(16,crop.bytes(),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
            if(crop.data) {
                for(unsigned y=0;y<crop.height;++y) memcpy((uint8_t*)crop.data+y*crop.row_step(),(uint8_t*)img.data+(y+y0)*img.row_step()+x0*3,crop.row_step());
                roi=dl::image::sw_encode_jpeg(crop,80);heap_caps_free(crop.data);
            }
        }
    }
    int64_t t3=esp_timer_get_time();
#if !AIOT_QUIC_BATCH_EXPERIMENT && !AIOT_MOQT_CONCURRENT
    heap_caps_free(img.data);
#endif
    state->inference_count++;
    moq_audit_printf("[ESPDET_PHASE] {\"seq\":%lu,\"model\":\"cat224\",\"decode_us\":%lld,\"detect_us\":%lld,\"roi_us\":%lld,\"roi_bytes\":%u,\"found\":%u}\n",(unsigned long)seq,(long long)(t1-t0),(long long)(t2-t1),(long long)(t3-t2),(unsigned)roi.data_len,(unsigned)found.size());
    return true;
}
