/* Experiment-only: retain one aligned RGB output buffer across conditions.
 * Same decoder/configuration as ESP-DL sw_decode_jpeg, without a recurring
 * 3 MiB contiguous allocation changing whether the AI workload exists. */
#include "esp_jpeg_dec.h"
static dl::image::img_t batch_decode_jpeg(const dl::image::jpeg_img_t &input)
{
    static void *rgb;
    constexpr size_t capacity=1024U*1024U*3U;
    jpeg_dec_config_t cfg={.output_type=JPEG_PIXEL_FORMAT_RGB888,
        .scale={.width=0,.height=0},.clipper={.width=0,.height=0},
        .rotate=JPEG_ROTATE_0D,.block_enable=false};
    jpeg_dec_handle_t decoder=nullptr;
    if(jpeg_dec_open(&cfg,&decoder)!=JPEG_ERR_OK) return {};
    jpeg_dec_io_t io{};io.inbuf=(uint8_t*)input.data;io.inbuf_len=input.data_len;
    jpeg_dec_header_info_t info{};
    if(jpeg_dec_parse_header(decoder,&io,&info)!=JPEG_ERR_OK) {jpeg_dec_close(decoder);return {};}
    dl::image::img_t img{};img.width=info.width;img.height=info.height;img.pix_type=dl::image::DL_IMAGE_PIX_TYPE_RGB888;
    if(!info.width||!info.height||info.width>1024||info.height>1024||img.bytes()>capacity) {
        jpeg_dec_close(decoder);return {};
    }
    if(!rgb) rgb=heap_caps_aligned_alloc(16,capacity,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!rgb) {jpeg_dec_close(decoder);return {};}
    io.outbuf=(uint8_t*)rgb;
    auto status=jpeg_dec_process(decoder,&io);
    jpeg_dec_close(decoder);
    if(status!=JPEG_ERR_OK) return {};
    img.data=rgb;return img;
}
