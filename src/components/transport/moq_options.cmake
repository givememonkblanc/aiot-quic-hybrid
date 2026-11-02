# MoQ experiment options. Included after idf_component_register.
set(AIOT_MOQT_IMAGE_SLOTS "2" CACHE STRING "Concurrent MoQ JPEG objects: 1 or 2")
if(NOT AIOT_MOQT_IMAGE_SLOTS MATCHES "^[12]$")
    message(FATAL_ERROR "AIOT_MOQT_IMAGE_SLOTS must be 1 or 2")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE AIOT_MOQT_IMAGE_SLOTS=${AIOT_MOQT_IMAGE_SLOTS})
set(AIOT_MOQT_RESULT_SLOTS "1" CACHE STRING "Result objects in flight at once: 1 to 4")
if(NOT AIOT_MOQT_RESULT_SLOTS MATCHES "^[1-4]$")
    message(FATAL_ERROR "AIOT_MOQT_RESULT_SLOTS must be 1 to 4")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE AIOT_MOQT_RESULT_SLOTS=${AIOT_MOQT_RESULT_SLOTS})

set(AIOT_MOQT_PREVIEW_KBPS "0" CACHE STRING "Fixed preview pacing rate in decimal kbit/s; 0 disables")
set(AIOT_MOQT_PREVIEW_UNACKED_KB "0" CACHE STRING "Maximum submitted but unacknowledged preview KiB; 0 disables")
set(AIOT_MOQT_DELIVERY_TIMEOUT_RESET "0" CACHE STRING "Reset admitted object streams after mission deadline")
if(NOT AIOT_MOQT_PREVIEW_KBPS MATCHES "^[0-9]+$" OR
   NOT AIOT_MOQT_PREVIEW_UNACKED_KB MATCHES "^[0-9]+$" OR
   NOT AIOT_MOQT_DELIVERY_TIMEOUT_RESET MATCHES "^[012]$")
    message(FATAL_ERROR "MoQ preview pacing/cap values must be non-negative integers")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    AIOT_MOQT_PREVIEW_KBPS=${AIOT_MOQT_PREVIEW_KBPS}
    AIOT_MOQT_PREVIEW_UNACKED_KB=${AIOT_MOQT_PREVIEW_UNACKED_KB}
    AIOT_MOQT_DELIVERY_TIMEOUT_RESET=${AIOT_MOQT_DELIVERY_TIMEOUT_RESET})

set(AIOT_MOQT_FEASIBILITY "0" CACHE STRING "Preview feasibility admission: 0 or 1")
if(NOT AIOT_MOQT_FEASIBILITY MATCHES "^[01]$")
    message(FATAL_ERROR "Invalid MoQ feasibility option")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE AIOT_MOQT_FEASIBILITY=${AIOT_MOQT_FEASIBILITY})

set(AIOT_MOQT_RATE_ESTIMATOR "0" CACHE STRING "ACK-service estimator: 0 per object, 1 SWMAth math adaptation, 2 windows of one second while an object is under way")
if(NOT AIOT_MOQT_RATE_ESTIMATOR MATCHES "^[012]$")
    message(FATAL_ERROR "Invalid MoQ service estimator")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE AIOT_MOQT_RATE_ESTIMATOR=${AIOT_MOQT_RATE_ESTIMATOR})
option(AIOT_MOQT_DOOMED_DROP "An Object with a delivery timeout is reset as soon as the rest of its bytes, at its own acknowledged rate, needs a quarter more than the time left (not only at the timeout)" OFF)
if(AIOT_MOQT_DOOMED_DROP)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE AIOT_MOQT_DOOMED_DROP=1)
endif()
option(AIOT_MOQT_RATE_EVIDENCE_ONLY "The service rate of the evidence planner counts the evidence objects only, not the offload track (alias 1) that shares the image slots" OFF)
if(AIOT_MOQT_RATE_EVIDENCE_ONLY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE AIOT_MOQT_RATE_EVIDENCE_ONLY=1)
endif()
set(AIOT_MOQT_OFFLOAD_SCHED "0" CACHE STRING "Offload scheduling: 1 evidence first, an offload starts only when it can end within its timeout and is cut when it no longer can; 2 as 1, offload held only while the evidence needs the link")
if(NOT AIOT_MOQT_OFFLOAD_SCHED MATCHES "^[012]$")
    message(FATAL_ERROR "Invalid AIOT_MOQT_OFFLOAD_SCHED")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE AIOT_MOQT_OFFLOAD_SCHED=${AIOT_MOQT_OFFLOAD_SCHED})
option(AIOT_MOQT_OFFLOAD_DECOUPLE "Offload objects are not the evidence in flight for the evidence planner (as OFFLOAD_SCHED does), without the scheduling: priorities and the delivery timeout only" OFF)
if(AIOT_MOQT_OFFLOAD_DECOUPLE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE AIOT_MOQT_OFFLOAD_DECOUPLE=1)
endif()
option(AIOT_MOQT_TX_FIFO "Objects leave in the order they were queued, whatever their priorities (one send queue per communication unit, Synergy, Gong et al. TMC 2025, Sec. IV-F); control and request streams first" OFF)
if(AIOT_MOQT_TX_FIFO)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE AIOT_MOQT_TX_FIFO=1)
endif()
set(AIOT_MOQT_TX_ORDER "0" CACHE STRING "Wire policy (POLICY 6) send order: 0 draft-21 priorities; 1 strict priority to the results; 2 round robin over results, evidence and offload; 3 earliest deadline first (same send path, slices and window)")
if(NOT AIOT_MOQT_TX_ORDER MATCHES "^[0-3]$")
    message(FATAL_ERROR "Invalid AIOT_MOQT_TX_ORDER")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE AIOT_MOQT_TX_ORDER=${AIOT_MOQT_TX_ORDER})

