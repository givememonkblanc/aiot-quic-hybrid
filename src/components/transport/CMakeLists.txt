# Optional QUIC server-certificate pinning: drop a real PEM at
# components/transport/server_ca.pem and it is embedded automatically and
# peer verification is turned on (APP_QUIC_VERIFY_PEER). See docs/PRODUCT.md.
set(_embed_files "")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/server_ca.pem")
    set(_embed_files "server_ca.pem")
endif()

idf_component_register(
    SRCS "ngtcp2_sample.c" "esp_ev_compat.c" "mqtt_quic_transport.c"
    INCLUDE_DIRS "include" "."
    REQUIRES esp_timer core_mqtt
    PRIV_REQUIRES esp-http3 ngtcp2
    EMBED_TXTFILES ${_embed_files}
)

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/server_ca.pem")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_QUIC_VERIFY_PEER=1)
endif()
