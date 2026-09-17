#include "ryz_script_store.h"
#include "ryz_script_store_platform.h"

#include <stdatomic.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROOT_MAX 192U
#define PATH_MAX_BYTES (ROOT_MAX + RYZ_SCRIPT_STORE_NAME_MAX + 2U)
#define JOURNAL_NAME ".ryz-txn"
#define STAGE_NAME ".ryz-new"
#define BACKUP_NAME ".ryz-old"
#define TIME_STAGE_NAME ".ryz-time-new"
#define TIME_MIN INT64_C(1704067200)
#define TIME_MAX INT64_C(253402300799)
#define CAPACITY_RESERVE 4096U

/* Only byte fields: stable on both 32-bit target and 64-bit Host. The digest
 * detects torn/corrupted records; it is not an authentication signature. */
typedef struct {
    char magic[8];
    char operation;
    char had_target;
    char target[RYZ_SCRIPT_STORE_NAME_MAX + 1U];
    char old_sha[65];
    char new_sha[65];
    char record_sha[65];
} legacy_journal_t;

typedef struct {
    char magic[8];
    char target[RYZ_SCRIPT_STORE_NAME_MAX + 1U];
    char source_sha[65];
    char created[17];
    char modified[17];
    char record_sha[65];
} time_record_t;

typedef struct {
    char magic[8], operation, had_target;
    char target[RYZ_SCRIPT_STORE_NAME_MAX + 1U];
    char old_sha[65], new_sha[65];
    char had_time;
    time_record_t old_time, new_time;
    char record_sha[65];
} journal_t;

_Static_assert(sizeof(legacy_journal_t) == 246, "legacy byte layout");
_Static_assert(sizeof(time_record_t) == 213, "timestamp byte layout");
_Static_assert(sizeof(journal_t) == 673, "v3 byte layout");

typedef struct {
    bool exists;
    size_t bytes;
    char sha[65];
} source_identity_t;

static atomic_flag s_lock = ATOMIC_FLAG_INIT;
static char s_root[ROOT_MAX + 1U];
static ryz_script_store_status_t s_status;
static void observe_read_failure(esp_err_t error);

static bool enter(void)
{
    return !atomic_flag_test_and_set_explicit(&s_lock, memory_order_acquire);
}

static void leave(void)
{
    atomic_flag_clear_explicit(&s_lock, memory_order_release);
}

static bool valid_sha(const char *sha)
{
    if (sha == NULL || strnlen(sha, 65) != 64) return false;
    for (size_t i = 0; i < 64; ++i) {
        if (!((sha[i] >= '0' && sha[i] <= '9') ||
              (sha[i] >= 'a' && sha[i] <= 'f'))) return false;
    }
    return true;
}

bool ryz_script_store_valid_name(const char *name)
{
    if (name == NULL) return false;
    const size_t length = strnlen(name, RYZ_SCRIPT_STORE_NAME_MAX + 1U);
    if (length < 5 || length > RYZ_SCRIPT_STORE_NAME_MAX ||
        strcmp(name + length - 4, ".lua") != 0) return false;
    for (size_t i = 0; i < length - 4; ++i) {
        const char ch = name[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) return false;
    }
    return true;
}

esp_err_t ryz_script_store_source_sha256(const char *source, size_t length,
                                        char out_sha256[65])
{
    if (out_sha256 == NULL) return ESP_ERR_INVALID_ARG;
    out_sha256[0] = '\0';
    if (source == NULL) return ESP_ERR_INVALID_ARG;
    if (length == 0 || length > RYZ_SCRIPT_STORE_SOURCE_MAX) return ESP_ERR_INVALID_SIZE;
    if (memchr(source, '\0', length) != NULL) return ESP_ERR_INVALID_ARG;
    return ryz_script_store_platform_sha256(source, length, out_sha256);
}

static void path_for(const char *name, char path[PATH_MAX_BYTES])
{
    const size_t root_length = strlen(s_root);
    memcpy(path, s_root, root_length);
    path[root_length] = '/';
    strcpy(path + root_length + 1U, name);
}

static esp_err_t named_info(const char *name, ryz_script_store_file_info_t *info)
{
    char path[PATH_MAX_BYTES];
    path_for(name, path);
    esp_err_t error = ryz_script_store_platform_info(path, info);
    observe_read_failure(error);
    return error;
}

static esp_err_t remove_named(const char *name)
{
    char path[PATH_MAX_BYTES];
    path_for(name, path);
    return ryz_script_store_platform_remove(path);
}

static esp_err_t rename_named(const char *from, const char *to)
{
    char source[PATH_MAX_BYTES], destination[PATH_MAX_BYTES];
    path_for(from, source);
    path_for(to, destination);
    return ryz_script_store_platform_rename(source, destination);
}

static esp_err_t write_named(const char *name, const void *data, size_t length)
{
    char path[PATH_MAX_BYTES];
    path_for(name, path);
    return ryz_script_store_platform_write_new(path, data, length);
}

static esp_err_t read_named(const char *name, void *data, size_t capacity,
                            size_t *length)
{
    char path[PATH_MAX_BYTES];
    path_for(name, path);
    esp_err_t error = ryz_script_store_platform_read(path, data, capacity, length);
    observe_read_failure(error);
    return error;
}

static void finish_result(ryz_script_store_mutation_t *result)
{
    result->recovery_required = s_status.recovery_required;
    result->revision = s_status.revision;
}

static esp_err_t blocked(esp_err_t error, ryz_script_store_mutation_t *result)
{
    s_status.recovery_required = true;
    s_status.recovery_error = error;
    finish_result(result);
    return error;
}

