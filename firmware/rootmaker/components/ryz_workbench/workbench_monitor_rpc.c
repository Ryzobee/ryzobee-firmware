#include "workbench_monitor_rpc.h"
#include "ryz_monitor.h"
#include "ryz_tools.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { A_INVALID, A_START, A_STOP, A_CONFIGURE, A_STATUS, A_READ, A_PAUSE, A_CLEAR };

static const char *string_field(const cJSON *request, const char *name)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(request, name);
    return cJSON_IsString(value) ? value->valuestring : NULL;
}

static unsigned action_id(const char *name)
{
    static const char *const names[] = {"", "start", "stop", "configure", "status", "read", "pause", "clear"};
    if (name) for (unsigned i = 1; i < sizeof(names) / sizeof(names[0]); ++i)
        if (!strcmp(name, names[i])) return i;
    return A_INVALID;
}

static cJSON *reply(const char *id, const char *boot, bool ok, esp_err_t error)
{
    cJSON *value = cJSON_CreateObject();
    if (!value) return NULL;
    if (!cJSON_AddStringToObject(value, "id", id ? id : "") ||
        !cJSON_AddBoolToObject(value, "ok", ok) ||
        !cJSON_AddStringToObject(value, "schema", "ryz-monitor/1") ||
        !cJSON_AddStringToObject(value, "boot_id", boot) ||
        !cJSON_AddNumberToObject(value, "error_code", error)) goto failed;
    if (!ok) {
        const char *message = error == ESP_ERR_INVALID_ARG ? "invalid Monitor request/configuration" :
            error == ESP_ERR_INVALID_STATE ? "Monitor boot/session/view/revision is stale or state is unavailable" :
            error == ESP_ERR_NOT_FINISHED || error == ESP_ERR_TIMEOUT ? "Monitor is busy; inspect status" :
            error == ESP_ERR_NO_MEM ? "insufficient memory for Monitor" : "Monitor operation failed";
        if (!cJSON_AddStringToObject(value, "error", message)) goto failed;
    }
    return value;
failed:
    cJSON_Delete(value);
    return NULL;
}

static bool integer_field(const cJSON *request, const char *name, uint32_t minimum,
                           uint32_t maximum, uint32_t *out)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(request, name);
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) ||
        value->valuedouble < minimum || value->valuedouble > maximum ||
        floor(value->valuedouble) != value->valuedouble) return false;
    *out = (uint32_t)value->valuedouble;
    return true;
}

static bool valid_fields(const cJSON *request, unsigned action)
{
    if (!cJSON_IsObject(request)) return false;
    const char *source = string_field(request, "source");
    const cJSON *field;
    cJSON_ArrayForEach(field, request) {
        if (!field->string) return false;
        for (const cJSON *previous = request->child; previous != field; previous = previous->next)
            if (!strcmp(previous->string, field->string)) return false;
        const char *name = field->string;
        bool text = !strcmp(name, "id") || !strcmp(name, "op") ||
            !strcmp(name, "action") || !strcmp(name, "boot_id");
        bool allowed = text;
        if (!strcmp(name, "expected_revision")) allowed = action == A_START || action == A_CONFIGURE;
        else if (!strcmp(name, "session_id")) allowed = action == A_CONFIGURE || action == A_STOP ||
            action == A_READ || action == A_PAUSE || action == A_CLEAR;
        else if (!strcmp(name, "expected_generation")) allowed = action == A_PAUSE || action == A_CLEAR;
        else if (!strcmp(name, "view_generation") || !strcmp(name, "after_sequence")) allowed = action == A_READ;
        else if (!strcmp(name, "paused")) allowed = action == A_PAUSE;
        else if (!strcmp(name, "source")) { allowed = action == A_CONFIGURE; text = true; }
        else if (!strcmp(name, "rx") || !strcmp(name, "tx") || !strcmp(name, "baud"))
            allowed = action == A_CONFIGURE && source && !strcmp(source, "uart");
        if (!allowed || (text && (!cJSON_IsString(field) || !field->valuestring))) return false;
    }
    return true;
}

static const char *source_name(ryz_monitor_source_t source)
{
    return source == RYZ_MONITOR_SYSTEM ? "system" : source == RYZ_MONITOR_UART ? "uart" : NULL;
}

static bool add_config(cJSON *value, const char *name, const ryz_monitor_config_t *config)
{
    if (!config) return cJSON_AddNullToObject(value, name) != NULL;
    const char *source = source_name(config->source);
    cJSON *object = source ? cJSON_AddObjectToObject(value, name) : NULL;
    return object && cJSON_AddStringToObject(object, "source", source) &&
        cJSON_AddNumberToObject(object, "rx", config->rx) &&
        cJSON_AddNumberToObject(object, "tx", config->tx) &&
        cJSON_AddNumberToObject(object, "baud", config->baud);
}

