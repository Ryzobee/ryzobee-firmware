#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** Single SDK callback producer. Capture the monotonic timestamp together
 * with the IMMED sync observation BEFORE waiting for the core snapshot lock;
 * a delayed callback must not rejuvenate its UTC anchor at publication. */
typedef void (*ryz_time_platform_sync_sink_t)(int64_t unix_seconds,
    uint32_t subsecond_us, int64_t observed_monotonic_us);

void ryz_time_platform_snapshot_lock(void);
bool ryz_time_platform_snapshot_try_lock(void);
void ryz_time_platform_snapshot_unlock(void);
esp_err_t ryz_time_platform_init(ryz_time_platform_sync_sink_t sink);
esp_err_t ryz_time_platform_start(void);
/* Monotonic time, independent of the wall clock being changed by SNTP. */
int64_t ryz_time_platform_now_us(void);
