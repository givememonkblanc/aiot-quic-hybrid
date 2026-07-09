idf_component_register(
    SRCS "ai_inference.c" "ai_inference_model_data.c" "ai_inference_tflm.cc" "ai_inference_person_model.c"
    INCLUDE_DIRS "include"
    REQUIRES common espressif__esp_jpeg espressif__esp-tflite-micro
)
