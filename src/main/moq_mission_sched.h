#ifndef AIOT_MOQ_MISSION_SCHED_H
#define AIOT_MOQ_MISSION_SCHED_H
/* Mission-utility scheduling of evidence.
 *
 * The device holds the source data of the latest events. Each can be sent in one of several
 * representations, which differ in size and in the quality that the receiver obtains. An
 * evidence object is worth its quality if it arrives before the evidence deadline of its
 * event and nothing afterwards. The planner assigns a representation, or none, to every
 * open event so that the sum of quality is largest while every planned object still finishes
 * before its deadline, in deadline order. An event that cannot be served in time receives
 * nothing, so no bytes are spent on it.
 *
 * The service rate is known with its deviation only. The plan is therefore valued in
 * MUS_SCENARIOS rates around the estimate, weighted as a normal distribution: an object
 * counts with the weight of the rates at which it arrives in time. A large representation
 * that probably arrives is preferred to a small one that certainly does when its quality
 * is worth the risk.
 *
 * The plan follows two resources. The link carries one object at a time at the estimated
 * service rate; bytes that were sent and not acknowledged occupy it even after their object
 * was abandoned. The codec produces one representation at a time into one buffer, which is
 * held until the link takes the object; a representation is therefore prepared while the
 * object before it is still under way.
 *
 * The assignment is found by exhaustive search over the latest MUS_SEARCH events.
 * Portable, no allocation. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define MUS_EVENTS 8
#define MUS_REPS 3
#define MUS_SEARCH 5
#define MUS_SCENARIOS 5
typedef struct {
 uint32_t quality[MUS_REPS];   /* quality of a representation, ascending, in thousandths */
 uint32_t prepare_us[MUS_REPS];/* time to produce the representation; 0: it is the source */
 uint32_t margin_permille;     /* share of the estimated rate that the plan relies on */
 uint32_t guard_us;            /* slack required before a deadline */
 uint32_t probe_interval_us;   /* least time between two probes */
 uint32_t keep_permille;       /* quality that a new plan must gain before the object under way is abandoned */
 bool sequential;              /* a representation is prepared only when the link is free: for comparison */
} mus_config;
typedef struct {
 uint32_t seq;
 uint64_t deadline_us;
 size_t size[MUS_REPS];        /* bytes of each representation; 0: not available */
 int8_t best;                  /* best representation delivered so far; -1: none */
 int8_t active;                /* representation in transmission; -1: none */
 int8_t prepared;              /* representation that waits in the buffer of the codec; -1: none */
 int8_t converting;            /* representation that the codec is producing; -1: none */
 size_t acked;                 /* bytes of the active object that were acknowledged */
 size_t unacked;               /* bytes of the active object that were sent and not acknowledged */
 bool probe;                   /* the object of this event is a probe and is not abandoned */
} mus_event;
typedef struct {
 uint64_t rate_Bps;            /* estimated service rate */
 uint64_t deviation_Bps;       /* mean deviation of the rate samples; 0: the plan relies on the estimate alone */
 size_t drain;                 /* bytes of abandoned objects that are still in the network */
 uint64_t codec_free_us;       /* time at which the codec finishes; at most now when it is idle */
} mus_link;
typedef enum { MUS_IDLE, MUS_CONTINUE, MUS_START, MUS_ABORT } mus_action;
typedef struct {
 mus_action action;
 int event;                    /* START: the next object for the link; CONTINUE, ABORT: the one under way */
 int rep;
 bool probe;
 int prepare_event;            /* representation that the codec should produce now; -1: none */
 int prepare_rep;
 int8_t plan[MUS_EVENTS];      /* planned representation of every event; -1: none */
} mus_decision;
/* `events` in any order; `last_probe_us` is the time of the last probe or 0. START names
 * the next object even when its representation is not prepared yet. */
mus_decision mus_plan(const mus_config *,const mus_event *events,unsigned count,uint64_t now_us,
                      const mus_link *link,uint64_t last_probe_us);
#endif
