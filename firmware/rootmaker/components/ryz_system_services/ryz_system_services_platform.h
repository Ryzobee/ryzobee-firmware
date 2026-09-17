#pragma once

#include <stdint.h>

#include "ryz_system_services.h"

/* Internal OS seam. Production uses a FreeRTOS task/mutex; host checks run
 * the same owner loop with a controlled clock and dependency outcomes. */
esp_err_t ryz_system_services_platform_init(void);
void ryz_system_services_platform_deinit(void);
esp_err_t ryz_system_services_platform_start(void (*entry)(void *));
void ryz_system_services_platform_lock(void);
void ryz_system_services_platform_unlock(void);
int64_t ryz_system_services_platform_now_us(void);
void ryz_system_services_platform_wait_ms(uint32_t milliseconds);
void ryz_system_services_platform_report(
    ryz_system_services_stage_t stage, esp_err_t error, uint32_t suppressed);
