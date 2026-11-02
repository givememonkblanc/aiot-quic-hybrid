#pragma once
#include "queue.h"
typedef QueueHandle_t SemaphoreHandle_t;
#define xSemaphoreCreateBinary() xQueueCreate(1,0)
#define xSemaphoreGive(q) xQueueSend(q,NULL,0)
#define xSemaphoreTake(q,t) xQueueReceive(q,NULL,t)
#define vSemaphoreDelete(q) vQueueDelete(q)
