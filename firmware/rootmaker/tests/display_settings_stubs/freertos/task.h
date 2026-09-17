#pragma once
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
BaseType_t xTaskCreate(void (*entry)(void *),const char *name,unsigned stack,
                       void *context,unsigned priority,TaskHandle_t *out);
void vTaskDelay(TickType_t delay);
