#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Portable value contract shared by native C and the application facade.
 * Error fields retain native diagnostic integers; no driver handles or SDK
 * dependency. The native Adapter interprets their numeric error namespace. */
#define RYZ_I2C_SCAN_FIRST 0x08U
#define RYZ_I2C_SCAN_LAST 0x77U
#define RYZ_I2C_SCAN_ADDRESSES 112U
#define RYZ_I2C_SCAN_ATTEMPTS 4U

typedef enum {
    RYZ_I2C_SCAN_IDLE = 0,
    RYZ_I2C_SCAN_QUEUED,
    RYZ_I2C_SCAN_RUNNING,
    RYZ_I2C_SCAN_CANCELLING,
    RYZ_I2C_SCAN_COMPLETED,
    RYZ_I2C_SCAN_CANCELLED,
    RYZ_I2C_SCAN_FAILED,
    /* A final/cancel result is not published until worker teardown returns. */
    RYZ_I2C_SCAN_RELEASING,
} ryz_i2c_scan_phase_t;

typedef struct {
    int sda, scl;
    uint32_t hz;
} ryz_i2c_scan_config_t;

#define RYZ_I2C_SCAN_DEFAULT_CONFIG ((ryz_i2c_scan_config_t){41, 40, 100000})

typedef enum {
    RYZ_I2C_SCAN_UNSCANNED = 0,
    RYZ_I2C_SCAN_ACK,
    RYZ_I2C_SCAN_NACK,
    RYZ_I2C_SCAN_BUSY,
    RYZ_I2C_SCAN_TIMEOUT,
    RYZ_I2C_SCAN_ERROR,
} ryz_i2c_scan_result_t;

typedef struct {
    /* Configuration is boot-local. scan_config is frozen at scan admission;
     * changing idle configuration must not relabel historical scan results. */
    ryz_i2c_scan_config_t config, scan_config;
    uint32_t config_revision, scan_config_revision;
    int cleanup_error;
    /* Last worker observation. In active phases this is not a promise that
     * pins are free; begin/end may be in progress outside the snapshot lock. */
    bool resources_held;
    uint32_t scan_id;
    ryz_i2c_scan_phase_t phase;
    /* Fatal controller/init or cleanup error. Per-address errors are below;
     * COMPLETED means traversal, not that every address is conclusive. */
    int error;
    int64_t queued_ms;
    int64_t started_ms;
    int64_t finished_ms;
    uint16_t completed_addresses;
    uint16_t probe_calls;
    uint16_t busy_responses;
    uint16_t ack_count, nack_count, busy_count, timeout_count, error_count;
    uint8_t current_address; /* 0 outside RUNNING/CANCELLING. */
    /* Indexed by the actual 7-bit address. Reserved addresses stay UNSCANNED.
     * ACK/NACK have OK/NOT_FOUND errors; BUSY has NOT_FINISHED. These are
     * address acknowledgements, never device-model identification. */
    uint8_t results[128];
    int errors[128];
} ryz_i2c_scan_snapshot_t;
