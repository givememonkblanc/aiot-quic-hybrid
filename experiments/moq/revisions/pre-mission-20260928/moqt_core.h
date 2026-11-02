#ifndef AIOT_MOQT_CORE_H
#define AIOT_MOQT_CORE_H
/* Transport-independent MOQT draft-21 endpoint for an Original Publisher.
 * The core owns protocol state and wire encoding; the adapter owns QUIC streams,
 * payload memory and scheduling. No dynamic allocation. Section numbers refer to
 * draft-ietf-moq-transport-21. Relay behaviour (section 7) is out of scope. */
#include "moqt21.h"
#define MOQT_CORE_TRACKS 4
#define MOQT_CORE_SUBS 6
#define MOQT_CORE_FETCHES 3
#define MOQT_CORE_NS_SUBS 2
#define MOQT_CORE_STREAMS 12
#define MOQT_CORE_RX 1024
#define MOQT_CORE_OUT 4096
#define MOQT_CORE_ACTIONS 24
#define MOQT_CORE_CACHE 48
#define MOQT_CORE_PROPS 48
#define MOQT_CORE_HEAD 112
#define MOQT_CORE_MAX_UPDATES 4
#define MOQT_CORE_PREFIX_FIELDS 4
#define MOQT_CORE_PREFIX_FIELD 32
#define MOQT_CORE_IMPLEMENTATION "aiot-moqt21/2"

typedef struct {
 const char *ns[4]; uint8_t ns_count; const char *name;
 uint8_t priority;      /* DEFAULT_PUBLISHER_PRIORITY, section 10.4; 0 selects 128 */
 uint8_t group_order;   /* DEFAULT_PUBLISHER_GROUP_ORDER, section 10.5; 0 selects Ascending */
 uint32_t object_timeout_ms,subgroup_timeout_ms,max_cache_ms;
 bool dynamic_groups;
 uint32_t source_timeout_ms; /* extension: SOURCE_DELIVERY_TIMEOUT, counted from sensing */
} moqt_track_cfg;

typedef enum {
 MOQT_ACT_NONE,
 MOQT_ACT_SEND,           /* stream, data, len, fin */
 MOQT_ACT_OPEN_BIDI,      /* token, data, len: open a request stream, then moqt_core_bidi_opened */
 MOQT_ACT_FETCH_START,    /* index: open a unidirectional stream and drain moqt_core_fetch_next */
 MOQT_ACT_FETCH_CANCEL,   /* index: reset the fetch data stream */
 MOQT_ACT_FILL_FAILED,    /* index (subscription), data, len, code: open a stream, send data, reset */
 MOQT_ACT_SUB_ENDING,     /* index, code: close its data streams, then moqt_core_sub_finish */
 MOQT_ACT_SUB_CANCELLED,  /* index, stream: reset its data streams and the request stream */
 MOQT_ACT_RESET_STREAM,   /* stream, code */
 MOQT_ACT_STOP_SENDING,   /* stream, code */
 MOQT_ACT_CLOSE_SESSION,  /* code */
 MOQT_ACT_SUBSCRIBED,     /* index, track */
 MOQT_ACT_UPDATED,        /* index, track */
 MOQT_ACT_NEW_GROUP,      /* track: the application should start a new Group */
 MOQT_ACT_GOAWAY,         /* code is the timeout in ms; data is the new session URI */
 MOQT_ACT_PEER_FIN,       /* index: the subscriber closed its side of the request stream */
 MOQT_ACT_NAMESPACE_RESULT,/* code: 0 accepted, otherwise REQUEST_ERROR code + 1 */
 MOQT_ACT_REQUEST_CLOSED   /* stream: both directions of a finished request stream are closed */
} moqt_act_kind;
typedef struct {
 moqt_act_kind kind; int64_t stream; uint64_t code; unsigned index,track,token;
 const uint8_t *data; size_t len; bool fin;
} moqt_action;

