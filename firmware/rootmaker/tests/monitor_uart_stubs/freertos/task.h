#pragma once
#include "FreeRTOS.h"
int xTaskGetCoreID(TaskHandle_t task);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
