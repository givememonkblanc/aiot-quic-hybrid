idf_component_register(
    SRCS "display.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES esp_lcd driver esp_wifi esp_netif esp_timer esp_app_format app_update
)