static void advance_revision(void)
{
    /* Recovery may finish cleanup at exhaustion, but mutations never wrap. */
    if (s_status.revision != UINT32_MAX) ++s_status.revision;
}

static void observe_read_failure(esp_err_t error)
{
    /* Only this typed PLATFORM result means sticky I/O degradation. Ordinary
     * file errors and caller CAS/revision conflicts do not poison the Store. */
    if (error == ESP_ERR_INVALID_STATE) {
        s_status.recovery_required = true;
        s_status.recovery_error = error;
        s_status.capacity_valid = false;
        s_status.capacity_error = error;
        s_status.total_bytes = 0;
        s_status.used_bytes = 0;
    }
}

static esp_err_t refresh_capacity(void)
{
    s_status.capacity_valid = false;
    s_status.total_bytes = 0;
    s_status.used_bytes = 0;
    size_t total = 0, used = 0;
    esp_err_t error = s_status.ready ?
        ryz_script_store_platform_capacity(s_root, &total, &used) :
        ESP_ERR_INVALID_STATE;
    if (error == ESP_OK && used > total) error = ESP_FAIL;
    s_status.capacity_error = error;
    if (error == ESP_OK) {
        s_status.total_bytes = total;
        s_status.used_bytes = used;
        s_status.capacity_valid = true;
    } else if (s_status.ready) {
        observe_read_failure(error);
    }
    return error;
}

static esp_err_t identity(const char *name, source_identity_t *out,
                           char **out_source)
{
    *out = (source_identity_t){0};
    if (out_source != NULL) *out_source = NULL;
    ryz_script_store_file_info_t info;
    esp_err_t error = named_info(name, &info);
    if (error != ESP_OK || !info.exists) return error;
    out->exists = true;
    if (!info.regular) return ESP_ERR_NOT_SUPPORTED;
    if (out_source == NULL) {
        char path[PATH_MAX_BYTES];
        path_for(name, path);
        size_t bytes = 0;
        error = ryz_script_store_platform_file_sha256(path, &bytes, out->sha);
        observe_read_failure(error);
        if (error == ESP_OK && bytes != info.bytes) error = ESP_ERR_INVALID_SIZE;
        if (error == ESP_OK) out->bytes = bytes;
        return error;
    }
    if (info.bytes > RYZ_SCRIPT_STORE_SOURCE_MAX) return ESP_ERR_INVALID_SIZE;
    char *source = ryz_script_store_platform_alloc(info.bytes + 1U);
    if (source == NULL) return ESP_ERR_NO_MEM;
    size_t length = 0;
    error = read_named(name, source, info.bytes, &length);
    if (error == ESP_OK && length != info.bytes) error = ESP_ERR_INVALID_SIZE;
    if (error == ESP_OK) {
        source[length] = '\0';
        out->bytes = length;
        error = ryz_script_store_platform_sha256(source, length, out->sha);
    }
    if (error == ESP_OK && out_source != NULL) *out_source = source;
    else ryz_script_store_platform_free(source);
    return error;
}

static bool matches(const source_identity_t *identity, const char *sha)
{
    return identity->exists && strcmp(identity->sha, sha) == 0;
}

static esp_err_t legacy_present(bool *present)
{
    *present = false;
    static const char *const names[] = {".upload.bak", ".upload.tmp"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        ryz_script_store_file_info_t info;
        esp_err_t error = named_info(names[i], &info);
        if (error != ESP_OK) return error;
        *present = *present || info.exists;
    }
    return ESP_OK;
}

static bool time_value_valid(int64_t value)
{
    return value == 0 || (value >= TIME_MIN && value <= TIME_MAX);
}

static bool decode_time(const char text[17], int64_t *out)
{
    uint64_t value = 0;
    if (text[16] != '\0') return false;
    for (size_t i = 0; i < 16; ++i) {
        const unsigned char c = (unsigned char)text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        value = value * 16U + (c <= '9' ? c - '0' : c - 'a' + 10U);
    }
    if (value > (uint64_t)TIME_MAX || !time_value_valid((int64_t)value)) return false;
    *out = (int64_t)value;
    return true;
}

static bool time_record_valid(const time_record_t *record, const char *name,
                               const char *source_sha)
{
    int64_t ignored;
    char digest[65];
    return memcmp(record->magic, "RYZTM01", 8) == 0 &&
        ryz_script_store_valid_name(record->target) && strcmp(record->target, name) == 0 &&
        valid_sha(record->source_sha) &&
        (source_sha == NULL || strcmp(record->source_sha, source_sha) == 0) &&
        decode_time(record->created, &ignored) && decode_time(record->modified, &ignored) &&
        valid_sha(record->record_sha) &&
        ryz_script_store_platform_sha256(record, offsetof(time_record_t, record_sha), digest) == ESP_OK &&
        strcmp(digest, record->record_sha) == 0;
}

static esp_err_t time_name(const char *name, char out[31])
{
    char digest[65];
    esp_err_t error = ryz_script_store_platform_sha256(name, strlen(name), digest);
    if (error == ESP_OK) {
        /* 6 + 24 = 30 bytes, including the real SPIFFS basename limit. The
         * complete basename in the record detects even a hash-prefix collision. */
        memcpy(out, ".ryz-t", 6);
        memcpy(out + 6, digest, 24);
        out[30] = '\0';
    }
    return error;
}

