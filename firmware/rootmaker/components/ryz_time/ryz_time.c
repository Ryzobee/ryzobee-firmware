#include "ryz_time.h"

#include <stddef.h>
#include <stdatomic.h>

#include "ryz_time_platform.h"

#define SYNC_WINDOW_US INT64_C(60000000)

static atomic_bool s_initialized = ATOMIC_VAR_INIT(false);
static ryz_time_snapshot_t s_snapshot;
/* Protected with the snapshot; only meaningful while online and SYNCING. */
static int64_t s_sync_started_us;
static bool s_failure_pending;
/* One immutable calibration plus a read high-water mark, all under the same
 * mutex as the public sync observation. Faults invalidate only this derived
 * current-time anchor; existing snapshot/retry semantics remain unchanged. */
static struct {
    int64_t unix_seconds;
    uint32_t subsecond_us;
    int64_t monotonic_us;
    int64_t last_observed_us;
    bool valid;
} s_utc_anchor;

static void receive_sync(int64_t unix_seconds, uint32_t subsecond_us,
                         int64_t observed_monotonic_us)
{
    if (!atomic_load_explicit(&s_initialized, memory_order_acquire)) return;
    ryz_time_platform_snapshot_lock();
    const int64_t now = ryz_time_platform_now_us();
    if (unix_seconds < RYZ_TIME_MIN_VALID_UNIX_SECONDS || subsecond_us >= 1000000U ||
        observed_monotonic_us < 0 || now < observed_monotonic_us) {
        s_snapshot.state = RYZ_TIME_FAILED;
        s_snapshot.last_error = ESP_ERR_INVALID_RESPONSE;
        s_snapshot.clock_valid = false;
        s_snapshot.last_sync_unix = 0;
        s_utc_anchor.valid = false;
        /* Return this asynchronous error once to the periodic owner, so its
         * normal retry backoff also applies to invalid clock responses. */
        s_failure_pending = true;
    } else {
        s_snapshot.clock_valid = true;
        s_snapshot.last_sync_unix = unix_seconds;
        s_utc_anchor.unix_seconds = unix_seconds;
        s_utc_anchor.subsecond_us = subsecond_us;
        s_utc_anchor.monotonic_us = observed_monotonic_us;
        s_utc_anchor.last_observed_us = now;
        s_utc_anchor.valid = true;
        s_snapshot.state = s_snapshot.network_ready
            ? RYZ_TIME_SYNCED
            : RYZ_TIME_WAITING_NETWORK;
        s_snapshot.last_error = ESP_OK;
        s_failure_pending = false;
    }
    ++s_snapshot.revision;
    ryz_time_platform_snapshot_unlock();
}

esp_err_t ryz_time_init(void)
{
    if (atomic_load_explicit(&s_initialized, memory_order_acquire)) return ESP_OK;
    esp_err_t error = ryz_time_platform_init(receive_sync);
    if (error != ESP_OK) return error;

    ryz_time_platform_snapshot_lock();
    s_snapshot = (ryz_time_snapshot_t){
        .state = RYZ_TIME_WAITING_NETWORK,
        .revision = 1,
        .last_error = ESP_OK,
    };
    s_sync_started_us = 0;
    s_failure_pending = false;
    s_utc_anchor.valid = false;
    atomic_store_explicit(&s_initialized, true, memory_order_release);
    ryz_time_platform_snapshot_unlock();
    return ESP_OK;
}

