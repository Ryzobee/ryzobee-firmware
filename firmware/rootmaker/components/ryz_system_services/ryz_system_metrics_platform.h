#pragma once

#include <stdint.h>
#include "esp_err.h"

/* Both are task-context-only observations. Caller never holds its public
 * snapshot lock across these. Failed outputs must not be consumed. */
esp_err_t ryz_system_metrics_platform_temperature(float *out_celsius);
/* Paired local-core observations, not one timestamp shared by two serial
 * IPC calls. All four values are cleared on failure. */
esp_err_t ryz_system_metrics_platform_idle(uint64_t idle_us[2],
                                          uint64_t sampled_at_us[2]);
