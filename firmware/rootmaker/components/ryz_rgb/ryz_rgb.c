#include "ryz_rgb.h"
#include "ryz_rgb_platform.h"
#include "ryz_rgb_driver.h"

#include <stdatomic.h>
#include <string.h>

enum { STOPPED, STARTING, READY };
static atomic_int s_lifecycle = ATOMIC_VAR_INIT(STOPPED);
static ryz_rgb_snapshot_t s_snapshot;
static uint32_t s_last_id;
/* Admission is separate from the short snapshot mutex. No RMT work runs
 * under either lock. Every public writer, including old RPC entrypoints,
 * participates, so a cached idle observation cannot race a new owner. */
static atomic_flag s_admission = ATOMIC_FLAG_INIT;
static uint32_t s_last_lease, s_lease, s_last_closed_lease, s_lease_request;
static uint32_t s_lease_completed;
static ryz_rgb_color_t s_lease_color;
static bool s_lease_closing;

static bool enter(void)
{ return !atomic_flag_test_and_set_explicit(&s_admission, memory_order_acquire); }
static void leave(void) { atomic_flag_clear_explicit(&s_admission, memory_order_release); }
static bool active(ryz_rgb_phase_t phase)
{ return phase == RYZ_RGB_QUEUED || phase == RYZ_RGB_SENDING || phase == RYZ_RGB_CLEANING; }
/* With a live worker these fields are under the snapshot mutex. Before
 * startup the admission gate is sufficient, as no worker can access them. */
static void retire_lease(void)
{
    s_last_closed_lease = s_lease;
    s_lease = s_lease_request = s_lease_completed = 0;
    s_lease_closing = false;
    memset(&s_lease_color, 0, sizeof(s_lease_color));
}

static void rgb_task(void *unused)
{
    (void)unused;
    for (;;) {
        uint32_t id = 0;
        bool cleanup = false;
        ryz_rgb_color_t color = {0};
        ryz_rgb_platform_lock();
        if (s_snapshot.phase == RYZ_RGB_QUEUED) {
            id = s_snapshot.request_id;
            color = s_snapshot.requested;
            s_snapshot.phase = RYZ_RGB_SENDING;
            s_snapshot.output_known = false;
        } else if (s_snapshot.phase == RYZ_RGB_CLEANING) {
            id = s_snapshot.request_id;
            cleanup = true;
        }
        ryz_rgb_platform_unlock();
        if (!id) {
            /* A retained/coalesced wake only requests another state check. */
            ryz_rgb_platform_wait();
            continue;
        }

        /* The driver receives a value copy. No snapshot lock spans RMT work,
         * and a failed send is never automatically retried by this worker. */
        esp_err_t error = cleanup ? ryz_rgb_driver_cleanup() : ryz_rgb_driver_write(color);
        ryz_rgb_driver_status_t status = ryz_rgb_driver_get_status();
        if (cleanup && error == ESP_OK && (status.resources_held || status.cleanup_error != ESP_OK)) {
            error = status.cleanup_error != ESP_OK ? status.cleanup_error : ESP_ERR_INVALID_STATE;
        }
        ryz_rgb_platform_lock();
        if (s_snapshot.request_id == id) {
            s_snapshot.resources_held = status.resources_held;
            s_snapshot.cleanup_error = cleanup ? error : status.cleanup_error;
            if (cleanup) {
                s_snapshot.phase = error == ESP_OK ? RYZ_RGB_CLEANED : RYZ_RGB_FAILED;
                s_snapshot.finished_ms = ryz_rgb_platform_now_ms();
                if (error == ESP_OK && s_lease && s_lease_closing && s_lease_request == id)
                    retire_lease();
            } else {
                s_snapshot.error = error;
                s_snapshot.output_known = error == ESP_OK;
                if (error == ESP_OK) {
                    s_snapshot.last_completed_id = id;
                    s_snapshot.last_completed_color = color;
                    if (s_lease && s_lease_request == id) {
                        s_lease_completed = id;
                        s_lease_color = color;
                    }
                }
                /* A cleanup accepted during write must not be overwritten by
                 * the write result. Only this worker can next tear it down. */
                if (s_snapshot.phase != RYZ_RGB_CLEANING) {
                    s_snapshot.finished_ms = ryz_rgb_platform_now_ms();
                    s_snapshot.phase = error == ESP_OK ? RYZ_RGB_COMPLETED : RYZ_RGB_FAILED;
                }
            }
        }
        ryz_rgb_platform_unlock();
    }
}

