#include "workbench_i2c_rpc.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "ryz_i2c_scan.h"
#include "ryz_tools.h"

#define SCAN_SCHEMA "ryz-i2c-scan/2"

static const char *string_field(const cJSON *request, const char *name)
{
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(request, name);
    return cJSON_IsString(field) ? field->valuestring : NULL;
}

static cJSON *reply(const char *id, const char *boot_id, bool ok,
                    esp_err_t code, const char *message)
{
    cJSON *value = cJSON_CreateObject();
    if (!value) return NULL;
    if (!cJSON_AddStringToObject(value, "id", id ? id : "") ||
        !cJSON_AddBoolToObject(value, "ok", ok) ||
        !cJSON_AddStringToObject(value, "schema", SCAN_SCHEMA) ||
        !cJSON_AddStringToObject(value, "boot_id", boot_id) ||
        !cJSON_AddNumberToObject(value, "error_code", code) ||
        (message && !cJSON_AddStringToObject(value, "error", message))) {
        cJSON_Delete(value);
        return NULL;
    }
    return value;
}

static cJSON *failure(const char *id, const char *boot_id, esp_err_t error)
{
    const char *message;
    switch (error) {
    case ESP_ERR_INVALID_ARG: message = "invalid I2C scan request"; break;
    case ESP_ERR_INVALID_STATE: message = "scan state or token does not match"; break;
    case ESP_ERR_NOT_FINISHED:
    case ESP_ERR_TIMEOUT: message = "I2C tool is busy or owned; inspect status"; break;
    case ESP_ERR_NO_MEM: message = "insufficient memory for I2C scanner"; break;
    default: message = "I2C scan operation failed"; break;
    }
    return reply(id, boot_id, false, error, message);
}

bool ryz_workbench_i2c_rpc_supports(const char *operation)
{
    return operation && !strcmp(operation, "i2c_scan");
}

static bool valid_fields(const cJSON *request, bool cancel, bool configure)
{
    if (!cJSON_IsObject(request)) return false;
    const cJSON *field;
    cJSON_ArrayForEach(field, request) {
        if (!field->string) return false;
        const bool token = !strcmp(field->string, "scan_id");
        const bool config_field = !strcmp(field->string, "sda") ||
            !strcmp(field->string, "scl") || !strcmp(field->string, "hz") ||
            !strcmp(field->string, "expected_revision");
        if (token) {
            if (!cancel) return false;
        } else if (config_field) {
            if (!configure) return false;
        } else if (strcmp(field->string, "id") && strcmp(field->string, "op") &&
                   strcmp(field->string, "action") && strcmp(field->string, "boot_id")) {
            return false;
        }
        for (const cJSON *earlier = request->child; earlier != field; earlier = earlier->next) {
            if (!strcmp(earlier->string, field->string)) return false;
        }
        if (!token && !config_field && (!cJSON_IsString(field) || !field->valuestring)) return false;
    }
    return true;
}

static bool integer_field(const cJSON *request, const char *name,
                           uint32_t minimum, uint32_t maximum, uint32_t *out)
{
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(request, name);
    if (!cJSON_IsNumber(field) || !isfinite(field->valuedouble) ||
        field->valuedouble < minimum || field->valuedouble > maximum ||
        floor(field->valuedouble) != field->valuedouble) return false;
    *out = (uint32_t)field->valuedouble;
    return true;
}

static bool add_config(cJSON *value, const char *name, const ryz_i2c_scan_config_t *config)
{
    if (!config) return cJSON_AddNullToObject(value, name) != NULL;
    cJSON *object = cJSON_AddObjectToObject(value, name);
    return object && cJSON_AddNumberToObject(object, "sda", config->sda) &&
        cJSON_AddNumberToObject(object, "scl", config->scl) &&
        cJSON_AddNumberToObject(object, "hz", config->hz);
}

