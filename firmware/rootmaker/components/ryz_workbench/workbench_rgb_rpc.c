#include "workbench_rgb_rpc.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "ryz_rgb.h"
#include "ryz_tools.h"

#define RGB_SCHEMA "ryz-rgb/1"

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
        !cJSON_AddStringToObject(value, "schema", RGB_SCHEMA) ||
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
    case ESP_ERR_INVALID_ARG: message = "invalid RGB request"; break;
    case ESP_ERR_INVALID_STATE: message = "RGB state or boot does not match"; break;
    case ESP_ERR_NOT_FINISHED:
    case ESP_ERR_TIMEOUT: message = "RGB tool is busy or owned; inspect status"; break;
    case ESP_ERR_NO_MEM: message = "insufficient memory for RGB worker"; break;
    default: message = "RGB operation failed"; break;
    }
    return reply(id, boot_id, false, error, message);
}

bool ryz_workbench_rgb_rpc_supports(const char *operation)
{
    return operation && !strcmp(operation, "rgb");
}

static bool valid_fields(const cJSON *request, bool set, bool cleanup)
{
    if (!cJSON_IsObject(request)) return false;
    const cJSON *field;
    cJSON_ArrayForEach(field, request) {
        if (!field->string) return false;
        const bool channel = !strcmp(field->string, "red") ||
            !strcmp(field->string, "green") || !strcmp(field->string, "blue");
        const bool token = !strcmp(field->string, "request_id");
        if (channel) {
            if (!set) return false;
        } else if (token) {
            if (!cleanup) return false;
        } else if (strcmp(field->string, "id") && strcmp(field->string, "op") &&
                   strcmp(field->string, "action") && strcmp(field->string, "boot_id")) {
            return false;
        }
        for (const cJSON *earlier = request->child; earlier != field; earlier = earlier->next) {
            if (!strcmp(earlier->string, field->string)) return false;
        }
        if (!channel && !token && (!cJSON_IsString(field) || !field->valuestring)) return false;
    }
    return true;
}

static bool channel_field(const cJSON *request, const char *name, uint8_t *out)
{
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(request, name);
    if (!cJSON_IsNumber(field) || !isfinite(field->valuedouble) ||
        field->valuedouble < 0 || field->valuedouble > 255 ||
        floor(field->valuedouble) != field->valuedouble) return false;
    *out = (uint8_t)field->valuedouble;
    return true;
}

static bool add_color(cJSON *value, const char *name, const ryz_rgb_color_t *color)
{
    if (!color) return cJSON_AddNullToObject(value, name) != NULL;
    cJSON *object = cJSON_AddObjectToObject(value, name);
    return object && cJSON_AddNumberToObject(object, "red", color->red) &&
        cJSON_AddNumberToObject(object, "green", color->green) &&
        cJSON_AddNumberToObject(object, "blue", color->blue);
}

typedef struct { bool cleanup; uint32_t token; ryz_rgb_color_t color; } admission_t;

static esp_err_t admit(void *context)
{
    admission_t *request = context;
    return request->cleanup ? ryz_rgb_cleanup(request->token) : ryz_rgb_submit(request->color, &request->token);
}

static cJSON *submit(const char *id, const char *boot_id, const char *action,
                    bool cleanup, uint32_t token, ryz_rgb_color_t color)
{
    cJSON *value = reply(id, boot_id, true, ESP_OK, NULL);
    if (!value) return NULL;
    cJSON *number = NULL;
    if (!cJSON_AddStringToObject(value, "action", action) ||
        !(number = cJSON_AddNumberToObject(value, "request_id", token)) ||
        !cJSON_AddBoolToObject(value, "accepted", true) ||
        (!cleanup && !add_color(value, "requested", &color))) {
        cJSON_Delete(value);
        return NULL;
    }
    admission_t admission = {.cleanup = cleanup, .token = token, .color = color};
    const esp_err_t error = ryz_tools_external(RYZ_TOOL_RGB, admit, &admission);
    if (error != ESP_OK) {
        cJSON_Delete(value);
        return failure(id, boot_id, error);
    }
    /* No allocation after admission, including the version and requested RGB.
     * A later transport serialization/lost ACK is still an unknown outcome. */
    cJSON_SetNumberValue(number, admission.token);
    return value;
}

