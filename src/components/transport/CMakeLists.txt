# Optional QUIC server-certificate pinning: drop a real PEM at
# components/transport/server_ca.pem and it is embedded automatically and
# peer verification is turned on (APP_QUIC_VERIFY_PEER). See docs/PRODUCT.md.
set(_embed_files "")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/server_ca.pem")
    set(_embed_files "server_ca.pem")
endif()

idf_component_register(
    SRCS "moqt_scheduler.c" "moqt21.c" "moqt_core.c" "ngtcp2_sample.c" "esp_ev_compat.c" "mqtt_quic_transport.c" "mqtt_tcp_transport.c"
    INCLUDE_DIRS "."
    REQUIRES esp_timer core_mqtt
    PRIV_REQUIRES common esp-http3 ngtcp2 esp-tls
    EMBED_TXTFILES ${_embed_files}
)

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/server_ca.pem")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_QUIC_VERIFY_PEER=1)
endif()

set(AIOT_QUIC_CIPHER_LIST "" CACHE STRING "Optional TLS 1.3 cipher list for transport experiments")
if(AIOT_QUIC_CIPHER_LIST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE AIOT_QUIC_CIPHER_LIST="${AIOT_QUIC_CIPHER_LIST}")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/moq_options.cmake")
