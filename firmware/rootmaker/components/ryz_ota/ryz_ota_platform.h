#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "ryz_ota.h"

typedef struct {
    bool configured;
    bool pending_confirmation;
    char running_project[RYZ_OTA_PROJECT_NAME_MAX_LENGTH + 1];
    char running_version[RYZ_OTA_VERSION_MAX_LENGTH + 1];
    ryz_ota_running_info_t running_info;
} ryz_ota_platform_boot_info_t;

typedef enum {
    RYZ_OTA_PLATFORM_CANDIDATE = 0,
    RYZ_OTA_PLATFORM_PROGRESS,
    RYZ_OTA_PLATFORM_VERIFYING,
    RYZ_OTA_PLATFORM_COMPLETED,
    RYZ_OTA_PLATFORM_CANCELLED,
    RYZ_OTA_PLATFORM_FAILED,
} ryz_ota_platform_event_type_t;

typedef struct {
    ryz_ota_platform_event_type_t type;
    uint32_t attempt_id;
    esp_err_t error;
    /** Terminal events only. Published after the LAST SDK resource access;
     * worker may subsequently only return/delete its task. Never retry a
     * handle consumed by SDK finish/abort, even when that operation fails. */
    bool cleanup_confirmed;
    esp_err_t cleanup_error;
    uint32_t downloaded_bytes;
    uint32_t total_bytes;
    char candidate_project[RYZ_OTA_PROJECT_NAME_MAX_LENGTH + 1];
    char candidate_version[RYZ_OTA_VERSION_MAX_LENGTH + 1];
} ryz_ota_platform_event_t;

typedef esp_err_t (*ryz_ota_platform_event_sink_t)(
    const ryz_ota_platform_event_t *event);

void ryz_ota_platform_snapshot_lock(void);
void ryz_ota_platform_snapshot_unlock(void);
esp_err_t ryz_ota_platform_init(
    ryz_ota_platform_event_sink_t event_sink,
    ryz_ota_platform_boot_info_t *out_boot_info);
/** Error means no task was created and no resources were acquired. Events
 * carry this immutable id. Never call the sink synchronously inside start. */
esp_err_t ryz_ota_platform_start(uint32_t attempt_id);
esp_err_t ryz_ota_platform_cancel(void);
/** Return the observed RUNNING partition metadata, including on failure.
 * Success requires that the same boot identity is actually read back VALID;
 * a non-pending/unknown state is not a successful confirmation shortcut. */
esp_err_t ryz_ota_platform_mark_running_valid(ryz_ota_running_info_t *out_info);
void ryz_ota_platform_reboot(void);
