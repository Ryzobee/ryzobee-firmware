#pragma once
#include <stdint.h>
typedef int BaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY UINT32_MAX
#ifndef HOST_TICK_MS
#define HOST_TICK_MS 1U
#endif
#define pdMS_TO_TICKS(milliseconds) ((TickType_t)((milliseconds) / HOST_TICK_MS))
