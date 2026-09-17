#pragma once

#include <stdint.h>
#include "esp_err.h"

/* Internal OS seam. init cleans partial allocation on failure; failed start
 * never invokes entry. Binary wake is retained before wait and may coalesce.
 * Protected state, not an event count, is the source of pending work. */
esp_err_t ryz_rgb_platform_init(void);
void ryz_rgb_platform_deinit(void);
esp_err_t ryz_rgb_platform_start(void (*entry)(void *));
void ryz_rgb_platform_lock(void);
void ryz_rgb_platform_unlock(void);
void ryz_rgb_platform_wake(void);
void ryz_rgb_platform_wait(void);
int64_t ryz_rgb_platform_now_ms(void);
