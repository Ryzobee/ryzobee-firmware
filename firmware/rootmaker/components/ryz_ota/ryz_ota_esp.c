#include "ryz_ota_platform.h"

#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_idf_version.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

/* Handle-consumption and cleanup confirmation below were audited against
 * this SDK. Re-audit before accepting a different SDK, not a blind rebuild. */
#if ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(5, 5, 4)
#error "Ryzobee OTA cleanup semantics require audited ESP-IDF 5.5.4"
#endif

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

static SemaphoreHandle_t s_snapshot_mutex;
static ryz_ota_platform_event_sink_t s_event_sink;
static atomic_bool s_cancel_requested;
static esp_partition_t s_running_partition;
static ryz_ota_running_info_t s_boot_running_info;
static bool s_running_partition_known;

static void copy_fixed_text(char *destination, size_t destination_size,
                            const char *source, size_t source_size)
{
    if (destination_size == 0) return;
    size_t length = 0;
    while (length < source_size && source[length] != '\0') ++length;
    if (length >= destination_size) length = destination_size - 1;
    if (length > 0) memcpy(destination, source, length);
    destination[length] = '\0';
}

static bool partition_shape(const esp_partition_t *partition, bool application)
{
    if (partition == NULL || !partition->address ||
        (unsigned)partition->subtype > UINT8_MAX ||
        partition->type != (application ? ESP_PARTITION_TYPE_APP : ESP_PARTITION_TYPE_DATA) ||
        partition->erase_size != 0x1000 ||
        partition->address % (application ? 0x10000U : 0x1000U) ||
        partition->size < (application ? 0x10000U : 0x2000U) ||
        partition->size % 0x1000U ||
        (uint64_t)partition->address + partition->size > UINT64_C(0x100000000) ||
        memchr(partition->label, '\0', sizeof(partition->label)) == NULL) return false;
    return true;
}

static bool overlaps(const esp_partition_t *left, const esp_partition_t *right)
{
    return (uint64_t)left->address < (uint64_t)right->address + right->size &&
           (uint64_t)right->address < (uint64_t)left->address + left->size;
}

static void read_ab_layout(ryz_ota_running_info_t *info)
{
    const esp_partition_t *a = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    const esp_partition_t *b = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    const esp_partition_t *data = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    /* Missing partitions are a known absence of this layout. Malformed returned
     * descriptors are unknown, not proof that an A/B layout is usable. */
    if ((a && (!partition_shape(a, true) || a->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0)) ||
        (b && (!partition_shape(b, true) || b->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_1)) ||
        (data && (!partition_shape(data, false) || data->subtype != ESP_PARTITION_SUBTYPE_DATA_OTA)))
        return;
    if (!a || !b || !data) { info->ab_slots_known = true; return; }
    if (a->flash_chip != b->flash_chip || a->flash_chip != data->flash_chip ||
        overlaps(a, b) || overlaps(a, data) || overlaps(b, data)) return;
    info->ab_slots_known = true;
    info->ab_slots = true;
}

static esp_err_t read_image_state(const esp_partition_t *partition,
                                  ryz_ota_running_info_t *info)
{
    info->state = RYZ_OTA_IMAGE_STATE_UNDEFINED;
    info->state_known = false;
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t error = esp_ota_get_state_partition(partition, &state);
    info->state_error = error;
    if (error != ESP_OK) return error;
    switch (state) {
    case ESP_OTA_IMG_NEW: info->state = RYZ_OTA_IMAGE_STATE_NEW; break;
    case ESP_OTA_IMG_PENDING_VERIFY: info->state = RYZ_OTA_IMAGE_STATE_PENDING_VERIFY; break;
    case ESP_OTA_IMG_VALID: info->state = RYZ_OTA_IMAGE_STATE_VALID; break;
    case ESP_OTA_IMG_INVALID: info->state = RYZ_OTA_IMAGE_STATE_INVALID; break;
    case ESP_OTA_IMG_ABORTED: info->state = RYZ_OTA_IMAGE_STATE_ABORTED; break;
    case ESP_OTA_IMG_UNDEFINED: break;
    default:
        info->state_error = ESP_ERR_INVALID_RESPONSE;
        return info->state_error;
    }
    info->state_known = true;
    return ESP_OK;
}

static bool same_running_partition(const esp_partition_t *partition)
{
    return partition && s_running_partition_known &&
        partition->flash_chip == s_running_partition.flash_chip &&
        partition->type == s_running_partition.type &&
        partition->subtype == s_running_partition.subtype &&
        partition->address == s_running_partition.address &&
        partition->size == s_running_partition.size &&
        partition->erase_size == s_running_partition.erase_size &&
        memcmp(partition->label, s_running_partition.label, sizeof(partition->label)) == 0;
}