static cJSON *configuration_failure(const char *id, const char *boot_id, esp_err_t error)
{
    const char *message;
    switch (error) {
    case ESP_ERR_INVALID_ARG:
        message = "invalid I2C configuration/revision; use distinct non-reserved pins, shared 41/40 requires 100000 Hz";
        break;
    case ESP_ERR_NOT_SUPPORTED: message = "I2C configuration uses reserved or unsupported pins/clock"; break;
    case ESP_ERR_INVALID_STATE:
        message = "I2C configuration is stale, busy, or pins/resources are unavailable; inspect status";
        break;
    case ESP_ERR_NOT_FINISHED:
    case ESP_ERR_TIMEOUT: message = "I2C configuration is busy; inspect status before retrying"; break;
    case ESP_ERR_NO_MEM: message = "insufficient memory for I2C configuration"; break;
    default: message = "I2C configuration commit failed"; break;
    }
    return reply(id, boot_id, false, error, message);
}

typedef struct {
    const ryz_i2c_scan_config_t *config;
    uint32_t revision, token;
    bool cancel;
} admission_t;

static esp_err_t admit(void *context)
{
    admission_t *request = context;
    if (request->config)
        return ryz_i2c_scan_configure(request->config, request->revision, &request->token);
    return request->cancel ? ryz_i2c_scan_cancel(request->token) : ryz_i2c_scan_start(&request->token);
}

static cJSON *configure(const char *id, const char *boot_id,
                         const ryz_i2c_scan_config_t *config, uint32_t expected_revision)
{
    cJSON *value = reply(id, boot_id, true, ESP_OK, NULL);
    if (!value) return NULL;
    cJSON *revision = NULL;
    if (!cJSON_AddStringToObject(value, "action", "configure") ||
        !cJSON_AddBoolToObject(value, "accepted", true) ||
        !add_config(value, "config", config) ||
        !(revision = cJSON_AddNumberToObject(value, "config_revision", 0))) {
        cJSON_Delete(value);
        return NULL;
    }
    admission_t admission = {.config = config, .revision = expected_revision};
    const esp_err_t error = ryz_tools_external(RYZ_TOOL_I2C, admit, &admission);
    if (error != ESP_OK) {
        cJSON_Delete(value);
        return configuration_failure(id, boot_id, error);
    }
    cJSON_SetNumberValue(revision, admission.token);
    return value;
}

static cJSON *mutate(const char *id, const char *boot_id, bool cancel, uint32_t token)
{
    /* Nothing is allocated after a successful side effect. In particular do
     * not append protocol/version fields in the caller after this returns. */
    cJSON *value = reply(id, boot_id, true, ESP_OK, NULL);
    if (!value) return NULL;
    cJSON *number = NULL;
    if (!cJSON_AddStringToObject(value, "action", cancel ? "cancel" : "start") ||
        !(number = cJSON_AddNumberToObject(value, "scan_id", token)) ||
        !cJSON_AddBoolToObject(value, "accepted", true)) {
        cJSON_Delete(value);
        return NULL;
    }
    admission_t admission = {.cancel = cancel, .token = token};
    esp_err_t error = ryz_tools_external(RYZ_TOOL_I2C, admit, &admission);
    if (error != ESP_OK) {
        cJSON_Delete(value);
        return failure(id, boot_id, error);
    }
    cJSON_SetNumberValue(number, admission.token);
    return value;
}

static const char *phase_name(ryz_i2c_scan_phase_t phase)
{
    switch (phase) {
    case RYZ_I2C_SCAN_IDLE: return "idle";
    case RYZ_I2C_SCAN_QUEUED: return "queued";
    case RYZ_I2C_SCAN_RUNNING: return "running";
    case RYZ_I2C_SCAN_CANCELLING: return "cancelling";
    case RYZ_I2C_SCAN_COMPLETED: return "completed";
    case RYZ_I2C_SCAN_CANCELLED: return "cancelled";
    case RYZ_I2C_SCAN_FAILED: return "failed";
    case RYZ_I2C_SCAN_RELEASING: return "releasing";
    default: return NULL;
    }
}

