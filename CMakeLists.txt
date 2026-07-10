cmake_minimum_required(VERSION 3.16)

set(COMPONENT_DIRS
    src/main
    $ENV{IDF_PATH}/components
    # First-party application components
    src/components/ai_inference
    src/components/common
    src/components/dl_qads
    src/components/sources
    src/components/transport
    src/components/wifi_link
    src/components/presence
    src/components/app_cfg
    src/components/provisioning
    src/components/ota
    src/components/status_led
    src/components/store_forward
    # Vendored third-party (QUIC/TLS stack + camera) — see src/third_party/README.md
    src/third_party/esp_camera
    src/third_party/esp-http3
    src/third_party/ngtcp2
    src/third_party/wolfssl
    src/third_party/core_mqtt
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(aiot_quic)
