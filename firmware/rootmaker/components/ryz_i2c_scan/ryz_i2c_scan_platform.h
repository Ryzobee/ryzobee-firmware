#pragma once

#include <stdint.h>
#include "esp_err.h"

/* Internal OS seam. init rolls back partial mutex/event allocation itself;
 * failed start never invokes entry. The binary event retains a wake before
 * wait; callers always inspect protected state rather than count events. */
esp_err_t ryz_i2c_scan_platform_init(void);
void ryz_i2c_scan_platform_deinit(void);
esp_err_t ryz_i2c_scan_platform_start(void (*entry)(void *));
void ryz_i2c_scan_platform_lock(void);
void ryz_i2c_scan_platform_unlock(void);
void ryz_i2c_scan_platform_wake(void);
void ryz_i2c_scan_platform_wait(void);
/* Yield for at least one OS tick; request wakeups do not shorten this delay. */
void ryz_i2c_scan_platform_delay_ms(uint32_t milliseconds);
int64_t ryz_i2c_scan_platform_now_ms(void);
