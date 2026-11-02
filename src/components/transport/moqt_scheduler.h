#ifndef AIOT_MOQT_SCHEDULER_H
#define AIOT_MOQT_SCHEDULER_H
#include <stddef.h>
#include <stdint.h>
#define MOQT_SCHED_SLOTS 6
#define MOQT_SCHED_QUANTUM 1024U
typedef enum {
    MOQT_FIFO,
    MOQT_AI_FIRST,
    MOQT_DRR,
    MOQT_EDF,
    MOQT_IMPORTANCE_EDF,
    MOQT_DRAFT21_PRIORITY,
    MOQT_POLICY_COUNT
} moqt_policy;
typedef struct {
    uint64_t admitted_us, deadline_us, group_id;
    size_t remaining;
    unsigned importance;
} moqt_job;
typedef struct {unsigned turn;size_t credit[2];} moqt_sched;
/* Bits 0..2 are protocol control, 3 is AI, 4..5 are preview. Ready excludes
 * ACK-held/flow-blocked streams. Choose does not charge proposed bytes. */
int moqt_sched_pick(moqt_sched *,moqt_policy,unsigned ready,const moqt_job[6],size_t *limit);
void moqt_sched_commit(moqt_sched *,moqt_policy,unsigned slot,size_t accepted);
const char *moqt_policy_name(moqt_policy);

/* Scheduling on the wire priorities of draft-ietf-moq-transport-21 section 5.1.2.
 * Unlike the slot policies above, this works on any number of pending streams. */
#include <stdbool.h>
enum { MOQT_CLASS_CONTROL, MOQT_CLASS_REQUEST, MOQT_CLASS_OBJECT };
typedef struct {
    uint8_t klass;
    uint8_t subscriber_priority, publisher_priority; /* lower number is served first */
    uint8_t group_order;                             /* 1 ascending, 2 descending */
    bool fill, fill_order_differs, datagram;
    uint32_t subscription;                           /* identity of the request */
    uint64_t group, subgroup, object, admitted_us;
} moqt_wire_job;
bool moqt_wire_before(const moqt_wire_job *a, const moqt_wire_job *b);
/* Index of the job to serve among those whose bit is set in `ready`; -1 when none. */
int moqt_wire_pick(const moqt_wire_job *jobs, unsigned count, uint64_t ready);
#endif
