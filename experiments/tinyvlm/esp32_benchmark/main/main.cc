#include <cstdio>
#include "tinyvlm.h"
#include "tinyvlm_assets.h"
#include "samples.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" void app_main(void) {
    printf("[TINYVLM_BEGIN]\n");
    // Leave time for the host to restore the published OTA release.
    vTaskDelay(pdMS_TO_TICKS(5000));
    constexpr size_t reserve=2*1024*1024;
    auto *arena=(uint8_t*)heap_caps_aligned_alloc(16,reserve,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    bool ok=arena && tinyvlm_encoder_init(tinyvlm_model,sizeof(tinyvlm_model),arena,reserve);
    printf("[TINYVLM_MEMORY] {\"initialized\":%s,\"arena_reserved\":%u,\"arena_used\":%u,\"internal_free\":%u,\"psram_free\":%u}\n",
        ok?"true":"false",unsigned(reserve),unsigned(tinyvlm_encoder_arena_used_bytes()),
        unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)),
        unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    for(int run=-3;ok && run<20;run++) {
        size_t sample=run<0?0:run%4,dim=0,best=0;int zp=0;int8_t embedding[256];float scores[TINYVLM_CLASSES];
        int64_t start=esp_timer_get_time();
        ok=tinyvlm_encoder_run(samples[sample],sizeof(samples[0]),embedding,sizeof(embedding),&dim,&zp);
        int64_t encoded=esp_timer_get_time();
        ok=ok && tinyvlm_cosine_i8(embedding,dim,zp,tinyvlm_prototypes,TINYVLM_CLASSES,TINYVLM_DIM,TINYVLM_DIM,scores,&best);
        int64_t end=esp_timer_get_time();
        if(ok && run>=0) {
            printf("[TINYVLM_SAMPLE] {\"run\":%d,\"sample\":%u,\"encode_us\":%lld,\"match_us\":%lld,\"best\":%u,\"scores\":[%.7f,%.7f],\"embedding\":[",
                run,unsigned(sample),(long long)(encoded-start),(long long)(end-encoded),unsigned(best),scores[0],scores[1]);
            for(size_t i=0;i<dim;i++)printf("%s%d",i?",":"",embedding[i]);
            printf("]}\n");
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    printf("[TINYVLM_DONE] {\"status\":\"%s\",\"internal_min_free\":%u}\n",ok?"completed":"failed",
        unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)));
    fflush(stdout);
    tinyvlm_encoder_reset();heap_caps_free(arena);
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_ota_mark_app_invalid_rollback_and_reboot();
    // If rollback is unavailable, retain output for serial recovery.
    while(true)vTaskDelay(pdMS_TO_TICKS(1000));
}
