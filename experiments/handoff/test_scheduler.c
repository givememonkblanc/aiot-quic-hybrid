#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mobility_scheduler.h"
#include "quic_stream_buffer.h"

int main(void)
{
    mobility_scheduler_t s;
    mobility_scheduler_init(&s);
    mobility_object_t normal = {.seq=1, .generated_us=100, .len=1};
    mobility_object_t critical = {.seq=2, .generated_us=100, .critical=true, .len=1};
    assert(mobility_scheduler_push(&s, &normal, 2));
    assert(mobility_scheduler_push(&s, &critical, 2));
    assert(mobility_scheduler_pick(&s, 2, 200) == -1);
    mobility_scheduler_update(&s, true, true, true, -40, 20, 200);
    assert(s.state == MOB_VALIDATING);
    assert(mobility_scheduler_pick(&s, 2, 200) == -1);
    mobility_scheduler_update(&s, true, false, true, -40, 20, 300);
    assert(s.state == MOB_RECOVERY);
    assert(mobility_scheduler_pick(&s, 1, 300) == 0); /* FIFO baseline */
    assert(mobility_scheduler_pick(&s, 2, 300) == 1); /* critical first */
    mobility_scheduler_commit(&s, 1, 2, 300);
    assert(mobility_scheduler_pick(&s, 2, 301) == -1); /* pacing */
    assert(mobility_scheduler_pick(&s, 2, 500100) == -1); /* telemetry expired */
    assert(s.expired == 1 && s.count == 0);
    for (int i=0; i<3; i++) mobility_scheduler_update(&s, true, false, true, -80, 200, 3000000+i);
    assert(s.state == MOB_PREPARE);
    for (int i=0; i<5; i++) mobility_scheduler_update(&s, true, false, true, -40, 20, 4000000+i);
    assert(s.state == MOB_NORMAL);
    for (int i=0; i<MOBILITY_QUEUE_CAPACITY; i++) assert(mobility_scheduler_push(&s, &normal, 2));
    assert(mobility_scheduler_push(&s, &critical, 2));
    assert(s.count == MOBILITY_QUEUE_CAPACITY && s.overflow == 1);
    mobility_scheduler_init(&s);
    for (int i=0; i<MOBILITY_QUEUE_CAPACITY; i++) assert(mobility_scheduler_push(&s, &critical, 2));
    assert(!mobility_scheduler_push(&s, &critical, 2));
    assert(s.critical_overflow == 1);

    uint8_t storage[8], *p;
    quic_stream_buffer_t b = {.data=storage, .capacity=sizeof(storage)};
    assert(quic_stream_buffer_push(&b, (const uint8_t *)"abcdef", 6));
    assert(quic_stream_buffer_pending(&b, &p) == 6 && memcmp(p, "abcdef", 6) == 0);
    b.submitted += 4; /* ngtcp2 accepted only a partial write */
    assert(!quic_stream_buffer_push(&b, (const uint8_t *)"XYZ", 3)); /* unACKed retained */
    assert(!quic_stream_buffer_ack(&b, 0, 5));
    assert(quic_stream_buffer_ack(&b, 0, 4));
    assert(quic_stream_buffer_push(&b, (const uint8_t *)"WXYZ", 4));
    assert(quic_stream_buffer_pending(&b, &p) == 4 && memcmp(p, "efWX", 4) == 0);
    b.submitted += 4;
    assert(quic_stream_buffer_ack(&b, 4, 4));
    assert(quic_stream_buffer_pending(&b, &p) == 2 && memcmp(p, "YZ", 2) == 0);
    b.submitted += 2;
    assert(quic_stream_buffer_ack(&b, 8, 2));
    assert(!quic_stream_buffer_ack(&b, 0, 1));
    puts("PASS: mobility state, priorities, expiry, overflow, pacing, stream partial writes/ACK lifetime/wrap");
}
