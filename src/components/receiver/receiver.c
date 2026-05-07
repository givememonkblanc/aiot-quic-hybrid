#include "include/receiver.h"
#include <string.h>
#include "../common/include/data_packet.h"
#include "../common/include/image_chunk.h"
#include "../common/include/ai_result.h"
#include "esp_log.h"

static const char *TAG = "RECEIVER";

void receiver_init(receiver_t *receiver)
{
    memset(receiver, 0, sizeof(*receiver));
}

void receiver_handle_event(receiver_t *receiver,
    quic_port_event_t event,
    const uint8_t *payload,
    uint16_t payload_len)
{
    (void)receiver;
    (void)event;
    (void)payload;
    (void)payload_len;
}
