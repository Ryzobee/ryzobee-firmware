#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

void fake_time_platform_reset(void);
void fake_time_platform_set_start_result(esp_err_t result);
void fake_time_platform_set_now_us(int64_t now_us);
void fake_time_platform_set_start_sync(bool enabled, int64_t unix_seconds);
void fake_time_platform_emit_sync(int64_t unix_seconds);
void fake_time_platform_emit_precise_sync(int64_t unix_seconds, uint32_t subsecond_us,
                                         int64_t observed_monotonic_us);
void fake_time_platform_hold_snapshot(void);
void fake_time_platform_release_snapshot(void);
unsigned fake_time_platform_init_count(void);
unsigned fake_time_platform_start_count(void);
