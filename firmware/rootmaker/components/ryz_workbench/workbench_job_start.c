#include "workbench_job_start.h"
#include "ryz_script_store.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

char *ryz_workbench_source_copy(const char *source, size_t length)
{
    if (!source || !length || length > RYZ_LUA_SOURCE_MAX) return NULL;
#ifdef ESP_PLATFORM
    char *copy = heap_caps_malloc(length + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    char *copy = malloc(length + 1U);
#endif
    if (!copy) return NULL;
    memcpy(copy, source, length);
    copy[length] = '\0';
    return copy;
}

static script_job_payload_t *allocate_payload(void)
{
#ifdef ESP_PLATFORM
    return heap_caps_calloc(
        1, sizeof(script_job_payload_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return calloc(1, sizeof(script_job_payload_t));
#endif
}

static void free_payload(script_job_payload_t *payload)
{
#ifdef ESP_PLATFORM
    heap_caps_free(payload);
#else
    free(payload);
#endif
}

static cJSON *rejected(const char *id, const char *message)
{
    cJSON *reply = cJSON_CreateObject();
    if (!reply || !cJSON_AddStringToObject(reply, "id", id ? id : "") ||
        !cJSON_AddBoolToObject(reply, "ok", false) ||
        !cJSON_AddStringToObject(reply, "error", message)) {
        cJSON_Delete(reply);
        return NULL;
    }
    return reply;
}

void ryz_workbench_job_destroy(script_job_t *job)
{
    if (!job) return;
    free(job->source);
    free(job->reply_id);
    free_payload(job->payload);
    free(job);
}

static cJSON *started_reply(const char *id, const script_job_t *job)
{
    cJSON *reply = cJSON_CreateObject();
    cJSON *value = reply ? cJSON_AddObjectToObject(reply, "job") : NULL;
    if (!value || !cJSON_AddStringToObject(reply, "id", id) ||
        !cJSON_AddBoolToObject(reply, "ok", true) ||
        !cJSON_AddStringToObject(reply, "output", "Lua job started") ||
        !cJSON_AddStringToObject(value, "job_id", job->id) ||
        !cJSON_AddStringToObject(value, "name", job->name) ||
        !cJSON_AddStringToObject(value, "sha256", job->sha256) ||
        !cJSON_AddStringToObject(value, "state", "running") ||
        !cJSON_AddBoolToObject(value, "stop_requested", false) ||
        !cJSON_AddNumberToObject(value, "elapsed_ms", 0) ||
        !cJSON_AddNumberToObject(value, "log_next_seq", 0) ||
        !cJSON_AddNumberToObject(value, "dropped_bytes", 0)) {
        cJSON_Delete(reply);
        return NULL;
    }
    return reply;
}

cJSON *ryz_workbench_job_start(const ryz_workbench_job_start_context_t *context,
                              const char *id, const char *name, char *source,
                              uint32_t timeout_ms, bool legacy)
{
    if (!context || !context->lock || !context->unlock || !context->ready ||
        !context->enqueue || !context->notify || !context->now_us ||
        !context->next_job || !context->current || !context->boot_id ||
        !context->boot_id[0] || !id || !name || !name[0] || strlen(name) > 40 ||
        !source) {
        free(source);
        return rejected(id, "script missing or invalid runtime request");
    }
    char sha256[65];
    const size_t length = strnlen(source, RYZ_LUA_SOURCE_MAX + 1U);
    if (ryz_script_store_source_sha256(source, length, sha256) != ESP_OK) {
        free(source);
        return rejected(id, "source validation or checksum failed");
    }
    script_job_t *job = calloc(1, sizeof(*job));
    if (!job) { free(source); return rejected(id, "out of memory"); }
    job->source = source;
    job->payload = allocate_payload();
    job->reply_id = strdup(id);
    if (!job->payload || !job->reply_id) {
        ryz_workbench_job_destroy(job);
        return rejected(id, "out of memory");
    }
    strcpy(job->name, name);
    strcpy(job->sha256, sha256);
    job->timeout_ms = timeout_ms;
    job->legacy = legacy;
    job->active = true;
    atomic_init(&job->cancel, false);
    job->started_us = context->now_us(context->context);

    context->lock(context->context);
    const char *error = *context->current ? "busy: runtime owner has an active job" :
        !context->ready(context->context) ? "system runtime unavailable; retry shortly" :
        *context->next_job == UINT_MAX ? "job identity exhausted; restart required" : NULL;
    unsigned next = *context->next_job + 1U;
    int id_length = error ? 0 : snprintf(job->id, sizeof(job->id), "%s-%u", context->boot_id, next);
    if (!error && (id_length <= 0 || (size_t)id_length >= sizeof(job->id)))
        error = "job identity exceeds runtime limit";
    if (error) {
        context->unlock(context->context);
        ryz_workbench_job_destroy(job);
        return rejected(id, error);
    }

    /* Never publish a job then manufacture an incomplete ok:true ACK. The
     * serial transport can still lose this complete ACK after publication. */
    cJSON *reply = legacy ? NULL : started_reply(id, job);
    if (!legacy && !reply) {
        context->unlock(context->context);
        ryz_workbench_job_destroy(job);
        return NULL;
    }
    *context->current = job;
    if (!context->enqueue(context->context, job)) {
        *context->current = NULL;
        context->unlock(context->context);
        cJSON_Delete(reply);
        ryz_workbench_job_destroy(job);
        return rejected(id, "runtime dispatch queue is busy");
    }
    *context->next_job = next;
    context->unlock(context->context);
    context->notify(context->context);
    return reply;
}
