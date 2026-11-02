set(audit_driver_requires)
if(AIOT_MOQT_AUDIT)
    list(APPEND audit_driver_requires esp_driver_usb_serial_jtag)
endif()
idf_component_register(
    SRCS "data_packet.c" "ai_result.c" "moq_audit.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_timer esp_system ${audit_driver_requires}
)
