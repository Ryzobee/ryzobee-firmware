#include "fake_platform.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "ryz_ota_platform.h"

static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static ryz_ota_platform_event_sink_t s_sink;
static bool s_configured;
static bool s_pending_confirmation;
static esp_err_t s_start_result;
static esp_err_t s_cancel_result;
static esp_err_t s_mark_valid_result;
static unsigned s_init_count;
static unsigned s_start_count;
static unsigned s_cancel_count;
static unsigned s_mark_valid_count;
static unsigned s_reboot_count;
static uint32_t s_attempt;
static void (*s_reboot_hook)(void *);
static void *s_reboot_context;
static ryz_ota_running_info_t s_running_info;

static esp_err_t emit(const ryz_ota_platform_event_t *event)
{
    ryz_ota_platform_event_t copy = *event;
    copy.attempt_id = s_attempt;
    return s_sink == NULL ? ESP_ERR_INVALID_STATE : s_sink(&copy);
}

void fake_ota_platform_reset(void)
{
    s_sink = NULL;
    s_configured = true;
    s_pending_confirmation = false;
    s_start_result = ESP_OK;
    s_cancel_result = ESP_OK;
    s_mark_valid_result = ESP_OK;
    s_init_count = 0;
    s_start_count = 0;
    s_cancel_count = 0;
    s_mark_valid_count = 0;
    s_reboot_count = 0;
    s_attempt = 0;
    s_reboot_hook = NULL;
    s_reboot_context = NULL;
    s_running_info = (ryz_ota_running_info_t){0};
}

void fake_ota_platform_set_configured(bool configured)
{
    s_configured = configured;
}

void fake_ota_platform_set_pending_confirmation(bool pending)
{
    s_pending_confirmation = pending;
    s_running_info = (ryz_ota_running_info_t){
        .running_label = "ota_0", .running_subtype = 0x10,
        .partition_known = true, .state_known = true,
        .state = pending ? RYZ_OTA_IMAGE_STATE_PENDING_VERIFY : RYZ_OTA_IMAGE_STATE_VALID,
    };
}

void fake_ota_platform_set_start_result(esp_err_t result)
{
    s_start_result = result;
}

void fake_ota_platform_set_cancel_result(esp_err_t result)
{
    s_cancel_result = result;
}

void fake_ota_platform_set_mark_valid_result(esp_err_t result)
{
    s_mark_valid_result = result;
}

void fake_ota_platform_set_running_info(const ryz_ota_running_info_t *info)
{
    assert(info != NULL);
    s_running_info = *info;
}

esp_err_t fake_ota_platform_emit_candidate(
    const char *project_name, const char *version, uint32_t total_bytes)
{
    return fake_ota_platform_emit_candidate_for_attempt(s_attempt, project_name, version, total_bytes);
}

esp_err_t fake_ota_platform_emit_candidate_for_attempt(
    uint32_t attempt, const char *project_name, const char *version, uint32_t total_bytes)
{
    ryz_ota_platform_event_t event = {
        .type = RYZ_OTA_PLATFORM_CANDIDATE,
        .attempt_id = attempt,
        .total_bytes = total_bytes,
    };
    snprintf(event.candidate_project, sizeof(event.candidate_project), "%s",
             project_name == NULL ? "" : project_name);
    snprintf(event.candidate_version, sizeof(event.candidate_version), "%s",
             version == NULL ? "" : version);
    return s_sink == NULL ? ESP_ERR_INVALID_STATE : s_sink(&event);
}

esp_err_t fake_ota_platform_emit_progress(
    uint32_t downloaded_bytes, uint32_t total_bytes)
{
    return emit(&(ryz_ota_platform_event_t){
        .type = RYZ_OTA_PLATFORM_PROGRESS,
        .downloaded_bytes = downloaded_bytes,
        .total_bytes = total_bytes,
    });
}

esp_err_t fake_ota_platform_emit_verifying(void)
{
    return emit(&(ryz_ota_platform_event_t){
        .type = RYZ_OTA_PLATFORM_VERIFYING,
    });
}

esp_err_t fake_ota_platform_emit_completed(void)
{
    return fake_ota_platform_emit_completed_with_cleanup(true, ESP_OK);
}