static esp_err_t submit(uint32_t lease, ryz_rgb_color_t color, uint32_t *out_id)
{
    if (!out_id) return ESP_ERR_INVALID_ARG;
    *out_id = 0;
    if (!enter()) return ESP_ERR_INVALID_STATE;
    bool ready = atomic_load_explicit(&s_lifecycle, memory_order_acquire) == READY;
    if (ready) ryz_rgb_platform_lock();
    bool allowed = lease ? lease == s_lease && !s_lease_closing : !s_lease;
    if (ready) ryz_rgb_platform_unlock();
    if (!allowed) { leave(); return ESP_ERR_NOT_FINISHED; }
    int expected = STOPPED;
    bool creating = atomic_compare_exchange_strong(&s_lifecycle, &expected, STARTING);
    if (!creating && expected != READY) { leave(); return ESP_ERR_INVALID_STATE; }
    if (creating) {
        esp_err_t error = ryz_rgb_platform_init();
        if (error == ESP_OK) {
            memset(&s_snapshot, 0, sizeof(s_snapshot));
            error = ryz_rgb_platform_start(rgb_task);
            if (error != ESP_OK) ryz_rgb_platform_deinit();
        }
        if (error != ESP_OK) {
            atomic_store_explicit(&s_lifecycle, STOPPED, memory_order_release);
            leave();
            return error;
        }
    }
    esp_err_t error = ESP_ERR_INVALID_STATE;
    ryz_rgb_platform_lock();
    if (s_snapshot.phase != RYZ_RGB_QUEUED && s_snapshot.phase != RYZ_RGB_SENDING &&
        s_snapshot.phase != RYZ_RGB_CLEANING &&
        s_last_id != UINT32_MAX) {
        s_snapshot.request_id = ++s_last_id;
        s_snapshot.phase = RYZ_RGB_QUEUED;
        s_snapshot.requested = color;
        s_snapshot.error = ESP_OK;
        s_snapshot.queued_ms = ryz_rgb_platform_now_ms();
        s_snapshot.finished_ms = 0;
        /* Keep historical output knowledge until the worker admits the send. */
        *out_id = s_snapshot.request_id;
        if (lease) s_lease_request = s_snapshot.request_id;
        error = ESP_OK;
    }
    ryz_rgb_platform_unlock();
    if (creating) atomic_store_explicit(&s_lifecycle, READY, memory_order_release);
    if (error == ESP_OK) ryz_rgb_platform_wake();
    leave();
    return error;
}

esp_err_t ryz_rgb_submit(ryz_rgb_color_t color, uint32_t *out_id)
{ return submit(0, color, out_id); }

esp_err_t ryz_rgb_lease_submit(uint32_t lease, ryz_rgb_color_t color, uint32_t *out_id)
{
    if (!lease) { if (out_id) *out_id = 0; return ESP_ERR_INVALID_ARG; }
    return submit(lease, color, out_id);
}

esp_err_t ryz_rgb_get_snapshot(ryz_rgb_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (atomic_load_explicit(&s_lifecycle, memory_order_acquire) != READY) {
        return ESP_ERR_INVALID_STATE;
    }
    ryz_rgb_platform_lock();
    *out = s_snapshot;
    ryz_rgb_platform_unlock();
    return ESP_OK;
}

esp_err_t ryz_rgb_cleanup(uint32_t request_id)
{
    if (!request_id) return ESP_ERR_INVALID_ARG;
    if (atomic_load_explicit(&s_lifecycle, memory_order_acquire) != READY) return ESP_ERR_INVALID_STATE;
    if (!enter()) return ESP_ERR_INVALID_STATE;
    ryz_rgb_platform_lock();
    esp_err_t error = s_lease ? ESP_ERR_NOT_FINISHED : ESP_ERR_INVALID_STATE;
    bool wake = false;
    if (!s_lease && request_id == s_snapshot.request_id) {
        error = ESP_OK;
        if (s_snapshot.phase != RYZ_RGB_CLEANING && s_snapshot.phase != RYZ_RGB_CLEANED) {
            s_snapshot.phase = RYZ_RGB_CLEANING;
            s_snapshot.finished_ms = 0;
            wake = true;
        }
    }
    ryz_rgb_platform_unlock();
    if (wake) ryz_rgb_platform_wake();
    leave();
    return error;
}

