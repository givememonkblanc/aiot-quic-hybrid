#include "include/transport_router.h"
#include <stdio.h>

void transport_router_init(transport_router_t *router, void *quic_port)
{
    router->quic_port = quic_port;
}

void transport_router_route(transport_router_t *router, const data_packet_t *packet, const schedule_decision_t *decision)
{
    (void)router;
    if (decision->mode == TX_MODE_DROP) {
        printf("[TRANSPORT-ROUTER] drop type=%u seq=%lu\n",
            packet->type,
            (unsigned long)packet->sequence);
        return;
    }
    printf("[TRANSPORT-ROUTER] tx mode=%u type=%u seq=%lu\n",
        decision->mode,
        packet->type,
        (unsigned long)packet->sequence);
}