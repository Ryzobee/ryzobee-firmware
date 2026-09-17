#include "workbench_file_rpc.h"

#include <math.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "ryz_script_store.h"
#include "ryz_script_metadata.h"

#define STORE_SCHEMA "ryz-script-store/1"

static const char *string_field(const cJSON *request, const char *key)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(request, key);
    return cJSON_IsString(value) ? value->valuestring : NULL;
}

static cJSON *reply(const char *id, bool ok, const char *error)
{
    cJSON *value = cJSON_CreateObject();
    if (!value) return NULL;
    if (!cJSON_AddStringToObject(value, "id", id ? id : "") ||
        !cJSON_AddBoolToObject(value, "ok", ok) ||
        (error && !cJSON_AddStringToObject(value, "error", error))) {
        cJSON_Delete(value);
        return NULL;
    }
    return value;
}

static const char *store_error(esp_err_t error)
{
    switch (error) {
    case ESP_ERR_INVALID_ARG: return "invalid script request";
    case ESP_ERR_INVALID_SIZE: return "script or catalog exceeds storage limits";
    case ESP_ERR_NOT_FOUND: return "script not found";
    case ESP_ERR_NOT_SUPPORTED: return "reserved script name or unsupported file type";
    case ESP_ERR_NO_MEM: return "insufficient memory or storage capacity";
    case ESP_ERR_TIMEOUT: return "script store is busy; retry after the operation finishes";
    case ESP_ERR_INVALID_STATE: return "script changed or storage recovery is required";
    default: return "script storage operation failed";
    }
}

bool ryz_workbench_file_rpc_supports(const char *operation)
{
    return operation && (!strcmp(operation, "list") ||
        !strcmp(operation, "get") || !strcmp(operation, "put") ||
        !strcmp(operation, "remove") || !strcmp(operation, "scripts"));
}

static bool add_entry(cJSON *array, const ryz_script_store_entry_t *entry)
{
    cJSON *file = cJSON_CreateObject();
    if (!file) return false;
    if (!cJSON_AddStringToObject(file, "name", entry->name) ||
        !cJSON_AddNumberToObject(file, "bytes", (double)entry->bytes) ||
        !cJSON_AddBoolToObject(file, "protected", entry->protected_file) ||
        !cJSON_AddItemToArray(array, file)) {
        cJSON_Delete(file);
        return false;
    }
    return true;
}

static bool number_field(const cJSON *request, const char *key,
                         uint32_t fallback, uint32_t maximum, uint32_t *out)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(request, key);
    if (!value) { *out = fallback; return true; }
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) ||
        value->valuedouble < 0 || value->valuedouble > maximum ||
        floor(value->valuedouble) != value->valuedouble) return false;
    *out = (uint32_t)value->valuedouble;
    return true;
}

static bool add_version(cJSON *value, const char *boot_id)
{
    return cJSON_AddStringToObject(value, "schema", STORE_SCHEMA) &&
           cJSON_AddStringToObject(value, "boot_id", boot_id);
}

static cJSON *storage_status(const char *id, const char *boot_id)
{
    ryz_script_store_status_t status;
    esp_err_t error = ryz_script_store_status(&status);
    if (error != ESP_OK) return reply(id, false, store_error(error));
    cJSON *value = reply(id, true, NULL);
    if (!value || !add_version(value, boot_id) ||
        !cJSON_AddBoolToObject(value, "ready", status.ready) ||
        !cJSON_AddBoolToObject(value, "recovery_required", status.recovery_required) ||
        !cJSON_AddBoolToObject(value, "legacy_backup_present", status.legacy_backup_present) ||
        !cJSON_AddNumberToObject(value, "recovery_error", status.recovery_error) ||
        !cJSON_AddNumberToObject(value, "revision", status.revision) ||
        !cJSON_AddBoolToObject(value, "capacity_valid", status.capacity_valid) ||
        !cJSON_AddNumberToObject(value, "capacity_error", status.capacity_error) ||
        !(status.capacity_valid
            ? cJSON_AddNumberToObject(value, "total_bytes", (double)status.total_bytes)
            : cJSON_AddNullToObject(value, "total_bytes")) ||
        !(status.capacity_valid
            ? cJSON_AddNumberToObject(value, "used_bytes", (double)status.used_bytes)
            : cJSON_AddNullToObject(value, "used_bytes"))) {
        cJSON_Delete(value);
        return NULL;
    }
    return value;
}