esp_err_t ryz_rgb_lease_open(uint32_t *out_lease)
{
    if (!out_lease) return ESP_ERR_INVALID_ARG;
    *out_lease = 0;
    if (!enter()) return ESP_ERR_NOT_FINISHED;
    int lifecycle = atomic_load_explicit(&s_lifecycle, memory_order_acquire);
    if (lifecycle == STARTING) { leave(); return ESP_ERR_NOT_FINISHED; }
    if (lifecycle == READY) ryz_rgb_platform_lock();
    bool parked = s_snapshot.phase == RYZ_RGB_COMPLETED && s_snapshot.output_known &&
        s_snapshot.error == ESP_OK && s_snapshot.cleanup_error == ESP_OK;
    esp_err_t error = ESP_ERR_NOT_FINISHED;
    if (!s_lease && !active(s_snapshot.phase) && (!s_snapshot.resources_held || parked)) {
        if (s_last_lease == UINT32_MAX) error = ESP_ERR_NO_MEM;
        else {
            *out_lease = s_lease = ++s_last_lease;
            s_lease_request = s_lease_completed = 0;
            s_lease_closing = false;
            memset(&s_lease_color, 0, sizeof(s_lease_color));
            error = ESP_OK;
        }
    }
    if (lifecycle == READY) ryz_rgb_platform_unlock();
    leave();
    return error;
}

esp_err_t ryz_rgb_lease_snapshot(uint32_t lease, ryz_rgb_snapshot_t *out)
{
    if (!lease || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!enter()) return ESP_ERR_NOT_FINISHED;
    int lifecycle = atomic_load_explicit(&s_lifecycle, memory_order_acquire);
    if (lifecycle == STARTING) { leave(); return ESP_ERR_NOT_FINISHED; }
    if (lifecycle == READY) ryz_rgb_platform_lock();
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (lease == s_lease) {
        error = ESP_OK;
        if (s_lease_request) {
            if (s_snapshot.request_id != s_lease_request) error = ESP_ERR_INVALID_STATE;
            else {
                *out = s_snapshot;
                out->last_completed_id = s_lease_completed;
                out->last_completed_color = s_lease_color;
                out->output_known = s_snapshot.phase == RYZ_RGB_COMPLETED &&
                    s_snapshot.output_known && s_lease_completed == s_lease_request;
            }
        }
    }
    if (lifecycle == READY) ryz_rgb_platform_unlock();
    leave();
    if (error != ESP_OK) memset(out, 0, sizeof(*out));
    return error;
}

esp_err_t ryz_rgb_lease_close(uint32_t lease)
{
    if (!lease) return ESP_ERR_INVALID_ARG;
    if (!enter()) return ESP_ERR_NOT_FINISHED;
    int lifecycle = atomic_load_explicit(&s_lifecycle, memory_order_acquire);
    if (lifecycle == STARTING) { leave(); return ESP_ERR_NOT_FINISHED; }
    if (lifecycle == READY) ryz_rgb_platform_lock();
    esp_err_t error = ESP_ERR_INVALID_STATE;
    bool wake = false;
    if (lease <= s_last_closed_lease) error = ESP_OK;
    else if (lease == s_lease) {
        if (!s_lease_request) {
            retire_lease();
            error = ESP_OK;
        } else if (lifecycle == READY && s_snapshot.request_id == s_lease_request) {
            if (s_lease_closing && s_snapshot.phase == RYZ_RGB_FAILED) {
                error = s_snapshot.cleanup_error != ESP_OK ? s_snapshot.cleanup_error : ESP_FAIL;
            } else {
                if (!s_lease_closing) {
                    s_lease_closing = true;
                    s_snapshot.phase = RYZ_RGB_CLEANING;
                    s_snapshot.finished_ms = 0;
                    wake = true;
                }
                error = ESP_ERR_NOT_FINISHED;
            }
        }
    }
    if (lifecycle == READY) ryz_rgb_platform_unlock();
    if (wake) ryz_rgb_platform_wake();
    leave();
    return error;
}

esp_err_t ryz_rgb_lease_retry_close(uint32_t lease)
{
    if (!lease) return ESP_ERR_INVALID_ARG;
    if (!enter()) return ESP_ERR_NOT_FINISHED;
    if (atomic_load_explicit(&s_lifecycle, memory_order_acquire) != READY) {
        leave(); return ESP_ERR_INVALID_STATE;
    }
    ryz_rgb_platform_lock();
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (lease == s_lease && s_lease_closing && s_snapshot.phase == RYZ_RGB_FAILED) {
        s_lease_closing = false;
        error = ESP_OK;
    }
    ryz_rgb_platform_unlock();
    leave();
    return error;
}
