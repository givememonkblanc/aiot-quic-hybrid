#pragma once
#include <stddef.h>
#include <stdint.h>
#define ESP_OK 0
#define JPEG_IMAGE_FORMAT_RGB565 1
#define JPEG_IMAGE_SCALE_1_2 1
#define JPEG_IMAGE_SCALE_1_4 2
typedef struct {uint8_t *indata,*outbuf;uint32_t indata_size,outbuf_size;int out_format,out_scale;struct {unsigned swap_color_bytes;} flags;} esp_jpeg_image_cfg_t;
typedef struct {uint16_t width,height;size_t output_len;} esp_jpeg_image_output_t;
int esp_jpeg_get_image_info(esp_jpeg_image_cfg_t *,esp_jpeg_image_output_t *);
int esp_jpeg_decode(esp_jpeg_image_cfg_t *,esp_jpeg_image_output_t *);
