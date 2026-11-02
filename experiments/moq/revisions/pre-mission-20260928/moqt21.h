#ifndef AIOT_MOQT21_H
#define AIOT_MOQT21_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Draft-21 scoped wire codec. All readers: 1=complete, 0=need more, -1=invalid. */
#define MOQT21_ALPN "moqt-21"
#define MOQT21_CONTROL_CAP 2048
#define MOQT21_MAX_JPEG (256U*1024U)
/* draft-ietf-moq-transport-21 section 12.5. */
#define MOQT21_STREAM_RESET_DELIVERY_TIMEOUT 0x2U
size_t moqt21_vi_write(uint8_t *dst, size_t cap, uint64_t value);
int moqt21_vi_read(const uint8_t *src, size_t len, uint64_t *value, size_t *used);
size_t moqt21_message(uint8_t *dst,size_t cap,uint64_t type,const uint8_t *body,size_t len);
int moqt21_message_read(const uint8_t *src,size_t len,uint64_t *type,const uint8_t **body,size_t *body_len,size_t *used);
size_t moqt21_setup(uint8_t *dst,size_t cap,const char *authority);
/* Validate peer server SETUP options; rejects server PATH/AUTHORITY. */
int moqt21_server_setup(const uint8_t *body,size_t len);
/* Minimal unfiltered, zero-parameter subscription to known namespace. */
int moqt21_subscribe(const uint8_t *body,size_t len,uint64_t *request);
size_t moqt21_subscribe_ok(uint8_t *dst,size_t cap);
size_t moqt21_object_header(uint8_t *dst,size_t cap,uint64_t group,size_t payload_len);
int moqt21_subscribe_track(const uint8_t *,size_t,uint64_t *,unsigned *alias);
size_t moqt21_subscribe_ok_track(uint8_t *,size_t,unsigned alias);
size_t moqt21_object_header_track(uint8_t *,size_t,unsigned alias,uint64_t group,size_t len);

/* ---- Full draft-21 codec (sections 8-12). Section numbers refer to
 * draft-ietf-moq-transport-21. ---- */

