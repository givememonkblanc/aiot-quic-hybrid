idf_component_register(
    SRCS "provisioning_console.c" "provisioning_portal.c"
    INCLUDE_DIRS "include"
    REQUIRES app_cfg esp_http_server esp_wifi esp_netif esp_event esp_hw_support
)
