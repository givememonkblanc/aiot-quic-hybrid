idf_component_register(
    SRCS "app_main.c" "app_tcp_baseline.c"
    INCLUDE_DIRS "."
    REQUIRES common dl_qads transport sources wifi_link esp_camera
            app_cfg presence provisioning ota status_led
            esp_https_ota app_update esp_wifi nvs_flash driver
)

if(DEFINED APP_EXPERIMENT_PRESET)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_EXPERIMENT_PRESET=${APP_EXPERIMENT_PRESET})
endif()
