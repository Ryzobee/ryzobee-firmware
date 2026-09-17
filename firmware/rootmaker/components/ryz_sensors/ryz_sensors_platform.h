#pragma once

#include <stdint.h>
#include "esp_err.h"

/* Internal OS seam: the same boot-lifetime worker runs on FreeRTOS and in
 * controlled-clock Host tests. No platform function owns the I2C bus. */
esp_err_t ryz_sensors_platform_init(void);
void ryz_sensors_platform_deinit(void);
/* Failure guarantees that entry was not started and retains no entry pointer. */
esp_err_t ryz_sensors_platform_start(void (*entry)(void *));
void ryz_sensors_platform_lock(void);
void ryz_sensors_platform_unlock(void);
int64_t ryz_sensors_platform_now_ms(void);
void ryz_sensors_platform_wait_ms(uint32_t milliseconds);