static bool add_u64(cJSON *value, const char *name, uint64_t number)
{
    char text[32];
    int length = snprintf(text, sizeof(text), "%" PRIu64, number);
    return length > 0 && (size_t)length < sizeof(text) && cJSON_AddStringToObject(value, name, text);
}

static bool add_time(cJSON *value, const char *name, int64_t number)
{
    char text[32];
    int length = snprintf(text, sizeof(text), "%" PRId64, number);
    return length > 0 && (size_t)length < sizeof(text) && cJSON_AddStringToObject(value, name, text);
}

static bool add_stream(cJSON *value, const ryz_monitor_stream_info_t *stream)
{
    const char *source = source_name(stream->source);
    cJSON *object = source ? cJSON_AddObjectToObject(value, "stream") : NULL;
    return object && cJSON_AddStringToObject(object, "source", source) &&
        cJSON_AddNumberToObject(object, "session_id", stream->session_id) &&
        cJSON_AddNumberToObject(object, "config_revision", stream->config_revision) &&
        cJSON_AddNumberToObject(object, "view_generation", stream->view_generation) &&
        cJSON_AddBoolToObject(object, "accepting", stream->accepting) &&
        cJSON_AddBoolToObject(object, "paused", stream->paused) &&
        cJSON_AddBoolToObject(object, "sequence_exhausted", stream->sequence_exhausted) &&
        cJSON_AddBoolToObject(object, "capture_gap_since_boot", stream->capture_gap_since_boot) &&
        cJSON_AddNumberToObject(object, "first_sequence", stream->first_sequence) &&
        cJSON_AddNumberToObject(object, "last_sequence", stream->last_sequence) &&
        cJSON_AddNumberToObject(object, "retained_records", stream->retained_records) &&
        add_u64(object, "chunks", stream->chunks) && add_u64(object, "bytes", stream->bytes) &&
        add_u64(object, "overwritten_chunks", stream->overwritten_chunks) &&
        add_u64(object, "overwritten_bytes", stream->overwritten_bytes) &&
        add_u64(object, "truncated_bytes", stream->truncated_bytes) &&
        add_u64(object, "filtered_logs", stream->filtered_logs);
}

static bool add_uart_events(cJSON *value, const ryz_monitor_uart_events_t *events)
{
    cJSON *object = cJSON_AddObjectToObject(value, "uart_events");
    return object && add_u64(object, "fifo_overflow", events->fifo_overflow) &&
        add_u64(object, "buffer_full", events->buffer_full) &&
        add_u64(object, "frame_error", events->frame_error) &&
        add_u64(object, "parity_error", events->parity_error) &&
        add_u64(object, "break_events", events->break_events) &&
        cJSON_AddBoolToObject(object, "events_may_be_lost", events->events_may_be_lost);
}

static cJSON *status_reply(const char *id, const char *boot)
{
    ryz_monitor_snapshot_t snapshot = {0};
    esp_err_t error = ryz_monitor_get_snapshot(&snapshot);
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return reply(id, boot, false, error);
    bool running = error == ESP_OK;
    if (!running) {
        memset(&snapshot, 0, sizeof(snapshot));
        snapshot.config = RYZ_MONITOR_DEFAULT_CONFIG;
        snapshot.config_revision = 1;
    }
    const char *const phases[] = {"idle", "starting", "running", "reconfiguring", "stopping", "stopped", "failed"};
    const char *const operations[] = {"none", "start", "configure", "stop"};
    if ((unsigned)snapshot.phase >= sizeof(phases) / sizeof(phases[0]) ||
        (unsigned)snapshot.operation >= sizeof(operations) / sizeof(operations[0]))
        return reply(id, boot, false, ESP_ERR_INVALID_RESPONSE);
    cJSON *value = reply(id, boot, true, error);
    if (!value) return NULL;
    /* start admits its new session before the worker resets the stream.
     * A cleared stream is absence of history, not a SYSTEM view with ID 0. */
    const bool has_stream = snapshot.session_id && snapshot.stream.session_id == snapshot.session_id;
    if (!cJSON_AddStringToObject(value, "action", "status") ||
        !cJSON_AddBoolToObject(value, "worker_running", running) ||
        !cJSON_AddStringToObject(value, "phase", running ? phases[snapshot.phase] : "unstarted") ||
        !add_config(value, "config", &snapshot.config) ||
        !add_config(value, "capture_config", snapshot.session_id ? &snapshot.capture_config : NULL) ||
        !add_config(value, "requested_config", snapshot.operation ? &snapshot.requested_config : NULL) ||
        !cJSON_AddNumberToObject(value, "config_revision", snapshot.config_revision) ||
        !cJSON_AddNumberToObject(value, "session_id", snapshot.session_id) ||
        !cJSON_AddNumberToObject(value, "operation_id", snapshot.operation_id) ||
        !cJSON_AddStringToObject(value, "operation", operations[snapshot.operation]) ||
        !cJSON_AddBoolToObject(value, "operation_pending", snapshot.operation_pending) ||
        !cJSON_AddBoolToObject(value, "source_active", snapshot.source_active) ||
        !cJSON_AddBoolToObject(value, "resources_held", snapshot.resources_held) ||
        !cJSON_AddNumberToObject(value, "operation_error", snapshot.operation_error) ||
        !cJSON_AddNumberToObject(value, "cleanup_error", snapshot.cleanup_error) ||
        !cJSON_AddNumberToObject(value, "restore_error", snapshot.restore_error) ||
        !add_time(value, "started_ms", snapshot.started_ms) ||
        !add_time(value, "operation_finished_ms", snapshot.operation_finished_ms) ||
        !cJSON_AddBoolToObject(value, "capture_gap_since_boot", snapshot.stream.capture_gap_since_boot) ||
        !(has_stream ? add_stream(value, &snapshot.stream) : cJSON_AddNullToObject(value, "stream") != NULL) ||
        !add_uart_events(value, &snapshot.uart_events)) {
        cJSON_Delete(value);
        return NULL;
    }
    return value;
}

