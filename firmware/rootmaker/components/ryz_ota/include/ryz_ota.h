#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RYZ_OTA_PROJECT_NAME_MAX_LENGTH 31
#define RYZ_OTA_VERSION_MAX_LENGTH 31

/** Portable image-state values, deliberately not the ESP-IDF wire enum. */
typedef enum {
    RYZ_OTA_IMAGE_STATE_UNDEFINED = 0,
    RYZ_OTA_IMAGE_STATE_NEW,
    RYZ_OTA_IMAGE_STATE_PENDING_VERIFY,
    RYZ_OTA_IMAGE_STATE_VALID,
    RYZ_OTA_IMAGE_STATE_INVALID,
    RYZ_OTA_IMAGE_STATE_ABORTED,
} ryz_ota_image_state_t;

/** Boot-cached facts about the RUNNING image, never the next boot selection.
 * Missing/unsupported OTA records remain unknown with their original error.
 * A/B describes partition structure only, not image validity or update policy.
 * No installation timestamp/channel is inferred from the image build fields. */
typedef struct {
    char running_label[17];
    uint8_t running_subtype;
    bool partition_known;
    ryz_ota_image_state_t state;
    bool state_known;
    esp_err_t state_error;
    bool ab_slots_known;
    bool ab_slots;
} ryz_ota_running_info_t;

typedef enum {
    RYZ_OTA_DISABLED = 0,
    RYZ_OTA_WAITING_PREREQUISITES,
    RYZ_OTA_IDLE,
    RYZ_OTA_STARTING,
    RYZ_OTA_DOWNLOADING,
    RYZ_OTA_VERIFYING,
    RYZ_OTA_CANCELLING,
    RYZ_OTA_CANCELLED,
    RYZ_OTA_READY_TO_REBOOT,
    RYZ_OTA_FAILED,
} ryz_ota_state_t;

/**
 * Copy-only OTA state for UI and orchestration code.
 *
 * The configured update URL is intentionally not exposed. Download progress is
 * monotonic within one attempt; total_bytes may be zero when the server uses
 * chunked transfer encoding.
 */
typedef struct {
    ryz_ota_state_t state;
    uint32_t revision;
    esp_err_t last_error;
    bool configured;
    bool network_ready;
    bool clock_valid;
    bool cancel_requested;
    bool reboot_required;
    /** A matching restart owns admission until restart or its returned failure.
     * Network hold/prerequisite updates do not revoke this reservation. */
    bool reboot_pending;
    bool pending_confirmation;
    /** Network control intent; independent of actual connectivity. */
    bool network_held;
    /** True until the attempt has made its LAST SDK/resource access. A failed
     * candidate can already have state FAILED while this remains true. */
    bool worker_active;
    /** SDK-owned handles were consumed. May be true with nonzero cleanup_error:
     * IDF abort can consume its handle while reporting an inner OTA error. */
    bool cleanup_confirmed;
    esp_err_t cleanup_error;
    /** Boot-local attempt identity; never wraps, including failed task starts. */
    uint32_t attempt_id;
    uint32_t downloaded_bytes;
    uint32_t total_bytes;
    char running_version[RYZ_OTA_VERSION_MAX_LENGTH + 1];
    char candidate_version[RYZ_OTA_VERSION_MAX_LENGTH + 1];
    ryz_ota_running_info_t running_info;
} ryz_ota_snapshot_t;

/** Initialize the OTA module and inspect the running image. Idempotent. */
esp_err_t ryz_ota_init(void);

/**
 * Publish connectivity prerequisites owned by provisioning and time modules.
 * Starting an update requires both ONLINE connectivity and a valid clock.
 */
esp_err_t ryz_ota_set_prerequisites(bool network_ready, bool clock_valid);

/** Accept network hold intent, including before init. Always returns ESP_OK:
 * this acknowledges intent, NOT worker cancellation or resource release.
 * true blocks new starts immediately and requests cooperative cancellation in
 * STARTING/DOWNLOADING; VERIFYING only waits. Cancel failures remain visible in
 * last_error while worker_active stays true. false only releases the hold; it
 * never starts OTA or reconnects Wi-Fi. A single trusted network controller
 * owns this intent. Stop networking only after !worker_active &&
 * cleanup_confirmed; phase alone is not a teardown acknowledgement. */
esp_err_t ryz_ota_network_hold(bool hold);

/** Start the configured HTTPS update in a dedicated worker task. */
esp_err_t ryz_ota_start_update(void);

/** Request cooperative cancellation of the active update attempt. */
esp_err_t ryz_ota_cancel_update(void);

/**
 * Confirm a rollback-pending running image after the wider system is healthy.
 * This is a no-op when the running image is not pending confirmation.
 */
esp_err_t ryz_ota_confirm_running_image_healthy(void);

/** Trusted, synchronously borrowed wider-system restart admission. Both
 * callbacks are required and run WITHOUT the OTA snapshot mutex held.
 * try_begin must undo any partial acquisition before returning an error; OTA
 * does not call end in that case. On success the lease must remain held until
 * reboot; OTA calls end exactly once only if platform reboot returns. Neither
 * callback may retain this borrowed guard or assume a failed return rebooted. */
typedef struct {
    void *context;
    esp_err_t (*try_begin)(void *context);
    void (*end)(void *context);
} ryz_ota_reboot_guard_t;

/** Reserve and reboot ONLY the exact nonzero READY_TO_REBOOT attempt, after
 * worker completion and successful cleanup, with no boot confirmation pending.
 * Stale/duplicate or otherwise unready requests return INVALID_STATE without
 * invoking the guard; malformed arguments return INVALID_ARG. A rejected guard
 * releases the OTA reservation and returns its error unchanged. The platform
 * reboot should not return: if it does, release the guard and reservation,
 * return ESP_FAIL, and retain the ready candidate for an explicit retry. This
 * function never returns ESP_OK as evidence of a real reboot. No mutex is held
 * across guard callbacks or platform reboot. Caller owns the guard throughout. */
esp_err_t ryz_ota_reboot_to_updated_image(
    uint32_t expected_attempt, const ryz_ota_reboot_guard_t *guard);

/** Return one coherent copy of the latest OTA state. */
esp_err_t ryz_ota_get_snapshot(ryz_ota_snapshot_t *out_snapshot);

#ifdef __cplusplus
}
#endif
