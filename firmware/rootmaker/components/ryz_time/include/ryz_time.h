#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 2024-01-01T00:00:00Z. Earlier values cannot establish a valid TLS clock. */
#define RYZ_TIME_MIN_VALID_UNIX_SECONDS INT64_C(1704067200)

typedef enum {
    RYZ_TIME_WAITING_NETWORK = 0,
    RYZ_TIME_SYNCING,
    RYZ_TIME_SYNCED,
    RYZ_TIME_FAILED,
} ryz_time_state_t;

typedef struct {
    ryz_time_state_t state;
    uint32_t revision;
    esp_err_t last_error;
    bool network_ready;
    bool clock_valid;
    int64_t last_sync_unix;
} ryz_time_snapshot_t;

typedef struct {
    int64_t unix_seconds;
    bool valid;
} ryz_time_utc_sample_t;

/** Initialize on the sole system owner; idempotent after success. */
esp_err_t ryz_time_init(void);

/**
 * Sole system owner periodically publishes provisioning ONLINE/OFFLINE.
 * SNTP starts on an offline-to-online transition. Repeated ONLINE observes a
 * fixed 60-second monotonic initial-sync window, without extending it. Its
 * expiry returns ESP_ERR_TIMEOUT and publishes FAILED; that call does not
 * restart SNTP. An asynchronous invalid clock is likewise returned once.
 * A later ONLINE call retries FAILED; the owner applies its five-second
 * backoff. OFFLINE immediately cancels local waiting, not the SDK client.
 * Previously calibrated time survives timeout/offline. Late clock callbacks
 * still publish the actual clock, not an attributed network attempt result.
 * Do not call concurrently or reenter this function from platform callbacks.
 */
esp_err_t ryz_time_set_network_ready(bool ready);

/** Return a coherent read-only copy; does not advance deadlines or do I/O. */
esp_err_t ryz_time_get_snapshot(ryz_time_snapshot_t *out_snapshot);

/** Sample current calibrated UTC, rounded down to a whole Unix second.
 * No network operation, timezone conversion, allocation or waiting: one
 * try-lock only. Non-NULL output is first cleared; missing initialization
 * returns INVALID_STATE and lock contention TIMEOUT. ESP_OK with valid=false
 * means no usable calibration, not Unix epoch zero. Offline/timeout does not
 * expire calibration. Invalid sync, monotonic regression or arithmetic
 * overflow leaves this sample unknown until a new valid sync arrives.
 * last_sync_unix remains a diagnostic sync point, not a substitute for this
 * sample. A later calibration may legitimately move UTC backwards. */
esp_err_t ryz_time_sample_utc(ryz_time_utc_sample_t *out_sample);

#ifdef __cplusplus
}
#endif