set(AIOT_MOQT_CACHE_KB "1024" CACHE STRING "KiB of published objects retained for FETCH")
set(AIOT_MOQT_CACHE_OBJECTS "16" CACHE STRING "Images retained for FETCH")
set(AIOT_MOQT_RESULT_CACHE_OBJECTS "32" CACHE STRING "Objects retained per non-image track")
if(NOT AIOT_MOQT_CACHE_KB MATCHES "^[0-9]+$" OR NOT AIOT_MOQT_CACHE_OBJECTS MATCHES "^[0-9]+$" OR
   NOT AIOT_MOQT_RESULT_CACHE_OBJECTS MATCHES "^[0-9]+$" OR
   AIOT_MOQT_CACHE_OBJECTS GREATER 24 OR AIOT_MOQT_RESULT_CACHE_OBJECTS GREATER 36)
    message(FATAL_ERROR "Invalid MoQ FETCH cache limits")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    AIOT_MOQT_CACHE_KB=${AIOT_MOQT_CACHE_KB}
    AIOT_MOQT_CACHE_OBJECTS=${AIOT_MOQT_CACHE_OBJECTS}
    AIOT_MOQT_RESULT_CACHE_OBJECTS=${AIOT_MOQT_RESULT_CACHE_OBJECTS})

set(AIOT_MOQT_BEHIND_LIMIT "0" CACHE STRING "Consecutive refused objects before TOO_FAR_BEHIND; 0 disables")
set(AIOT_MOQT_SUBSCRIPTION_LIFETIME_MS "0" CACHE STRING "EXPIRES for every subscription in ms; 0 disables")
if(NOT AIOT_MOQT_BEHIND_LIMIT MATCHES "^[0-9]+$" OR NOT AIOT_MOQT_SUBSCRIPTION_LIFETIME_MS MATCHES "^[0-9]+$")
    message(FATAL_ERROR "Invalid MoQ subscription limits")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    AIOT_MOQT_BEHIND_LIMIT=${AIOT_MOQT_BEHIND_LIMIT}
    AIOT_MOQT_SUBSCRIPTION_LIFETIME_MS=${AIOT_MOQT_SUBSCRIPTION_LIFETIME_MS})

option(AIOT_MOQT_EXT_LIMITS "Extension: state control-message, subscription and fetch limits in SETUP" OFF)
option(AIOT_MOQT_EXT_SOURCE_TIMEOUT "Extension: SOURCE_AGE property and SOURCE_DELIVERY_TIMEOUT" OFF)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    AIOT_MOQT_EXT_LIMITS=$<BOOL:${AIOT_MOQT_EXT_LIMITS}>
    AIOT_MOQT_EXT_SOURCE_TIMEOUT=$<BOOL:${AIOT_MOQT_EXT_SOURCE_TIMEOUT}>)

option(AIOT_MOQT_KEEP_CACHE "Retained objects and counters survive a lost connection" OFF)
target_compile_definitions(${COMPONENT_LIB} PRIVATE AIOT_MOQT_KEEP_CACHE=$<BOOL:${AIOT_MOQT_KEEP_CACHE}>)

# ngtcp2_sample.c selects the algorithm by these macros; the project defines no Kconfig entry for them
set(AIOT_QUIC_CC "cubic" CACHE STRING "Congestion control of the device: cubic, bbr or reno")
if(NOT AIOT_QUIC_CC MATCHES "^(cubic|bbr|reno)$")
    message(FATAL_ERROR "Invalid AIOT_QUIC_CC")
elseif(AIOT_QUIC_CC STREQUAL "bbr")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_QUIC_CC_ALGO_BBR=1)
elseif(AIOT_QUIC_CC STREQUAL "reno")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_QUIC_CC_ALGO_RENO=1)
endif()

option(AIOT_MOQT_WMM "Wi-Fi access category by stream: images and fetches best effort, everything else voice (IP_TOS per packet)" OFF)
target_compile_definitions(${COMPONENT_LIB} PRIVATE AIOT_MOQT_WMM=$<BOOL:${AIOT_MOQT_WMM}>)

set(AIOT_MOQT_RESULT_PROBE_MS "0" CACHE STRING "While a result waits for its acknowledgement, a PING after this many ms without a sent packet (0: off)")
target_compile_definitions(${COMPONENT_LIB} PRIVATE AIOT_MOQT_RESULT_PROBE_MS=${AIOT_MOQT_RESULT_PROBE_MS})
option(AIOT_MOQT_MUS_PREDICT "Scheduler: image bytes that drain before the next inference can end are admitted beyond the evidence window" OFF)
target_compile_definitions(${COMPONENT_LIB} PRIVATE AIOT_MOQT_MUS_PREDICT=$<BOOL:${AIOT_MOQT_MUS_PREDICT}>)
set(AIOT_MOQT_TRACE_MS "0" CACHE STRING "Training trace of the link for the learned MBM: a [MOQ_TRACE] record every this many ms (0: off)")
if(NOT AIOT_MOQT_TRACE_MS MATCHES "^[0-9]+$")
    message(FATAL_ERROR "Invalid AIOT_MOQT_TRACE_MS")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE AIOT_MOQT_TRACE_MS=${AIOT_MOQT_TRACE_MS})
