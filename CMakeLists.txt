cmake_minimum_required(VERSION 3.16)

option(AIOT_MOQT_AUDIT "Bounded deferred measurement journal" OFF)
if(AIOT_MOQT_AUDIT)
    if(NOT AIOT_MOQT_CONCURRENT OR NOT AIOT_MOQT_POLICY_EXPERIMENT)
        message(FATAL_ERROR "MoQ audit requires concurrent policy experiment")
    endif()
    add_compile_definitions(AIOT_MOQT_AUDIT=1)
endif()

option(AIOT_MOQT_POLICY_EXPERIMENT "MoQ policy selection and event FIFO" OFF)
if(AIOT_MOQT_POLICY_EXPERIMENT)
    add_compile_definitions(AIOT_MOQT_POLICY_EXPERIMENT=1)
endif()
option(AIOT_MOQT_REPLAY_INPUT "Use the embedded deterministic JPEG as MoQ input" OFF)
if(AIOT_MOQT_REPLAY_INPUT)
    if(NOT AIOT_MOQT_POLICY_EXPERIMENT)
        message(FATAL_ERROR "MoQ replay input requires policy experiment")
    endif()
    add_compile_definitions(AIOT_MOQT_REPLAY_INPUT=1)
else()
    add_compile_definitions(AIOT_MOQT_REPLAY_INPUT=0)
endif()

option(AIOT_MOQT_BOUNDED_PUMP "Bound MoQ packet work per application iteration" OFF)
if(AIOT_MOQT_BOUNDED_PUMP)
    add_compile_definitions(AIOT_MOQT_BOUNDED_PUMP=1)
endif()

option(AIOT_MOQT_CONCURRENT "MoQ independent capture and AI tracks" OFF)
if(AIOT_MOQT_CONCURRENT)
    add_compile_definitions(AIOT_MOQT_CONCURRENT=1)
endif()

option(AIOT_MOQT_EXPERIMENT "Opt-in native MOQT-21 JPEG publisher milestone" OFF)
if(AIOT_MOQT_EXPERIMENT)
    if(NOT EXISTS "${AIOT_MOQT_PIN_HEADER}")
        message(FATAL_ERROR "MoQ experiment requires a pinned receiver certificate header")
    endif()
    add_compile_definitions(AIOT_MOQT_EXPERIMENT=1 AIOT_MOQT_PIN_HEADER="${AIOT_MOQT_PIN_HEADER}")
endif()
# Helper mode (src/main/app_moq_helper.inc): no camera and no publishing; the board subscribes to
# the camera frames at the relay and runs the detector on them. Nothing below is defined while OFF.
option(AIOT_MOQT_HELPER "MoQ helper: subscribe to camera frames at a relay and run the detector" OFF)
set(AIOT_MOQT_HELPER_HOST "192.168.0.38" CACHE STRING "Helper: relay host (downstream side)")
set(AIOT_MOQT_HELPER_PORT "14600" CACHE STRING "Helper: relay downstream UDP port")
set(AIOT_MOQT_HELPER_TRACK "offload/jpeg" CACHE STRING "Helper: track name in namespace aiot/esp32")
set(AIOT_MOQT_HELPER_EVENTS_TRACK "helper/events" CACHE STRING "Helper: track of its own results (namespace aiot/esp32), published on the relay session")
set(AIOT_MOQT_HELPER_REPORT_HOST "192.168.0.38" CACHE STRING "Helper: host that receives one UDP datagram per inference")
set(AIOT_MOQT_HELPER_REPORT_PORT "5320" CACHE STRING "Helper: UDP port of the detection reports")
option(AIOT_MQTT_HELPER "MQTT baseline of the helper: the camera's frames from the broker (aiot/esp32/offload/jpeg) and the results to it (aiot/esp32/helper/events), MQTT 3.1.1 over TLS at AIOT_MOQT_HELPER_HOST:PORT" OFF)
if(AIOT_MQTT_HELPER)
    add_compile_definitions(AIOT_MQTT_HELPER=1)
endif()
option(AIOT_MOQT_HELPER_ANY_GROUP "Helper serving several cameras through the relay: the frames' groups come from different cameras and are not ordered; only a repeated group is not published again" OFF)
if(AIOT_MOQT_HELPER_ANY_GROUP)
    add_compile_definitions(AIOT_MOQT_HELPER_ANY_GROUP=1)
