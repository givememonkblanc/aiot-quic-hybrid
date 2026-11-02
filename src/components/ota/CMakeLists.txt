idf_component_register(
    SRCS "ota.c" "ota_pull.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_https_ota app_update esp_http_client mbedtls nvs_flash
)

# Optional local trust anchor and update endpoint; contains no private key.
if(AIOT_OTA_SITE_HEADER)
    if(NOT EXISTS "${AIOT_OTA_SITE_HEADER}")
        message(FATAL_ERROR "AIOT_OTA_SITE_HEADER does not exist")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE AIOT_OTA_SITE_HEADER="${AIOT_OTA_SITE_HEADER}")
endif()