/* Message types, section 9 table 5. */
enum {
 MOQT_MSG_REQUEST_UPDATE=0x2, MOQT_MSG_SUBSCRIBE=0x3, MOQT_MSG_SUBSCRIBE_OK=0x4,
 MOQT_MSG_REQUEST_ERROR=0x5, MOQT_MSG_PUBLISH_NAMESPACE=0x6, MOQT_MSG_REQUEST_OK=0x7,
 MOQT_MSG_NAMESPACE=0x8, MOQT_MSG_PUBLISH_DONE=0xB, MOQT_MSG_TRACK_STATUS=0xD,
 MOQT_MSG_NAMESPACE_DONE=0xE, MOQT_MSG_PUBLISH_SKIPPED=0xF, MOQT_MSG_GOAWAY=0x10,
 MOQT_MSG_FETCH=0x16, MOQT_MSG_FETCH_OK=0x18, MOQT_MSG_PUBLISH=0x1D,
 MOQT_MSG_PUBLISH_STATE_NOTIFY=0x22, MOQT_MSG_SUBSCRIBE_NAMESPACE=0x50,
 MOQT_MSG_SUBSCRIBE_TRACKS=0x51, MOQT_MSG_SETUP=0x2F00
};
/* Unidirectional stream types, section 6.4.1. */
#define MOQT_STREAM_FETCH_HEADER 0x05U
#define MOQT_STREAM_PADDING 0x132B3E28U
/* Session termination codes, section 12.2. */
enum {
 MOQT_S_NO_ERROR=0x0, MOQT_S_INTERNAL_ERROR=0x1, MOQT_S_UNAUTHORIZED=0x2,
 MOQT_S_PROTOCOL_VIOLATION=0x3, MOQT_S_INVALID_REQUEST_ID=0x4,
 MOQT_S_DUPLICATE_TRACK_ALIAS=0x5, MOQT_S_KEY_VALUE_FORMATTING_ERROR=0x6,
 MOQT_S_INVALID_PATH=0x8, MOQT_S_MALFORMED_PATH=0x9, MOQT_S_GOAWAY_TIMEOUT=0x10,
 MOQT_S_CONTROL_MESSAGE_TIMEOUT=0x11, MOQT_S_DATA_STREAM_TIMEOUT=0x12,
 MOQT_S_AUTH_TOKEN_CACHE_OVERFLOW=0x13, MOQT_S_DUPLICATE_AUTH_TOKEN_ALIAS=0x14,
 MOQT_S_MALFORMED_AUTH_TOKEN=0x16, MOQT_S_UNKNOWN_AUTH_TOKEN_ALIAS=0x17,
 MOQT_S_EXPIRED_AUTH_TOKEN=0x18, MOQT_S_INVALID_AUTHORITY=0x19,
 MOQT_S_MALFORMED_AUTHORITY=0x1A, MOQT_S_TOO_MANY_REQUEST_UPDATES=0x1B
};
/* REQUEST_ERROR codes, section 16.11.2. */
enum {
 MOQT_R_INTERNAL_ERROR=0x0, MOQT_R_UNAUTHORIZED=0x1, MOQT_R_TIMEOUT=0x2,
 MOQT_R_NOT_SUPPORTED=0x3, MOQT_R_MALFORMED_AUTH_TOKEN=0x4, MOQT_R_EXPIRED_AUTH_TOKEN=0x5,
 MOQT_R_GOING_AWAY=0x6, MOQT_R_EXCESSIVE_LOAD=0x9, MOQT_R_DOES_NOT_EXIST=0x10,
 MOQT_R_INVALID_RANGE=0x11, MOQT_R_MALFORMED_TRACK=0x12, MOQT_R_UNINTERESTED=0x20,
 MOQT_R_PREFIX_OVERLAP=0x30, MOQT_R_NAMESPACE_TOO_LARGE=0x31,
 MOQT_R_UNSUPPORTED_EXTENSION=0x33, MOQT_R_REDIRECT=0x34,
 MOQT_R_CONFLICTING_FILTERS=0x35, MOQT_R_INVALID_FILTER=0x36
};
/* PUBLISH_DONE codes, section 16.11.3. */
enum {
 MOQT_D_INTERNAL_ERROR=0x0, MOQT_D_UNAUTHORIZED=0x1, MOQT_D_TRACK_ENDED=0x2,
 MOQT_D_GOING_AWAY=0x4, MOQT_D_TOO_FAR_BEHIND=0x5, MOQT_D_EXPIRED=0x6,
 MOQT_D_UPDATE_FAILED=0x8, MOQT_D_EXCESSIVE_LOAD=0x9, MOQT_D_MALFORMED_TRACK=0x12
};
/* Stream reset codes, section 12.5. */
enum {
 MOQT_RST_INTERNAL_ERROR=0x0, MOQT_RST_CANCELLED=0x1, MOQT_RST_DELIVERY_TIMEOUT=0x2,
 MOQT_RST_SESSION_CLOSED=0x3, MOQT_RST_GOING_AWAY=0x4, MOQT_RST_TOO_FAR_BEHIND=0x5,
 MOQT_RST_UNKNOWN_OBJECT_STATUS=0x6, MOQT_RST_EXPIRED_AUTH_TOKEN=0x7,
 MOQT_RST_EXCESSIVE_LOAD=0x9, MOQT_RST_MALFORMED_TRACK=0x12
};
/* Setup options, section 9.1. */
enum {
 MOQT_OPT_PATH=0x01, MOQT_OPT_AUTHORIZATION_TOKEN=0x03, MOQT_OPT_MAX_AUTH_TOKEN_CACHE_SIZE=0x04,
 MOQT_OPT_AUTHORITY=0x05, MOQT_OPT_MAX_FILTER_RANGES=0x06, MOQT_OPT_IMPLEMENTATION=0x07,
 MOQT_OPT_MAX_REQUEST_UPDATES=0x08
};
/* Message parameters, section 16.7. */
enum {
 MOQT_P_OBJECT_DELIVERY_TIMEOUT=0x02, MOQT_P_AUTHORIZATION_TOKEN=0x03,
 MOQT_P_RENDEZVOUS_TIMEOUT=0x04, MOQT_P_SUBGROUP_DELIVERY_TIMEOUT=0x06, MOQT_P_EXPIRES=0x08,
 MOQT_P_LARGEST_OBJECT=0x09, MOQT_P_FILL_TIMEOUT=0x0A, MOQT_P_FORWARD=0x10,
 MOQT_P_SUBSCRIBER_PRIORITY=0x20, MOQT_P_LOCATION_FILTER=0x21, MOQT_P_GROUP_ORDER=0x22,
 MOQT_P_FILL_PARAMETERS=0x23, MOQT_P_SUBGROUP_FILTER=0x25, MOQT_P_OBJECTID_FILTER=0x26,
 MOQT_P_PRIORITY_FILTER=0x27, MOQT_P_OBJECT_PROPERTY_FILTER=0x28,
 MOQT_P_TRACK_PROPERTY_FILTER=0x29, MOQT_P_NEW_GROUP_REQUEST=0x32,
 MOQT_P_TRACK_NAMESPACE_PREFIX=0x34, MOQT_P_INCLUDE_PROPERTIES=0x35
};
/* Properties, section 16.8. */
enum {
 MOQT_X_OBJECT_DELIVERY_TIMEOUT=0x02, MOQT_X_MAX_CACHE_DURATION=0x04,
 MOQT_X_SUBGROUP_DELIVERY_TIMEOUT=0x06, MOQT_X_IMMUTABLE=0x0B,
 MOQT_X_DEFAULT_PUBLISHER_PRIORITY=0x0E, MOQT_X_DEFAULT_PUBLISHER_GROUP_ORDER=0x22,
 MOQT_X_DYNAMIC_GROUPS=0x30, MOQT_X_PRIOR_GROUP_ID_GAP=0x3C, MOQT_X_PRIOR_OBJECT_ID_GAP=0x3E
};
/* Object status, section 11.1.2. */
enum { MOQT_STATUS_NORMAL=0x0, MOQT_STATUS_END_OF_GROUP=0x3, MOQT_STATUS_END_OF_TRACK=0x4 };
enum { MOQT_ORDER_ASCENDING=0x1, MOQT_ORDER_DESCENDING=0x2 };
/* SUBGROUP_HEADER flags, section 11.3.1. */
#define MOQT_SG_PROPERTIES 0x01U
#define MOQT_SG_ID_MODE_MASK 0x06U
#define MOQT_SG_END_OF_GROUP 0x08U
#define MOQT_SG_ALWAYS 0x10U
#define MOQT_SG_DEFAULT_PRIORITY 0x20U
#define MOQT_SG_FIRST_OBJECT 0x40U
/* OBJECT_DATAGRAM flags, section 11.2.1. */
#define MOQT_DG_PROPERTIES 0x01U
#define MOQT_DG_END_OF_GROUP 0x02U
#define MOQT_DG_ZERO_OBJECT_ID 0x04U
#define MOQT_DG_DEFAULT_PRIORITY 0x08U
#define MOQT_DG_STATUS 0x20U
/* Fetch object serialization flags, section 11.4.1.1. */
#define MOQT_FO_SUBGROUP_MASK 0x03U
#define MOQT_FO_OBJECT_ID 0x04U
#define MOQT_FO_GROUP_ID 0x08U
#define MOQT_FO_PRIORITY 0x10U
#define MOQT_FO_PROPERTIES 0x20U
#define MOQT_FO_DATAGRAM 0x40U
#define MOQT_FO_END_NONEXISTENT 0x8CU
#define MOQT_FO_END_UNKNOWN 0x10CU
#define MOQT_FO_END_TIMED_OUT 0x20CU

