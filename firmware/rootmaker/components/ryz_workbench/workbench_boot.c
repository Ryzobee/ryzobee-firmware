#include "workbench_boot.h"

#define BOOT_CONFIRM_RETRY_US INT64_C(5000000)

ryz_workbench_boot_result_t ryz_workbench_boot_step(
    ryz_workbench_boot_t *state, const ryz_workbench_boot_inputs_t *inputs,
    int64_t now_us, esp_err_t (*confirm)(void *), void *context)
{
    ryz_workbench_boot_result_t result = { .error = ESP_OK };
    if (!state || !inputs || !confirm || now_us < 0) {
        result.error = ESP_ERR_INVALID_ARG;
        return result;
    }
    if (state->confirmed) {
        result.runtime_ready = true;
        return result;
    }
    if (!inputs->services_ready || !inputs->font_ready || !inputs->touch_ready ||
        !inputs->ui_active || inputs->render_dirty || now_us < state->retry_at_us) {
        return result;
    }
    result.attempted = true;
    result.error = confirm(context);
    if (result.error == ESP_OK) {
        state->confirmed = true;
        result.runtime_ready = true;
    } else {
        state->retry_at_us = now_us > INT64_MAX - BOOT_CONFIRM_RETRY_US ?
            INT64_MAX : now_us + BOOT_CONFIRM_RETRY_US;
    }
    return result;
}
