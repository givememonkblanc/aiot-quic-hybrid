idf_component_register(
    SRCS "app_cfg.c"
    INCLUDE_DIRS "include"
    REQUIRES common presence nvs_flash esp_netif
    PRIV_REQUIRES esp_hw_support
)
target_include_directories(${COMPONENT_LIB} PUBLIC "${CMAKE_SOURCE_DIR}/src/main")