/* Extensions proposed by this project for constrained AIoT endpoints. They are not part of
 * draft-ietf-moq-transport-21. Code points are provisional and avoid the grease pattern.
 * See papers/mission_aiot_moq/MOQT_AIOT_EXTENSIONS_KO.md. */
enum {
 MOQT_OPT_MAX_CONTROL_MESSAGE_SIZE=0xA1E0, /* largest control message body the sender accepts */
 MOQT_OPT_MAX_SUBSCRIPTIONS=0xA1E2,        /* concurrent subscriptions the sender serves */
 MOQT_OPT_MAX_FETCHES=0xA1E4,              /* concurrent fetches the sender serves, fill fetches included */
 MOQT_OPT_SOURCE_TIMEOUT=0xA1E8,           /* 1: SOURCE_DELIVERY_TIMEOUT is understood */
 MOQT_P_SOURCE_DELIVERY_TIMEOUT=0xA1EA,    /* message parameter, varint, milliseconds */
 MOQT_X_SOURCE_AGE=0xA1EC,                 /* object property: ms since the source was sensed */
 MOQT_X_SOURCE_DELIVERY_TIMEOUT=0xA1EE     /* track property, milliseconds */
};
/* `limits` argument of the parsers: the low 16 bits are the range-filter limit. */
#define MOQT_LIMIT_SOURCE_TIMEOUT 0x10000U
#define MOQT_LIMIT_RANGES(limits) ((limits)&0xFFFFU)

