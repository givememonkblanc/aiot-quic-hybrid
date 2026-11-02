# MoQ experiment options. Included after idf_component_register.
set(AIOT_MOQT_IMAGE_SLOTS "2" CACHE STRING "Concurrent MoQ JPEG objects: 1 or 2")
if(NOT AIOT_MOQT_IMAGE_SLOTS MATCHES "^[12]$")
    message(FATAL_ERROR "AIOT_MOQT_IMAGE_SLOTS must be 1 or 2")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE AIOT_MOQT_IMAGE_SLOTS=${AIOT_MOQT_IMAGE_SLOTS})

set(AIOT_MOQT_PREVIEW_KBPS "0" CACHE STRING "Fixed preview pacing rate in decimal kbit/s; 0 disables")
set(AIOT_MOQT_PREVIEW_UNACKED_KB "0" CACHE STRING "Maximum submitted but unacknowledged preview KiB; 0 disables")
set(AIOT_MOQT_DELIVERY_TIMEOUT_RESET "0" CACHE STRING "Reset admitted object streams after mission deadline")
if(NOT AIOT_MOQT_PREVIEW_KBPS MATCHES "^[0-9]+$" OR
   NOT AIOT_MOQT_PREVIEW_UNACKED_KB MATCHES "^[0-9]+$" OR
   NOT AIOT_MOQT_DELIVERY_TIMEOUT_RESET MATCHES "^[01]$")
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

set(AIOT_MOQT_RATE_ESTIMATOR "0" CACHE STRING "ACK-service estimator: 0 project EWMA, 1 SWMAth math adaptation")
if(NOT AIOT_MOQT_RATE_ESTIMATOR MATCHES "^[01]$")
    message(FATAL_ERROR "Invalid MoQ service estimator")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE AIOT_MOQT_RATE_ESTIMATOR=${AIOT_MOQT_RATE_ESTIMATOR})