static bool url_is_configured(void)
{
    return CONFIG_RYZ_OTA_URL[0] != '\0';
}

static bool url_is_https(void)
{
    static const char prefix[] = "https://";
    return strncmp(CONFIG_RYZ_OTA_URL, prefix, sizeof(prefix) - 1) == 0 &&
           CONFIG_RYZ_OTA_URL[sizeof(prefix) - 1] != '\0';
}

static uint32_t nonnegative_size(int value)
{
    return value > 0 ? (uint32_t)value : 0;
}

static esp_err_t publish_event(const ryz_ota_platform_event_t *event)
{
    if (s_event_sink == NULL) return ESP_ERR_INVALID_STATE;
    return s_event_sink(event);
}

static void publish_terminal(uint32_t attempt, ryz_ota_platform_event_type_t type,
                             esp_err_t error, bool confirmed, esp_err_t cleanup_error)
{
    (void)publish_event(&(ryz_ota_platform_event_t){
        .type = type,
        .attempt_id = attempt,
        .error = error,
        .cleanup_confirmed = confirmed,
        .cleanup_error = cleanup_error,
    });
}

static void abort_transfer(uint32_t attempt, esp_https_ota_handle_t handle,
                           ryz_ota_platform_event_type_t intended_type,
                           esp_err_t intended_error)
{
    /* A successful begin yields BEGIN/RESUME; subsequent SDK calls can only
     * advance to IN_PROGRESS/SUCCESS. In those states IDF 5.5.4 consumes the
     * HTTP/outer handle even if inner esp_ota_abort reports an error. */
    esp_err_t abort_error = esp_https_ota_abort(handle);
    if (abort_error != ESP_OK) {
        publish_terminal(attempt, RYZ_OTA_PLATFORM_FAILED, abort_error, true, abort_error);
        return;
    }
    publish_terminal(attempt, intended_type, intended_error, true, ESP_OK);
}

