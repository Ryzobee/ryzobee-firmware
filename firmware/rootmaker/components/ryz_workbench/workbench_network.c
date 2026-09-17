#include "workbench_network.h"

#include <string.h>

static bool setup_failed(const ryz_system_services_snapshot_t *s)
{
    const ryz_system_network_operation_t *op = &s->network_operation;
    return op->completed && op->requested == op->completed &&
        op->completed_action == RYZ_SYSTEM_NETWORK_OPEN_AP && op->result != ESP_OK &&
        /* ONLINE telemetry advances revision too; compare the completion's
         * native state instead. A later real STA failure supersedes an old
         * OTA rejection, while native AP-start FAILED still retries setup.
         * Unknown completion sampling never fabricates setup success. */
        (!op->completed_network_valid || op->completed_network_state == s->network.state);
}

void ryz_workbench_network_controls(const ryz_system_services_snapshot_t *s,
                                    ryz_v5_network_model_t *v)
{
    if (!s || !v) return;
    const ryz_system_network_operation_t *op = &s->network_operation;
    const bool pending = op->requested != op->completed;
    v->enabled = s->network_valid && s->network.enabled;
    v->busy = !s->provisioning_ready || !s->network_valid || pending;
    v->setup_pending = pending && op->action == RYZ_SYSTEM_NETWORK_OPEN_AP;
    v->setup_failed = setup_failed(s);
    v->can_setup = !v->busy && v->enabled;
    v->can_retry = !v->busy &&
        (s->network.state == RYZ_PROVISIONING_FAILED || v->setup_failed);
    v->can_cancel = false; /* Cancel has a separate, still-unfrozen contract. */
    v->initialization_error = s->provisioning_ready ? ESP_OK :
        s->errors[RYZ_SYSTEM_PROVISIONING_INIT];
    v->error = s->network.last_error;
    v->failure_detail = s->network.detail;
    v->operation_error = ESP_OK;
    memset(v->operation_label, 0, sizeof(v->operation_label));
    if (op->completed && !v->busy && op->result != ESP_OK &&
        !(op->completed_action == RYZ_SYSTEM_NETWORK_OPEN_AP && !v->setup_failed)) {
        v->operation_error = op->result;
        const char *label = v->setup_failed ? "SETUP FAILED" :
            op->completed_action == RYZ_SYSTEM_NETWORK_OFF ? "OFF FAILED" : "START FAILED";
        memcpy(v->operation_label, label, strlen(label) + 1);
    }
}

esp_err_t ryz_workbench_network_request(ryz_system_ui_action_t intent,
                                       uint32_t *operation_id)
{
    if (!operation_id) return ESP_ERR_INVALID_ARG;
    *operation_id = 0;
    ryz_system_network_action_t action;
    switch (intent) {
    case RYZ_SYSTEM_UI_ACTION_REPROVISION: action = RYZ_SYSTEM_NETWORK_OPEN_AP; break;
    case RYZ_SYSTEM_UI_ACTION_NETWORK_ON: action = RYZ_SYSTEM_NETWORK_ON; break;
    case RYZ_SYSTEM_UI_ACTION_NETWORK_OFF: action = RYZ_SYSTEM_NETWORK_OFF; break;
    case RYZ_SYSTEM_UI_ACTION_NETWORK_RETRY: {
        ryz_system_services_snapshot_t current;
        esp_err_t error = ryz_system_services_get_snapshot(&current);
        if (error != ESP_OK) return error;
        if (!current.provisioning_ready || !current.network_valid ||
            current.network_operation.requested != current.network_operation.completed)
            return ESP_ERR_INVALID_STATE;
        if (setup_failed(&current)) action = RYZ_SYSTEM_NETWORK_OPEN_AP;
        else if (current.network.state == RYZ_PROVISIONING_FAILED) action = RYZ_SYSTEM_NETWORK_ON;
        else return ESP_ERR_INVALID_STATE;
        break;
    }
    default: return ESP_ERR_NOT_SUPPORTED;
    }
    return ryz_system_services_request_network(action, operation_id);
}
