#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#define PIXFORMAT_RGB565 1
typedef size_t (*jpg_out_cb)(void *,size_t,const void *,size_t);
bool fmt2jpg_cb(uint8_t *,size_t,uint16_t,uint16_t,int,uint8_t,jpg_out_cb,void *);
