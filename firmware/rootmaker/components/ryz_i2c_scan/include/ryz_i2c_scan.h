#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "ryz_i2c_scan_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Validate fixed board pins and standard 100/400kHz. Only the exact default
 * 41/40/100kHz pair borrows I2C0. Everything else must be an eligible, distinct
 * external pair. Validation does not allocate or access GPIO/I2C. */
esp_err_t ryz_i2c_scan_validate_config(const ryz_i2c_scan_config_t *config);

/* CAS-commit idle configuration, never a physical probe/init or pin claim.
 * Lazily creates the worker as needed. Initial/default revision is 1; reject
 * stale revision, active/releasing work, or resources held after failed
 * teardown. Reject unavailable custom pins without altering old configuration.
 * Actual resource admission is repeated when scanning starts. No persistence
 * or automatic scan. Revision never wraps, out is zero on failure. */
esp_err_t ryz_i2c_scan_configure(const ryz_i2c_scan_config_t *config,
                              uint32_t expected_revision, uint32_t *out_revision);

/* Trusted C Interface, scanning the currently committed configuration.
 * Lazily creates one boot-lifetime priority-2/4096-byte worker pinned to CPU1
 * (I2C1 interrupt allocation and teardown stay on the same core), then enqueues
 * one scan. No I2C in this caller. Out is cleared on failure.
 * Active work rejects a new start;
 * IDs increase within the boot and never wrap. Worker allocation is retryable.
 * Status/requests never reset/reconfigure/delete the touch/IMU bus or handles.
 * Custom I2C1 is created/leased only by the worker and released at scan end. */
esp_err_t ryz_i2c_scan_start(uint32_t *out_id);

/* Token-checked cancellation request, not synchronous completion. An in-flight
 * probe may finish; CANCELLED is published only after worker acknowledgement
 * and successful custom resource release. Failed cleanup is FAILED, never a
 * successful cancellation; cleanup_error/resources_held describe the fault.
 * Cancelling that same FAILED/held ID retries cleanup only, without another
 * scan/probe. A different or historical ID cannot affect current resources.
 * No later probe from that scan can start after CANCELLED. Repeating cancel on
 * that CANCELLED ID succeeds; an old ID cannot cancel a later scan. */
esp_err_t ryz_i2c_scan_cancel(uint32_t scan_id);

/* Cached copy only, no allocation, I2C or implicit worker startup. Before
 * worker creation returns INVALID_STATE with cleared output. Scan results are
 * last-observed ACKs, not continuously monitored devices. A conclusive empty
 * scan requires COMPLETED, 112 NACKs and zero uncertain/unscanned addresses. */
esp_err_t ryz_i2c_scan_get_snapshot(ryz_i2c_scan_snapshot_t *out);

#ifdef __cplusplus
}
#endif
