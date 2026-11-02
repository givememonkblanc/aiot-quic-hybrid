idf_component_register(
    SRCS "source_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES ai_inference common esp_camera dl_qads
)

set(AIOT_PROFILE_AI_CORE "0" CACHE STRING "AI profiling task core, 0 or 1")
if(NOT AIOT_PROFILE_AI_CORE MATCHES "^[01]$")
    message(FATAL_ERROR "AIOT_PROFILE_AI_CORE must be 0 or 1")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE AIOT_PROFILE_AI_CORE=${AIOT_PROFILE_AI_CORE})
set(AIOT_PROFILE_AI_PRIORITY "0" CACHE STRING "AI task priority, 0 (that of the idle task) to 2")
if(NOT AIOT_PROFILE_AI_PRIORITY MATCHES "^[012]$")
    message(FATAL_ERROR "AIOT_PROFILE_AI_PRIORITY must be 0, 1 or 2")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE AIOT_PROFILE_AI_PRIORITY=${AIOT_PROFILE_AI_PRIORITY})

if(AIOT_MOBILITY_CAMERA AND AIOT_EMBED_REPLAY_JPEG)
    set(AIOT_REPLAY_JPEG "${CMAKE_CURRENT_LIST_DIR}/../../../experiments/aiot_resource_management/fixtures/replay.jpg" CACHE FILEPATH "Fixed replay JPEG")
    target_add_binary_data(${COMPONENT_LIB} "${AIOT_REPLAY_JPEG}" BINARY RENAME_TO replay_jpeg)
endif()

if(AIOT_CAMERA_INTEGRITY_TEST)
    idf_component_optional_requires(PRIVATE mbedtls)
endif()

set(AIOT_MOQT_RESULT_QUEUE "32" CACHE STRING "Inference results that wait for transmission, 2..64")
if(NOT AIOT_MOQT_RESULT_QUEUE MATCHES "^[0-9]+$" OR AIOT_MOQT_RESULT_QUEUE LESS 2 OR AIOT_MOQT_RESULT_QUEUE GREATER 64)
    message(FATAL_ERROR "Invalid AIOT_MOQT_RESULT_QUEUE")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE AIOT_MOQT_RESULT_QUEUE=${AIOT_MOQT_RESULT_QUEUE})

set(AIOT_AI_REPEAT "1" CACHE STRING "Model-time sweep: the detector runs this many times on every frame (same result, k times the time), 1..8")
if(NOT AIOT_AI_REPEAT MATCHES "^[1-8]$")
    message(FATAL_ERROR "Invalid AIOT_AI_REPEAT")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE AIOT_AI_REPEAT=${AIOT_AI_REPEAT})