static void encode_base64(const uint8_t *bytes, size_t length, char *out)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t used = 0;
    for (size_t index = 0; index < length; index += 3) {
        const size_t remaining = length - index;
        const uint32_t bits = (uint32_t)bytes[index] << 16 |
            (remaining > 1 ? (uint32_t)bytes[index + 1] << 8 : 0) |
            (remaining > 2 ? bytes[index + 2] : 0);
        out[used++] = alphabet[(bits >> 18) & 63];
        out[used++] = alphabet[(bits >> 12) & 63];
        out[used++] = remaining > 1 ? alphabet[(bits >> 6) & 63] : '=';
        out[used++] = remaining > 2 ? alphabet[bits & 63] : '=';
    }
    out[used] = 0;
}

static cJSON *read_reply(const char *id, const char *boot, uint32_t session,
                          uint32_t generation, uint32_t after)
{
    ryz_monitor_page_t page = {0};
    esp_err_t error = ryz_monitor_read(session, generation, after, &page);
    if (error != ESP_OK) return reply(id, boot, false, error);
    if (page.count > RYZ_MONITOR_PAGE_RECORDS) return reply(id, boot, false, ESP_ERR_INVALID_RESPONSE);
    for (unsigned i = 0; i < page.count; ++i)
        if (page.records[i].length > RYZ_MONITOR_RECORD_BYTES) return reply(id, boot, false, ESP_ERR_INVALID_RESPONSE);
    cJSON *value = reply(id, boot, true, ESP_OK);
    if (!value) return NULL;
    cJSON *records = NULL;
    if (!cJSON_AddStringToObject(value, "action", "read") || !add_stream(value, &page.stream) ||
        !cJSON_AddNumberToObject(value, "next_sequence", page.next_sequence) ||
        !cJSON_AddBoolToObject(value, "gap", page.gap) || !cJSON_AddBoolToObject(value, "more", page.more) ||
        !cJSON_AddNumberToObject(value, "count", page.count) ||
        !(records = cJSON_AddArrayToObject(value, "records"))) goto failed;
    for (unsigned i = 0; i < page.count; ++i) {
        const ryz_monitor_record_t *record = &page.records[i];
        char encoded[4 * ((RYZ_MONITOR_RECORD_BYTES + 2) / 3) + 1];
        encode_base64(record->bytes, record->length, encoded);
        cJSON *item = cJSON_CreateObject();
        if (!item) goto failed;
        if (!cJSON_AddNumberToObject(item, "sequence", record->sequence) ||
            !add_time(item, "captured_ms", record->captured_ms) ||
            !cJSON_AddNumberToObject(item, "length", record->length) ||
            !cJSON_AddBoolToObject(item, "truncated", record->truncated) ||
            !cJSON_AddStringToObject(item, "data_b64", encoded) || !cJSON_AddItemToArray(records, item)) {
            cJSON_Delete(item);
            goto failed;
        }
    }
    return value;
failed:
    cJSON_Delete(value);
    return NULL;
}

static bool baud_valid(uint32_t baud)
{
    const uint32_t allowed[] = {1200,2400,4800,9600,19200,38400,57600,115200,230400,460800,921600};
    for (unsigned i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i) if (baud == allowed[i]) return true;
    return false;
}

