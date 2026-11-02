#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "../../common/include/ai_result.h"
#include "../../common/include/data_packet.h"

typedef struct {
    uint8_t data[AIOT_AI_RESULT_MAX_PAYLOAD_LEN];
    uint16_t len;
} source_manager_ai_payload_t;

typedef struct {
    uint32_t next_sequence;
    uint8_t slot;
    source_manager_ai_payload_t ai_payload;
    /* Result of the most recent on-capture inference. Consumers (app_main)
     * read this instead of re-running inference on the reassembled frame,
     * so the model runs exactly once per frame. */
    aiot_ai_result_t last_result;
    bool last_frame_real;       /* true only for a validated physical camera frame */
    bool last_ran;              /* inference actually executed this frame */
    uint32_t last_inference_us; /* wall-clock of the last inference run */
    uint8_t last_importance;    /* aiot_importance_t of the last frame */
} source_manager_t;

void source_manager_init(source_manager_t *manager);
bool source_manager_next_packet(source_manager_t *manager, data_packet_t *packet);

/* Copy the most recent inference result into *out (and its duration into
 * *inference_us when non-NULL). Returns true if inference ran for that frame,
 * mirroring ai_inference_run()'s return. */
bool source_manager_get_last_ai_result(const source_manager_t *manager,
    aiot_ai_result_t *out, uint32_t *inference_us);

/* Camera initialization status, distinct from synthetic fallback frames. */
bool source_manager_camera_ready(void);

#if AIOT_MOBILITY_CAMERA
bool source_manager_get_async_ai(aiot_ai_result_t *result, uint32_t *frame_ts);
void source_manager_submit_camera_ai(const data_packet_t *packet);
bool source_manager_replay_next(data_packet_t *packet);
void source_manager_evidence_enable(bool enabled);
size_t source_manager_copy_evidence(uint32_t ts, uint8_t *dst, size_t capacity);
size_t source_manager_copy_evidence_for_sequence(uint32_t seq,uint32_t ts,uint8_t *dst,size_t capacity);
uint32_t source_manager_ai_completed(void);
/* time until the running (or next) inference can end at the earliest; 0 when not known */
uint32_t source_manager_alert_safe_us(void);
/* time since the last inference started, the shortest recent inference, inferences started */
void source_manager_ai_phase(uint32_t *elapsed_us, uint32_t *floor_us, uint32_t *started);
#endif

#if AIOT_MOQT_POLICY_EXPERIMENT
bool source_manager_ai_idle(void);
void source_manager_audit_pending(void);
bool source_manager_peek_ai_event(aiot_ai_result_t *,uint32_t *);
bool source_manager_pop_ai_event(uint32_t seq);
unsigned source_manager_ai_event_count(void);
unsigned source_manager_ai_event_capacity(void);
/* `image` is the input of the inference when evidence is enabled and the result has detections. */
typedef void (*source_result_hook)(const aiot_ai_result_t *result,uint32_t frame_ms,const uint8_t *image,size_t image_len,
                                   uint32_t *count_new,uint32_t *count_total);
void source_manager_set_result_hook(source_result_hook hook);
/* Split inference (Chen et al., VTC2023-Fall: the model partitioned between devices): before a frame's
 * inference, `decide` (in the inference task) says whether the frame goes to a helper after its first
 * stage; then only the decode runs here and `deliver` takes its raster (ai_inference_copy_raster, in
 * the inference task); no camera result is produced for that frame. */
typedef bool (*source_split_decide)(uint32_t seq,uint32_t frame_ms);
typedef void (*source_split_deliver)(uint32_t seq,uint32_t frame_ms,uint16_t width,uint16_t height);
void source_manager_set_split_hooks(source_split_decide decide,source_split_deliver deliver);
bool source_manager_peek_ai_counts(uint32_t seq,uint32_t *count_new,uint32_t *count_total);
#endif

#if AIOT_CAMERA_INTEGRITY_TEST
size_t source_manager_camera_diagnostic(char *out, size_t capacity);
#endif
