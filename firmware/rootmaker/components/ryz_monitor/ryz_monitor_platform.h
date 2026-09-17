#pragma once
#include "esp_err.h"
#include <stdint.h>

esp_err_t ryz_monitor_platform_init(void);
void ryz_monitor_platform_deinit(void);
esp_err_t ryz_monitor_platform_start(void (*entry)(void *));
void ryz_monitor_platform_lock(void);
void ryz_monitor_platform_unlock(void);
void ryz_monitor_platform_wake(void);
void ryz_monitor_platform_wait(void);
void ryz_monitor_platform_delay_ms(uint32_t milliseconds);
int64_t ryz_monitor_platform_now_ms(void);