static esp_err_t read_time(const char *name, bool *exists, time_record_t *record)
{
    *exists = false;
    memset(record, 0, sizeof(*record));
    char leaf[31];
    esp_err_t error = time_name(name, leaf);
    ryz_script_store_file_info_t info;
    if (error == ESP_OK) error = named_info(leaf, &info);
    if (error != ESP_OK || !info.exists) return error;
    *exists = true;
    if (!info.regular || info.bytes != sizeof(*record)) return ESP_ERR_INVALID_STATE;
    size_t bytes = 0;
    error = read_named(leaf, record, sizeof(*record), &bytes);
    if (error == ESP_OK && (bytes != sizeof(*record) || !time_record_valid(record, name, NULL)))
        error = ESP_ERR_INVALID_STATE;
    return error;
}

static void project_time(const char *name, const char *sha, ryz_script_store_times_t *out)
{
    *out = (ryz_script_store_times_t){0};
    if (s_status.recovery_required) return;
    bool exists;
    time_record_t record;
    esp_err_t error = read_time(name, &exists, &record);
    if (error == ESP_OK && exists && strcmp(record.source_sha, sha) != 0)
        error = ESP_ERR_INVALID_STATE;
    if (error != ESP_OK) {
        /* Keep source maintenance available, but never promote uncertain dates
         * or overwrite an unrecognized sidecar in a subsequent mutation. */
        s_status.recovery_required = true;
        s_status.recovery_error = error;
    } else if (exists) {
        (void)decode_time(record.created, &out->created_unix);
        (void)decode_time(record.modified, &out->modified_unix);
    }
}

static esp_err_t time_index_visit(void *context, const char *leaf)
{
    if (strncmp(leaf, ".ryz-t", 6) != 0 || strlen(leaf) != 30) return ESP_OK;
    for (size_t i = 6; i < 30; ++i)
        if (!((leaf[i] >= '0' && leaf[i] <= '9') || (leaf[i] >= 'a' && leaf[i] <= 'f')))
            return ESP_OK;
    size_t *count = context;
    if (++*count > RYZ_SCRIPT_STORE_INDEX_MAX) return ESP_ERR_INVALID_STATE;
    ryz_script_store_file_info_t info;
    esp_err_t error = named_info(leaf, &info);
    if (error != ESP_OK) return error;
    if (!info.exists || !info.regular || info.bytes != sizeof(time_record_t)) return ESP_ERR_INVALID_STATE;
    time_record_t record;
    size_t bytes = 0;
    error = read_named(leaf, &record, sizeof(record), &bytes);
    if (error != ESP_OK) return error;
    if (bytes != sizeof(record) || !time_record_valid(&record, record.target, NULL))
        return ESP_ERR_INVALID_STATE;
    char expected[31];
    if ((error = time_name(record.target, expected)) != ESP_OK) return error;
    if (strcmp(leaf, expected) != 0) return ESP_ERR_INVALID_STATE;
    source_identity_t source;
    error = identity(record.target, &source, NULL);
    if (error == ESP_OK && !matches(&source, record.source_sha)) error = ESP_ERR_INVALID_STATE;
    return error;
}

static bool legacy_journal_valid(const legacy_journal_t *journal)
{
    char digest[65];
    const bool legacy = memcmp(journal->magic, "RYZST01", 8) == 0;
    if ((!legacy && memcmp(journal->magic, "RYZST02", 8) != 0) ||
        (journal->operation != 'P' && journal->operation != 'R') ||
        (journal->had_target != '0' && journal->had_target != '1') ||
        !ryz_script_store_valid_name(journal->target) ||
        (legacy && strcmp(journal->target, "boot.lua") == 0) ||
        !valid_sha(journal->record_sha)) return false;
    if (journal->had_target == '1' ? !valid_sha(journal->old_sha) :
                                    journal->old_sha[0] != '\0') return false;
    if (journal->operation == 'P' ? !valid_sha(journal->new_sha) :
        (journal->new_sha[0] != '\0' || journal->had_target != '1')) return false;
    return ryz_script_store_platform_sha256(
               journal, offsetof(legacy_journal_t, record_sha), digest) == ESP_OK &&
           strcmp(digest, journal->record_sha) == 0;
}

static bool journal_valid(const journal_t *journal)
{
    static const time_record_t empty;
    char digest[65];
    if (memcmp(journal->magic, "RYZST03", 8) != 0 ||
        (journal->operation != 'P' && journal->operation != 'R') ||
        (journal->had_target != '0' && journal->had_target != '1') ||
        (journal->had_time != '0' && journal->had_time != '1') ||
        !ryz_script_store_valid_name(journal->target) || !valid_sha(journal->record_sha)) return false;
    if (journal->had_target == '1' ? !valid_sha(journal->old_sha) : journal->old_sha[0] != '\0')
        return false;
    if (journal->had_time == '1' ?
        (journal->had_target != '1' || !time_record_valid(&journal->old_time, journal->target, journal->old_sha)) :
        memcmp(&journal->old_time, &empty, sizeof(empty)) != 0) return false;
    if (journal->operation == 'P' ?
        (!valid_sha(journal->new_sha) || !time_record_valid(&journal->new_time, journal->target, journal->new_sha)) :
        (journal->new_sha[0] != '\0' || journal->had_target != '1' ||
         memcmp(&journal->new_time, &empty, sizeof(empty)) != 0)) return false;
    return ryz_script_store_platform_sha256(journal, offsetof(journal_t, record_sha), digest) == ESP_OK &&
        strcmp(digest, journal->record_sha) == 0;
}

