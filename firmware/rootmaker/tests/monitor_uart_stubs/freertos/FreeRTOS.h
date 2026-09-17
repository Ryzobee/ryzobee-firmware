#pragma once
#include <stddef.h>
#include <stdint.h>
typedef void *TaskHandle_t;
typedef void *QueueHandle_t;
typedef unsigned TickType_t;
typedef int BaseType_t;
#define pdTRUE 1
#define pdFALSE 0
int xPortGetCoreID(void);
