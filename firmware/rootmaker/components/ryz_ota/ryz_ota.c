#include "ryz_ota.h"

#include <stddef.h>
#include <stdatomic.h>

#include "ryz_ota_platform.h"

static atomic_bool s_initialized;
static atomic_flag s_initializing = ATOMIC_FLAG_INIT;
static atomic_bool s_network_held;
static ryz_ota_snapshot_t s_snapshot;
static char s_running_project[RYZ_OTA_PROJECT_NAME_MAX_LENGTH + 1];

static esp_err_t cancel_locked(void);

static bool state_is_active(ryz_ota_state_t state)
{
    return state == RYZ_OTA_STARTING ||
           state == RYZ_OTA_DOWNLOADING ||
           state == RYZ_OTA_VERIFYING ||
           state == RYZ_OTA_CANCELLING;
}

static void copy_text(char *destination, size_t destination_size,
                      const char *source)
{
    if (destination_size == 0) return;
    size_t index = 0;
    if (source != NULL) {
        while (index + 1 < destination_size && source[index] != '\0') {
            destination[index] = source[index];
            ++index;
        }
    }
    destination[index] = '\0';
}

static bool text_equals(const char *left, size_t left_capacity,
                        const char *right, size_t right_capacity)
{
    size_t index = 0;
    while (index < left_capacity && index < right_capacity) {
        if (left[index] != right[index]) return false;
        if (left[index] == '\0') return true;
        ++index;
    }
    return false;
}

