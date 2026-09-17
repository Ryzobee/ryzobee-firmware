#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float x_mg;
    float y_mg;
    float z_mg;
    uint64_t timestamp_us; /* Monotonic host acquisition completion, not UTC. */
    uint32_t sequence;     /* Boot-lifetime successful samples; saturates. */
} ryz_imu_sample_t;

typedef struct {
    bool ready;
    uint8_t address;       /* Raw 7-bit address, 0x18 or 0x19; zero if unknown. */
    uint8_t who_am_i;
    uint8_t resolution_bits;
    uint8_t full_scale_g;
    uint16_t odr_hz;
    uint32_t samples;
    uint32_t errors;
    esp_err_t last_error;
} ryz_imu_info_t;

/* Trusted C, task context only, synchronous singleton; no heap, task, ISR or
 * raw-register Lua access. All public calls use a nonblocking entry guard;
 * concurrent/reentrant calls return TIMEOUT without changing driver state.
 * Neither this module nor its caller may recreate/reset the shared I2C bus.
 *
 * Every init explicitly re-identifies and reconfigures the sensor, including
 * after prior success (needed for no-data watchdog recovery). It may be retried
 * after failure. Only WHO_AM_I=0x44 at exactly one of 0x18/0x19 is accepted;
 * two matches return INVALID_STATE. A bus timeout is never absence. No sensor
 * configuration is written until identity and uniqueness have been checked.
 * Fixed mode: LIS2DW12 HP14-bit, +/-2g, 25Hz, LPF ODR/4, BDU, auto-increment,
 * FIFO bypass, no interrupts. Soft-reset polling is limited to 10 reads and a
 * checked 100ms deadline; each shared-bus call has its own finite timeout.
 * The limits are NOT a 100ms end-to-end init promise. Failure after selection
 * makes one best-effort power-down write; failure never resets/deletes I2C.
 * Call init off the UI owner. This is not a hardware self-test or proof of PCB
 * BOM, sensor mounting orientation, interrupt wiring or a gyroscope.
 */
esp_err_t ryz_imu_init(void);

/* Stop conversion and invalidate readiness, without deleting the shared bus.
 * A failed power-down is reported, not treated as confirmed hardware OFF. */
esp_err_t ryz_imu_deinit(void);

/* One STATUS read and, only on DRDY, one coherent six-byte burst; never waits
 * for the next conversion. OK means a new sample in physical sensor axes,
 * in mg (1g = 1000mg), without a screen-orientation transform. NOT_FINISHED
 * means no new data, including the first fresh conversion discarded after
 * initialization for LPF settling. No stale sample is returned. All non-OK
 * paths clear a non-NULL output; real I2C errors clear ready until init succeeds.
 */
esp_err_t ryz_imu_read_sample(ryz_imu_sample_t *sample);

/* Copies only cached diagnostic state; no I2C or waiting. Errors and samples
 * are saturating boot-lifetime counters. Failed/busy calls clear the output.
 * ready proves ID/configuration admission only, not recent data or self-test.
 */
esp_err_t ryz_imu_get_info(ryz_imu_info_t *info);

#ifdef __cplusplus
}
#endif