static bool add_time(cJSON *value, const char *name, int64_t milliseconds)
{
    char text[32];
    const int length = snprintf(text, sizeof(text), "%" PRId64, milliseconds);
    return length > 0 && (size_t)length < sizeof(text) &&
        cJSON_AddStringToObject(value, name, text);
}

static bool add_address(cJSON *array, unsigned address, const esp_err_t *error)
{
    cJSON *item = error ? cJSON_CreateObject() : cJSON_CreateNumber(address);
    if (!item) return false;
    if ((error && (!cJSON_AddNumberToObject(item, "address", address) ||
                   !cJSON_AddNumberToObject(item, "code", *error))) ||
        !cJSON_AddItemToArray(array, item)) {
        cJSON_Delete(item);
        return false;
    }
    return true;
}

static cJSON *status_reply(const char *id, const char *boot_id)
{
    ryz_i2c_scan_snapshot_t snapshot = {0};
    const esp_err_t error = ryz_i2c_scan_get_snapshot(&snapshot);
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return failure(id, boot_id, error);
    const bool worker_running = error == ESP_OK;
    if (!worker_running) {
        memset(&snapshot, 0, sizeof(snapshot));
        snapshot.config = RYZ_I2C_SCAN_DEFAULT_CONFIG;
        snapshot.config_revision = 1;
    }
    const char *phase = worker_running ? phase_name(snapshot.phase) : "unstarted";
    if (!phase) return failure(id, boot_id, ESP_ERR_INVALID_RESPONSE);

    unsigned unscanned = 0;
    unsigned nacks = 0;
    for (unsigned address = RYZ_I2C_SCAN_FIRST; address <= RYZ_I2C_SCAN_LAST; ++address) {
        if (snapshot.results[address] > RYZ_I2C_SCAN_ERROR)
            return failure(id, boot_id, ESP_ERR_INVALID_RESPONSE);
        unscanned += snapshot.results[address] == RYZ_I2C_SCAN_UNSCANNED;
        nacks += snapshot.results[address] == RYZ_I2C_SCAN_NACK;
    }
    const bool empty = worker_running && snapshot.phase == RYZ_I2C_SCAN_COMPLETED &&
        snapshot.error == ESP_OK && snapshot.completed_addresses == RYZ_I2C_SCAN_ADDRESSES &&
        nacks == RYZ_I2C_SCAN_ADDRESSES && snapshot.nack_count == RYZ_I2C_SCAN_ADDRESSES &&
        !snapshot.ack_count && !snapshot.busy_count && !snapshot.timeout_count &&
        !snapshot.error_count && !unscanned && !snapshot.resources_held &&
        snapshot.cleanup_error == ESP_OK;
    cJSON *value = reply(id, boot_id, true, worker_running ? snapshot.error : error, NULL);
    if (!value) return NULL;
    if (!cJSON_AddStringToObject(value, "action", "status") ||
        !cJSON_AddBoolToObject(value, "worker_running", worker_running) ||
        !cJSON_AddStringToObject(value, "phase", phase) ||
        !cJSON_AddNumberToObject(value, "scan_id", snapshot.scan_id) ||
        !cJSON_AddBoolToObject(value, "empty", empty) ||
        !add_config(value, "config", &snapshot.config) ||
        !cJSON_AddNumberToObject(value, "config_revision", snapshot.config_revision) ||
        !add_config(value, "scan_config", snapshot.scan_config_revision ? &snapshot.scan_config : NULL) ||
        !cJSON_AddNumberToObject(value, "scan_config_revision", snapshot.scan_config_revision) ||
        !cJSON_AddNumberToObject(value, "cleanup_error", snapshot.cleanup_error) ||
        !cJSON_AddBoolToObject(value, "resources_held", snapshot.resources_held) ||
        !add_time(value, "queued_ms", snapshot.queued_ms) ||
        !add_time(value, "started_ms", snapshot.started_ms) ||
        !add_time(value, "finished_ms", snapshot.finished_ms) ||
        !cJSON_AddNumberToObject(value, "current_address", snapshot.current_address) ||
        !cJSON_AddNumberToObject(value, "completed_addresses", snapshot.completed_addresses) ||
        !cJSON_AddNumberToObject(value, "probe_calls", snapshot.probe_calls) ||
        !cJSON_AddNumberToObject(value, "busy_responses", snapshot.busy_responses) ||
        !cJSON_AddNumberToObject(value, "ack_count", snapshot.ack_count) ||
        !cJSON_AddNumberToObject(value, "nack_count", snapshot.nack_count) ||
        !cJSON_AddNumberToObject(value, "busy_count", snapshot.busy_count) ||
        !cJSON_AddNumberToObject(value, "timeout_count", snapshot.timeout_count) ||
        !cJSON_AddNumberToObject(value, "error_count", snapshot.error_count) ||
        !cJSON_AddNumberToObject(value, "unscanned_count", unscanned)) goto allocation_failed;
    cJSON *results = cJSON_AddObjectToObject(value, "results");
    if (!results) goto allocation_failed;
    const char *names[] = {"unscanned", "ack", "nack", "busy", "timeout", "error"};
    cJSON *arrays[6] = {0};
    for (unsigned i = 0; i < 6; ++i) {
        arrays[i] = cJSON_AddArrayToObject(results, names[i]);
        if (!arrays[i]) goto allocation_failed;
    }
    for (unsigned address = RYZ_I2C_SCAN_FIRST; address <= RYZ_I2C_SCAN_LAST; ++address) {
        const unsigned result = snapshot.results[address];
        if (!add_address(arrays[result], address,
                         result == RYZ_I2C_SCAN_ERROR ? &snapshot.errors[address] : NULL))
            goto allocation_failed;
    }
    return value;
allocation_failed:
    cJSON_Delete(value);
    return NULL;
}

