#pragma once
#include "FreeRTOS.h"
BaseType_t xQueueReceive(QueueHandle_t queue, void *out, TickType_t wait);