static const char *phase_name(ryz_rgb_phase_t phase)
{
    switch (phase) {
    case RYZ_RGB_IDLE: return "idle";
    case RYZ_RGB_QUEUED: return "queued";
    case RYZ_RGB_SENDING: return "sending";
    case RYZ_RGB_COMPLETED: return "completed";
    case RYZ_RGB_FAILED: return "failed";
    case RYZ_RGB_CLEANING: return "cleaning";
    case RYZ_RGB_CLEANED: return "cleaned";
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

static cJSON *status_reply(const char *id, const char *boot_id)
{
    ryz_rgb_snapshot_t snapshot = {0};
    const esp_err_t error = ryz_rgb_get_snapshot(&snapshot);
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return failure(id, boot_id, error);
    const bool worker_running = error == ESP_OK;
    if (!worker_running) memset(&snapshot, 0, sizeof(snapshot));
    const char *phase = worker_running ? phase_name(snapshot.phase) : "unstarted";
    if (!phase) return failure(id, boot_id, ESP_ERR_INVALID_RESPONSE);
    cJSON *value = reply(id, boot_id, true, worker_running ? snapshot.error : error, NULL);
    if (!value) return NULL;
    if (!cJSON_AddStringToObject(value, "action", "status") ||
        !cJSON_AddBoolToObject(value, "worker_running", worker_running) ||
        !cJSON_AddStringToObject(value, "phase", phase) ||
        !cJSON_AddNumberToObject(value, "request_id", snapshot.request_id) ||
        !add_color(value, "requested", snapshot.request_id ? &snapshot.requested : NULL) ||
        !cJSON_AddNumberToObject(value, "last_completed_id", snapshot.last_completed_id) ||
        !add_color(value, "color", snapshot.last_completed_id ? &snapshot.last_completed_color : NULL) ||
        !cJSON_AddBoolToObject(value, "output_known", snapshot.output_known) ||
        !cJSON_AddBoolToObject(value, "resources_held", snapshot.resources_held) ||
        !cJSON_AddNumberToObject(value, "cleanup_error", snapshot.cleanup_error) ||
        !add_time(value, "queued_ms", snapshot.queued_ms) ||
        !add_time(value, "finished_ms", snapshot.finished_ms)) {
        cJSON_Delete(value);
        return NULL;
    }
    return value;
}

cJSON *ryz_workbench_rgb_rpc(const cJSON *request, const char *boot_id)
{
    if (!boot_id || !boot_id[0]) return NULL;
    const char *id = string_field(request, "id");
    const char *action = string_field(request, "action");
    const bool set = action && !strcmp(action, "set");
    const bool off = action && !strcmp(action, "off");
    const bool cleanup = action && !strcmp(action, "cleanup");
    const bool status = action && !strcmp(action, "status");
    if (!valid_fields(request, set, cleanup) ||
        !ryz_workbench_rgb_rpc_supports(string_field(request, "op")) ||
        (!set && !off && !status && !cleanup)) return failure(id, boot_id, ESP_ERR_INVALID_ARG);
    const char *requested_boot = string_field(request, "boot_id");
    if ((!status && !requested_boot) || (requested_boot && strcmp(requested_boot, boot_id)))
        return failure(id, boot_id, ESP_ERR_INVALID_STATE);
    ryz_rgb_color_t color = {0};
    uint32_t token = 0;
    if (cleanup) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(request, "request_id");
        if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) || value->valuedouble < 1 ||
            value->valuedouble > UINT32_MAX || floor(value->valuedouble) != value->valuedouble)
            return failure(id, boot_id, ESP_ERR_INVALID_ARG);
        token = (uint32_t)value->valuedouble;
    }
    if (set && (!channel_field(request, "red", &color.red) ||
                !channel_field(request, "green", &color.green) ||
                !channel_field(request, "blue", &color.blue)))
        return failure(id, boot_id, ESP_ERR_INVALID_ARG);
    return status ? status_reply(id, boot_id) : submit(id, boot_id, action, cleanup, token, color);
}
