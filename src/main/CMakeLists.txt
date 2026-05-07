idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES common dl_qads receiver transport sources wifi_link esp_camera
)

if(DEFINED APP_EXPERIMENT_PRESET)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_EXPERIMENT_PRESET=${APP_EXPERIMENT_PRESET})
endif()