endif()
if(AIOT_MOQT_HELPER)
    if(NOT AIOT_MOQT_EXPERIMENT)
        message(FATAL_ERROR "AIOT_MOQT_HELPER needs AIOT_MOQT_EXPERIMENT (MoQ transport)")
    endif()
    if(NOT AIOT_MOQT_HELPER_HOST MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+$" OR NOT AIOT_MOQT_HELPER_PORT MATCHES "^[0-9]+$"
       OR NOT AIOT_MOQT_HELPER_REPORT_HOST MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+$" OR NOT AIOT_MOQT_HELPER_REPORT_PORT MATCHES "^[0-9]+$")
        message(FATAL_ERROR "AIOT_MOQT_HELPER_*HOST must be IPv4 addresses and *_PORT numbers")
    endif()
    add_compile_definitions(AIOT_MOQT_HELPER=1 AIOT_MOQT_HELPER_HOST="${AIOT_MOQT_HELPER_HOST}"
        AIOT_MOQT_HELPER_PORT="${AIOT_MOQT_HELPER_PORT}" AIOT_MOQT_HELPER_TRACK="${AIOT_MOQT_HELPER_TRACK}"
        AIOT_MOQT_HELPER_EVENTS_TRACK="${AIOT_MOQT_HELPER_EVENTS_TRACK}"
        AIOT_MOQT_HELPER_REPORT_HOST="${AIOT_MOQT_HELPER_REPORT_HOST}" AIOT_MOQT_HELPER_REPORT_PORT=${AIOT_MOQT_HELPER_REPORT_PORT})
endif()
option(AIOT_MOQT_OTA_REMOTE "Keep signed pull-OTA available in native MoQ experiments" OFF)
if(AIOT_MOQT_OTA_REMOTE)
    if(NOT AIOT_MOQT_EXPERIMENT OR NOT AIOT_OTA_SITE_HEADER)
        message(FATAL_ERROR "Remote MoQ OTA requires the MoQ experiment and OTA site header")
    endif()
    add_compile_definitions(AIOT_MOQT_OTA_REMOTE=1)
endif()

option(AIOT_QUIC_BATCH_EXPERIMENT "Opt-in fixed-fragment QUIC submission experiment" OFF)
if(AIOT_QUIC_BATCH_EXPERIMENT)
    add_compile_definitions(AIOT_QUIC_BATCH_EXPERIMENT=1 APP_SNAPSHOT_CHUNK_SIZE=1024)
endif()

option(AIOT_PIPELINE_PROFILE "Nested image/QUIC CPU and timing probes" OFF)
if(AIOT_PIPELINE_PROFILE)
    if(NOT AIOT_QUIC_BATCH_EXPERIMENT)
        message(FATAL_ERROR "Pipeline probes require the batching runtime counter")
    endif()
    add_compile_definitions(AIOT_PIPELINE_PROFILE=1)
endif()

option(AIOT_MOBILITY_CAMERA "Run real camera and AI in the mobility application" OFF)
option(AIOT_EMBED_REPLAY_JPEG "Embed the fixed JPEG used by replay experiments" ON)
if(AIOT_EMBED_REPLAY_JPEG)
    add_compile_definitions(AIOT_EMBED_REPLAY_JPEG=1)
elseif(AIOT_MOQT_REPLAY_INPUT)
    message(FATAL_ERROR "MoQ replay input requires embedded replay JPEG")
endif()
option(AIOT_CAMERA_UXGA "Use native 1600x1200 camera output for remote inspection" OFF)
option(AIOT_CAMERA_SVGA_TEST "Compare native SVGA with deployed UXGA quality-6 firmware" OFF)
if(AIOT_CAMERA_SVGA_TEST)
    if(NOT AIOT_CAMERA_UXGA)
        message(FATAL_ERROR "SVGA comparison requires UXGA inspection configuration")
    endif()
    # Match deployed v2 pin configuration during the resolution-only A/B test.
    add_compile_definitions(AIOT_CAMERA_SVGA_TEST=1 CAMERA_PIN_PWDN=38)
endif()
option(AIOT_CAMERA_INTEGRITY_TEST "Capture hashes and OV2640 register snapshot" OFF)
if(AIOT_CAMERA_INTEGRITY_TEST)
    add_compile_definitions(AIOT_CAMERA_INTEGRITY_TEST=1 CAMERA_PIN_PWDN=38)
endif()
option(AIOT_CAMERA_DEFAULTS_TEST "Skip application sensor tuning; preserve UXGA JPEG6" OFF)
if(AIOT_CAMERA_DEFAULTS_TEST)
    if(NOT AIOT_CAMERA_UXGA OR AIOT_CAMERA_EXPOSURE_TEST OR AIOT_CAMERA_SVGA_TEST)
        message(FATAL_ERROR "Defaults comparison requires UXGA without exposure/SVGA tests")
    endif()
    add_compile_definitions(AIOT_CAMERA_DEFAULTS_TEST=1 CAMERA_PIN_PWDN=38)
endif()
option(AIOT_CAMERA_COLORBAR_TEST "Enable the OV2640 internal color-bar generator" OFF)
if(AIOT_CAMERA_COLORBAR_TEST)
    if(NOT AIOT_CAMERA_UXGA OR AIOT_CAMERA_SVGA_TEST)
        message(FATAL_ERROR "Color-bar diagnostic requires native UXGA")
    endif()
    add_compile_definitions(AIOT_CAMERA_COLORBAR_TEST=1 CAMERA_PIN_PWDN=38)
endif()
set(AIOT_CAMERA_JPEG_QUALITY "" CACHE STRING "OV2640 JPEG quality 4..63 (lower is finer); empty: the default of the frame size")
if(AIOT_CAMERA_JPEG_QUALITY)
    if(NOT AIOT_CAMERA_JPEG_QUALITY MATCHES "^[0-9]+$" OR AIOT_CAMERA_JPEG_QUALITY LESS 4 OR AIOT_CAMERA_JPEG_QUALITY GREATER 63)
        message(FATAL_ERROR "Invalid AIOT_CAMERA_JPEG_QUALITY")
    endif()
    add_compile_definitions(AIOT_CAMERA_JPEG_QUALITY=${AIOT_CAMERA_JPEG_QUALITY})
endif()
set(AIOT_CAMERA_BYTE_BUDGET "" CACHE STRING "Frame bytes the JPEG quality is steered to (never finer than the configured quality); empty: fixed quality")
if(AIOT_CAMERA_BYTE_BUDGET)
    if(NOT AIOT_CAMERA_BYTE_BUDGET MATCHES "^[0-9]+$" OR AIOT_CAMERA_BYTE_BUDGET LESS 8192 OR AIOT_CAMERA_BYTE_BUDGET GREATER 200000)
        message(FATAL_ERROR "Invalid AIOT_CAMERA_BYTE_BUDGET")
    endif()
    add_compile_definitions(AIOT_CAMERA_BYTE_BUDGET=${AIOT_CAMERA_BYTE_BUDGET})
endif()
set(AIOT_CAMERA_XCLK_HZ "" CACHE STRING "Camera clock in Hz; empty: 20 MHz")
if(AIOT_CAMERA_XCLK_HZ)
    if(NOT AIOT_CAMERA_XCLK_HZ MATCHES "^[0-9]+$" OR AIOT_CAMERA_XCLK_HZ LESS 5000000 OR AIOT_CAMERA_XCLK_HZ GREATER 24000000)
        message(FATAL_ERROR "Invalid AIOT_CAMERA_XCLK_HZ")
    endif()
    add_compile_definitions(AIOT_CAMERA_XCLK_HZ=${AIOT_CAMERA_XCLK_HZ})
endif()
option(AIOT_CAMERA_DIAG "Print the sensor identification and capture QVGA frames" OFF)
if(AIOT_CAMERA_DIAG)
    add_compile_definitions(AIOT_CAMERA_DIAG=1)
endif()
option(AIOT_CAMERA_EXPOSURE_TEST "Diagnostic fixed OV2640 exposure (120 register units)" OFF)
if(AIOT_CAMERA_EXPOSURE_TEST)
    add_compile_definitions(AIOT_CAMERA_EXPOSURE_TEST=1)
endif()
if(AIOT_CAMERA_UXGA)
    add_compile_definitions(AIOT_CAMERA_UXGA=1)
endif()
option(AIOT_CAMERA_QVGA "The sensor scales the full field of view to 320x240 JPEG (the detector decodes it at 1/2, 160x120): no UXGA decode on the camera; needs AIOT_CAMERA_UXGA (buffers sized for UXGA)" OFF)
if(AIOT_CAMERA_QVGA)
    if(NOT AIOT_CAMERA_UXGA OR AIOT_CAMERA_SVGA_TEST)
        message(FATAL_ERROR "AIOT_CAMERA_QVGA needs AIOT_CAMERA_UXGA without the SVGA test")
    endif()
    add_compile_definitions(AIOT_CAMERA_QVGA=1)
endif()
if(AIOT_MOBILITY_CAMERA)
    add_compile_definitions(AIOT_MOBILITY_CAMERA=1)
endif()

option(AIOT_MOBILITY_RESEARCH "Build the ESP32 handoff measurement application" OFF)
if(AIOT_MOBILITY_RESEARCH)
    add_compile_definitions(AIOT_MOBILITY_RESEARCH=1)
endif()

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
    src/components/display
    # Vendored third-party (QUIC/TLS stack + camera) — see src/third_party/README.md
    src/third_party/esp_camera
    src/third_party/esp-http3
    src/third_party/ngtcp2
    src/third_party/wolfssl
    src/third_party/core_mqtt
)

option(AIOT_TINYVLM_RUNTIME "Build the opt-in TinyVLM encoder and prototype matcher" OFF)
if(AIOT_TINYVLM_RUNTIME)
    list(APPEND COMPONENT_DIRS src/components/tinyvlm)
endif()

option(AIOT_ESPDET_RUNTIME "Use ESP-DL ESPDet instead of TFLM in camera experiments" OFF)
if(AIOT_ESPDET_RUNTIME)
    add_compile_definitions(AIOT_ESPDET_RUNTIME=1)
    list(APPEND COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/experiments/vlm_split/vendor/esp-dl/models/cat_detect")
endif()

option(AIOT_ESP32_HW_AES "Experiment: ESP32-S3 hardware AES with 32-bit GHASH" OFF)
if(AIOT_ESP32_HW_AES)
    add_compile_definitions(AIOT_ESP32_HW_AES=1)
endif()

option(AIOT_QUIC_PSRAM_POOL "Keep large ngtcp2 allocations out of internal SRAM" OFF)
if(AIOT_QUIC_PSRAM_POOL)
    add_compile_definitions(AIOT_QUIC_PSRAM_POOL=1)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(aiot_quic)