static esp_err_t receive_platform_event(const ryz_ota_platform_event_t *event)
{
    if (event == NULL) return ESP_ERR_INVALID_ARG;
    if (!atomic_load_explicit(&s_initialized, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }

    ryz_ota_platform_snapshot_lock();
    if (!s_snapshot.worker_active || !event->attempt_id ||
        event->attempt_id != s_snapshot.attempt_id) {
        ryz_ota_platform_snapshot_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (event->type == RYZ_OTA_PLATFORM_CANDIDATE) {
        if (s_snapshot.state != RYZ_OTA_STARTING) {
            ryz_ota_platform_snapshot_unlock();
            return ESP_ERR_INVALID_STATE;
        }
        if (!text_equals(event->candidate_project,
                         sizeof(event->candidate_project),
                         s_running_project, sizeof(s_running_project))) {
            s_snapshot.state = RYZ_OTA_FAILED;
            s_snapshot.last_error = ESP_ERR_INVALID_RESPONSE;
            ++s_snapshot.revision;
            ryz_ota_platform_snapshot_unlock();
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (event->candidate_version[0] == '\0' ||
            text_equals(event->candidate_version,
                        sizeof(event->candidate_version),
                        s_snapshot.running_version,
                        sizeof(s_snapshot.running_version))) {
            s_snapshot.state = RYZ_OTA_FAILED;
            s_snapshot.last_error = ESP_ERR_INVALID_VERSION;
            ++s_snapshot.revision;
            ryz_ota_platform_snapshot_unlock();
            return ESP_ERR_INVALID_VERSION;
        }
        copy_text(s_snapshot.candidate_version,
                  sizeof(s_snapshot.candidate_version),
                  event->candidate_version);
        s_snapshot.total_bytes = event->total_bytes;
        s_snapshot.state = RYZ_OTA_DOWNLOADING;
        s_snapshot.last_error = ESP_OK;
        ++s_snapshot.revision;
        ryz_ota_platform_snapshot_unlock();
        return ESP_OK;
    }

    if (event->type == RYZ_OTA_PLATFORM_PROGRESS) {
        if (s_snapshot.state != RYZ_OTA_DOWNLOADING) {
            ryz_ota_platform_snapshot_unlock();
            return ESP_ERR_INVALID_STATE;
        }
        bool changed = false;
        if (event->downloaded_bytes > s_snapshot.downloaded_bytes) {
            s_snapshot.downloaded_bytes = event->downloaded_bytes;
            changed = true;
        }
        if (event->total_bytes > s_snapshot.total_bytes) {
            s_snapshot.total_bytes = event->total_bytes;
            changed = true;
        }
        if (changed) ++s_snapshot.revision;
        ryz_ota_platform_snapshot_unlock();
        return ESP_OK;
    }

    if (event->type == RYZ_OTA_PLATFORM_VERIFYING) {
        if (s_snapshot.state != RYZ_OTA_DOWNLOADING) {
            ryz_ota_platform_snapshot_unlock();
            return ESP_ERR_INVALID_STATE;
        }
        /* Hold intent is latched before taking this mutex. Do not enter the
         * irreversible finish boundary ahead of an already requested hold. */
        if (atomic_load(&s_network_held)) {
            (void)cancel_locked();
            ryz_ota_platform_snapshot_unlock();
            return ESP_ERR_INVALID_STATE;
        }
        s_snapshot.state = RYZ_OTA_VERIFYING;
        ++s_snapshot.revision;
        ryz_ota_platform_snapshot_unlock();
        return ESP_OK;
    }

    if (event->type == RYZ_OTA_PLATFORM_COMPLETED) {
        if (s_snapshot.state != RYZ_OTA_VERIFYING || !event->cleanup_confirmed) {
            ryz_ota_platform_snapshot_unlock();
            return ESP_ERR_INVALID_STATE;
        }
        s_snapshot.state = RYZ_OTA_READY_TO_REBOOT;
        s_snapshot.last_error = ESP_OK;
        s_snapshot.reboot_required = true;
        s_snapshot.worker_active = false;
        s_snapshot.cleanup_confirmed = true;
        s_snapshot.cleanup_error = event->cleanup_error;
        ++s_snapshot.revision;
        ryz_ota_platform_snapshot_unlock();
        return ESP_OK;
    }

    if (event->type == RYZ_OTA_PLATFORM_CANCELLED) {
        if (s_snapshot.state != RYZ_OTA_CANCELLING) {
            ryz_ota_platform_snapshot_unlock();
            return ESP_ERR_INVALID_STATE;
        }
        s_snapshot.state = RYZ_OTA_CANCELLED;
        s_snapshot.last_error = ESP_OK;
        s_snapshot.cancel_requested = false;
        s_snapshot.reboot_required = false;
        s_snapshot.worker_active = false;
        s_snapshot.cleanup_confirmed = event->cleanup_confirmed;
        s_snapshot.cleanup_error = event->cleanup_error;
        if (!event->cleanup_confirmed) {
            s_snapshot.state = RYZ_OTA_FAILED;
            s_snapshot.last_error = event->cleanup_error != ESP_OK
                ? event->cleanup_error : ESP_ERR_INVALID_STATE;
        }
        ++s_snapshot.revision;
        ryz_ota_platform_snapshot_unlock();
        return ESP_OK;
    }

    if (event->type == RYZ_OTA_PLATFORM_FAILED) {
        /* Rejection/cancel-dispatch failure may have exposed FAILED already;
         * only this final, attempt-matched event proves worker quiescence. */
        esp_err_t earlier = s_snapshot.state == RYZ_OTA_FAILED
            ? s_snapshot.last_error : ESP_OK;
        s_snapshot.state = RYZ_OTA_FAILED;
        s_snapshot.last_error = earlier != ESP_OK ? earlier
            : event->error == ESP_OK ? ESP_FAIL : event->error;
        s_snapshot.cancel_requested = false;
        s_snapshot.reboot_required = false;
        s_snapshot.worker_active = false;
        s_snapshot.cleanup_confirmed = event->cleanup_confirmed;
        s_snapshot.cleanup_error = event->cleanup_error;
        ++s_snapshot.revision;
        ryz_ota_platform_snapshot_unlock();
        return ESP_OK;
    }

    ryz_ota_platform_snapshot_unlock();
    return ESP_ERR_INVALID_STATE;
}

esp_err_t ryz_ota_init(void)
{
    if (atomic_load_explicit(&s_initialized, memory_order_acquire)) {
        return ESP_OK;
    }
    if (atomic_flag_test_and_set_explicit(&s_initializing, memory_order_acquire))
        return ESP_ERR_INVALID_STATE;

    ryz_ota_platform_boot_info_t boot_info = {0};
    esp_err_t error = ryz_ota_platform_init(receive_platform_event, &boot_info);
    if (error != ESP_OK) {
        atomic_flag_clear_explicit(&s_initializing, memory_order_release);
        return error;
    }

    ryz_ota_platform_snapshot_lock();
    s_snapshot = (ryz_ota_snapshot_t){
        .state = boot_info.configured
            ? RYZ_OTA_WAITING_PREREQUISITES
            : RYZ_OTA_DISABLED,
        .revision = 1,
        .last_error = ESP_OK,
        .configured = boot_info.configured,
        .pending_confirmation = boot_info.pending_confirmation,
        .cleanup_confirmed = true,
        .running_info = boot_info.running_info,
    };
    copy_text(s_running_project, sizeof(s_running_project),
              boot_info.running_project);
    copy_text(s_snapshot.running_version,
              sizeof(s_snapshot.running_version),
              boot_info.running_version);
    atomic_store_explicit(&s_initialized, true, memory_order_release);
    /* Publish initialized before the final intent load: a racing hold either
     * is included here or observes initialized and updates under this lock. */
    s_snapshot.network_held = atomic_load(&s_network_held);
    ryz_ota_platform_snapshot_unlock();
    return ESP_OK;
}

esp_err_t ryz_ota_set_prerequisites(bool network_ready, bool clock_valid)
{
    if (!atomic_load_explicit(&s_initialized, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }

    ryz_ota_platform_snapshot_lock();
    if (s_snapshot.network_ready == network_ready &&
        s_snapshot.clock_valid == clock_valid) {
        ryz_ota_platform_snapshot_unlock();
        return ESP_OK;
    }

    s_snapshot.network_ready = network_ready;
    s_snapshot.clock_valid = clock_valid;
    if (!s_snapshot.configured) {
        s_snapshot.state = RYZ_OTA_DISABLED;
    } else if (s_snapshot.state == RYZ_OTA_WAITING_PREREQUISITES ||
               s_snapshot.state == RYZ_OTA_IDLE) {
        s_snapshot.state = network_ready && clock_valid && !atomic_load(&s_network_held)
            ? RYZ_OTA_IDLE
            : RYZ_OTA_WAITING_PREREQUISITES;
    }
    ++s_snapshot.revision;
    ryz_ota_platform_snapshot_unlock();
    return ESP_OK;
}

esp_err_t ryz_ota_start_update(void)
{
    if (!atomic_load_explicit(&s_initialized, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }

    ryz_ota_platform_snapshot_lock();
    if (!s_snapshot.configured) {
        ryz_ota_platform_snapshot_unlock();
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (atomic_load(&s_network_held) || !s_snapshot.network_ready || !s_snapshot.clock_valid ||
        s_snapshot.pending_confirmation || s_snapshot.worker_active || s_snapshot.reboot_pending ||
        !s_snapshot.cleanup_confirmed || s_snapshot.attempt_id == UINT32_MAX ||
        state_is_active(s_snapshot.state) ||
        s_snapshot.state == RYZ_OTA_READY_TO_REBOOT) {
        ryz_ota_platform_snapshot_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    s_snapshot.state = RYZ_OTA_STARTING;
    s_snapshot.last_error = ESP_OK;
    s_snapshot.cancel_requested = false;
    s_snapshot.reboot_required = false;
    s_snapshot.downloaded_bytes = 0;
    s_snapshot.total_bytes = 0;
    s_snapshot.candidate_version[0] = '\0';
    ++s_snapshot.attempt_id;
    s_snapshot.worker_active = true;
    s_snapshot.cleanup_confirmed = false;
    s_snapshot.cleanup_error = ESP_OK;
    ++s_snapshot.revision;
    esp_err_t error = ryz_ota_platform_start(s_snapshot.attempt_id);
    if (error != ESP_OK) {
        s_snapshot.state = RYZ_OTA_FAILED;
        s_snapshot.last_error = error;
        s_snapshot.worker_active = false;
        s_snapshot.cleanup_confirmed = true;
        ++s_snapshot.revision;
    }
    ryz_ota_platform_snapshot_unlock();
    return error;
}

static esp_err_t cancel_locked(void)
{
    if (s_snapshot.state == RYZ_OTA_CANCELLING) {
        return ESP_OK;
    }
    if (!s_snapshot.worker_active ||
        (s_snapshot.state != RYZ_OTA_STARTING &&
         s_snapshot.state != RYZ_OTA_DOWNLOADING)) {
        return ESP_ERR_INVALID_STATE;
    }
    s_snapshot.state = RYZ_OTA_CANCELLING;
    s_snapshot.cancel_requested = true;
    ++s_snapshot.revision;
    esp_err_t error = ryz_ota_platform_cancel();
    if (error != ESP_OK) {
        s_snapshot.state = RYZ_OTA_FAILED;
        s_snapshot.last_error = error;
        s_snapshot.cancel_requested = false;
        ++s_snapshot.revision;
    }
    return error;
}

esp_err_t ryz_ota_cancel_update(void)
{
    if (!atomic_load_explicit(&s_initialized, memory_order_acquire))
        return ESP_ERR_INVALID_STATE;
    ryz_ota_platform_snapshot_lock();
    esp_err_t error = cancel_locked();
    ryz_ota_platform_snapshot_unlock();
    return error;
}

esp_err_t ryz_ota_network_hold(bool hold)
{
    /* This latch also covers the interval before a snapshot mutex exists. */
    atomic_store(&s_network_held, hold);
    if (!atomic_load_explicit(&s_initialized, memory_order_acquire)) return ESP_OK;
    ryz_ota_platform_snapshot_lock();
    bool current = atomic_load(&s_network_held);
    if (s_snapshot.network_held != current) {
        s_snapshot.network_held = current;
        if (s_snapshot.state == RYZ_OTA_IDLE ||
            s_snapshot.state == RYZ_OTA_WAITING_PREREQUISITES) {
            s_snapshot.state = !current && s_snapshot.network_ready && s_snapshot.clock_valid
                ? RYZ_OTA_IDLE : RYZ_OTA_WAITING_PREREQUISITES;
        }
        ++s_snapshot.revision;
    }
    if (current && s_snapshot.worker_active &&
        (s_snapshot.state == RYZ_OTA_STARTING || s_snapshot.state == RYZ_OTA_DOWNLOADING))
        (void)cancel_locked();
    ryz_ota_platform_snapshot_unlock();
    return ESP_OK;
}

esp_err_t ryz_ota_confirm_running_image_healthy(void)
{
    if (!atomic_load_explicit(&s_initialized, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }

    ryz_ota_platform_snapshot_lock();
    bool confirmation_required = s_snapshot.pending_confirmation;
    ryz_ota_platform_snapshot_unlock();
    if (!confirmation_required) return ESP_OK;

    ryz_ota_running_info_t info = {0};
    esp_err_t error = ryz_ota_platform_mark_running_valid(&info);
    if (error == ESP_OK && (!info.partition_known || !info.state_known ||
        info.state_error != ESP_OK || info.state != RYZ_OTA_IMAGE_STATE_VALID))
        error = ESP_ERR_INVALID_STATE;
    ryz_ota_platform_snapshot_lock();
    /* Confirmation may refresh state, but must never switch our boot identity. */
    if (error == ESP_OK && s_snapshot.running_info.partition_known &&
        (!text_equals(s_snapshot.running_info.running_label,
                      sizeof(s_snapshot.running_info.running_label),
                      info.running_label, sizeof(info.running_label)) ||
         s_snapshot.running_info.running_subtype != info.running_subtype))
        error = ESP_ERR_INVALID_STATE;
    if (error == ESP_OK) {
        s_snapshot.running_info = info;
    } else {
        s_snapshot.running_info.state = RYZ_OTA_IMAGE_STATE_UNDEFINED;
        s_snapshot.running_info.state_known = false;
        s_snapshot.running_info.state_error = info.state_error != ESP_OK
            ? info.state_error : error;
    }
    s_snapshot.last_error = error;
    if (error == ESP_OK) s_snapshot.pending_confirmation = false;
    ++s_snapshot.revision;
    ryz_ota_platform_snapshot_unlock();
    return error;
}

esp_err_t ryz_ota_reboot_to_updated_image(
    uint32_t expected_attempt, const ryz_ota_reboot_guard_t *guard)
{
    if (!expected_attempt || guard == NULL ||
        guard->try_begin == NULL || guard->end == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!atomic_load_explicit(&s_initialized, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }

    const ryz_ota_reboot_guard_t borrowed = *guard;
    ryz_ota_platform_snapshot_lock();
    if (s_snapshot.attempt_id != expected_attempt ||
        s_snapshot.state != RYZ_OTA_READY_TO_REBOOT ||
        !s_snapshot.reboot_required || s_snapshot.pending_confirmation ||
        s_snapshot.worker_active || !s_snapshot.cleanup_confirmed ||
        s_snapshot.cleanup_error != ESP_OK || s_snapshot.reboot_pending) {
        ryz_ota_platform_snapshot_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_snapshot.reboot_pending = true;
    ++s_snapshot.revision;
    ryz_ota_platform_snapshot_unlock();

    /* Keep admission reserved across callbacks, including end. Reentrant or
     * concurrent requests may observe it but cannot restart this candidate or
     * begin another update. Network holds do not revoke a restart reservation. */
    esp_err_t error = borrowed.try_begin(borrowed.context);
    if (error == ESP_OK) {
        ryz_ota_platform_reboot();
        /* Real restart is non-returning. Do not publish success if it returns. */
        borrowed.end(borrowed.context);
        error = ESP_FAIL;
    }

    ryz_ota_platform_snapshot_lock();
    s_snapshot.reboot_pending = false;
    ++s_snapshot.revision;
    ryz_ota_platform_snapshot_unlock();
    return error;
}

esp_err_t ryz_ota_get_snapshot(ryz_ota_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL) return ESP_ERR_INVALID_ARG;
    if (!atomic_load_explicit(&s_initialized, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    ryz_ota_platform_snapshot_lock();
    *out_snapshot = s_snapshot;
    ryz_ota_platform_snapshot_unlock();
    return ESP_OK;
}
