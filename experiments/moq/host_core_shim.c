/* Host-only helpers for driving the portable publisher core through ctypes. */
#include "moqt_core.h"
size_t moqt_core_sizeof(void){return sizeof(moqt_core);}
size_t moqt_action_sizeof(void){return sizeof(moqt_action);}
size_t moqt_delivery_sizeof(void){return sizeof(moqt_delivery);}
size_t moqt_offer_sizeof(void){return sizeof(moqt_offer);}
size_t moqt_track_cfg_sizeof(void){return sizeof(moqt_track_cfg);}
uint64_t moqt_core_sub_streams(const moqt_core *c,unsigned sub){return sub<MOQT_CORE_SUBS?c->subs[sub].streams:0;}
int moqt_core_sub_state(const moqt_core *c,unsigned sub){return sub<MOQT_CORE_SUBS?c->subs[sub].state:-1;}
int moqt_core_fetch_sub(const moqt_core *c,unsigned fetch){return fetch<MOQT_CORE_FETCHES?c->fetches[fetch].sub:-2;}
int moqt_core_fetch_priority(const moqt_core *c,unsigned fetch){return fetch<MOQT_CORE_FETCHES?c->fetches[fetch].priority:-1;}
int moqt_core_fetch_descending(const moqt_core *c,unsigned fetch){return fetch<MOQT_CORE_FETCHES?c->fetches[fetch].descending:-1;}
uint64_t moqt_core_peer_max_ranges(const moqt_core *c){return c->peer.max_filter_ranges;}
int moqt_core_is_closed(const moqt_core *c){return c->closed;}
void moqt_core_set_lifetime(moqt_core *c,uint64_t ms){c->subscription_lifetime_ms=ms;}
void moqt_core_host_extensions(moqt_core *c,int message,int subscriptions,int fetches,int source_timeout){
 moqt_setup_ext x={.has_control_message=message>=0,.max_control_message=message>0?(uint64_t)message:0,
  .has_subscriptions=subscriptions>=0,.max_subscriptions=subscriptions>0?(uint64_t)subscriptions:0,
  .has_fetches=fetches>=0,.max_fetches=fetches>0?(uint64_t)fetches:0,.source_timeout=source_timeout!=0};
 moqt_core_extensions(c,&x);
}
uint64_t moqt_core_source_expired(const moqt_core *c){return c->source_expired;}
void moqt_core_host_session_reset(moqt_core *c){moqt_core_session_reset(c);}
