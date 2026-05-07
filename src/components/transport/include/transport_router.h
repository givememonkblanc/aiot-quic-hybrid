#pragma once

#include "../../common/include/data_packet.h"
#include "../../dl_qads/include/dl_qads.h"

typedef struct {
    void *quic_port;
} transport_router_t;

void transport_router_init(transport_router_t *router, void *quic_port);
void transport_router_route(transport_router_t *router, const data_packet_t *packet, const schedule_decision_t *decision);
