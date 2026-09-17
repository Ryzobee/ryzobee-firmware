#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "imu.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool started;
    bool initialized;
    bool ever_sampled;
    /* Last successful sample in the current initialized generation. A short
     * NO_DATA interval keeps it, with its original timestamp. Real failures
     * and the one-second no-data watchdog clear sample and this flag. */
    bool sample_valid;
    /* Driver diagnostics, with ready additionally revoked by worker errors
     * (including no-data watchdog). Not a hardware self-test result. */
    ryz_imu_info_t imu;
    ryz_imu_sample_t sample;
    /* Boot-monotonic, never a wall clock. Historical time survives errors.
     * Age is computed by get_snapshot; both require ever_sampled == true. */
    uint64_t last_sampled_ms;
    uint64_t sample_age_ms;
    esp_err_t last_error;
    uint64_t last_error_ms;
    /* Saturating boot-lifetime worker counters. NO_DATA is not an error until
     * one second passes without a fresh sample; each watchdog counts once. */
    uint32_t init_attempts;
    uint32_t read_attempts;
    uint32_t sample_count;
    uint32_t no_data_count;
    uint32_t error_count;
} ryz_sensors_snapshot_t;

/* Create the boot-lifetime, priority-3/4096-byte-stack IMU owner. No driver/I2C
 * call occurs in the caller. Success means task creation, not sensor health.
 * Repeated success is idempotent; concurrent startup returns INVALID_STATE.
 * Failed platform/task allocation is retryable without a duplicate worker. */
esp_err_t ryz_sensors_start(void);

/* Copy cached state under a short mutex, then calculate age without I2C.
 * Before successful start returns INVALID_STATE and clears out. Sensor errors
 * are data in a successful snapshot, never system startup health-gate results.
 * Consumers must check sample_valid AND age; no result implies hardware PASS. */
esp_err_t ryz_sensors_get_snapshot(ryz_sensors_snapshot_t *out);

#ifdef __cplusplus
}
#endif