static esp_err_t load_journal(size_t length, journal_t *out, bool *legacy)
{
    memset(out, 0, sizeof(*out));
    *legacy = length == sizeof(legacy_journal_t);
    size_t bytes = 0;
    if (!*legacy) {
        if (length != sizeof(*out)) return ESP_ERR_INVALID_STATE;
        esp_err_t error = read_named(JOURNAL_NAME, out, sizeof(*out), &bytes);
        if (error != ESP_OK) return error;
        return bytes == sizeof(*out) && journal_valid(out) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    legacy_journal_t old;
    esp_err_t error = read_named(JOURNAL_NAME, &old, sizeof(old), &bytes);
    if (error != ESP_OK) return error;
    if (bytes != sizeof(old) || !legacy_journal_valid(&old)) return ESP_ERR_INVALID_STATE;
    memcpy(out->magic, old.magic, sizeof(old.magic));
    out->operation = old.operation;
    out->had_target = old.had_target;
    memcpy(out->target, old.target, sizeof(old.target));
    memcpy(out->old_sha, old.old_sha, sizeof(old.old_sha));
    memcpy(out->new_sha, old.new_sha, sizeof(old.new_sha));
    /* No downgrade interpretation of a newer metadata-bearing namespace. */
    bool exists;
    time_record_t unused;
    error = read_time(old.target, &exists, &unused);
    return error == ESP_OK && exists ? ESP_ERR_INVALID_STATE : error;
}

static esp_err_t settle_time(const journal_t *journal, bool committed)
{
    const time_record_t *wanted = committed ?
        (journal->operation == 'P' ? &journal->new_time : NULL) :
        (journal->had_time == '1' ? &journal->old_time : NULL);
    bool exists;
    time_record_t current;
    esp_err_t error = read_time(journal->target, &exists, &current);
    if (error != ESP_OK) return error;
    if (exists && !(journal->had_time == '1' &&
        memcmp(&current, &journal->old_time, sizeof(current)) == 0) &&
        !(journal->operation == 'P' && memcmp(&current, &journal->new_time, sizeof(current)) == 0))
        return ESP_ERR_INVALID_STATE;
    ryz_script_store_file_info_t stage;
    error = named_info(TIME_STAGE_NAME, &stage);
    if (error != ESP_OK) return error;
    /* This private stage was absent at admission and is owned by the verified
     * journal. Partial writes are repairable; unknown/nonregular data is not. */
    if (stage.exists && (!stage.regular || stage.bytes > sizeof(current))) return ESP_ERR_INVALID_STATE;
    if (stage.exists) {
        time_record_t prefix;
        size_t bytes = 0;
        error = read_named(TIME_STAGE_NAME, &prefix, sizeof(prefix), &bytes);
        if (error != ESP_OK) return error;
        if (bytes != stage.bytes ||
            !((journal->had_time == '1' && memcmp(&prefix, &journal->old_time, bytes) == 0) ||
              (journal->operation == 'P' && memcmp(&prefix, &journal->new_time, bytes) == 0)))
            return ESP_ERR_INVALID_STATE;
    }
    if ((error = remove_named(TIME_STAGE_NAME)) != ESP_OK) return error;
    if (wanted != NULL && exists && memcmp(&current, wanted, sizeof(current)) == 0) return ESP_OK;
    char leaf[31];
    if ((error = time_name(journal->target, leaf)) != ESP_OK) return error;
    if (wanted == NULL) return exists ? remove_named(leaf) : ESP_OK;
    error = write_named(TIME_STAGE_NAME, wanted, sizeof(*wanted));
    if (error == ESP_OK) {
        size_t bytes = 0;
        error = read_named(TIME_STAGE_NAME, &current, sizeof(current), &bytes);
        if (error == ESP_OK && (bytes != sizeof(current) || memcmp(&current, wanted, sizeof(current)) != 0))
            error = ESP_FAIL;
    }
    if (error == ESP_OK && exists) error = remove_named(leaf);
    if (error == ESP_OK) error = rename_named(TIME_STAGE_NAME, leaf);
    return error;
}

static esp_err_t clean_transaction(ryz_script_store_mutation_t *result)
{
    /* The identity record is removed LAST. A failed cleanup remains recoverable. */
    static const char *const names[] = {BACKUP_NAME, STAGE_NAME, JOURNAL_NAME};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        esp_err_t error = remove_named(names[i]);
        if (error != ESP_OK) {
            result->cleanup_error = error;
            return blocked(error, result);
        }
    }
    s_status.recovery_required = false;
    s_status.recovery_error = ESP_OK;
    finish_result(result);
    return ESP_OK;
}