/* Parse results: 0 accepted; >0 session termination code (close the session);
 * <0 reject the request with REQUEST_ERROR code MOQT_REJECT_CODE(result). */
#define MOQT_REJECT(code) (-(int)(code)-1)
#define MOQT_REJECT_CODE(result) ((unsigned)(-(result)-1))

typedef struct { uint64_t group,object; } moqt_loc;
static inline int moqt_loc_cmp(moqt_loc a,moqt_loc b){
 if(a.group!=b.group)return a.group<b.group?-1:1;
 if(a.object!=b.object)return a.object<b.object?-1:1;
 return 0;
}
typedef struct { const uint8_t *p; size_t n; bool bad; } moqt_rd;
typedef struct { uint8_t *p; size_t cap,n; bool bad; } moqt_wr;
static inline moqt_rd moqt_rd_of(const uint8_t *p,size_t n){return (moqt_rd){p,n,false};}
static inline moqt_wr moqt_wr_of(uint8_t *p,size_t cap){return (moqt_wr){p,cap,0,false};}
uint64_t moqt_rd_vi(moqt_rd *);
uint8_t moqt_rd_u8(moqt_rd *);
const uint8_t *moqt_rd_bytes(moqt_rd *,size_t len);
void moqt_wr_vi(moqt_wr *,uint64_t);
void moqt_wr_u8(moqt_wr *,uint8_t);
void moqt_wr_bytes(moqt_wr *,const void *,size_t);
/* Reserve a 2-byte length; patch it once the body is complete. */
size_t moqt_wr_begin_message(moqt_wr *,uint64_t type);
void moqt_wr_end_message(moqt_wr *,size_t mark);

/* Key-Value-Pairs (section 8.3): properties and setup options. */
typedef struct { uint64_t type; bool is_bytes; uint64_t value; const uint8_t *bytes; size_t len; } moqt_kvp;
typedef struct { moqt_rd rd; uint64_t prev; bool first; } moqt_kvp_it;
static inline moqt_kvp_it moqt_kvp_begin(const uint8_t *p,size_t n){return (moqt_kvp_it){moqt_rd_of(p,n),0,true};}
/* 1 pair produced, 0 end, -1 formatting error. */
int moqt_kvp_next(moqt_kvp_it *,moqt_kvp *);
/* Writer keeps ascending type order; `prev` starts at 0. */
void moqt_kvp_put_int(moqt_wr *,uint64_t *prev,uint64_t type,uint64_t value);
void moqt_kvp_put_bytes(moqt_wr *,uint64_t *prev,uint64_t type,const void *,size_t);
/* Validates a property block; checks nesting and duplicate rules of 10.7-10.9. */
int moqt_props_validate(const uint8_t *,size_t,bool object_scope);
/* Finds an integer property in the mutable list or inside IMMUTABLE_PROPERTIES. */
bool moqt_props_find_int(const uint8_t *,size_t,uint64_t type,uint64_t *value);

/* Track namespace (section 8.7). Fields reference the source buffer. */
#define MOQT_NS_MAX_FIELDS 32
typedef struct { uint8_t count; struct { const uint8_t *p; uint16_t n; } f[MOQT_NS_MAX_FIELDS]; size_t total; } moqt_ns;
int moqt_ns_read(moqt_rd *,moqt_ns *,bool allow_empty);
void moqt_ns_write(moqt_wr *,const char *const *fields,unsigned count);
bool moqt_ns_equals(const moqt_ns *,const char *const *fields,unsigned count);
/* True when `prefix` is a prefix of the namespace given by fields. */
bool moqt_ns_is_prefix(const moqt_ns *prefix,const char *const *fields,unsigned count);