static cJSON *catalog(const cJSON *request, const char *id,
                      const char *boot_id, bool versioned)
{
    uint32_t offset = 0, limit = RYZ_SCRIPT_STORE_PAGE_MAX, revision = 0;
    if (versioned &&
        (!number_field(request, "offset", 0, RYZ_SCRIPT_STORE_INDEX_MAX, &offset) ||
         !number_field(request, "limit", RYZ_SCRIPT_STORE_PAGE_MAX,
                       RYZ_SCRIPT_STORE_PAGE_MAX, &limit) || !limit ||
         !number_field(request, "revision", 0, UINT32_MAX, &revision) ||
         (offset && !revision))) {
        return reply(id, false, "invalid catalog page; later pages require revision");
    }
    cJSON *value = reply(id, true, NULL);
    cJSON *files = value ? cJSON_AddArrayToObject(value, "files") : NULL;
    if (!files) { cJSON_Delete(value); return NULL; }
    ryz_script_store_page_t page;
    do {
        esp_err_t error = ryz_script_store_list(offset, limit, revision, &page);
        if (error != ESP_OK) {
            cJSON_Delete(value);
            return reply(id, false, store_error(error));
        }
        for (size_t index = 0; index < page.count; ++index) {
            if (!add_entry(files, &page.entries[index])) {
                cJSON_Delete(value);
                return NULL;
            }
        }
        revision = page.revision;
        offset += (uint32_t)page.count;
    } while (!versioned && offset < page.total && page.count);

    if (versioned && (!add_version(value, boot_id) ||
        !cJSON_AddNumberToObject(value, "revision", page.revision) ||
        !cJSON_AddNumberToObject(value, "offset", (double)page.offset) ||
        !cJSON_AddNumberToObject(value, "total", (double)page.total) ||
        !cJSON_AddNumberToObject(value, "count", (double)page.count))) {
        cJSON_Delete(value);
        return NULL;
    }
    return value;
}

static cJSON *read_file(const char *id, const char *name)
{
    ryz_script_store_snapshot_t snapshot;
    esp_err_t error = ryz_script_store_get(name, &snapshot);
    if (error != ESP_OK) return reply(id, false, store_error(error));
    cJSON *value = reply(id, true, NULL);
    bool ok = value && cJSON_AddStringToObject(value, "name", snapshot.entry.name) &&
        cJSON_AddNumberToObject(value, "bytes", (double)snapshot.entry.bytes) &&
        cJSON_AddStringToObject(value, "sha256", snapshot.sha256) &&
        cJSON_AddStringToObject(value, "source", snapshot.source);
    ryz_script_store_snapshot_free(&snapshot);
    if (!ok) { cJSON_Delete(value); return NULL; }
    return value;
}

static bool timestamp_field(cJSON *value, const char *key, int64_t seconds)
{
    char text[21];
    struct tm utc;
    const time_t epoch = (time_t)seconds;
    if (seconds >= INT64_C(1704067200) && seconds <= INT64_C(253402300799) &&
        (int64_t)epoch == seconds && gmtime_r(&epoch, &utc) != NULL &&
        strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc) == 20)
        return cJSON_AddStringToObject(value, key, text) != NULL;
    return cJSON_AddNullToObject(value, key) != NULL;
}

static cJSON *describe_file(const char *id, const char *name, const char *boot_id)
{
    /* Inspect raw identity even when the old file is empty, contains NUL or
     * exceeds the execution limit, so CAS repair/removal remains possible. */
    ryz_script_store_description_t description;
    esp_err_t error = ryz_script_store_describe(name, &description);
    if (error != ESP_OK) return reply(id, false, store_error(error));
    cJSON *value = reply(id, true, NULL);
    /* Store-journaled UTC only, never raw SPIFFS timestamps. */
    if (!value || !add_version(value, boot_id) ||
        !cJSON_AddStringToObject(value, "name", description.entry.name) ||
        !cJSON_AddNumberToObject(value, "bytes", (double)description.entry.bytes) ||
        !cJSON_AddStringToObject(value, "sha256", description.sha256) ||
        !cJSON_AddBoolToObject(value, "protected", description.entry.protected_file) ||
        !timestamp_field(value, "created_at", description.times.created_unix) ||
        !timestamp_field(value, "modified_at", description.times.modified_unix)) {
        cJSON_Delete(value);
        return NULL;
    }
    return value;
}

static bool valid_sha(const char *value, bool allow_absent)
{
    if (!value) return false;
    if (!value[0]) return allow_absent;
    if (strlen(value) != 64) return false;
    for (size_t i = 0; i < 64; ++i) {
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f'))) return false;
    }
    return true;
}

static bool metadata_field(cJSON *object, const char *name, const char *text,
                            bool present, bool truncated, bool invalid)
{
    cJSON *value = cJSON_AddObjectToObject(object, name);
    return value && (present ? cJSON_AddStringToObject(value, "value", text)
                             : cJSON_AddNullToObject(value, "value")) &&
        cJSON_AddBoolToObject(value, "present", present) &&
        cJSON_AddBoolToObject(value, "truncated", truncated) &&
        cJSON_AddBoolToObject(value, "invalid", invalid);
}

