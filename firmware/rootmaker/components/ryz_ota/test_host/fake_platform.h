#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "ryz_ota.h"

void fake_ota_platform_reset(void);
void fake_ota_platform_set_configured(bool configured);
void fake_ota_platform_set_pending_confirmation(bool pending);
void fake_ota_platform_set_start_result(esp_err_t result);
void fake_ota_platform_set_cancel_result(esp_err_t result);
void fake_ota_platform_set_mark_valid_result(esp_err_t result);
void fake_ota_platform_set_running_info(const ryz_ota_running_info_t *info);
esp_err_t fake_ota_platform_emit_candidate(
    const char *project_name, const char *version, uint32_t total_bytes);
esp_err_t fake_ota_platform_emit_candidate_for_attempt(
    uint32_t attempt, const char *project_name, const char *version, uint32_t total_bytes);
esp_err_t fake_ota_platform_emit_progress(
    uint32_t downloaded_bytes, uint32_t total_bytes);
esp_err_t fake_ota_platform_emit_verifying(void);
esp_err_t fake_ota_platform_emit_completed(void);
esp_err_t fake_ota_platform_emit_completed_with_cleanup(
    bool cleanup_confirmed, esp_err_t cleanup_error);
/** Test-only platform boundary: invoked synchronously before fake reboot returns. */
void fake_ota_platform_set_reboot_hook(void (*hook)(void *), void *context);
esp_err_t fake_ota_platform_emit_cancelled(void);
esp_err_t fake_ota_platform_emit_failed(esp_err_t error);
esp_err_t fake_ota_platform_emit_terminal(uint32_t attempt_id,
                                         bool cleanup_confirmed,
                                         esp_err_t cleanup_error);
unsigned fake_ota_platform_init_count(void);
unsigned fake_ota_platform_start_count(void);
unsigned fake_ota_platform_cancel_count(void);
unsigned fake_ota_platform_mark_valid_count(void);
unsigned fake_ota_platform_reboot_count(void);
