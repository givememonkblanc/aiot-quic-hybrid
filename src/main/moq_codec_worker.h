#pragma once
#include "moq_representation.h"

typedef struct {
    const uint8_t *input;
    uint8_t *output;
    size_t length,capacity;
    unsigned level;
} moq_codec_request;
typedef struct {
    moq_representation_metrics metrics;
    uint32_t cpu_us;
    bool ok;
} moq_codec_result;

/* Single owner (application task), at most ONE outstanding job. Input/output
 * must remain alive and untouched until take() succeeds or stop() returns.
 * stop joins ongoing work; it never asynchronously deletes the encoder. */
bool moq_codec_worker_start(void);
bool moq_codec_worker_submit(const moq_codec_request *request);
bool moq_codec_worker_take(moq_codec_result *result);
bool moq_codec_worker_busy(void);
void moq_codec_worker_stop(void);
moq_codec_result moq_codec_execute(const moq_codec_request *request);
