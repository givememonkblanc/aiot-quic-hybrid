cmake_minimum_required(VERSION 3.16)

set(COMPONENT_DIRS
    src/main
    $ENV{IDF_PATH}/components
    src/components/ai_inference
    src/components/common
    src/components/dl_qads
    src/components/receiver
    src/components/sources
    src/components/transport
    src/components/wifi_link
    src/components/esp_camera
    src/components/esp-http3
    src/components/ngtcp2
    src/components/wolfssl
    src/components/core_mqtt
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(aiot_quic)
