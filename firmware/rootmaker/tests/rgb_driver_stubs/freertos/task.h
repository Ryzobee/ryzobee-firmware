#pragma once
#include "freertos/FreeRTOS.h"
typedef void *TaskHandle_t;
TaskHandle_t xTaskGetCurrentTaskHandle(void);
BaseType_t xTaskGetCoreID(TaskHandle_t task);
