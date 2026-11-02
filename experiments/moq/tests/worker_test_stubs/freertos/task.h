#pragma once
#include "FreeRTOS.h"
BaseType_t xTaskCreatePinnedToCore(void (*)(void*),const char*,unsigned,void*,unsigned,void*,unsigned);
void vTaskDelay(TickType_t);
void vTaskDelete(void*);