cJSON *ryz_workbench_i2c_rpc(const cJSON *request, const char *boot_id)
{
    if (!boot_id || !boot_id[0]) return NULL;
    const char *id = string_field(request, "id");
    const char *action = string_field(request, "action");
    const bool cancel = action && !strcmp(action, "cancel");
    const bool start = action && !strcmp(action, "start");
    const bool status = action && !strcmp(action, "status");
    const bool configuration = action && !strcmp(action, "configure");
    if (!valid_fields(request, cancel, configuration) ||
        !ryz_workbench_i2c_rpc_supports(string_field(request, "op")) ||
        (!cancel && !start && !status && !configuration)) return failure(id, boot_id, ESP_ERR_INVALID_ARG);
    const char *requested_boot = string_field(request, "boot_id");
    if ((!status && !requested_boot) || (requested_boot && strcmp(requested_boot, boot_id)))
        return failure(id, boot_id, ESP_ERR_INVALID_STATE);
    uint32_t token = 0;
    if (cancel && !integer_field(request, "scan_id", 1, UINT32_MAX, &token))
        return failure(id, boot_id, ESP_ERR_INVALID_ARG);
    if (configuration) {
        uint32_t sda = 0, scl = 0, hz = 0, expected_revision = 0;
        if (!integer_field(request, "sda", 0, 48, &sda) ||
            !integer_field(request, "scl", 0, 48, &scl) ||
            !integer_field(request, "hz", 100000, 400000, &hz) ||
            (hz != 100000 && hz != 400000) ||
            !integer_field(request, "expected_revision", 1, UINT32_MAX, &expected_revision))
            return configuration_failure(id, boot_id, ESP_ERR_INVALID_ARG);
        const ryz_i2c_scan_config_t config = {(int)sda, (int)scl, hz};
        return configure(id, boot_id, &config, expected_revision);
    }
    return status ? status_reply(id, boot_id) : mutate(id, boot_id, cancel, token);
}
