#include "workbench_restart.h"

#include "ryz_ota.h"
#include "ryz_script_store.h"

typedef struct {
    ryz_workbench_write_guard_t writer;
    bool held;
} restart_context_t;

static void end(void *opaque)
{
    restart_context_t *context = opaque;
    if (!context->held) return;
    context->held = false;
    context->writer.end_write(context->writer.context);
}

static esp_err_t begin(void *opaque)
{
    restart_context_t *context = opaque;
    if (context->held) return ESP_ERR_INVALID_STATE;
    if (!context->writer.try_begin_write(context->writer.context))
        return ESP_ERR_TIMEOUT;
    context->held = true;

    /* The reservation, not the Job mutex, spans this copy-only Store call.
     * All runtime writers must honor it; Store contention is not quiescence. */
    ryz_script_store_status_t status = {0};
    esp_err_t error = ryz_script_store_status(&status);
    if (error == ESP_OK && (!status.ready || status.recovery_required ||
                           status.legacy_backup_present || status.recovery_error != ESP_OK))
        error = status.recovery_error != ESP_OK ? status.recovery_error : ESP_ERR_INVALID_STATE;
    if (error != ESP_OK) end(context);
    return error;
}

esp_err_t ryz_workbench_restart_updated(uint32_t expected_attempt,
                                      const ryz_workbench_write_guard_t *guard)
{
    if (!guard || !guard->try_begin_write || !guard->end_write)
        return ESP_ERR_INVALID_ARG;
    restart_context_t context = {.writer = *guard};
    const ryz_ota_reboot_guard_t ota_guard = {
        .context = &context, .try_begin = begin, .end = end,
    };
    return ryz_ota_reboot_to_updated_image(expected_attempt, &ota_guard);
}