static esp_err_t recover_locked(ryz_script_store_mutation_t *result)
{
    *result = (ryz_script_store_mutation_t){.commit = RYZ_SCRIPT_STORE_COMMIT_UNKNOWN};
    if (!s_status.ready) return ESP_ERR_INVALID_STATE;
    esp_err_t error = legacy_present(&s_status.legacy_backup_present);
    if (error != ESP_OK) return blocked(error, result);
    if (s_status.legacy_backup_present) return blocked(ESP_ERR_INVALID_STATE, result);

    ryz_script_store_file_info_t record, stage, backup, time_stage;
    if ((error = named_info(JOURNAL_NAME, &record)) != ESP_OK ||
        (error = named_info(STAGE_NAME, &stage)) != ESP_OK ||
        (error = named_info(BACKUP_NAME, &backup)) != ESP_OK ||
        (error = named_info(TIME_STAGE_NAME, &time_stage)) != ESP_OK) {
        return blocked(error, result);
    }
    if (!record.exists) {
        if (stage.exists || backup.exists || time_stage.exists) return blocked(ESP_ERR_INVALID_STATE, result);
        /* Init/explicit recovery may do I/O. Never report a corrupt/orphan
         * persistent time namespace clean just because its journal is absent. */
        size_t count = 0;
        error = ryz_script_store_platform_visit(s_root, time_index_visit, &count);
        observe_read_failure(error);
        if (error != ESP_OK) return blocked(error, result);
        result->commit = RYZ_SCRIPT_STORE_NOT_COMMITTED;
        s_status.recovery_required = false;
        s_status.recovery_error = ESP_OK;
        finish_result(result);
        return ESP_OK;
    }
    if (!record.regular ||
        (stage.exists && !stage.regular) || (backup.exists && !backup.regular)) {
        return blocked(ESP_ERR_INVALID_STATE, result);
    }
    journal_t journal;
    bool legacy;
    error = load_journal(record.bytes, &journal, &legacy);
    if (error != ESP_OK) return blocked(error, result);
    if (legacy && time_stage.exists) {
        return blocked(ESP_ERR_INVALID_STATE, result);
    }
    source_identity_t target, old;
    error = identity(journal.target, &target, NULL);
    if (error != ESP_OK) return blocked(error, result);
    error = identity(BACKUP_NAME, &old, NULL);
    if (error != ESP_OK) return blocked(error, result);
    if (old.exists && (journal.had_target != '1' ||
                       !matches(&old, journal.old_sha))) {
        return blocked(ESP_ERR_INVALID_STATE, result);
    }

    if (journal.operation == 'R') {
        if (stage.exists) return blocked(ESP_ERR_INVALID_STATE, result);
        if (!target.exists) {
            result->commit = RYZ_SCRIPT_STORE_COMMITTED;
            advance_revision();
        } else if (matches(&target, journal.old_sha) && !old.exists) {
            result->commit = RYZ_SCRIPT_STORE_NOT_COMMITTED;
        } else {
            return blocked(ESP_ERR_INVALID_STATE, result);
        }
    } else if (matches(&target, journal.new_sha) && !stage.exists) {
        result->commit = RYZ_SCRIPT_STORE_COMMITTED;
        result->bytes = target.bytes;
        strcpy(result->sha256, target.sha);
        advance_revision();
    } else if (!target.exists && old.exists) {
        error = rename_named(BACKUP_NAME, journal.target);
        if (error != ESP_OK) return blocked(error, result);
        /* Restore verified old data before removing an incomplete/new stage. */
        result->commit = RYZ_SCRIPT_STORE_NOT_COMMITTED;
        advance_revision();
    } else if (!old.exists &&
               ((journal.had_target == '0' && !target.exists) ||
                (journal.had_target == '1' && matches(&target, journal.old_sha)))) {
        result->commit = RYZ_SCRIPT_STORE_NOT_COMMITTED;
    } else {
        return blocked(ESP_ERR_INVALID_STATE, result);
    }
    if (!legacy) {
        error = settle_time(&journal, result->commit == RYZ_SCRIPT_STORE_COMMITTED);
        if (error != ESP_OK) {
            result->cleanup_error = error;
            return blocked(error, result);
        }
    }
    return clean_transaction(result);
}

esp_err_t ryz_script_store_init(const char *base_path,
                               ryz_script_store_mutation_t *out_recovery)
{
    if (out_recovery != NULL) *out_recovery = (ryz_script_store_mutation_t){0};
    if (base_path == NULL || out_recovery == NULL) return ESP_ERR_INVALID_ARG;
    size_t length = strnlen(base_path, ROOT_MAX + 1U);
    if (length < 2 || length > ROOT_MAX || base_path[0] != '/' ||
        base_path[length - 1U] == '/') return ESP_ERR_INVALID_ARG;
    if (!enter()) return ESP_ERR_TIMEOUT;
    esp_err_t error = ESP_OK;
    if (s_root[0] != '\0' && strcmp(base_path, s_root) != 0) {
        error = ESP_ERR_INVALID_STATE;
    } else {
        ryz_script_store_file_info_t info;
        error = ryz_script_store_platform_info(base_path, &info);
        if (error == ESP_OK && (!info.exists || !info.directory)) {
            error = ESP_ERR_INVALID_STATE;
        }
        if (error == ESP_OK) {
            strcpy(s_root, base_path);
            s_status.ready = true;
            if (s_status.revision == 0) s_status.revision = 1;
            error = recover_locked(out_recovery);
        } else {
            s_status.ready = false;
            s_status.recovery_error = error;
        }
    }
    esp_err_t capacity_error = refresh_capacity();
    if (error == ESP_OK) error = capacity_error;
    finish_result(out_recovery);
    leave();
    return error;
}

esp_err_t ryz_script_store_status(ryz_script_store_status_t *out_status)
{
    if (out_status == NULL) return ESP_ERR_INVALID_ARG;
    *out_status = (ryz_script_store_status_t){0};
    if (!enter()) return ESP_ERR_TIMEOUT;
    *out_status = s_status;
    leave();
    return ESP_OK;
}

esp_err_t ryz_script_store_recover(ryz_script_store_mutation_t *out_result)
{
    if (out_result == NULL) return ESP_ERR_INVALID_ARG;
    *out_result = (ryz_script_store_mutation_t){0};
    if (!enter()) return ESP_ERR_TIMEOUT;
    esp_err_t error = recover_locked(out_result);
    esp_err_t capacity_error = refresh_capacity();
    if (error == ESP_OK) error = capacity_error;
    finish_result(out_result);
    leave();
    return error;
}

typedef struct {
    size_t count;
    ryz_script_store_entry_t entries[RYZ_SCRIPT_STORE_INDEX_MAX];
} index_t;

