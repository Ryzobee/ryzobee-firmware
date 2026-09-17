#pragma once
typedef void *TaskHandle_t;
TaskHandle_t xTaskGetCurrentTaskHandle(void);
void ryz_host_yield(void);
#define taskYIELD() ryz_host_yield()