bool ryz_workbench_monitor_rpc_supports(const char *operation)
{
    return operation && !strcmp(operation, "monitor");
}

typedef struct {
    unsigned action;
    uint32_t revision, session, generation, token;
    bool paused;
    ryz_monitor_config_t config;
} admission_t;

static esp_err_t admit(void *context)
{
    admission_t *request = context;
    switch (request->action) {
    case A_START: return ryz_monitor_start(request->revision, &request->token);
    case A_CONFIGURE: return ryz_monitor_configure(&request->config, request->revision, request->session, &request->token);
    case A_STOP: return ryz_monitor_stop(request->session, &request->token);
    case A_PAUSE: return ryz_monitor_set_paused(request->session, request->generation, request->paused, &request->token);
    case A_CLEAR: return ryz_monitor_clear(request->session, request->generation, &request->token);
    default: return ESP_ERR_INVALID_ARG;
    }
}

cJSON *ryz_workbench_monitor_rpc(const cJSON *request, const char *boot)
{
    if (!boot || !boot[0]) return NULL;
    const char *id = string_field(request, "id");
    const char *action_name = string_field(request, "action");
    const unsigned action = action_id(action_name);
    if (!action || !valid_fields(request, action) ||
        !ryz_workbench_monitor_rpc_supports(string_field(request, "op"))) return reply(id, boot, false, ESP_ERR_INVALID_ARG);
    const char *requested_boot = string_field(request, "boot_id");
    if ((action != A_STATUS && !requested_boot) || (requested_boot && strcmp(requested_boot, boot)))
        return reply(id, boot, false, ESP_ERR_INVALID_STATE);
    if (action == A_STATUS) return status_reply(id, boot);
    uint32_t session = 0, revision = 0, generation = 0, after = 0;
    if ((action != A_START && !integer_field(request, "session_id", action == A_CONFIGURE ? 0 : 1, UINT32_MAX, &session)) ||
        ((action == A_START || action == A_CONFIGURE) && !integer_field(request, "expected_revision", 1, UINT32_MAX, &revision)) ||
        ((action == A_PAUSE || action == A_CLEAR) && !integer_field(request, "expected_generation", 1, UINT32_MAX, &generation)) ||
        (action == A_READ && (!integer_field(request, "view_generation", 1, UINT32_MAX, &generation) ||
                             !integer_field(request, "after_sequence", 0, UINT32_MAX, &after))))
        return reply(id, boot, false, ESP_ERR_INVALID_ARG);
    if (action == A_READ) return read_reply(id, boot, session, generation, after);
    const cJSON *paused = cJSON_GetObjectItemCaseSensitive(request, "paused");
    if (action == A_PAUSE && !cJSON_IsBool(paused)) return reply(id, boot, false, ESP_ERR_INVALID_ARG);
    ryz_monitor_config_t config = RYZ_MONITOR_DEFAULT_CONFIG;
    if (action == A_CONFIGURE) {
        const char *source = string_field(request, "source");
        if (source && !strcmp(source, "uart")) {
            uint32_t rx = 0, tx = 0;
            if (!integer_field(request, "rx", 0, 48, &rx) || !integer_field(request, "tx", 0, 48, &tx) ||
                !integer_field(request, "baud", 1200, 921600, &config.baud) || !baud_valid(config.baud))
                return reply(id, boot, false, ESP_ERR_INVALID_ARG);
            config.source = RYZ_MONITOR_UART;
            config.rx = (int)rx;
            config.tx = (int)tx;
        } else if (!source || strcmp(source, "system")) return reply(id, boot, false, ESP_ERR_INVALID_ARG);
    }
    cJSON *value = reply(id, boot, true, ESP_OK);
    if (!value) return NULL;
    bool view_action = action == A_PAUSE || action == A_CLEAR;
    cJSON *number = NULL, *start_session = NULL;
    if (!cJSON_AddStringToObject(value, "action", action_name) ||
        !cJSON_AddBoolToObject(value, "accepted", true) ||
        !(number = cJSON_AddNumberToObject(value, view_action ? "view_generation" : "operation_id", 0)) ||
        (action == A_START && !(start_session = cJSON_AddNumberToObject(value, "session_id", 0)))) {
        cJSON_Delete(value);
        return NULL;
    }
    admission_t admission = {.action = action, .revision = revision, .session = session,
        .generation = generation, .paused = cJSON_IsTrue(paused), .config = config};
    esp_err_t error = ryz_tools_external(RYZ_TOOL_MONITOR, admit, &admission);
    if (error != ESP_OK) {
        cJSON_Delete(value);
        return reply(id, boot, false, error);
    }
    cJSON_SetNumberValue(number, admission.token);
    if (start_session) cJSON_SetNumberValue(start_session, admission.token);
    return value;
}