static esp_err_t index_visit(void *context, const char *name)
{
    index_t *index = context;
    if (!ryz_script_store_valid_name(name)) return ESP_OK;
    ryz_script_store_file_info_t info;
    esp_err_t error = named_info(name, &info);
    if (error != ESP_OK || !info.exists || !info.regular) return error;
    if (index->count == RYZ_SCRIPT_STORE_INDEX_MAX) return ESP_ERR_INVALID_SIZE;
    ryz_script_store_entry_t *entry = &index->entries[index->count++];
    strcpy(entry->name, name);
    entry->bytes = info.bytes;
    entry->protected_file = false;
    return ESP_OK;
}

static int compare_entries(const void *left, const void *right)
{
    const ryz_script_store_entry_t *a = left, *b = right;
    return strcmp(a->name, b->name);
}

static esp_err_t collect_index_locked(index_t **out)
{
    *out = ryz_script_store_platform_alloc(sizeof(**out));
    if (*out == NULL) return ESP_ERR_NO_MEM;
    memset(*out, 0, sizeof(**out));
    esp_err_t error = ryz_script_store_platform_visit(s_root, index_visit, *out);
    observe_read_failure(error);
    if (error == ESP_OK)
        qsort((*out)->entries, (*out)->count, sizeof((*out)->entries[0]), compare_entries);
    return error;
}

esp_err_t ryz_script_store_list(size_t offset, size_t limit,
                               uint32_t expected_revision,
                               ryz_script_store_page_t *out_page)
{
    if (out_page == NULL) return ESP_ERR_INVALID_ARG;
    *out_page = (ryz_script_store_page_t){0};
    if (limit == 0 || limit > RYZ_SCRIPT_STORE_PAGE_MAX) return ESP_ERR_INVALID_ARG;
    if (!enter()) return ESP_ERR_TIMEOUT;
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (s_status.ready && (expected_revision == 0 ||
                          expected_revision == s_status.revision)) {
        index_t *index = NULL;
        error = collect_index_locked(&index);
        if (index != NULL) {
            if (error == ESP_OK && offset > index->count) error = ESP_ERR_INVALID_ARG;
            if (error == ESP_OK) {
                out_page->revision = s_status.revision;
                out_page->offset = offset;
                out_page->total = index->count;
                out_page->count = index->count - offset;
                if (out_page->count > limit) out_page->count = limit;
                memcpy(out_page->entries, index->entries + offset,
                       out_page->count * sizeof(out_page->entries[0]));
            }
            ryz_script_store_platform_free(index);
        }
    }
    leave();
    return error;
}

static esp_err_t get_locked(const char *name, ryz_script_store_snapshot_t *out_snapshot)
{
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (s_status.ready) {
        source_identity_t found;
        char *source = NULL;
        error = identity(name, &found, &source);
        if (error == ESP_OK && !found.exists) error = ESP_ERR_NOT_FOUND;
        if (error == ESP_OK && found.bytes == 0) error = ESP_ERR_INVALID_SIZE;
        if (error == ESP_OK && memchr(source, '\0', found.bytes) != NULL) {
            error = ESP_ERR_INVALID_ARG;
        }
        if (error == ESP_OK) {
            strcpy(out_snapshot->entry.name, name);
            out_snapshot->entry.bytes = found.bytes;
            out_snapshot->entry.protected_file = false;
            strcpy(out_snapshot->sha256, found.sha);
            project_time(name, found.sha, &out_snapshot->times);
            out_snapshot->source = source;
        } else {
            ryz_script_store_platform_free(source);
        }
    }
    return error;
}

esp_err_t ryz_script_store_get(const char *name,
                              ryz_script_store_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL) return ESP_ERR_INVALID_ARG;
    *out_snapshot = (ryz_script_store_snapshot_t){0};
    if (!ryz_script_store_valid_name(name)) return ESP_ERR_INVALID_ARG;
    if (!enter()) return ESP_ERR_TIMEOUT;
    esp_err_t error = get_locked(name, out_snapshot);
    leave();
    return error;
}

void ryz_script_store_snapshot_free(ryz_script_store_snapshot_t *snapshot)
{
    if (snapshot == NULL) return;
    ryz_script_store_platform_free(snapshot->source);
    *snapshot = (ryz_script_store_snapshot_t){0};
}

esp_err_t ryz_script_store_describe(const char *name,
                                   ryz_script_store_description_t *out_description)
{
    if (out_description == NULL) return ESP_ERR_INVALID_ARG;
    *out_description = (ryz_script_store_description_t){0};
    if (!ryz_script_store_valid_name(name)) return ESP_ERR_INVALID_ARG;
    if (!enter()) return ESP_ERR_TIMEOUT;
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (s_status.ready) {
        source_identity_t found;
        error = identity(name, &found, NULL);
        if (error == ESP_OK && !found.exists) error = ESP_ERR_NOT_FOUND;
        if (error == ESP_OK) {
            strcpy(out_description->entry.name, name);
            out_description->entry.bytes = found.bytes;
            out_description->entry.protected_file = false;
            strcpy(out_description->sha256, found.sha);
            project_time(name, found.sha, &out_description->times);
        }
    }
    leave();
    return error;
}

