#include "workbench_tools.h"
#include <stdatomic.h>
#include <math.h>
#include <string.h>

typedef struct {
    char job_id[32];
    bool retired;
    ryz_workbench_tools_report_t report;
} record_t;
static record_t s_records[RYZ_WORKBENCH_TOOLS_RETIRED_MAX];
/* Never acquired with a blocking wait. No JSON, caller callback, or hardware
 * operation occurs here. Native workers never acquire this gate. */
static atomic_flag s_gate = ATOMIC_FLAG_INIT;
static bool enter(void) { return !atomic_flag_test_and_set_explicit(&s_gate, memory_order_acquire); }
static void leave(void) { atomic_flag_clear_explicit(&s_gate, memory_order_release); }
static record_t *find_session(uint32_t session)
{
    for (unsigned i = 0; i < RYZ_WORKBENCH_TOOLS_RETIRED_MAX; ++i)
        if (session && s_records[i].report.session_id == session) return &s_records[i];
    return NULL;
}
static int mapped(esp_err_t error)
{
    switch (error) {
    case ESP_OK: return RYZ_TOOL_CALL_OK;
    case ESP_ERR_NOT_FINISHED: case ESP_ERR_TIMEOUT: return RYZ_TOOL_CALL_BUSY;
    case ESP_ERR_INVALID_ARG: case ESP_ERR_INVALID_SIZE: return RYZ_TOOL_CALL_INVALID;
    case ESP_ERR_INVALID_STATE: return RYZ_TOOL_CALL_STATE;
    case ESP_ERR_NOT_SUPPORTED: return RYZ_TOOL_CALL_UNAVAILABLE;
    case ESP_ERR_NO_MEM: return RYZ_TOOL_CALL_NO_MEMORY;
    default: return RYZ_TOOL_CALL_FAILED;
    }
}

