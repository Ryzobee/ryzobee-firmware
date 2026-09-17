#include "ryz_apps_reader.h"

#include <string.h>

static esp_err_t check_revision(uint32_t expected_revision)
{
    ryz_script_store_status_t status = {0};
    const esp_err_t error = ryz_script_store_status(&status);
    if (error != ESP_OK) return error;
    if (!status.ready || status.recovery_required ||
        status.revision != expected_revision) return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

static bool source_error_kind(esp_err_t error)
{
    return error == ESP_ERR_INVALID_ARG || error == ESP_ERR_INVALID_SIZE;
}

static bool same_file(const ryz_script_store_description_t *first,
                       const ryz_script_store_description_t *last)
{
    return first->entry.bytes == last->entry.bytes &&
        first->entry.protected_file == last->entry.protected_file &&
        strcmp(first->entry.name, last->entry.name) == 0 &&
        strcmp(first->sha256, last->sha256) == 0;
}

static esp_err_t read_invalid_source(const char *name, esp_err_t source_error,
                                     ryz_apps_detail_t *detail)
{
    ryz_script_store_description_t first = {0};
    esp_err_t error = ryz_script_store_describe(name, &first);
    if (error != ESP_OK) return error;
    const bool valid_size = first.entry.bytes > 0 &&
        first.entry.bytes <= RYZ_SCRIPT_STORE_SOURCE_MAX;
    if ((source_error == ESP_ERR_INVALID_SIZE && valid_size) ||
        (source_error == ESP_ERR_INVALID_ARG && !valid_size))
        return ESP_ERR_INVALID_STATE;

    /* A failed get has no identity. Bracket a second, identically failing get
     * with raw identities, so a different file cannot inherit that failure. */
    ryz_script_store_snapshot_t snapshot = {0};
    error = ryz_script_store_get(name, &snapshot);
    ryz_script_store_snapshot_free(&snapshot);
    if (error != source_error) {
        return error == ESP_OK || source_error_kind(error)
            ? ESP_ERR_INVALID_STATE : error;
    }

    ryz_script_store_description_t last = {0};
    error = ryz_script_store_describe(name, &last);
    if (error != ESP_OK) return error;
    if (!same_file(&first, &last)) return ESP_ERR_INVALID_STATE;
    detail->file = first;
    detail->source_error = source_error;
    return ESP_OK;
}

esp_err_t ryz_apps_read_detail(const char *name, uint32_t expected_revision,
                               ryz_apps_detail_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    char selected[RYZ_SCRIPT_STORE_NAME_MAX + 1U];
    bool valid_name = ryz_script_store_valid_name(name);
    if (valid_name) {
        const size_t length = strnlen(name, sizeof(selected));
        if (length >= sizeof(selected)) valid_name = false;
        else memcpy(selected, name, length + 1U);
    }
    /* Copy the bounded name before clearing a possibly aliased old result. */
    memset(out, 0, sizeof(*out));
    if (!valid_name || !expected_revision) return ESP_ERR_INVALID_ARG;
    esp_err_t error = check_revision(expected_revision);
    if (error != ESP_OK) return error;

    ryz_apps_detail_t detail = {0};
    ryz_script_store_snapshot_t snapshot = {0};
    error = ryz_script_store_get(selected, &snapshot);
    if (error == ESP_OK) {
        detail.file.entry = snapshot.entry;
        detail.file.times = snapshot.times;
        memcpy(detail.file.sha256, snapshot.sha256, sizeof(detail.file.sha256));
        error = ryz_script_metadata_parse(snapshot.source, snapshot.entry.bytes,
                                           &detail.metadata);
        if (error == ESP_OK) detail.source_valid = true;
        ryz_script_store_snapshot_free(&snapshot);
    } else {
        ryz_script_store_snapshot_free(&snapshot);
        if (source_error_kind(error))
            error = read_invalid_source(selected, error, &detail);
    }
    if (error != ESP_OK) return error;
    error = check_revision(expected_revision);
    if (error != ESP_OK) return error;
    detail.revision = expected_revision;
    *out = detail;
    return ESP_OK;
}