static esp_err_t mutation_admission(const char *previous_sha,
                                    const source_identity_t *old)
{
    if (previous_sha == NULL) return ESP_OK;
    if (previous_sha[0] == '\0') return old->exists ? ESP_ERR_INVALID_STATE : ESP_OK;
    return matches(old, previous_sha) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t check_clean(void)
{
    bool legacy;
    esp_err_t error = legacy_present(&legacy);
    s_status.legacy_backup_present = error == ESP_OK && legacy;
    if (error == ESP_OK && legacy) error = ESP_ERR_INVALID_STATE;
    static const char *const names[] = {JOURNAL_NAME, STAGE_NAME, BACKUP_NAME, TIME_STAGE_NAME};
    for (size_t i = 0; error == ESP_OK && i < sizeof(names) / sizeof(names[0]); ++i) {
        ryz_script_store_file_info_t info;
        error = named_info(names[i], &info);
        if (error == ESP_OK && info.exists) error = ESP_ERR_INVALID_STATE;
    }
    if (error != ESP_OK) {
        s_status.recovery_required = true;
        s_status.recovery_error = error;
    }
    return error;
}

static esp_err_t count_visit(void *context, const char *name)
{
    if (!ryz_script_store_valid_name(name)) return ESP_OK;
    ryz_script_store_file_info_t info;
    esp_err_t error = named_info(name, &info);
    if (error != ESP_OK || !info.exists || !info.regular) return error;
    size_t *count = context;
    if (++*count >= RYZ_SCRIPT_STORE_INDEX_MAX) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

static esp_err_t mutate_locked(const char *name, const char *source, size_t length,
                               const char *previous_sha, bool remove,
                               ryz_script_store_mutation_t *result)
{
    if (!s_status.ready || s_status.recovery_required || s_status.revision == UINT32_MAX) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = check_clean();
    if (error != ESP_OK) return error;
    source_identity_t old;
    error = identity(name, &old, NULL);
    if (error != ESP_OK) return error;
    error = mutation_admission(previous_sha, &old);
    if (error != ESP_OK) return error;
    if (remove && !old.exists) return ESP_ERR_NOT_FOUND;
    if (!old.exists) {
        size_t count = 0;
        error = ryz_script_store_platform_visit(s_root, count_visit, &count);
        observe_read_failure(error);
        if (error != ESP_OK) return error;
    }

    journal_t journal = {.operation = remove ? 'R' : 'P', .had_time = '0',
                         .had_target = old.exists ? '1' : '0'};
    memcpy(journal.magic, "RYZST03", 8);
    strcpy(journal.target, name);
    if (old.exists) strcpy(journal.old_sha, old.sha);
    bool had_time;
    error = read_time(name, &had_time, &journal.old_time);
    if (error == ESP_OK && had_time && (!old.exists || strcmp(journal.old_time.source_sha, old.sha) != 0))
        error = ESP_ERR_INVALID_STATE;
    if (error != ESP_OK) return blocked(error, result);
    journal.had_time = had_time ? '1' : '0';
    if (!remove) {
        error = ryz_script_store_platform_sha256(source, length, journal.new_sha);
        if (error != ESP_OK) return error;
        result->bytes = length;
        strcpy(result->sha256, journal.new_sha);
        if (matches(&old, journal.new_sha)) {
            result->commit = RYZ_SCRIPT_STORE_COMMITTED;
            return ESP_OK; /* Already the requested bytes; no transaction needed. */
        }
        int64_t created = 0;
        if (had_time) (void)decode_time(journal.old_time.created, &created);
        int64_t now = ryz_script_store_platform_utc();
        if (!time_value_valid(now)) now = 0;
        if (!old.exists) created = now;
        time_record_t *time = &journal.new_time;
        memcpy(time->magic, "RYZTM01", 8);
        strcpy(time->target, name);
        strcpy(time->source_sha, journal.new_sha);
        snprintf(time->created, sizeof(time->created), "%016" PRIx64, (uint64_t)created);
        snprintf(time->modified, sizeof(time->modified), "%016" PRIx64, (uint64_t)now);
        error = ryz_script_store_platform_sha256(time, offsetof(time_record_t, record_sha), time->record_sha);
        if (error != ESP_OK) return error;
    }
    error = ryz_script_store_platform_sha256(
        &journal, offsetof(journal_t, record_sha), journal.record_sha);
    if (error != ESP_OK) return error;
    size_t total, used;
    error = ryz_script_store_platform_capacity(s_root, &total, &used);
    observe_read_failure(error);
    if (error != ESP_OK) return error;
    const size_t required = length + sizeof(journal) + sizeof(time_record_t) + CAPACITY_RESERVE;
    if (used > total || required > total - used) return ESP_ERR_NO_MEM;

    s_status.recovery_required = true;
    error = write_named(JOURNAL_NAME, &journal, sizeof(journal));
    if (error == ESP_OK) {
        journal_t readback;
        size_t bytes = 0;
        error = read_named(JOURNAL_NAME, &readback, sizeof(readback), &bytes);
        if (error == ESP_OK && (bytes != sizeof(readback) ||
                                memcmp(&readback, &journal, sizeof(journal)) != 0)) {
            error = ESP_FAIL;
        }
    }
    if (error == ESP_OK && !remove) {
        error = write_named(STAGE_NAME, source, length);
        if (error == ESP_OK) {
            source_identity_t staged;
            error = identity(STAGE_NAME, &staged, NULL);
            if (error == ESP_OK && (!matches(&staged, journal.new_sha) ||
                                    staged.bytes != length)) error = ESP_FAIL;
        }
    }
    if (error == ESP_OK && old.exists) error = rename_named(name, BACKUP_NAME);
    if (error == ESP_OK && !remove) error = rename_named(STAGE_NAME, name);
    if (error == ESP_OK) {
        result->commit = RYZ_SCRIPT_STORE_COMMITTED;
        advance_revision();
        error = settle_time(&journal, true);
        if (error != ESP_OK) {
            result->cleanup_error = error;
            return blocked(error, result);
        }
        return clean_transaction(result);
    }

    /* Even an I/O error can follow a completed namespace operation. Determine
     * its outcome from the identity record before claiming rollback/success. */
    ryz_script_store_mutation_t recovery;
    (void)recover_locked(&recovery);
    const size_t requested_bytes = result->bytes;
    char requested_sha[65];
    strcpy(requested_sha, result->sha256);
    *result = recovery;
    result->bytes = requested_bytes;
    strcpy(result->sha256, requested_sha);
    return error;
}

static esp_err_t mutate(const char *name, const char *source, size_t length,
                        const char *previous_sha, bool remove,
                        ryz_script_store_mutation_t *result)
{
    if (result == NULL) return ESP_ERR_INVALID_ARG;
    *result = (ryz_script_store_mutation_t){0};
    if (!ryz_script_store_valid_name(name) ||
        (previous_sha != NULL && previous_sha[0] != '\0' && !valid_sha(previous_sha))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!remove && (source == NULL || length == 0 || length > RYZ_SCRIPT_STORE_SOURCE_MAX)) {
        return source == NULL ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_SIZE;
    }
    if (!remove && memchr(source, '\0', length) != NULL) return ESP_ERR_INVALID_ARG;
    if (!enter()) return ESP_ERR_TIMEOUT;
    esp_err_t error = mutate_locked(name, source, length, previous_sha, remove, result);
    esp_err_t capacity_error = refresh_capacity();
    if (error == ESP_OK) error = capacity_error;
    finish_result(result);
    leave();
    return error;
}

esp_err_t ryz_script_store_put(const char *name, const char *source, size_t length,
                              const char *previous_sha256,
                              ryz_script_store_mutation_t *out_result)
{
    return mutate(name, source, length, previous_sha256, false, out_result);
}

esp_err_t ryz_script_store_remove(const char *name, const char *previous_sha256,
                                 ryz_script_store_mutation_t *out_result)
{
    return mutate(name, NULL, 0, previous_sha256, true, out_result);
}

static esp_err_t revision_admission(uint32_t expected_revision)
{
    if (!s_status.ready || s_status.recovery_required ||
        s_status.revision == UINT32_MAX || expected_revision != s_status.revision)
        return ESP_ERR_INVALID_STATE;
    return check_clean();
}

esp_err_t ryz_script_store_copy_boot(const char *source_name, const char *source_sha256,
                                    uint32_t expected_revision,
                                    ryz_script_store_mutation_t *out_result)
{
    if (out_result == NULL) return ESP_ERR_INVALID_ARG;
    *out_result = (ryz_script_store_mutation_t){0};
    if (!expected_revision || !ryz_script_store_valid_name(source_name) ||
        !valid_sha(source_sha256)) return ESP_ERR_INVALID_ARG;
    if (!enter()) return ESP_ERR_TIMEOUT;
    esp_err_t error = revision_admission(expected_revision);
    ryz_script_store_snapshot_t source = {0};
    if (error == ESP_OK) error = get_locked(source_name, &source);
    if (error == ESP_OK && strcmp(source.sha256, source_sha256) != 0)
        error = ESP_ERR_INVALID_STATE;
    if (error == ESP_OK)
        error = mutate_locked("boot.lua", source.source, source.entry.bytes, NULL, false, out_result);
    ryz_script_store_snapshot_free(&source);
    esp_err_t capacity_error = refresh_capacity();
    if (error == ESP_OK) error = capacity_error;
    finish_result(out_result);
    leave();
    return error;
}

esp_err_t ryz_script_store_delete_all(uint32_t expected_revision,
                                     ryz_script_store_bulk_t *out_result)
{
    if (out_result == NULL) return ESP_ERR_INVALID_ARG;
    *out_result = (ryz_script_store_bulk_t){0};
    if (!expected_revision) return ESP_ERR_INVALID_ARG;
    if (!enter()) return ESP_ERR_TIMEOUT;
    esp_err_t error = revision_admission(expected_revision);
    index_t *index = NULL;
    if (error == ESP_OK) error = collect_index_locked(&index);
    if (error == ESP_OK) {
        out_result->total = index->count;
        /* Reject predictable exhaustion/capacity before the first deletion.
         * Each file still performs its own platform checks and transaction. */
        if (index->count > UINT32_MAX - s_status.revision) error = ESP_ERR_INVALID_STATE;
        if (error == ESP_OK && index->count != 0) {
            size_t total = 0, used = 0;
            error = ryz_script_store_platform_capacity(s_root, &total, &used);
            observe_read_failure(error);
            if (error == ESP_OK && (used > total || sizeof(journal_t) + sizeof(time_record_t) + CAPACITY_RESERVE > total - used))
                error = ESP_ERR_NO_MEM;
        }
        for (size_t i = 0; error == ESP_OK && i < index->count; ++i) {
            ryz_script_store_mutation_t one = {0};
            error = mutate_locked(index->entries[i].name, NULL, 0, NULL, true, &one);
            if (one.commit == RYZ_SCRIPT_STORE_COMMITTED) ++out_result->removed;
            if (error == ESP_OK && one.commit != RYZ_SCRIPT_STORE_COMMITTED)
                error = ESP_ERR_INVALID_STATE;
            out_result->cleanup_error = one.cleanup_error;
            out_result->outcome_unknown = one.commit == RYZ_SCRIPT_STORE_COMMIT_UNKNOWN;
            if (error != ESP_OK) strcpy(out_result->failed_name, index->entries[i].name);
        }
        out_result->complete = out_result->removed == out_result->total && !out_result->outcome_unknown;
    }
    ryz_script_store_platform_free(index);
    esp_err_t capacity_error = refresh_capacity();
    if (error == ESP_OK) error = capacity_error;
    out_result->recovery_required = s_status.recovery_required;
    out_result->revision = s_status.revision;
    leave();
    return error;
}