/* Filters (sections 3.3, 8.6, 9.20.10-9.20.15). */
#define MOQT_MAX_RANGES 8
#define MOQT_MAX_RFILTERS 6
typedef struct { uint64_t lo,hi; } moqt_range; /* inclusive; open end is UINT64_MAX */
typedef struct { uint8_t type,set,first,count; uint64_t prop; } moqt_rfilter;
typedef struct { uint8_t fields; uint64_t v[4]; } moqt_locfilter;
typedef struct {
 moqt_rfilter f[MOQT_MAX_RFILTERS]; uint8_t count;
 moqt_range r[MOQT_MAX_RANGES]; uint8_t ranges;
 uint8_t removed; /* bit (type-0x25): an empty filter of that type was received */
} moqt_filters;
typedef struct { uint64_t subgroup,object,priority; bool datagram; const uint8_t *props; size_t props_len; } moqt_filter_subject;
bool moqt_filters_pass(const moqt_filters *,const moqt_filter_subject *);
/* REQUEST_UPDATE semantics: a type present in `update` replaces that type in `base`. */
int moqt_filters_merge(moqt_filters *base,const moqt_filters *update);

/* Resolved location filter. */
typedef struct { bool has_start,has_end,end_whole_group; moqt_loc start,end; } moqt_window;
/* Section 9.20.10. `for_fetch` applies the FETCH rules (end defaults to Largest Object). */
int moqt_window_resolve(const moqt_locfilter *,bool present,bool has_largest,moqt_loc largest,bool for_fetch,moqt_window *);
bool moqt_window_contains(const moqt_window *,moqt_loc);

/* Parameters (section 9.20). */
typedef enum {
 MOQT_CTX_SUBSCRIBE, MOQT_CTX_UPDATE_SUBSCRIPTION, MOQT_CTX_UPDATE_FETCH, MOQT_CTX_UPDATE_NAMESPACE,
 MOQT_CTX_UPDATE_TRACKS, MOQT_CTX_FETCH, MOQT_CTX_TRACK_STATUS, MOQT_CTX_SUBSCRIBE_NAMESPACE,
 MOQT_CTX_SUBSCRIBE_TRACKS, MOQT_CTX_PUBLISH_OK, MOQT_CTX_NAMESPACE_OK, MOQT_CTX_FILL
} moqt_ctx;
#define MOQT_HAS(params,type) (((params)->present>>moqt_param_bit(type))&1U)
unsigned moqt_param_bit(uint64_t type);
typedef struct moqt_params {
 uint32_t present;
 uint64_t object_timeout,subgroup_timeout,rendezvous_timeout,fill_timeout,expires,new_group,source_timeout;
 uint8_t forward,subscriber_priority,group_order,include_properties;
 moqt_locfilter location;
 moqt_filters filters;
 moqt_ns prefix;
 unsigned tokens;
 const uint8_t *fill; size_t fill_len; /* raw FILL_PARAMETERS value */
} moqt_params;
/* `count` parameters follow; with MOQT_CTX_FILL the block is bounded by length. */
int moqt_params_read(moqt_rd *,uint64_t count,moqt_ctx,unsigned limits,moqt_params *);

/* Requests received by a publisher. */
typedef struct {
 uint64_t type,request_id; moqt_ns ns; const uint8_t *name; size_t name_len; moqt_params params;
} moqt_request;
/* SUBSCRIBE, FETCH, TRACK_STATUS, SUBSCRIBE_NAMESPACE, SUBSCRIBE_TRACKS. */
int moqt_request_read(uint64_t type,const uint8_t *body,size_t len,unsigned limits,moqt_request *);
int moqt_update_read(const uint8_t *body,size_t len,moqt_ctx,unsigned limits,uint64_t *request_id,moqt_params *);
/* REQUEST_OK received by a publisher (PUBLISH_OK, PUBLISH_NAMESPACE_OK). */
int moqt_request_ok_read(const uint8_t *body,size_t len,moqt_ctx,unsigned limits,moqt_params *);
typedef struct { uint64_t code,retry; const uint8_t *reason; size_t reason_len; } moqt_error;
int moqt_request_error_read(const uint8_t *body,size_t len,moqt_error *);
typedef struct { const uint8_t *uri; size_t uri_len; uint64_t timeout; } moqt_goaway;
int moqt_goaway_read(const uint8_t *body,size_t len,bool receiver_is_server,moqt_goaway *);
/* Peer SETUP options. */
/* Extension options of one endpoint. A limit applies only when its `has_` flag is set. */
typedef struct {
 bool has_control_message,has_subscriptions,has_fetches,source_timeout;
 uint64_t max_control_message,max_subscriptions,max_fetches;
} moqt_setup_ext;
typedef struct {
 uint64_t max_filter_ranges,max_request_updates,max_token_cache; const uint8_t *implementation; size_t implementation_len;
 moqt_setup_ext ext;
} moqt_setup_options;
size_t moqt_setup_write_ext(uint8_t *,size_t cap,const char *path,const char *authority,const char *implementation,uint64_t max_filter_ranges,uint64_t max_request_updates,const moqt_setup_ext *);
int moqt_setup_read(const uint8_t *body,size_t len,bool from_server,moqt_setup_options *);
size_t moqt_setup_write(uint8_t *,size_t cap,const char *path,const char *authority,const char *implementation,uint64_t max_filter_ranges,uint64_t max_request_updates);

