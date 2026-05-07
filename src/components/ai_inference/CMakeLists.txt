idf_component_register(
    SRCS "ai_inference.c" "ai_inference_model_data.c" "ai_inference_tflm.cc"
    INCLUDE_DIRS "include"
    REQUIRES common espressif__esp_jpeg espressif__esp-tflite-micro
)