enum { MOQT_SUB_FREE, MOQT_SUB_PENDING, MOQT_SUB_ESTABLISHED, MOQT_SUB_ENDING };
typedef struct {
 uint8_t state,track,forward,subscriber_priority,group_order;
 bool by_publish,early;
 int64_t stream; uint64_t request_id,done_status,streams;
 uint64_t object_timeout_ms,subgroup_timeout_ms,source_timeout_ms;
 uint64_t expires_at_ms; /* 0: the subscription does not expire */
 moqt_locfilter location; bool has_location;
 moqt_window window; moqt_filters filters;
} moqt_sub;
enum { MOQT_FETCH_FREE, MOQT_FETCH_READY, MOQT_FETCH_STREAMING, MOQT_FETCH_DRAINED };
typedef struct {
 uint8_t state,track,priority;
 bool descending,header_sent,started;
 int sub; /* -1 for FETCH, otherwise the subscription that owns a fill fetch stream */
 int64_t stream; uint64_t request_id;
 moqt_window window; moqt_filters filters;
 moqt_fetch_prev prev; moqt_loc cursor;
} moqt_fetch;
typedef struct {
 bool used,datagram; uint8_t track,priority;
 uint64_t group,object,subgroup,stored_ms,serial,source_age_ms;
 bool has_source_age;
 uint16_t props_len; uint8_t props[MOQT_CORE_PROPS];
 void *payload; size_t len;
} moqt_cached;
typedef struct {
 bool used,tracks; int64_t stream; uint64_t request_id;
 uint8_t count,len[MOQT_CORE_PREFIX_FIELDS],field[MOQT_CORE_PREFIX_FIELDS][MOQT_CORE_PREFIX_FIELD];
 uint8_t announced; /* bit per track whose namespace was sent in NAMESPACE */
 uint8_t forward,subscriber_priority,group_order;
 uint64_t object_timeout_ms,subgroup_timeout_ms,source_timeout_ms;
 moqt_locfilter location; bool has_location;
 moqt_filters filters;
} moqt_ns_sub;
enum {
 MOQT_STREAM_FREE, MOQT_STREAM_CONTROL, MOQT_STREAM_REQUEST, MOQT_STREAM_SUB, MOQT_STREAM_FETCH,
 MOQT_STREAM_NAMESPACES, MOQT_STREAM_TRACKS, MOQT_STREAM_CLOSED, MOQT_STREAM_OWN_PUBLISH,
 MOQT_STREAM_OWN_NAMESPACE, MOQT_STREAM_IGNORED
};
typedef struct {
 uint8_t kind,index; bool peer_fin,goaway,typed;
 int64_t id; uint64_t serial; uint16_t len; uint8_t buf[MOQT_CORE_RX];
} moqt_stream;
typedef struct {
 moqt_track_cfg cfg; bool has_largest,ended; moqt_loc largest;
 uint8_t props[MOQT_CORE_PROPS+8]; uint8_t props_len;
} moqt_track;

typedef struct moqt_core {
 bool is_client,setup_sent,peer_setup,going_away,closed;
 uint64_t subscription_lifetime_ms; /* EXPIRES sent with every subscription; 0 disables */
 moqt_setup_ext ext;                 /* extensions this endpoint advertises; all off by default */
 uint64_t source_expired;            /* deliveries skipped because the source was already too old */
 uint64_t now_ms;
 uint64_t close_code;
 moqt_setup_options peer;
 moqt_track tracks[MOQT_CORE_TRACKS]; unsigned track_count;
 moqt_sub subs[MOQT_CORE_SUBS];
 moqt_fetch fetches[MOQT_CORE_FETCHES];
 moqt_ns_sub ns_subs[MOQT_CORE_NS_SUBS];
 moqt_stream streams[MOQT_CORE_STREAMS]; uint64_t stream_serial;
 moqt_cached cache[MOQT_CORE_CACHE]; uint64_t cache_serial;
 uint64_t next_request_id;               /* own requests */
 uint64_t peer_request_max; bool peer_request_any; uint64_t peer_request_seen[4]; /* 256-ID window */
 int64_t namespace_stream; uint8_t namespace_state; /* 0 idle, 1 sent, 2 accepted, 3 rejected */
 moqt_action actions[MOQT_CORE_ACTIONS]; unsigned act_head,act_count;
 uint8_t out[MOQT_CORE_OUT]; size_t out_len;
} moqt_core;