/* Messages sent by a publisher. `props` is a serialized Track Properties block. */
typedef struct { bool has_largest; moqt_loc largest; uint64_t expires; bool has_expires; } moqt_ok_params;
size_t moqt_subscribe_ok_write(uint8_t *,size_t,uint64_t alias,const moqt_ok_params *,const uint8_t *props,size_t props_len);
size_t moqt_request_ok_write(uint8_t *,size_t,const moqt_ok_params *,const uint8_t *props,size_t props_len);
size_t moqt_request_error_write(uint8_t *,size_t,uint64_t code,uint64_t retry,const char *reason);
size_t moqt_publish_done_write(uint8_t *,size_t,uint64_t status,uint64_t streams,const char *reason,size_t reason_len);
size_t moqt_fetch_ok_write(uint8_t *,size_t,bool end_of_track,moqt_loc end,const uint8_t *props,size_t props_len);
typedef struct {
 bool has_forward,has_priority,has_order,has_object_timeout,has_subgroup_timeout,has_largest,has_location,has_source_timeout;
 uint8_t forward,priority,order; uint64_t object_timeout,subgroup_timeout,source_timeout; moqt_loc largest;
 moqt_locfilter location;
} moqt_publish_params;
size_t moqt_publish_write(uint8_t *,size_t,uint64_t request_id,const char *const *ns,unsigned ns_count,const char *name,uint64_t alias,const moqt_publish_params *,const uint8_t *props,size_t props_len);
size_t moqt_publish_state_notify_write(uint8_t *,size_t,const moqt_publish_params *);
size_t moqt_publish_namespace_write(uint8_t *,size_t,uint64_t request_id,const char *const *ns,unsigned ns_count);
/* NAMESPACE, NAMESPACE_DONE: suffix after `skip` prefix fields. */
size_t moqt_namespace_write(uint8_t *,size_t,uint64_t type,const char *const *ns,unsigned ns_count,unsigned skip);
size_t moqt_publish_skipped_write(uint8_t *,size_t,const char *const *ns,unsigned ns_count,unsigned skip,const char *name);
size_t moqt_goaway_write(uint8_t *,size_t,uint64_t timeout);

/* Data plane (section 11). */
typedef struct {
 uint64_t alias,group,subgroup,object; uint8_t priority,status;
 bool has_priority,end_of_group,first_object,datagram;
 const uint8_t *props; size_t props_len; size_t payload_len;
} moqt_object;
/* SUBGROUP_HEADER; every object on the stream carries properties when `with_props`. */
size_t moqt_subgroup_header_write(uint8_t *,size_t,const moqt_object *,bool with_props);
/* Object fields that follow the header; `prev_object` is ignored for the first object. */
size_t moqt_subgroup_object_write(uint8_t *,size_t,const moqt_object *,bool with_props,bool first,uint64_t prev_object);
/* Whole OBJECT_DATAGRAM header; the payload follows to the end of the datagram. */
size_t moqt_datagram_header_write(uint8_t *,size_t,const moqt_object *);
size_t moqt_fetch_header_write(uint8_t *,size_t,uint64_t request_id);
typedef struct { bool valid; uint64_t group,object,subgroup; uint8_t priority; bool has_subgroup,has_priority; } moqt_fetch_prev;
size_t moqt_fetch_object_write(uint8_t *,size_t,const moqt_object *,bool descending,moqt_fetch_prev *);
size_t moqt_fetch_end_range_write(uint8_t *,size_t,uint64_t kind,moqt_loc at,bool descending,moqt_fetch_prev *);

/* Delivery timeouts (section 5.2): the smaller non-zero value applies. */
static inline uint64_t moqt_timeout_effective(uint64_t publisher_ms,uint64_t subscriber_ms){
 if(!publisher_ms)return subscriber_ms;
 if(!subscriber_ms)return publisher_ms;
 return publisher_ms<subscriber_ms?publisher_ms:subscriber_ms;
}
#endif
