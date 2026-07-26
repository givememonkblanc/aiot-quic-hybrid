#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ai_result.h"

/*
 * Presence detection state machine.
 *
 * Feeds on per-frame AI inference results and derives a debounced
 * PRESENT/ABSENT state with hysteresis:
 *   - enter: `enter_frames` consecutive frames with score >= enter_confidence
 *   - exit:  no frame with score >= exit_confidence for `exit_timeout_ms`
 * The state machine is pure logic (no ESP dependencies) so it can be
 * unit-tested on the host.
 */

typedef enum {
    PRESENCE_STATE_UNKNOWN = 0,
    PRESENCE_STATE_ABSENT = 1,
    PRESENCE_STATE_PRESENT = 2,
} presence_state_t;

typedef struct {
    uint8_t enter_confidence;   /* frame is positive when score >= this */
    uint8_t exit_confidence;    /* frame keeps presence alive when score >= this */
    uint8_t enter_frames;       /* consecutive positive frames to enter PRESENT */
    uint32_t exit_timeout_ms;   /* silence period after which PRESENT -> ABSENT */
} presence_config_t;

/* Tuned for the motion-based detector: enter on a couple of frames with clear
 * movement, hold PRESENT while any motion keeps arriving, drop to ABSENT after
 * a few seconds of stillness. */
#define PRESENCE_CONFIG_DEFAULTS { \
    .enter_confidence = 50,        \
    .exit_confidence = 30,         \
    .enter_frames = 2,             \
    .exit_timeout_ms = 6000,       \
}

typedef struct {
    presence_state_t state;     /* state after the transition */
    uint32_t event_seq;         /* monotonically increasing per transition */
    uint32_t prev_state_dur_ms; /* how long the previous state lasted */
    uint8_t score;              /* frame score that triggered the transition */
} presence_event_t;

typedef struct {
    presence_config_t config;
    presence_state_t state;
    uint32_t event_seq;
    uint8_t consecutive_positive;
    int64_t state_since_us;
    int64_t last_positive_us;
    /* rolling stats for heartbeats */
    uint32_t frames_total;
    uint32_t frames_positive;
    uint8_t last_score;
} presence_t;

void presence_init(presence_t *p, const presence_config_t *config);

/* Best frame score: the model's pre-threshold raw score or the highest
 * detection confidence, whichever is larger (0 when neither). */
uint8_t presence_score(const aiot_ai_result_t *result);

/*
 * Consume one frame result. Returns true when the presence state changed,
 * with the transition described in *out_event.
 */
bool presence_update(presence_t *p, const aiot_ai_result_t *result,
                     int64_t now_us, presence_event_t *out_event);

/*
 * Time-only re-evaluation (exit timeout) for periods without frames.
 * Same contract as presence_update.
 */
bool presence_tick(presence_t *p, int64_t now_us, presence_event_t *out_event);

const char *presence_state_name(presence_state_t state);

/* JSON encoders for the product's MQTT payloads. Return bytes written
 * (excluding NUL), or -1 when the buffer is too small. */
int presence_event_json(const presence_event_t *event, const char *device_id,
                        int64_t now_us, char *buf, size_t cap);
int presence_heartbeat_json(const presence_t *p, const char *device_id,
                            int64_t now_us, int wifi_rssi_dbm,
                            uint32_t heap_free, char *buf, size_t cap);