static void ota_worker(void *argument)
{
    const uint32_t attempt = (uint32_t)(uintptr_t)argument;
    if (atomic_load(&s_cancel_requested)) {
        publish_terminal(attempt, RYZ_OTA_PLATFORM_CANCELLED, ESP_OK, true, ESP_OK);
        vTaskDelete(NULL);
        return;
    }
    esp_http_client_config_t http_config = {
        .url = CONFIG_RYZ_OTA_URL,
        .timeout_ms = CONFIG_RYZ_OTA_HTTP_TIMEOUT_MS,
        .disable_auto_redirect = true,
        .keep_alive_enable = true,
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };
    esp_https_ota_handle_t handle = NULL;
    esp_err_t error = esp_https_ota_begin(&ota_config, &handle);
    if (error != ESP_OK) {
        /* This SDK frees partial allocations and clears output on failure.
         * Preserve unknown ownership if that audited contract is violated. */
        publish_terminal(attempt, RYZ_OTA_PLATFORM_FAILED, error, handle == NULL,
                         handle == NULL ? ESP_OK : ESP_ERR_INVALID_STATE);
        vTaskDelete(NULL);
        return;
    }

    if (handle == NULL) {
        publish_terminal(attempt, RYZ_OTA_PLATFORM_FAILED, ESP_ERR_INVALID_RESPONSE, true, ESP_OK);
        vTaskDelete(NULL);
        return;
    }
    if (atomic_load(&s_cancel_requested)) {
        abort_transfer(attempt, handle, RYZ_OTA_PLATFORM_CANCELLED, ESP_OK);
        vTaskDelete(NULL);
        return;
    }

    esp_app_desc_t candidate = {0};
    error = esp_https_ota_get_img_desc(handle, &candidate);
    if (error != ESP_OK) {
        abort_transfer(attempt, handle, RYZ_OTA_PLATFORM_FAILED, error);
        vTaskDelete(NULL);
        return;
    }

    if (atomic_load(&s_cancel_requested)) {
        abort_transfer(attempt, handle, RYZ_OTA_PLATFORM_CANCELLED, ESP_OK);
        vTaskDelete(NULL);
        return;
    }

    ryz_ota_platform_event_t candidate_event = {
        .type = RYZ_OTA_PLATFORM_CANDIDATE,
        .attempt_id = attempt,
        .total_bytes = nonnegative_size(esp_https_ota_get_image_size(handle)),
    };
    copy_fixed_text(candidate_event.candidate_project,
                    sizeof(candidate_event.candidate_project),
                    candidate.project_name, sizeof(candidate.project_name));
    copy_fixed_text(candidate_event.candidate_version,
                    sizeof(candidate_event.candidate_version),
                    candidate.version, sizeof(candidate.version));
    error = publish_event(&candidate_event);
    if (error != ESP_OK) {
        if (atomic_load(&s_cancel_requested)) {
            abort_transfer(attempt, handle, RYZ_OTA_PLATFORM_CANCELLED, ESP_OK);
        } else {
            abort_transfer(attempt, handle, RYZ_OTA_PLATFORM_FAILED, error);
        }
        vTaskDelete(NULL);
        return;
    }

    do {
        if (atomic_load(&s_cancel_requested)) {
            abort_transfer(attempt, handle, RYZ_OTA_PLATFORM_CANCELLED, ESP_OK);
            vTaskDelete(NULL);
            return;
        }
        error = esp_https_ota_perform(handle);
        ryz_ota_platform_event_t progress_event = {
            .type = RYZ_OTA_PLATFORM_PROGRESS,
            .attempt_id = attempt,
            .downloaded_bytes =
                nonnegative_size(esp_https_ota_get_image_len_read(handle)),
            .total_bytes =
                nonnegative_size(esp_https_ota_get_image_size(handle)),
        };
        (void)publish_event(&progress_event);
    } while (error == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

    if (atomic_load(&s_cancel_requested)) {
        abort_transfer(attempt, handle, RYZ_OTA_PLATFORM_CANCELLED, ESP_OK);
        vTaskDelete(NULL);
        return;
    }
    if (error != ESP_OK) {
        abort_transfer(attempt, handle, RYZ_OTA_PLATFORM_FAILED, error);
        vTaskDelete(NULL);
        return;
    }
    if (!esp_https_ota_is_complete_data_received(handle)) {
        abort_transfer(attempt, handle, RYZ_OTA_PLATFORM_FAILED,
                       ESP_ERR_INVALID_RESPONSE);
        vTaskDelete(NULL);
        return;
    }

    /* Last cancellable point: finish() may switch the boot partition. */
    if (atomic_load(&s_cancel_requested)) {
        abort_transfer(attempt, handle, RYZ_OTA_PLATFORM_CANCELLED, ESP_OK);
        vTaskDelete(NULL);
        return;
    }
    error = publish_event(&(ryz_ota_platform_event_t){
        .type = RYZ_OTA_PLATFORM_VERIFYING,
        .attempt_id = attempt,
    });
    if (error != ESP_OK) {
        if (atomic_load(&s_cancel_requested)) {
            abort_transfer(attempt, handle, RYZ_OTA_PLATFORM_CANCELLED, ESP_OK);
        } else {
            abort_transfer(attempt, handle, RYZ_OTA_PLATFORM_FAILED, error);
        }
        vTaskDelete(NULL);
        return;
    }

    /* finish consumes the valid handle even on validation/boot-selection
     * errors. Do not abort it afterwards: that would reuse freed memory. */
    error = esp_https_ota_finish(handle);
    if (error != ESP_OK) {
        publish_terminal(attempt, RYZ_OTA_PLATFORM_FAILED, error, true, ESP_OK);
        vTaskDelete(NULL);
        return;
    }
    publish_terminal(attempt, RYZ_OTA_PLATFORM_COMPLETED, ESP_OK, true, ESP_OK);
    vTaskDelete(NULL);
}

void ryz_ota_platform_snapshot_lock(void)
{
    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
}

void ryz_ota_platform_snapshot_unlock(void)
{
    xSemaphoreGive(s_snapshot_mutex);
}

esp_err_t ryz_ota_platform_init(
    ryz_ota_platform_event_sink_t event_sink,
    ryz_ota_platform_boot_info_t *out_boot_info)
{
    if (event_sink == NULL || out_boot_info == NULL) return ESP_ERR_INVALID_ARG;
    if (s_snapshot_mutex != NULL) return ESP_ERR_INVALID_STATE;
    if (url_is_configured() && !url_is_https()) return ESP_ERR_INVALID_ARG;
#if !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    if (url_is_configured()) return ESP_ERR_NOT_SUPPORTED;
#endif

    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    if (mutex == NULL) return ESP_ERR_NO_MEM;

    const esp_app_desc_t *running_description = esp_app_get_description();
    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    if (running_description == NULL || running_partition == NULL) {
        vSemaphoreDelete(mutex);
        return ESP_ERR_INVALID_STATE;
    }

    ryz_ota_running_info_t running_info = {0};
    if (!partition_shape(running_partition, true)) {
        vSemaphoreDelete(mutex);
        return ESP_ERR_INVALID_RESPONSE;
    }
    running_info.partition_known = true;
    running_info.running_subtype = (uint8_t)running_partition->subtype;
    copy_fixed_text(running_info.running_label, sizeof(running_info.running_label),
                    running_partition->label, sizeof(running_partition->label));
    read_ab_layout(&running_info);
    esp_err_t state_error = read_image_state(running_partition, &running_info);
    if (state_error != ESP_OK && state_error != ESP_ERR_NOT_FOUND &&
        state_error != ESP_ERR_NOT_SUPPORTED) {
        vSemaphoreDelete(mutex);
        return state_error;
    }

    *out_boot_info = (ryz_ota_platform_boot_info_t){
        .configured = url_is_configured(),
        .pending_confirmation =
            state_error == ESP_OK && running_info.state == RYZ_OTA_IMAGE_STATE_PENDING_VERIFY,
        .running_info = running_info,
    };
    copy_fixed_text(out_boot_info->running_project,
                    sizeof(out_boot_info->running_project),
                    running_description->project_name,
                    sizeof(running_description->project_name));
    copy_fixed_text(out_boot_info->running_version,
                    sizeof(out_boot_info->running_version),
                    running_description->version,
                    sizeof(running_description->version));

    s_running_partition = *running_partition;
    s_boot_running_info = running_info;
    s_running_partition_known = true;
    s_event_sink = event_sink;
    s_snapshot_mutex = mutex;
    atomic_store(&s_cancel_requested, false);
    return ESP_OK;
}

esp_err_t ryz_ota_platform_start(uint32_t attempt_id)
{
    if (!attempt_id) return ESP_ERR_INVALID_ARG;
    if (!url_is_configured()) return ESP_ERR_NOT_SUPPORTED;
    /* The core serializes start/cancel and terminal events with the snapshot
     * mutex. Keep only the cancellation signal here; duplicating an "active"
     * state in the adapter creates a race with terminal publication. */
    atomic_store(&s_cancel_requested, false);
    BaseType_t created = xTaskCreate(
        ota_worker, "ryz-https-ota", CONFIG_RYZ_OTA_TASK_STACK_SIZE, (void *)(uintptr_t)attempt_id,
        CONFIG_RYZ_OTA_TASK_PRIORITY, NULL);
    if (created == pdPASS) return ESP_OK;
    return ESP_ERR_NO_MEM;
}

esp_err_t ryz_ota_platform_cancel(void)
{
    atomic_store(&s_cancel_requested, true);
    return ESP_OK;
}

esp_err_t ryz_ota_platform_mark_running_valid(ryz_ota_running_info_t *out_info)
{
    if (out_info == NULL) return ESP_ERR_INVALID_ARG;
    *out_info = s_boot_running_info;
    out_info->state = RYZ_OTA_IMAGE_STATE_UNDEFINED;
    out_info->state_known = false;
    out_info->state_error = ESP_ERR_INVALID_STATE;
    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    if (!same_running_partition(running_partition)) return ESP_ERR_INVALID_STATE;
    esp_err_t error = read_image_state(running_partition, out_info);
    if (error != ESP_OK) return error;
    if (out_info->state == RYZ_OTA_IMAGE_STATE_VALID) return ESP_OK;
    if (out_info->state != RYZ_OTA_IMAGE_STATE_PENDING_VERIFY) return ESP_ERR_INVALID_STATE;
    error = esp_ota_mark_app_valid_cancel_rollback();
    if (error != ESP_OK) {
        /* A failed write may have an unknown persistent outcome. Do not keep
         * an old pending observation or publish VALID as if it were read back. */
        out_info->state = RYZ_OTA_IMAGE_STATE_UNDEFINED;
        out_info->state_known = false;
        out_info->state_error = error;
        return error;
    }
    if (!same_running_partition(esp_ota_get_running_partition())) {
        out_info->state = RYZ_OTA_IMAGE_STATE_UNDEFINED;
        out_info->state_known = false;
        out_info->state_error = ESP_ERR_INVALID_STATE;
        return ESP_ERR_INVALID_STATE;
    }
    error = read_image_state(running_partition, out_info);
    if (error != ESP_OK) return error;
    return out_info->state == RYZ_OTA_IMAGE_STATE_VALID ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void ryz_ota_platform_reboot(void)
{
    esp_restart();
}
