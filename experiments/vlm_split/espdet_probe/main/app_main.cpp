#include <algorithm>
#include <cstdio>
#include <cstring>
#include "cat_detect.hpp"
#include "dl_image_jpeg.hpp"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern const uint8_t cat_jpg_start[] asm("_binary_cat_jpg_start");
extern const uint8_t cat_jpg_end[] asm("_binary_cat_jpg_end");
static void memory(const char *phase) {
    const unsigned in=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT, ps=MALLOC_CAP_SPIRAM;
    printf("[ESPDET_MEM] {\"phase\":\"%s\",\"internal_free\":%u,\"internal_min\":%u,\"internal_largest\":%u,\"psram_free\":%u,\"psram_min\":%u}\n",phase,
        (unsigned)heap_caps_get_free_size(in),(unsigned)heap_caps_get_minimum_free_size(in),
        (unsigned)heap_caps_get_largest_free_block(in),(unsigned)heap_caps_get_free_size(ps),
        (unsigned)heap_caps_get_minimum_free_size(ps));
}
static void return_to_baseline(const char *status) {
    printf("[ESPDET_DONE] {\"status\":\"%s\",\"action\":\"rollback_to_baseline\"}\n",status);fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_err_t err=esp_ota_mark_app_invalid_rollback_and_reboot();
    // Never mark this temporary probe valid or change NVS/partitions if rollback is unavailable.
    printf("[ESPDET_ROLLBACK_ERROR] %s\n",esp_err_to_name(err));
    while(true) vTaskDelay(pdMS_TO_TICKS(1000));
}
extern "C" void app_main(void) {
    const auto *slot=esp_ota_get_running_partition();
    printf("[ESPDET_BEGIN] {\"slot\":\"%s\",\"core\":%d,\"jpeg_bytes\":%u,\"warmup\":3,\"runs\":20,\"psram_mhz\":40}\n",slot?slot->label:"unknown",xPortGetCoreID(),(unsigned)(cat_jpg_end-cat_jpg_start));
    vTaskDelay(pdMS_TO_TICKS(2000));memory("before_model");
    int64_t load_start=esp_timer_get_time();
    auto *detector=new CatDetect(CatDetect::ESPDET_PICO_224_224_CAT,false);
    printf("[ESPDET_LOAD] {\"us\":%lld}\n",(long long)(esp_timer_get_time()-load_start));memory("model_loaded");
    const dl::image::jpeg_img_t jpeg{(void*)cat_jpg_start,(size_t)(cat_jpg_end-cat_jpg_start)};
    for(int i=0;i<23;++i) {
        int64_t t0=esp_timer_get_time();
        auto img=dl::image::sw_decode_jpeg(jpeg,dl::image::DL_IMAGE_PIX_TYPE_RGB888);
        int64_t t1=esp_timer_get_time();
        if(!img.data) return_to_baseline("decode_failed");
        auto &found=detector->run(img);
        int64_t t2=esp_timer_get_time();
        printf("[ESPDET_RUN] {\"index\":%d,\"warmup\":%d,\"width\":%u,\"height\":%u,\"decode_us\":%lld,\"detect_us\":%lld,\"jpeg_to_detection_us\":%lld,\"detections\":%u}\n",i,i<3,img.width,img.height,(long long)(t1-t0),(long long)(t2-t1),(long long)(t2-t0),(unsigned)found.size());
        memory("decoded_and_detected");
        unsigned n=0;
        for(const auto &r:found) {
            printf("[ESPDET_BOX] {\"index\":%d,\"roi\":%u,\"class\":%d,\"score\":%.6f,\"box\":[%d,%d,%d,%d]}\n",i,n++,r.category,r.score,r.box[0],r.box[1],r.box[2],r.box[3]);
        }
        // Encode every detected crop. These costs are separate from detector timing.
        int64_t crop_us=0;size_t roi_bytes=0;unsigned roi_ok=0;n=0;
        for(const auto &r:found) {
            int64_t roi_start=esp_timer_get_time();
            int x0=std::clamp(r.box[0],0,(int)img.width), y0=std::clamp(r.box[1],0,(int)img.height);
            int x1=std::clamp(r.box[2],0,(int)img.width), y1=std::clamp(r.box[3],0,(int)img.height);
            if(x1<=x0||y1<=y0) continue;
            dl::image::img_t crop{};crop.width=x1-x0;crop.height=y1-y0;crop.pix_type=img.pix_type;
            crop.data=heap_caps_malloc(crop.bytes(),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
            if(!crop.data) return_to_baseline("crop_alloc_failed");
            for(unsigned y=0;y<crop.height;++y) memcpy((uint8_t*)crop.data+y*crop.row_step(),(uint8_t*)img.data+(y+y0)*img.row_step()+x0*3,crop.row_step());
            auto encoded=dl::image::sw_encode_jpeg(crop,80);
            if(!encoded.data) {heap_caps_free(crop.data);return_to_baseline("crop_encode_failed");}
            crop_us+=esp_timer_get_time()-roi_start;roi_bytes+=encoded.data_len;roi_ok++;
            // Verify each ROI once, outside both detector and crop/encode timing.
            if(i==0) {
                auto decoded=dl::image::sw_decode_jpeg(encoded,dl::image::DL_IMAGE_PIX_TYPE_RGB888);
                bool ok=decoded.data&&decoded.width==crop.width&&decoded.height==crop.height;
                printf("[ESPDET_ROI_CHECK] {\"roi\":%u,\"width\":%u,\"height\":%u,\"bytes\":%u,\"decode_ok\":%d}\n",n,crop.width,crop.height,(unsigned)encoded.data_len,ok);
                heap_caps_free(decoded.data);
                if(!ok) return_to_baseline("crop_validation_failed");
            }
            memory("roi_encoded");heap_caps_free(encoded.data);heap_caps_free(crop.data);n++;
        }
        printf("[ESPDET_ROI] {\"index\":%d,\"count\":%u,\"bytes\":%u,\"crop_encode_us\":%lld,\"quality\":80}\n",i,roi_ok,(unsigned)roi_bytes,(long long)crop_us);
        heap_caps_free(img.data);memory("iteration_freed");vTaskDelay(pdMS_TO_TICKS(100));
    }
    delete detector;memory("model_freed");return_to_baseline("completed");
}