esp_err_t ryz_time_set_network_ready(bool ready)
{
    if (!atomic_load_explicit(&s_initialized, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }

    const int64_t now = ryz_time_platform_now_us();
    ryz_time_platform_snapshot_lock();
    if (ready && s_snapshot.network_ready) {
        if (s_snapshot.state == RYZ_TIME_SYNCING && now >= s_sync_started_us &&
            (uint64_t)now - (uint64_t)s_sync_started_us >= (uint64_t)SYNC_WINDOW_US) {
            s_snapshot.state = RYZ_TIME_FAILED;
            s_snapshot.last_error = ESP_ERR_TIMEOUT;
            ++s_snapshot.revision;
            /* This ends our waiting window, not the SDK's background client.
             * A later callback still describes the actual system clock. */
            ryz_time_platform_snapshot_unlock();
            return ESP_ERR_TIMEOUT;
        }
        if (s_snapshot.state == RYZ_TIME_FAILED && s_failure_pending) {
            esp_err_t error = s_snapshot.last_error;
            s_failure_pending = false;
            ryz_time_platform_snapshot_unlock();
            return error;
        }
    }
    const bool retry_failed_start =
        ready && s_snapshot.network_ready &&
        s_snapshot.state == RYZ_TIME_FAILED;
    if (s_snapshot.network_ready == ready && !retry_failed_start) {
        ryz_time_platform_snapshot_unlock();
        return ESP_OK;
    }
    s_snapshot.network_ready = ready;
    s_snapshot.state = ready ? RYZ_TIME_SYNCING : RYZ_TIME_WAITING_NETWORK;
    s_snapshot.last_error = ESP_OK;
    s_failure_pending = false;
    s_sync_started_us = now;
    ++s_snapshot.revision;
    const uint32_t start_revision = s_snapshot.revision;
    ryz_time_platform_snapshot_unlock();

    if (!ready) return ESP_OK;
    esp_err_t error = ryz_time_platform_start();
    if (error == ESP_OK) return ESP_OK;

    ryz_time_platform_snapshot_lock();
    /* start synchronously enters TCP/IP, which may deliver a clock callback.
     * Never hold this lock across it or overwrite a newer clock observation
     * with the older start result. This revision is not a network request ID. */
    if (s_snapshot.revision == start_revision && s_snapshot.network_ready &&
        s_snapshot.state == RYZ_TIME_SYNCING) {
        s_snapshot.state = RYZ_TIME_FAILED;
        s_snapshot.last_error = error;
        ++s_snapshot.revision;
    } else if (s_snapshot.state == RYZ_TIME_FAILED) {
        error = s_snapshot.last_error;
        s_failure_pending = false;
    } else {
        error = ESP_OK;
    }
    ryz_time_platform_snapshot_unlock();
    return error;
}

esp_err_t ryz_time_get_snapshot(ryz_time_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL) return ESP_ERR_INVALID_ARG;
    if (!atomic_load_explicit(&s_initialized, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    ryz_time_platform_snapshot_lock();
    *out_snapshot = s_snapshot;
    ryz_time_platform_snapshot_unlock();
    return ESP_OK;
}

esp_err_t ryz_time_sample_utc(ryz_time_utc_sample_t *out_sample)
{
    if (out_sample == NULL) return ESP_ERR_INVALID_ARG;
    *out_sample = (ryz_time_utc_sample_t){0};
    if (!atomic_load_explicit(&s_initialized, memory_order_acquire))
        return ESP_ERR_INVALID_STATE;
    if (!ryz_time_platform_snapshot_try_lock()) return ESP_ERR_TIMEOUT;
    if (s_utc_anchor.valid && s_snapshot.clock_valid) {
        /* Sample AFTER acquiring the calibration lock: a newer callback must
         * never be combined with a monotonic sample taken before that callback. */
        const int64_t now = ryz_time_platform_now_us();
        if (now < s_utc_anchor.monotonic_us || now < s_utc_anchor.last_observed_us) {
            s_utc_anchor.valid = false;
        } else {
            const uint64_t elapsed = (uint64_t)now - (uint64_t)s_utc_anchor.monotonic_us;
            const uint64_t seconds = elapsed / UINT64_C(1000000) +
                (elapsed % UINT64_C(1000000) + s_utc_anchor.subsecond_us) / UINT64_C(1000000);
            if (seconds > (uint64_t)(INT64_MAX - s_utc_anchor.unix_seconds)) {
                s_utc_anchor.valid = false;
            } else {
                out_sample->unix_seconds = s_utc_anchor.unix_seconds + (int64_t)seconds;
                out_sample->valid = true;
                s_utc_anchor.last_observed_us = now;
            }
        }
    }
    ryz_time_platform_snapshot_unlock();
    return ESP_OK;
}
