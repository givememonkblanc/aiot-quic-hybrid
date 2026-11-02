idf_component_register(
    SRCS "app_main.c" "app_tcp_baseline.c" "moq_representation.c" "moq_codec_worker.c" "moq_passage.c" "moq_mission_sched.c" "moq_mqtt_shim.c" "netlog.c"
    INCLUDE_DIRS "."
    REQUIRES common dl_qads transport sources wifi_link esp_camera
            app_cfg presence provisioning ota status_led store_forward
            display esp_https_ota app_update esp_wifi nvs_flash driver lwip mqtt
)

if(DEFINED APP_EXPERIMENT_PRESET)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_EXPERIMENT_PRESET=${APP_EXPERIMENT_PRESET})
endif()

include("${CMAKE_CURRENT_LIST_DIR}/moq_options.cmake")

set(AIOT_NETLOG_HOST "" CACHE STRING "Host that receives a copy of the console over UDP; empty: off")
set(AIOT_NETLOG_PORT "14570" CACHE STRING "UDP port of the console copy")
if(AIOT_NETLOG_HOST)
    if(NOT AIOT_NETLOG_HOST MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+$" OR NOT AIOT_NETLOG_PORT MATCHES "^[0-9]+$")
        message(FATAL_ERROR "AIOT_NETLOG_HOST must be an IPv4 address and AIOT_NETLOG_PORT a number")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE AIOT_NETLOG_HOST="${AIOT_NETLOG_HOST}" AIOT_NETLOG_PORT=${AIOT_NETLOG_PORT})
endif()