void ryz_workbench_tools_begin(ryz_workbench_tools_job_t *job, const char *id)
{
    if (!job) return;
    memset(job, 0, sizeof(*job));
    if (id && strlen(id) < sizeof(job->job_id)) strcpy(job->job_id, id);
}
int ryz_workbench_tools_call(ryz_workbench_tools_job_t *job,
    const ryz_tool_request_t *request, ryz_tool_reply_t *reply)
{
    if (!reply) return RYZ_TOOL_CALL_INVALID;
    memset(reply, 0, sizeof(*reply));
    esp_err_t error = ESP_ERR_INVALID_ARG;
    if (!job || !request || !job->job_id[0] || job->finished) goto done;
    if (!enter()) { error = ESP_ERR_NOT_FINISHED; goto done; }
    record_t *record = find_session(job->session_id);
    if (!job->session_id) {
        for (unsigned i = 0; i < RYZ_WORKBENCH_TOOLS_RETIRED_MAX; ++i) {
            if (!s_records[i].report.session_id) { record = &s_records[i]; break; }
        }
        if (!record) { error = ESP_ERR_NO_MEM; leave(); goto done; }
        /* Fixed retention is reserved under our gate BEFORE broker open.
         * A rejected open must not consume a record or affect an old job. */
        uint32_t session = 0;
        error = ryz_tools_open(&session);
        if (error != ESP_OK) { leave(); goto done; }
        job->session_id = session;
        memset(record, 0, sizeof(*record));
        strcpy(record->job_id, job->job_id);
        record->report.session_id = session;
        record->report.phase = RYZ_WB_TOOLS_ACTIVE;
    }
    if (!record || record->retired) error = ESP_ERR_INVALID_STATE;
    else error = ryz_tools_call(job->session_id, request, reply);
    leave();
done:
    if (error != ESP_OK) memset(reply, 0, sizeof(*reply));
    reply->error_code = error;
    return mapped(error);
}
static void progress(record_t *record, int64_t now)
{
    ryz_workbench_tools_report_t *report = &record->report;
    /* One failed tool must not stop observation of other pending tools. The
     * broker skips failed submissions itself; close never retries them. */
    if (report->phase == RYZ_WB_TOOLS_FAILED && !report->pending_mask) return;
    const bool previously_failed = report->phase == RYZ_WB_TOOLS_FAILED;
    const esp_err_t previous_error = report->error;
    ryz_tools_close_result_t result;
    esp_err_t error = ryz_tools_close(report->session_id, &result);
    report->error = error;
    report->observed_us = now;
    if (now >= report->retired_us && now - report->retired_us >= RYZ_WORKBENCH_TOOLS_OBSERVATION_US)
        report->timed_out = true;
    /* A zero result on gate contention is NOT a release acknowledgement. */
    if (error == ESP_ERR_NOT_FINISHED && !result.session_id) {
        report->phase = previously_failed ? RYZ_WB_TOOLS_FAILED : RYZ_WB_TOOLS_PENDING;
        if (previously_failed) report->error = previous_error;
        return;
    }
    if (result.session_id != report->session_id) {
        report->phase = RYZ_WB_TOOLS_FAILED;
        if (error == ESP_OK) report->error = ESP_ERR_INVALID_STATE;
        return;
    }
    report->observation_valid = true;
    report->remaining_mask = result.remaining_mask;
    report->pending_mask = result.pending_mask;
    report->failed_mask = result.failed_mask;
    memcpy(report->errors, result.errors, sizeof(report->errors));
    report->phase = error == ESP_OK ? RYZ_WB_TOOLS_CLEAN :
        error == ESP_ERR_NOT_FINISHED ? RYZ_WB_TOOLS_PENDING : RYZ_WB_TOOLS_FAILED;
}
bool ryz_workbench_tools_finish(ryz_workbench_tools_job_t *job, int64_t now)
{
    if (!job) return false;
    if (job->finished) return true;
    if (!job->session_id) { job->finished = true; return true; }
    if (!enter()) return false;
    record_t *record = find_session(job->session_id);
    if (record) {
        record->retired = true;
        record->report.phase = RYZ_WB_TOOLS_PENDING;
        record->report.retired_us = now;
        record->report.retirement_recorded = true;
        progress(record, now);
        job->final = record->report;
        if (record->report.phase == RYZ_WB_TOOLS_CLEAN) memset(record, 0, sizeof(*record));
    } else {
        /* Impossible under the reservation contract; never fabricate clean. */
        job->final = (ryz_workbench_tools_report_t){.session_id = job->session_id,
            .phase = RYZ_WB_TOOLS_FAILED, .error = ESP_ERR_INVALID_STATE};
    }
    job->finished = true;
    leave();
    return true;
}
void ryz_workbench_tools_tick(int64_t now)
{
    if (now < 0 || !enter()) return;
    for (unsigned i = 0; i < RYZ_WORKBENCH_TOOLS_RETIRED_MAX; ++i) {
        record_t *record = &s_records[i];
        if (!record->report.session_id || !record->retired) continue;
        progress(record, now);
        if (record->report.phase == RYZ_WB_TOOLS_CLEAN) memset(record, 0, sizeof(*record));
    }
    leave();
}
esp_err_t ryz_workbench_tools_report(const char *id,
    const ryz_workbench_tools_report_t *final, ryz_workbench_tools_report_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    const ryz_workbench_tools_report_t completed = final ? *final : (ryz_workbench_tools_report_t){0};
    memset(out, 0, sizeof(*out));
    if (!id) return ESP_ERR_INVALID_ARG;
    /* Queue-handed-off immutable success needs no shared state. In particular
     * unrelated tool admission cannot fail a job which never used tools. */
    if (final && (completed.phase == RYZ_WB_TOOLS_UNUSED || completed.phase == RYZ_WB_TOOLS_CLEAN ||
        (completed.phase == RYZ_WB_TOOLS_FAILED && !completed.retirement_recorded))) {
        *out = completed;
        return ESP_OK;
    }
    if (!enter()) return ESP_ERR_NOT_FINISHED;
    bool found = false;
    for (unsigned i = 0; i < RYZ_WORKBENCH_TOOLS_RETIRED_MAX; ++i) {
        const record_t *record = &s_records[i];
        if (record->report.session_id && !strcmp(record->job_id, id)) {
            *out = record->report; found = true; break;
        }
    }
    if (!found && final) {
        *out = completed;
        if (completed.retirement_recorded && completed.session_id &&
            (completed.phase == RYZ_WB_TOOLS_PENDING || completed.phase == RYZ_WB_TOOLS_FAILED)) {
            out->phase = RYZ_WB_TOOLS_CLEAN;
            out->observation_valid = true;
            out->remaining_mask = out->pending_mask = out->failed_mask = 0;
            out->error = ESP_OK;
            memset(out->errors, 0, sizeof(out->errors));
        }
    }
    leave();
    return ESP_OK;
}
bool ryz_workbench_tools_clean(const ryz_workbench_tools_report_t *report)
{ return report && (report->phase == RYZ_WB_TOOLS_UNUSED || report->phase == RYZ_WB_TOOLS_CLEAN); }
static const char *phase_name(ryz_workbench_tools_phase_t phase)
{
    switch (phase) {
    case RYZ_WB_TOOLS_UNUSED: return "unused";
    case RYZ_WB_TOOLS_ACTIVE: return "active";
    case RYZ_WB_TOOLS_PENDING: return "pending";
    case RYZ_WB_TOOLS_FAILED: return "failed";
    case RYZ_WB_TOOLS_CLEAN: return "clean";
    default: return NULL;
    }
}
cJSON *ryz_workbench_tools_report_json(const ryz_workbench_tools_report_t *report)
{
    if (!report) return NULL;
    const char *phase = phase_name(report->phase);
    if (!phase) return NULL;
    cJSON *out = cJSON_CreateObject();
    cJSON *errors = NULL;
    if (!out || !cJSON_AddNumberToObject(out, "session_id", report->session_id) ||
        !cJSON_AddStringToObject(out, "state", phase) ||
        !cJSON_AddBoolToObject(out, "observation_valid", report->observation_valid) ||
        !cJSON_AddBoolToObject(out, "timed_out", report->timed_out) ||
        !cJSON_AddBoolToObject(out, "retirement_recorded", report->retirement_recorded) ||
        !cJSON_AddNumberToObject(out, "remaining_mask", report->remaining_mask) ||
        !cJSON_AddNumberToObject(out, "pending_mask", report->pending_mask) ||
        !cJSON_AddNumberToObject(out, "failed_mask", report->failed_mask) ||
        !cJSON_AddNumberToObject(out, "error_code", report->error) ||
        !(errors = cJSON_AddArrayToObject(out, "errors"))) goto fail;
    for (unsigned i = 0; i < RYZ_TOOL_COUNT; ++i) {
        cJSON *value = cJSON_CreateNumber(report->errors[i]);
        if (!value) goto fail;
        if (!cJSON_AddItemToArray(errors, value)) { cJSON_Delete(value); goto fail; }
    }
    return out;
fail:
    cJSON_Delete(out); return NULL;
}
static const char *text_field(const cJSON *request, const char *name)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(request, name);
    return cJSON_IsString(value) ? value->valuestring : NULL;
}
static cJSON *response(const char *id, const char *boot, esp_err_t error)
{
    cJSON *out = cJSON_CreateObject();
    if (!out || !cJSON_AddStringToObject(out, "id", id ? id : "") ||
        !cJSON_AddStringToObject(out, "boot_id", boot ? boot : "") ||
        !cJSON_AddBoolToObject(out, "ok", error == ESP_OK) ||
        !cJSON_AddNumberToObject(out, "error_code", error)) { cJSON_Delete(out); return NULL; }
    return out;
}
static bool fields_valid(const cJSON *request, bool retry)
{
    for (const cJSON *a = request->child; a; a = a->next) {
        if (!a->string || (strcmp(a->string, "id") && strcmp(a->string, "op") &&
            strcmp(a->string, "action") && strcmp(a->string, "boot_id") &&
            !(retry && !strcmp(a->string, "session_id")))) return false;
        for (const cJSON *b = a->next; b; b = b->next)
            if (b->string && !strcmp(a->string, b->string)) return false;
    }
    return true;
}
cJSON *ryz_workbench_tools_rpc(const cJSON *request, const char *boot)
{
    const char *id = text_field(request, "id"), *op = text_field(request, "op");
    const char *action = text_field(request, "action"), *requested_boot = text_field(request, "boot_id");
    bool retry = action && !strcmp(action, "retry_close");
    if (!cJSON_IsObject(request) || !id || strlen(id) > 64 || !boot || !*boot ||
        !op || strcmp(op, "tools") || !action || (!retry && strcmp(action, "status")) ||
        !fields_valid(request, retry)) return response(id, boot, ESP_ERR_INVALID_ARG);
    if ((cJSON_HasObjectItem(request, "boot_id") && (!requested_boot || strcmp(requested_boot, boot))) ||
        (retry && !requested_boot)) return response(id, boot, ESP_ERR_INVALID_STATE);
    if (!retry) {
        record_t records[RYZ_WORKBENCH_TOOLS_RETIRED_MAX];
        if (!enter()) return response(id, boot, ESP_ERR_NOT_FINISHED);
        memcpy(records, s_records, sizeof(records));
        leave();
        cJSON *out = response(id, boot, ESP_OK);
        cJSON *array = out ? cJSON_AddArrayToObject(out, "sessions") : NULL;
        if (!array) { cJSON_Delete(out); return NULL; }
        for (unsigned i = 0; i < RYZ_WORKBENCH_TOOLS_RETIRED_MAX; ++i) {
            if (!records[i].report.session_id) continue;
            cJSON *entry = ryz_workbench_tools_report_json(&records[i].report);
            if (!entry || !cJSON_AddStringToObject(entry, "job_id", records[i].job_id) ||
                !cJSON_AddBoolToObject(entry, "retired", records[i].retired)) {
                cJSON_Delete(entry); cJSON_Delete(out); return NULL;
            }
            if (!cJSON_AddItemToArray(array, entry)) { cJSON_Delete(entry); cJSON_Delete(out); return NULL; }
        }
        return out;
    }
    const cJSON *token = cJSON_GetObjectItemCaseSensitive(request, "session_id");
    if (!cJSON_IsNumber(token) || !isfinite(token->valuedouble) || token->valuedouble < 1 ||
        token->valuedouble > UINT32_MAX || token->valuedouble != (double)(uint32_t)token->valuedouble)
        return response(id, boot, ESP_ERR_INVALID_ARG);
    const uint32_t session = (uint32_t)token->valuedouble;
    cJSON *ack = response(id, boot, ESP_OK);
    if (!ack || !cJSON_AddBoolToObject(ack, "accepted", true) ||
        !cJSON_AddNumberToObject(ack, "session_id", session)) { cJSON_Delete(ack); return NULL; }
    esp_err_t error = ESP_ERR_NOT_FINISHED;
    if (enter()) {
        record_t *record = find_session(session);
        error = !record || !record->retired ? ESP_ERR_INVALID_STATE : ryz_tools_retry_close(session);
        if (error == ESP_OK) {
            record->report.phase = RYZ_WB_TOOLS_PENDING;
            record->report.error = ESP_ERR_NOT_FINISHED;
        }
        leave();
    }
    if (error == ESP_OK) return ack;
    cJSON_Delete(ack);
    return response(id, boot, error);
}
