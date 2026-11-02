idf_component_register(SRCS "app_main.cpp" INCLUDE_DIRS "." REQUIRES cat_detect esp_timer app_update)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/../../vendor/esp-dl/examples/cat_detect/main/cat.jpg" BINARY RENAME_TO cat_jpg)