/* Object offered by the application. */
typedef struct {
 uint64_t group,object,subgroup;
 uint8_t priority;       /* used when has_priority; otherwise the track default applies */
 uint8_t status;         /* MOQT_STATUS_* */
 bool has_priority,end_of_group,datagram;
 const uint8_t *props; size_t props_len; /* serialized Object Properties */
 size_t payload_len;
 bool has_source_age; uint64_t source_age_ms; /* extension: time since sensing at this call */
} moqt_offer;
/* One delivery per matching subscription. `head` precedes the payload on the wire. */
typedef struct {
 unsigned sub; uint64_t alias,request_id;
 uint8_t subscriber_priority,publisher_priority,group_order;
 bool datagram;
 uint64_t object_timeout_ms,subgroup_timeout_ms; /* effective values, 0 means none */
 uint64_t source_timeout_ms,source_age_ms;       /* extension; the object expires when age reaches the timeout */
 uint8_t head[MOQT_CORE_HEAD]; size_t head_len;
} moqt_delivery;

void moqt_core_init(moqt_core *,const moqt_track_cfg *tracks,unsigned count,bool is_client);
/* Extensions to advertise in SETUP. Call after moqt_core_init and before moqt_core_setup.
 * Limits above the capacity of the core are reduced to that capacity. */
void moqt_core_extensions(moqt_core *,const moqt_setup_ext *);
/* Own SETUP message for the control stream. */
size_t moqt_core_setup(moqt_core *,uint8_t *dst,size_t cap,const char *path,const char *authority);
/* Bytes received on any stream opened by the peer or on an own request stream. */
void moqt_core_rx(moqt_core *,int64_t stream,const uint8_t *data,size_t len,bool fin,uint64_t now_ms);
/* The peer reset a request stream or asked to stop sending on it. */
void moqt_core_stream_cancelled(moqt_core *,int64_t stream);
bool moqt_core_next_action(moqt_core *,moqt_action *);

/* Publisher-initiated requests. Negative return: no capacity. */
int moqt_core_publish_track(moqt_core *,unsigned track,bool forward,bool early);
int moqt_core_publish_namespace(moqt_core *);
void moqt_core_bidi_opened(moqt_core *,unsigned token,int64_t stream);
int moqt_core_goaway(moqt_core *,uint8_t *dst,size_t cap,uint64_t timeout_ms);

/* Data plane. Returns the number of deliveries written, or -1 for an invalid object. */
int moqt_core_publish(moqt_core *,unsigned track,const moqt_offer *,uint64_t now_ms,moqt_delivery *out,unsigned cap);
/* The adapter opened (or failed to open) the data stream of a delivery. */
void moqt_core_stream_opened(moqt_core *,unsigned sub);
/* Retains an object for FETCH. 0 retained, -1 not retained. `evicted` receives the payload
 * handle that was displaced, or NULL. */
int moqt_core_cache_put(moqt_core *,unsigned track,const moqt_offer *,void *payload,uint64_t now_ms,void **evicted);
/* Evicts the oldest retained object of a track (any track when track >= count). */
void *moqt_core_cache_evict(moqt_core *,unsigned track);
unsigned moqt_core_cache_count(const moqt_core *,unsigned track);
/* Drops retained objects older than the track's max_cache_ms; one handle per call. */
void *moqt_core_cache_expire(moqt_core *,uint64_t now_ms);

/* Fetch streaming. Returns 1 when an object head was produced (payload follows),
 * 0 when the stream is complete (send FIN), -1 when the fetch is not active. */
int moqt_core_fetch_next(moqt_core *,unsigned fetch,uint8_t *head,size_t cap,size_t *head_len,void **payload,size_t *payload_len);
void moqt_core_fetch_closed(moqt_core *,unsigned fetch);

/* Subscription termination (sections 9.9, 12.4). */
void moqt_core_sub_end(moqt_core *,unsigned sub,uint64_t status);
/* All data streams of the subscription are closed: emits PUBLISH_DONE and FIN. */
void moqt_core_sub_finish(moqt_core *,unsigned sub,const char *reason,size_t reason_len);
/* Ends every subscription of a track with TRACK_ENDED and announces NAMESPACE_DONE when
 * no track of that namespace remains. */
void moqt_core_track_end(moqt_core *,unsigned track);
/* Section 9.20.17: ends subscriptions whose lifetime passed with PUBLISH_DONE EXPIRED. */
void moqt_core_tick(moqt_core *,uint64_t now_ms);
/* Section 9.10: reports the Forward State and Largest Object now in effect. */
int moqt_core_state_notify(moqt_core *,unsigned sub);
unsigned moqt_core_active_subs(const moqt_core *,unsigned track);
bool moqt_core_any_forwarding(const moqt_core *,unsigned track);
#endif
