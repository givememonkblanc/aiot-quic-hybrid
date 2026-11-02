if(AIOT_ESPDET_RUNTIME OR "$ENV{AIOT_ESPDET_RUNTIME}" STREQUAL "1")
    idf_component_register(SRCS "ai_inference_espdet.cc" INCLUDE_DIRS "include"
        REQUIRES common cat_detect dl_qads esp_timer)
    return()
endif()
set(MODEL_SOURCE "ai_inference_vehicle_model.c")
if(AIOT_MOBILITY_CAMERA)
    set(PACKED_MODEL_DIR "${CMAKE_CURRENT_BINARY_DIR}/packed_model")
    set(AIOT_CAMERA_MODEL_SOURCE "${CMAKE_CURRENT_LIST_DIR}/ai_inference_vehicle_model.c" CACHE FILEPATH "Camera model C array")
    set(AIOT_CAMERA_MODEL_METADATA "${CMAKE_CURRENT_LIST_DIR}/include/ai_inference_model_asset_generated.h" CACHE FILEPATH "Camera model metadata")
    execute_process(COMMAND ${PYTHON} "${CMAKE_CURRENT_LIST_DIR}/tools/pack_model.py"
        "${AIOT_CAMERA_MODEL_SOURCE}"
        "${AIOT_CAMERA_MODEL_METADATA}"
        "${PACKED_MODEL_DIR}" COMMAND_ERROR_IS_FATAL ANY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${AIOT_CAMERA_MODEL_SOURCE}" "${AIOT_CAMERA_MODEL_METADATA}" "tools/pack_model.py")
    set(MODEL_SOURCE "${PACKED_MODEL_DIR}/ai_model_compact.c")
endif()
idf_component_register(
    SRCS "ai_inference.c" "ai_inference_model_data.c" "ai_inference_tflm.cc" ${MODEL_SOURCE}
    INCLUDE_DIRS "include"
    REQUIRES common espressif__esp_jpeg espressif__esp-tflite-micro
)
if(AIOT_MOBILITY_CAMERA)
    target_include_directories(${COMPONENT_LIB} PRIVATE "${PACKED_MODEL_DIR}")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE AIOT_MODEL_COMPRESSED=1)
endif()

option(AIOT_AI_DARK_BLOBS "Detect dark objects on a bright plate instead of running the model" OFF)
set(AIOT_AI_DARK_FRACTION "55" CACHE STRING "Dark pixel threshold, percent of the median luma")
# The model sees a region of the frame, and every local maximum of its output is one object.
# The frame is decoded at 1/8 (UXGA: 200x150), or at 1/4 (400x300), which takes a full
# inverse DCT and is several times slower on the ESP32-S3.
option(AIOT_AI_ROI "Model input: a region of the decoded frame" OFF)
set(AIOT_AI_ROI_BOX "300,300,700,700" CACHE STRING "Region x0,y0,x1,y1 in thousandths of the frame")
set(AIOT_AI_ROI_SCALE "8" CACHE STRING "Decoding scale of the frame for the region: 8 or 4")
if(AIOT_AI_ROI)
    if(NOT AIOT_AI_ROI_SCALE MATCHES "^[48]$")
        message(FATAL_ERROR "AIOT_AI_ROI_SCALE must be 8 or 4")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE AIOT_AI_ROI_SCALE=${AIOT_AI_ROI_SCALE})
    string(REPLACE "," ";" AIOT_AI_ROI_LIST "${AIOT_AI_ROI_BOX}")
    list(GET AIOT_AI_ROI_LIST 0 AIOT_AI_ROI_X0)
    list(GET AIOT_AI_ROI_LIST 1 AIOT_AI_ROI_Y0)
    list(GET AIOT_AI_ROI_LIST 2 AIOT_AI_ROI_X1)
    list(GET AIOT_AI_ROI_LIST 3 AIOT_AI_ROI_Y1)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE AIOT_AI_ROI=1 AIOT_AI_ROI_X0=${AIOT_AI_ROI_X0}
        AIOT_AI_ROI_Y0=${AIOT_AI_ROI_Y0} AIOT_AI_ROI_X1=${AIOT_AI_ROI_X1} AIOT_AI_ROI_Y1=${AIOT_AI_ROI_Y1})
endif()
option(AIOT_AI_LUMA_NORM "Model input: row offsets removed, a dark region raised to the day level" OFF)
if(AIOT_AI_LUMA_NORM)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE AIOT_AI_LUMA_NORM=1)
endif()
if(AIOT_AI_DARK_BLOBS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE AIOT_AI_DARK_BLOBS=1 AIOT_AI_DARK_FRACTION=${AIOT_AI_DARK_FRACTION})
endif()