static cJSON *inspect_file(const char *id, const char *name, const char *boot_id)
{
    ryz_script_store_snapshot_t snapshot;
    esp_err_t error = ryz_script_store_get(name, &snapshot);
    if (error != ESP_OK) return reply(id, false, store_error(error));
    ryz_script_metadata_t metadata;
    error = ryz_script_metadata_parse(snapshot.source, snapshot.entry.bytes, &metadata);
    cJSON *value = error == ESP_OK ? reply(id, true, NULL) : NULL;
    cJSON *fields = value ? cJSON_AddObjectToObject(value, "metadata") : NULL;
    bool ok = fields && add_version(value, boot_id) &&
        cJSON_AddStringToObject(value, "name", snapshot.entry.name) &&
        cJSON_AddNumberToObject(value, "bytes", (double)snapshot.entry.bytes) &&
        cJSON_AddStringToObject(value, "sha256", snapshot.sha256) &&
        cJSON_AddBoolToObject(value, "protected", snapshot.entry.protected_file) &&
        timestamp_field(value, "created_at", snapshot.times.created_unix) &&
        timestamp_field(value, "modified_at", snapshot.times.modified_unix) &&
        metadata_field(fields, "author", metadata.author, metadata.author_present,
                       metadata.author_truncated, metadata.author_invalid) &&
        metadata_field(fields, "version", metadata.version, metadata.version_present,
                       metadata.version_truncated, metadata.version_invalid) &&
        metadata_field(fields, "description", metadata.description, metadata.description_present,
                       metadata.description_truncated, metadata.description_invalid);
    ryz_script_store_snapshot_free(&snapshot);
    if (!ok) { cJSON_Delete(value); return error == ESP_OK ? NULL : reply(id, false, store_error(error)); }
    return value;
}

static cJSON *run_file(const cJSON *request, const char *id, const char *name,
                       const char *boot_id, ryz_workbench_run_script_fn run,
                       void *context)
{
    const char *sha256 = string_field(request, "sha256");
    if (!valid_sha(sha256, false)) return reply(id, false, "selected source SHA-256 is required");
    if (!run) return reply(id, false, "version-bound script execution is unavailable");
    ryz_script_store_status_t status;
    esp_err_t error = ryz_script_store_status(&status);
    if (error != ESP_OK || !status.ready || status.recovery_required)
        return reply(id, false, "script storage unavailable or awaiting recovery");
    ryz_script_store_snapshot_t snapshot;
    error = ryz_script_store_get(name, &snapshot);
    if (error != ESP_OK) return reply(id, false, store_error(error));
    error = ryz_script_store_status(&status);
    if (error != ESP_OK || !status.ready || status.recovery_required ||
        strcmp(sha256, snapshot.sha256)) {
        ryz_script_store_snapshot_free(&snapshot);
        return reply(id, false, error != ESP_OK || !status.ready || status.recovery_required
            ? "script storage unavailable or awaiting recovery"
            : "selected script changed; refresh before running");
    }
    /* This is the selected source, not a promise that its path will still
     * contain the same bytes later. The runtime consumes this exact snapshot. */
    cJSON *value = run(context, id, &snapshot);
    ryz_script_store_snapshot_free(&snapshot);
    if (value && (!cJSON_IsObject(value) || !add_version(value, boot_id))) {
        /* The callback may already have published a job. Never turn allocation
         * failure here into a definite rejection or free its runtime source. */
        cJSON_Delete(value);
        return NULL;
    }
    return value;
}

