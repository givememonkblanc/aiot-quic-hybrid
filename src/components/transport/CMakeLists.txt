idf_component_register(
    SRCS "transport_router.c" "moqt_quic.c" "moqt_quic_client.c" "moqt_udp_client.c" "ngtcp2_sample.c" "esp_ev_compat.c" "mqtt_quic_transport.c"
    INCLUDE_DIRS "include" "."
    REQUIRES esp_timer core_mqtt
    PRIV_REQUIRES esp-http3 ngtcp2
)
