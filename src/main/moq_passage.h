#ifndef AIOT_MOQ_PASSAGE_H
#define AIOT_MOQ_PASSAGE_H
/* Counts passage events: one object crossing a counting line once in the configured
 * direction. Detections of successive inference results are associated with the nearest
 * track; an object seen in several results is counted once. Portable, no allocation.
 *
 * The count is only as good as the detections it is given. Objects that move farther
 * than `max_step` between two results are not followed. With `predict` and `count_unseen`
 * the counter assumes one direction of flow, as on a turntable or a conveyor. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define MOQ_PASSAGE_TRACKS 24
#define MOQ_PASSAGE_EVENTS 8
typedef struct {
 uint8_t line;      /* position of the counting line on the chosen axis, 0..255 */
 bool vertical_axis;/* false: compare x; true: compare y */
 int8_t direction;  /* +1 counts increasing coordinates, -1 decreasing, 0 both */
 uint8_t max_step;  /* largest displacement between two results for one object */
 uint8_t max_missed;/* results a track may go unmatched before it is dropped */
 bool predict;      /* objects flow in the counted direction: a track is looked for where its
                     * velocity puts it, which keeps close objects and missed results apart */
 bool count_unseen; /* an object first seen less than one step past the line crossed it unseen */
 bool settled;      /* ...once the flow is known: at the start every object is new, and one
                     * just past the line had not crossed it */
 uint8_t assoc;     /* association (AIOT_MOQT_PASSAGE_ASSOC): 0 greedy nearest pair, result-count
                     * velocity along the axis; 1 minimum-cost assignment of all pairs, per-track
                     * 2-D velocity per second of capture time, tracks coast while their predicted
                     * travel stays within max_step. Needs moq_passage_update_at. */
} moq_passage_cfg;
typedef struct { uint32_t id,source_sequence; uint8_t x,y; int8_t direction; } moq_passage_event;
typedef struct {
 moq_passage_cfg cfg;
 struct { bool used,counted,moving; uint8_t x,y,missed; int16_t v;
          int32_t va,vl; uint32_t ms; } tracks[MOQ_PASSAGE_TRACKS]; /* assoc 1: velocity along and across
                     * the axis in units per second x16, capture time last seen */
 int16_t flow;bool has_flow; /* displacement per result along the counting axis, smoothed
                     * (assoc 1: units per second) */
 int32_t flow_a,flow_l; /* assoc 1: flow along and across the axis, units per second x16 */
 uint32_t last_ms;bool has_last; /* assoc 1: capture time of the previous result */
 uint32_t total;    /* passage events so far; also the ID of the latest event */
 uint32_t dropped_detections; /* detections without a free track */
} moq_passage_counter;
typedef struct { uint8_t x,y; } moq_passage_point;
void moq_passage_init(moq_passage_counter *,const moq_passage_cfg *);
/* Feeds the detections of one inference result. Returns the number of new passage events,
 * at most MOQ_PASSAGE_EVENTS of which are described in `events`. */
unsigned moq_passage_update(moq_passage_counter *,const moq_passage_point *points,unsigned count,
                            uint32_t source_sequence,moq_passage_event *events);
/* The same with the capture time of the frame in ms (increasing); assoc 0 ignores it. */
unsigned moq_passage_update_at(moq_passage_counter *,const moq_passage_point *points,unsigned count,
                               uint32_t source_sequence,uint32_t capture_ms,moq_passage_event *events);
#endif