esp_err_t fake_ota_platform_emit_completed_with_cleanup(
    bool cleanup_confirmed, esp_err_t cleanup_error)
{
    return emit(&(ryz_ota_platform_event_t){
        .type = RYZ_OTA_PLATFORM_COMPLETED,
        .cleanup_confirmed = cleanup_confirmed,
        .cleanup_error = cleanup_error,
    });
}

void fake_ota_platform_set_reboot_hook(void (*hook)(void *), void *context)
{
    s_reboot_hook = hook;
    s_reboot_context = context;
}

esp_err_t fake_ota_platform_emit_cancelled(void)
{
    return emit(&(ryz_ota_platform_event_t){
        .type = RYZ_OTA_PLATFORM_CANCELLED,
        .cleanup_confirmed = true,
    });
}

esp_err_t fake_ota_platform_emit_failed(esp_err_t error)
{
    return emit(&(ryz_ota_platform_event_t){
        .type = RYZ_OTA_PLATFORM_FAILED,
        .error = error,
        .cleanup_confirmed = true,
    });
}

esp_err_t fake_ota_platform_emit_terminal(uint32_t attempt_id,
                                         bool cleanup_confirmed,
                                         esp_err_t cleanup_error)
{
    const ryz_ota_platform_event_t event = {
        .type = RYZ_OTA_PLATFORM_FAILED,
        .attempt_id = attempt_id,
        .error = ESP_FAIL,
        .cleanup_confirmed = cleanup_confirmed,
        .cleanup_error = cleanup_error,
    };
    return s_sink == NULL ? ESP_ERR_INVALID_STATE : s_sink(&event);
}

unsigned fake_ota_platform_init_count(void) { return s_init_count; }
unsigned fake_ota_platform_start_count(void) { return s_start_count; }
unsigned fake_ota_platform_cancel_count(void) { return s_cancel_count; }
unsigned fake_ota_platform_mark_valid_count(void) { return s_mark_valid_count; }
unsigned fake_ota_platform_reboot_count(void) { return s_reboot_count; }

void ryz_ota_platform_snapshot_lock(void)
{
    pthread_mutex_lock(&s_mutex);
}

void ryz_ota_platform_snapshot_unlock(void)
{
    pthread_mutex_unlock(&s_mutex);
}

esp_err_t ryz_ota_platform_init(
    ryz_ota_platform_event_sink_t event_sink,
    ryz_ota_platform_boot_info_t *out_boot_info)
{
    ++s_init_count;
    if (event_sink == NULL || out_boot_info == NULL) return ESP_ERR_INVALID_ARG;
    if (s_sink != NULL) return ESP_ERR_INVALID_STATE;
    s_sink = event_sink;
    *out_boot_info = (ryz_ota_platform_boot_info_t){
        .configured = s_configured,
        .pending_confirmation = s_pending_confirmation,
        .running_info = s_running_info,
    };
    snprintf(out_boot_info->running_project,
             sizeof(out_boot_info->running_project), "%s", "ryzobee_rootmaker");
    snprintf(out_boot_info->running_version,
             sizeof(out_boot_info->running_version), "%s", "0.7.0");
    return ESP_OK;
}

esp_err_t ryz_ota_platform_start(uint32_t attempt_id)
{
    ++s_start_count;
    if (s_start_result == ESP_OK) s_attempt = attempt_id;
    return s_start_result;
}

esp_err_t ryz_ota_platform_cancel(void)
{
    ++s_cancel_count;
    return s_cancel_result;
}

esp_err_t ryz_ota_platform_mark_running_valid(ryz_ota_running_info_t *out_info)
{
    ++s_mark_valid_count;
    assert(out_info != NULL);
    *out_info = s_running_info;
    if (s_mark_valid_result == ESP_OK) {
        out_info->state = RYZ_OTA_IMAGE_STATE_VALID;
        out_info->state_known = true;
        out_info->state_error = ESP_OK;
    } else {
        out_info->state = RYZ_OTA_IMAGE_STATE_UNDEFINED;
        out_info->state_known = false;
        out_info->state_error = s_mark_valid_result;
    }
    return s_mark_valid_result;
}

void ryz_ota_platform_reboot(void)
{
    ++s_reboot_count;
    if (s_reboot_hook != NULL) s_reboot_hook(s_reboot_context);
}
