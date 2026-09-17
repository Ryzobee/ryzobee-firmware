#include "workbench_network_rpc.h"
#include "ryz_system_services.h"
#include <string.h>

static const char *field(const cJSON *request, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(request, name);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static cJSON *response(const char *id, const char *boot, esp_err_t error)
{
    cJSON *out = cJSON_CreateObject();
    if (!out || !cJSON_AddStringToObject(out, "schema", "ryz-network/1") ||
        !cJSON_AddStringToObject(out, "id", id ? id : "") ||
        !cJSON_AddStringToObject(out, "boot_id", boot) ||
        !cJSON_AddBoolToObject(out, "ok", error == ESP_OK) ||
        !cJSON_AddNumberToObject(out, "error_code", error)) {
        cJSON_Delete(out);
        return NULL;
    }
    return out;
}

static const char *network_state(ryz_provisioning_state_t state)
{
    switch (state) {
    case RYZ_PROVISIONING_UNCONFIGURED: return "unconfigured";
    case RYZ_PROVISIONING_AP_STARTING: return "ap_starting";
    case RYZ_PROVISIONING_AP_READY: return "ap_ready";
    case RYZ_PROVISIONING_CREDENTIALS_RECEIVED: return "credentials_received";
    case RYZ_PROVISIONING_CONNECTING: return "connecting";
    case RYZ_PROVISIONING_ONLINE: return "online";
    case RYZ_PROVISIONING_FAILED: return "failed";
    case RYZ_PROVISIONING_STOPPING: return "stopping";
    case RYZ_PROVISIONING_OFF: return "off";
    default: return NULL;
    }
}

static const char *action_name(ryz_system_network_action_t action)
{
    switch (action) {
    case RYZ_SYSTEM_NETWORK_NONE: return "none";
    case RYZ_SYSTEM_NETWORK_ON: return "on";
    case RYZ_SYSTEM_NETWORK_OFF: return "off";
    case RYZ_SYSTEM_NETWORK_REPROVISION: return "reprovision";
    case RYZ_SYSTEM_NETWORK_OPEN_AP: return "setup";
    default: return NULL;
    }
}

static const char *phase_name(ryz_system_network_phase_t phase)
{
    switch (phase) {
    case RYZ_SYSTEM_NETWORK_IDLE: return "idle";
    case RYZ_SYSTEM_NETWORK_QUEUED: return "queued";
    case RYZ_SYSTEM_NETWORK_WAITING_OTA: return "waiting_ota";
    case RYZ_SYSTEM_NETWORK_APPLYING: return "applying";
    case RYZ_SYSTEM_NETWORK_DONE: return "done";
    case RYZ_SYSTEM_NETWORK_FAILED: return "failed";
    default: return NULL;
    }
}

static cJSON *status(const char *id, const char *boot)
{
    ryz_system_services_snapshot_t services;
    esp_err_t error = ryz_system_services_get_snapshot(&services);
    if (error != ESP_OK) return response(id, boot, error);
    const ryz_provisioning_snapshot_t *net = &services.network;
    const ryz_system_network_operation_t *op = &services.network_operation;
    const char *state = network_state(net->state), *action = action_name(op->action);
    const char *completed_action = action_name(op->completed_action), *phase = phase_name(op->phase);
    if (!state || !action || !completed_action || !phase) return response(id, boot, ESP_ERR_INVALID_STATE);
    cJSON *out = response(id, boot, ESP_OK);
    cJSON *network = out ? cJSON_AddObjectToObject(out, "network") : NULL;
    cJSON *operation = out ? cJSON_AddObjectToObject(out, "operation") : NULL;
    if (!network || !operation ||
        !cJSON_AddBoolToObject(out, "network_valid", services.network_valid) ||
        !cJSON_AddBoolToObject(network, "enabled", net->enabled) ||
        !cJSON_AddBoolToObject(network, "stop_confirmed", net->stop_confirmed) ||
        !cJSON_AddStringToObject(network, "state", state) ||
        !cJSON_AddNumberToObject(network, "revision", net->revision) ||
        !cJSON_AddNumberToObject(network, "last_error", net->last_error) ||
        !cJSON_AddNumberToObject(network, "detail", net->detail) ||
        !cJSON_AddBoolToObject(network, "credentials_stored", net->credentials_stored) ||
        !cJSON_AddBoolToObject(network, "portal_active", net->portal_active) ||
        !cJSON_AddStringToObject(network, "portal_ip", net->portal_ip) ||
        !cJSON_AddStringToObject(network, "sta_ssid", net->sta_ssid) ||
        !cJSON_AddStringToObject(network, "ipv4", net->ipv4) ||
        !cJSON_AddNumberToObject(operation, "requested", op->requested) ||
        !cJSON_AddNumberToObject(operation, "completed", op->completed) ||
        !cJSON_AddStringToObject(operation, "action", action) ||
        !cJSON_AddStringToObject(operation, "completed_action", completed_action) ||
        !cJSON_AddStringToObject(operation, "phase", phase) ||
        !cJSON_AddNumberToObject(operation, "result", op->result)) {
        cJSON_Delete(out);
        return NULL;
    }
    return out;
}

cJSON *ryz_workbench_network_rpc(const cJSON *request, const char *boot)
{
    const char *id = field(request, "id"), *op = field(request, "op");
    const char *action = field(request, "action"), *requested_boot = field(request, "boot_id");
    if (!boot || !*boot) return NULL;
    if (!cJSON_IsObject(request) || !id || strlen(id) > 64 || !op || strcmp(op, "network") || !action)
        return response(id, boot, ESP_ERR_INVALID_ARG);
    bool query = !strcmp(action, "status"), reprovision = !strcmp(action, "reprovision");
    ryz_system_network_action_t command = reprovision ? RYZ_SYSTEM_NETWORK_REPROVISION :
        !strcmp(action, "setup") ? RYZ_SYSTEM_NETWORK_OPEN_AP :
        !strcmp(action, "on") ? RYZ_SYSTEM_NETWORK_ON :
        !strcmp(action, "off") ? RYZ_SYSTEM_NETWORK_OFF : RYZ_SYSTEM_NETWORK_NONE;
    if (!query && command == RYZ_SYSTEM_NETWORK_NONE) return response(id, boot, ESP_ERR_INVALID_ARG);
    for (const cJSON *a = request->child; a; a = a->next) {
        if (!a->string || (strcmp(a->string, "id") && strcmp(a->string, "op") &&
            strcmp(a->string, "action") && strcmp(a->string, "boot_id") &&
            !(reprovision && !strcmp(a->string, "confirm")))) return response(id, boot, ESP_ERR_INVALID_ARG);
        for (const cJSON *b = a->next; b; b = b->next)
            if (b->string && !strcmp(a->string, b->string)) return response(id, boot, ESP_ERR_INVALID_ARG);
    }
    if ((!query && !requested_boot) ||
        (cJSON_HasObjectItem(request, "boot_id") && (!requested_boot || strcmp(requested_boot, boot))))
        return response(id, boot, ESP_ERR_INVALID_STATE);
    if (reprovision && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "confirm")))
        return response(id, boot, ESP_ERR_INVALID_ARG);
    if (query) return status(id, boot);
    cJSON *ack = response(id, boot, ESP_OK);
    cJSON *token = ack ? cJSON_AddNumberToObject(ack, "operation_id", 0) : NULL;
    if (!token || !cJSON_AddBoolToObject(ack, "accepted", true) ||
        !cJSON_AddStringToObject(ack, "action", action)) {
        cJSON_Delete(ack);
        return NULL;
    }
    /* After acceptance only an existing numeric node is changed. Allocation
     * failure cannot hide an accepted network request. Transport may still
     * lose the complete ACK; callers must inspect status, never blind retry. */
    uint32_t operation_id = 0;
    esp_err_t error = ryz_system_services_request_network(command, &operation_id);
    if (error != ESP_OK) {
        cJSON_Delete(ack);
        return response(id, boot, error);
    }
    cJSON_SetNumberValue(token, operation_id);
    return ack;
}