static cJSON *mutate(const cJSON *request, const char *id, const char *name,
                     const char *operation, const char *boot_id, bool versioned,
                     const ryz_workbench_write_guard_t *guard)
{
    const char *previous = NULL;
    if (versioned) {
        previous = string_field(request, "previous_sha256");
        if (!valid_sha(previous, true))
            return reply(id, false, "previous_sha256 must be empty or 64 lowercase hex digits");
    }
    const bool putting = !strcmp(operation, "put");
    const char *source = NULL;
    size_t length = 0;
    if (putting) {
        source = string_field(request, "source");
        if (!source || !(length = strlen(source)) || length > RYZ_SCRIPT_STORE_SOURCE_MAX)
            return reply(id, false, "source must be 1..16384 bytes");
        const cJSON *hash_field = cJSON_GetObjectItemCaseSensitive(request, "sha256");
        const char *expected = string_field(request, "sha256");
        if ((hash_field || versioned) && !valid_sha(expected, false))
            return reply(id, false, "invalid source checksum");
        if (expected) {
            char actual[65];
            esp_err_t error = ryz_script_store_source_sha256(source, length, actual);
            if (error != ESP_OK) return reply(id, false, store_error(error));
            if (strcmp(actual, expected)) return reply(id, false, "source checksum mismatch");
        }
    }

    /* Allocate the basic envelope before touching files. Any later allocation
     * failure returns no reply, so the transport reports an unknown outcome;
     * it must never manufacture a definite uncommitted error after commit. */
    cJSON *value = reply(id, true, NULL);
    if (!value) return NULL;
    const char *admission_error = NULL;
    if (!guard || !guard->try_begin_write || !guard->end_write)
        admission_error = "file write admission unavailable";
    else if (!guard->try_begin_write(guard->context))
        admission_error = "busy: file write admission rejected";
    if (admission_error) {
        cJSON_SetBoolValue(cJSON_GetObjectItemCaseSensitive(value, "ok"), false);
        if (!cJSON_AddStringToObject(value, "error", admission_error)) {
            cJSON_Delete(value);
            return NULL;
        }
        return value;
    }

    /* Only the logical writer reservation spans Store I/O. The callbacks
     * must not retain the Job mutex, and result allocation happens after end. */
    ryz_script_store_mutation_t result = {0};
    esp_err_t error = putting
        ? ryz_script_store_put(name, source, length, previous, &result)
        : ryz_script_store_remove(name, previous, &result);
    guard->end_write(guard->context);
    const bool committed = result.commit == RYZ_SCRIPT_STORE_COMMITTED;
    const bool unknown = result.commit == RYZ_SCRIPT_STORE_COMMIT_UNKNOWN ||
                         (error == ESP_OK && !committed);
    cJSON_SetBoolValue(cJSON_GetObjectItemCaseSensitive(value, "ok"), committed);
    bool ok = true;
    if (!committed)
        ok = cJSON_AddStringToObject(value, "error", unknown
            ? "storage outcome unknown; inspect and recover before retrying"
            : store_error(error)) != NULL;
    if (ok && putting && committed)
        ok = cJSON_AddNumberToObject(value, "bytes", (double)result.bytes) &&
             cJSON_AddStringToObject(value, "sha256", result.sha256);
    if (ok && (versioned || unknown || result.recovery_required ||
               result.cleanup_error != ESP_OK)) {
        ok = cJSON_AddStringToObject(value, "store_commit", committed ? "committed" :
                                    unknown ? "unknown" : "not_committed") &&
             cJSON_AddBoolToObject(value, "store_recovery_required", result.recovery_required) &&
             cJSON_AddNumberToObject(value, "store_cleanup_error", result.cleanup_error);
    }
    if (ok && versioned)
        ok = add_version(value, boot_id) &&
             cJSON_AddNumberToObject(value, "revision", result.revision);
    if (!ok) { cJSON_Delete(value); return NULL; }
    return value;
}

cJSON *ryz_workbench_file_rpc(const cJSON *request, const char *boot_id,
                             bool runtime_busy, ryz_workbench_run_script_fn run,
                             void *run_context, const ryz_workbench_write_guard_t *guard)
{
    const char *id = string_field(request, "id");
    const char *operation = string_field(request, "op");
    if (!cJSON_IsObject(request) || !ryz_workbench_file_rpc_supports(operation))
        return reply(id, false, "invalid file operation");
    if (runtime_busy) return reply(id, false, "busy: runtime owner has an active job");
    const bool versioned = !strcmp(operation, "scripts");
    if (versioned) {
        const char *schema = string_field(request, "schema");
        const char *expected_boot = string_field(request, "boot_id");
        if (!schema || strcmp(schema, STORE_SCHEMA) || !boot_id || !boot_id[0] ||
            !expected_boot || strcmp(expected_boot, boot_id))
            return reply(id, false, "script store schema or boot identity mismatch");
        operation = string_field(request, "action");
        if (!operation || (strcmp(operation, "status") && strcmp(operation, "catalog") && strcmp(operation, "describe") &&
                           strcmp(operation, "inspect") && strcmp(operation, "run") &&
                           strcmp(operation, "put") && strcmp(operation, "remove")))
            return reply(id, false, "scripts action must be status, catalog, describe, inspect, run, put or remove");
    }
    if (!strcmp(operation, "status")) return storage_status(id, boot_id);
    if (!strcmp(operation, "list") || !strcmp(operation, "catalog"))
        return catalog(request, id, boot_id, versioned);
    const char *name = string_field(request, "name");
    if (!ryz_script_store_valid_name(name)) return reply(id, false, "invalid script name");
    if (!strcmp(operation, "get")) return read_file(id, name);
    if (!strcmp(operation, "describe")) return describe_file(id, name, boot_id);
    if (!strcmp(operation, "inspect")) return inspect_file(id, name, boot_id);
    if (!strcmp(operation, "run")) return run_file(request, id, name, boot_id, run, run_context);
    return mutate(request, id, name, operation, boot_id, versioned, guard);
}
