#include "ryz_fs_platform.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* SPIFFS has a flat namespace and usually a 32-byte object-name budget.
 * .rf + 96-bit SHA-256 prefix + slot = 28 bytes (+ slash + NUL = 30).
 * Complete identities are also checked (with CRC) inside every record:
 * a hash collision never grants access to another app's readable record. */
#define KEY_LEN 27U
#define PHYSICAL_LEN 28U
#define HEADER_SIZE 96U
#define COMMIT_SIZE (HEADER_SIZE + 4U)
#define ACK_SIZE 4U
#define RECORD_SIZE (HEADER_SIZE + RYZ_FS_MAX_FILE_BYTES + ACK_SIZE)
#define RECOVERY_RESERVE_BYTES 8192U
#define APP_OFFSET 21U
#define NAME_OFFSET 62U
#define CRC_OFFSET 88U

typedef struct { char key[KEY_LEN + 1U]; } fs_key_t;
typedef struct { fs_key_t keys[RYZ_FS_GLOBAL_MAX_FILES]; size_t count; } directory_t;
typedef struct {
    bool exists;
    bool deleted;
    unsigned slot;
    uint64_t generation;
    size_t size;
    char app[RYZ_FS_APP_ID_MAX + 1U];
    char name[RYZ_FS_NAME_MAX + 1U];
    uint8_t header[HEADER_SIZE];
} state_t;

static atomic_flag s_busy = ATOMIC_FLAG_INIT;

static bool ascii_alnum(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

static bool valid_name(const char *value, size_t limit, bool app)
{
    if (app) {
        if (value == NULL) return false;
        size_t length = 0;
        while (value[length] != '\0' && length <= limit) ++length;
        if (length < 5U || length > limit || strcmp(value + length - 4U, ".lua") != 0) return false;
        for (size_t i = 0; i < length - 4U; ++i) {
            unsigned char c = (unsigned char)value[i];
            if (!ascii_alnum(c) && c != '_' && c != '-') return false;
        }
        return true;
    }
    if (value == NULL || !ascii_alnum((unsigned char)value[0])) return false;
    size_t length = 0;
    for (; value[length] != '\0'; ++length) {
        if (length >= limit) return false;
        unsigned char c = (unsigned char)value[length];
        if (!ascii_alnum(c) && c != '_' && c != '-' && c != '.') return false;
        if (c == '.' && length > 0 && value[length - 1U] == '.') return false;
    }
    return true;
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8U * i));
}

static uint64_t get64(const uint8_t *p)
{
    return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32;
}

static void put64(uint8_t *p, uint64_t value)
{
    put32(p, (uint32_t)value);
    put32(p + 4, (uint32_t)(value >> 32));
}

static uint32_t crc32(const uint8_t *bytes, size_t size)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

static ryz_fs_result_t make_key(const char *app, const char *name, char key[KEY_LEN + 1U])
{
    uint8_t identity[RYZ_FS_APP_ID_MAX + RYZ_FS_NAME_MAX + 1U];
    size_t app_size = strlen(app), name_size = strlen(name);
    memcpy(identity, app, app_size + 1U);
    memcpy(identity + app_size + 1U, name, name_size);
    char hash[65];
    ryz_fs_result_t result = ryz_fs_platform_hash(identity, app_size + 1U + name_size, hash);
    if (result != RYZ_FS_OK) return result;
    memcpy(key, ".rf", 3U);
    memcpy(key + 3U, hash, 24U);
    key[KEY_LEN] = '\0';
    return RYZ_FS_OK;
}

static void physical_name(const char *key, unsigned slot, bool commit, char out[PHYSICAL_LEN + 1U])
{
    memcpy(out, key, KEY_LEN);
    out[KEY_LEN] = (char)((commit ? 'a' : '0') + slot);
    out[PHYSICAL_LEN] = '\0';
}

static ryz_fs_result_t collect(void *context, const char *name)
{
    directory_t *directory = context;
    if (strncmp(name, ".rf", 3U) != 0) return RYZ_FS_OK;
    if (strlen(name) != PHYSICAL_LEN) return RYZ_FS_CORRUPT;
    for (size_t i = 3U; i < KEY_LEN; ++i) {
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) return RYZ_FS_CORRUPT;
    }
    char suffix = name[KEY_LEN];
    if (suffix != '0' && suffix != '1' && suffix != 'a' && suffix != 'b') return RYZ_FS_CORRUPT;
    for (size_t i = 0; i < directory->count; ++i) {
        if (memcmp(directory->keys[i].key, name, KEY_LEN) == 0) return RYZ_FS_OK;
    }
    if (directory->count == RYZ_FS_GLOBAL_MAX_FILES) return RYZ_FS_TOO_MANY_FILES;
    memcpy(directory->keys[directory->count].key, name, KEY_LEN);
    directory->keys[directory->count++].key[KEY_LEN] = '\0';
    return RYZ_FS_OK;
}

static bool canonical_string(const uint8_t *bytes, size_t capacity)
{
    const uint8_t *end = memchr(bytes, 0, capacity);
    if (end == NULL) return false;
    for (++end; end < bytes + capacity; ++end) if (*end != 0) return false;
    return true;
}

static ryz_fs_result_t validate_header(const uint8_t header[HEADER_SIZE], const char *key)
{
    if (memcmp(header, "RYZFS01", 8U) != 0 || get64(header + 8U) == 0 ||
        get32(header + 16U) > RYZ_FS_MAX_FILE_BYTES || header[20U] > 1U ||
        (header[20U] != 0 && get32(header + 16U) != 0) || header[87U] != 0 ||
        get32(header + 92U) != 0 ||
        !canonical_string(header + APP_OFFSET, RYZ_FS_APP_ID_MAX + 1U) ||
        !canonical_string(header + NAME_OFFSET, RYZ_FS_NAME_MAX + 1U)) return RYZ_FS_CORRUPT;
    const char *app = (const char *)header + APP_OFFSET;
    const char *name = (const char *)header + NAME_OFFSET;
    if (!valid_name(app, RYZ_FS_APP_ID_MAX, true) || !valid_name(name, RYZ_FS_NAME_MAX, false)) return RYZ_FS_CORRUPT;
    char expected[KEY_LEN + 1U];
    ryz_fs_result_t result = make_key(app, name, expected);
    if (result != RYZ_FS_OK) return result;
    return strcmp(expected, key) == 0 ? RYZ_FS_OK : RYZ_FS_CORRUPT;
}

static ryz_fs_result_t read_commit(const char *key, unsigned slot, uint8_t commit[COMMIT_SIZE])
{
    char path[PHYSICAL_LEN + 1U];
    physical_name(key, slot, true, path);
    size_t size = 0;
    ryz_fs_result_t result = ryz_fs_platform_read(path, commit, COMMIT_SIZE, &size);
    if (result == RYZ_FS_TOO_LARGE) return RYZ_FS_RECOVERY_REQUIRED;
    if (result != RYZ_FS_OK) return result;
    if (size != COMMIT_SIZE || crc32(commit, HEADER_SIZE) != get32(commit + HEADER_SIZE)) return RYZ_FS_RECOVERY_REQUIRED;
    result = validate_header(commit, key);
    return result == RYZ_FS_CORRUPT ? RYZ_FS_RECOVERY_REQUIRED : result;
}

/* Only the selected committed payload is authoritative. An incomplete inactive
 * payload has no commit descriptor and cannot replace the acknowledged value.
 * A broken commit descriptor is ambiguous (torn publication or later damage),
 * so NEVER silently fall back to an older value. */
static ryz_fs_result_t load_state(const char *key, state_t *state, uint8_t *scratch)
{
    memset(state, 0, sizeof(*state));
    uint8_t commits[2][COMMIT_SIZE];
    bool found[2] = {false, false};
    for (unsigned i = 0; i < 2; ++i) {
        ryz_fs_result_t result = read_commit(key, i, commits[i]);
        if (result == RYZ_FS_NOT_FOUND) continue;
        if (result != RYZ_FS_OK) return result;
        found[i] = true;
    }
    if (!found[0] && !found[1]) {
        /* A first write interrupted before publication and a previously saved
         * value whose commit records disappeared are indistinguishable. Never
         * translate either into not_found/default inventory initialization. */
        for (unsigned i = 0; i < 2; ++i) {
            char path[PHYSICAL_LEN + 1U];
            physical_name(key, i, false, path);
            size_t size = 0;
            ryz_fs_result_t result = ryz_fs_platform_read(path, scratch, RECORD_SIZE, &size);
            if (result == RYZ_FS_NOT_FOUND) continue;
            if (result == RYZ_FS_OK || result == RYZ_FS_TOO_LARGE) return RYZ_FS_RECOVERY_REQUIRED;
            return result;
        }
        return RYZ_FS_OK;
    }
    if (found[0] && found[1] &&
        (memcmp(commits[0] + APP_OFFSET, commits[1] + APP_OFFSET, RYZ_FS_APP_ID_MAX + 1U) != 0 ||
         memcmp(commits[0] + NAME_OFFSET, commits[1] + NAME_OFFSET, RYZ_FS_NAME_MAX + 1U) != 0 ||
         get64(commits[0] + 8U) == get64(commits[1] + 8U))) return RYZ_FS_CORRUPT;
    unsigned slot = !found[0] ? 1U : !found[1] ? 0U : get64(commits[0] + 8U) > get64(commits[1] + 8U) ? 0U : 1U;
    const uint8_t *header = commits[slot];
    state->exists = true;
    state->slot = slot;
    state->generation = get64(header + 8U);
    state->size = get32(header + 16U);
    state->deleted = header[20U] != 0;
    memcpy(state->app, header + APP_OFFSET, sizeof(state->app));
    memcpy(state->name, header + NAME_OFFSET, sizeof(state->name));
    memcpy(state->header, header, HEADER_SIZE);
    if (state->deleted) return RYZ_FS_OK;
    char path[PHYSICAL_LEN + 1U];
    /* An ACK footer is written only AFTER a commit was published. If a newer
     * acknowledged payload survives but its commit disappeared, returning the
     * older slot would silently roll back data. An unacknowledged inactive
     * payload can instead be a normal interrupted pre-publication write. */
    physical_name(key, 1U - slot, false, path);
    size_t inactive_size = 0;
    ryz_fs_result_t result = ryz_fs_platform_read(path, scratch, RECORD_SIZE, &inactive_size);
    if (result != RYZ_FS_OK && result != RYZ_FS_NOT_FOUND && result != RYZ_FS_TOO_LARGE) return result;
    if (result == RYZ_FS_TOO_LARGE) return RYZ_FS_RECOVERY_REQUIRED;
    if (result == RYZ_FS_OK && inactive_size >= HEADER_SIZE && validate_header(scratch, key) == RYZ_FS_OK &&
        get64(scratch + 8U) > state->generation && inactive_size > HEADER_SIZE + get32(scratch + 16U)) {
        return RYZ_FS_RECOVERY_REQUIRED;
    }
    physical_name(key, slot, false, path);
    size_t size = 0;
    result = ryz_fs_platform_read(path, scratch, RECORD_SIZE, &size);
    if (result == RYZ_FS_NOT_FOUND || result == RYZ_FS_TOO_LARGE) return RYZ_FS_CORRUPT;
    if (result != RYZ_FS_OK) return result;
    size_t value_size = HEADER_SIZE + state->size;
    if (size < value_size || size > value_size + ACK_SIZE || memcmp(scratch, header, HEADER_SIZE) != 0 ||
        memcmp(scratch + value_size, "ACK1", size - value_size) != 0) return RYZ_FS_CORRUPT;
    uint32_t expected = get32(scratch + CRC_OFFSET);
    put32(scratch + CRC_OFFSET, 0);
    uint32_t actual = crc32(scratch, value_size);
    put32(scratch + CRC_OFFSET, expected);
    return actual == expected ? RYZ_FS_OK : RYZ_FS_CORRUPT;
}

static bool same_identity(const state_t *state, const char *app, const char *name)
{
    return !state->exists || (strcmp(state->app, app) == 0 && strcmp(state->name, name) == 0);
}

static int compare_entries(const void *left, const void *right)
{
    return strcmp(((const ryz_fs_entry_t *)left)->name, ((const ryz_fs_entry_t *)right)->name);
}

static ryz_fs_result_t scan(const char *app, directory_t *directory, uint8_t *scratch,
                            ryz_fs_info_t *info, size_t *global_used, size_t *global_count,
                            ryz_fs_entry_t *entries, size_t capacity)
{
    memset(directory, 0, sizeof(*directory));
    *info = (ryz_fs_info_t){.max_file_bytes = RYZ_FS_MAX_FILE_BYTES, .quota_bytes = RYZ_FS_QUOTA_BYTES,
                           .max_files = RYZ_FS_MAX_FILES};
    *global_used = *global_count = 0;
    ryz_fs_result_t result = ryz_fs_platform_visit(collect, directory);
    if (result != RYZ_FS_OK) return result;
    for (size_t i = 0; i < directory->count; ++i) {
        state_t state;
        result = load_state(directory->keys[i].key, &state, scratch);
        if (result != RYZ_FS_OK) return result;
        if (!state.exists || state.deleted) continue;
        *global_used += state.size;
        ++*global_count;
        if (*global_used > RYZ_FS_GLOBAL_QUOTA_BYTES) return RYZ_FS_QUOTA;
        if (strcmp(state.app, app) != 0) continue;
        if (info->file_count >= RYZ_FS_MAX_FILES || (entries != NULL && info->file_count >= capacity)) return RYZ_FS_TOO_MANY_FILES;
        if (entries != NULL) {
            memcpy(entries[info->file_count].name, state.name, sizeof(state.name));
            entries[info->file_count].size = state.size;
        }
        ++info->file_count;
        info->used_bytes += state.size;
        if (info->used_bytes > RYZ_FS_QUOTA_BYTES) return RYZ_FS_QUOTA;
    }
    if (entries != NULL) qsort(entries, info->file_count, sizeof(*entries), compare_entries);
    return RYZ_FS_OK;
}

static void build_header(uint8_t *record, const char *app, const char *name,
                         uint64_t generation, const void *data, size_t size, bool deleted)
{
    memset(record, 0, HEADER_SIZE);
    memcpy(record, "RYZFS01", 8U);
    put64(record + 8U, generation);
    put32(record + 16U, (uint32_t)size);
    record[20U] = deleted ? 1U : 0U;
    memcpy(record + APP_OFFSET, app, strlen(app));
    memcpy(record + NAME_OFFSET, name, strlen(name));
    if (size != 0) memcpy(record + HEADER_SIZE, data, size);
    put32(record + CRC_OFFSET, crc32(record, HEADER_SIZE + size));
}

static ryz_fs_result_t publish(const char *key, unsigned slot, const uint8_t *header)
{
    uint8_t commit[COMMIT_SIZE], verify[COMMIT_SIZE];
    memcpy(commit, header, HEADER_SIZE);
    put32(commit + HEADER_SIZE, crc32(commit, HEADER_SIZE));
    char path[PHYSICAL_LEN + 1U];
    physical_name(key, slot, true, path);
    ryz_fs_result_t result = ryz_fs_platform_write_new(path, commit, sizeof(commit));
    if (result != RYZ_FS_OK) return RYZ_FS_COMMIT_UNKNOWN;
    result = read_commit(key, slot, verify);
    if (result != RYZ_FS_OK || memcmp(commit, verify, sizeof(commit)) != 0) return RYZ_FS_COMMIT_UNKNOWN;
    return RYZ_FS_OK;
}

/* Explicit user deletion is also the recovery operation for this exact key.
 * Readable identities must match before any mutation. Bad/partial records may
 * be discarded, but valid records for a colliding namespace are never erased. */
static ryz_fs_result_t remove_key(const char *key, const char *app, const char *name, uint8_t *scratch)
{
    uint64_t generation = 0;
    unsigned newest = 1U;
    bool any_file = false;
    for (unsigned slot = 0; slot < 2; ++slot) {
        for (unsigned is_commit = 0; is_commit < 2; ++is_commit) {
            char path[PHYSICAL_LEN + 1U];
            physical_name(key, slot, is_commit != 0, path);
            size_t size = 0;
            ryz_fs_result_t result = ryz_fs_platform_read(path, scratch, RECORD_SIZE, &size);
            if (result == RYZ_FS_NOT_FOUND) continue;
            any_file = true;
            if (result != RYZ_FS_OK && result != RYZ_FS_TOO_LARGE) return result;
            if (result != RYZ_FS_OK || size < HEADER_SIZE || validate_header(scratch, key) != RYZ_FS_OK) continue;
            if (strcmp((const char *)scratch + APP_OFFSET, app) != 0 ||
                strcmp((const char *)scratch + NAME_OFFSET, name) != 0) return RYZ_FS_CORRUPT;
            uint64_t current = get64(scratch + 8U);
            if (current > generation) { generation = current; newest = slot; }
        }
    }
    if (!any_file) return RYZ_FS_NOT_FOUND;
    if (generation == UINT64_MAX) return RYZ_FS_RECOVERY_REQUIRED;
    unsigned slot = 1U - newest;
    char path[PHYSICAL_LEN + 1U];
    physical_name(key, slot, true, path);
    ryz_fs_result_t result = ryz_fs_platform_remove(path);
    if (result != RYZ_FS_OK) return RYZ_FS_COMMIT_UNKNOWN;
    build_header(scratch, app, name, generation + 1U, NULL, 0, true);
    result = publish(key, slot, scratch);
    if (result != RYZ_FS_OK) return result;
    /* Leave the deletion descriptor until all older records are gone. */
    for (unsigned i = 0; i < 2; ++i) {
        physical_name(key, i, false, path);
        if (ryz_fs_platform_remove(path) != RYZ_FS_OK) return RYZ_FS_COMMIT_UNKNOWN;
    }
    physical_name(key, 1U - slot, true, path);
    if (ryz_fs_platform_remove(path) != RYZ_FS_OK) return RYZ_FS_COMMIT_UNKNOWN;
    physical_name(key, slot, true, path);
    return ryz_fs_platform_remove(path) == RYZ_FS_OK ? RYZ_FS_OK : RYZ_FS_COMMIT_UNKNOWN;
}

static ryz_fs_result_t execute(const char *app, const ryz_fs_request_t *request, ryz_fs_response_t *response)
{
    ryz_fs_result_t result = ryz_fs_platform_ready();
    if (result != RYZ_FS_OK) return result;
    uint8_t *scratch = ryz_fs_platform_alloc(RECORD_SIZE);
    if (scratch == NULL) return RYZ_FS_NO_MEMORY;
    directory_t *directory = NULL;
    state_t current = {0};
    char key[KEY_LEN + 1U] = {0};
    if (request->op != RYZ_FS_LIST && request->op != RYZ_FS_INFO) {
        result = make_key(app, request->name, key);
        if (result != RYZ_FS_OK) goto done;
        if (request->op == RYZ_FS_REMOVE) {
            result = remove_key(key, app, request->name, scratch);
            goto done;
        }
        result = load_state(key, &current, scratch);
        if (result != RYZ_FS_OK) goto done;
        if (!same_identity(&current, app, request->name)) { result = RYZ_FS_CORRUPT; goto done; }
        if (request->op == RYZ_FS_READ) {
            if (!current.exists || current.deleted) { result = RYZ_FS_NOT_FOUND; goto done; }
            if (request->read_capacity < current.size || (current.size && request->read_buffer == NULL)) {
                result = RYZ_FS_TOO_LARGE; goto done;
            }
            if (current.size != 0) memcpy(request->read_buffer, scratch + HEADER_SIZE, current.size);
            response->size = current.size;
            goto done;
        }
    }
    directory = ryz_fs_platform_alloc(sizeof(*directory));
    if (directory == NULL) { result = RYZ_FS_NO_MEMORY; goto done; }
    size_t global_used = 0, global_count = 0;
    result = scan(app, directory, scratch, &response->info, &global_used, &global_count,
                  request->op == RYZ_FS_LIST ? request->entries : NULL, request->entries_capacity);
    if (result != RYZ_FS_OK) goto done;
    if (request->op == RYZ_FS_LIST || request->op == RYZ_FS_INFO) {
        response->count = response->info.file_count;
        goto done;
    }
    size_t old_size = current.exists && !current.deleted ? current.size : 0U;
    bool new_file = !current.exists || current.deleted;
    if (response->info.used_bytes - old_size + request->size > RYZ_FS_QUOTA_BYTES ||
        global_used - old_size + request->size > RYZ_FS_GLOBAL_QUOTA_BYTES) { result = RYZ_FS_QUOTA; goto done; }
    if (new_file && (response->info.file_count >= RYZ_FS_MAX_FILES || global_count >= RYZ_FS_GLOBAL_MAX_FILES)) {
        result = RYZ_FS_TOO_MANY_FILES; goto done;
    }
    bool key_found = false;
    for (size_t i = 0; i < directory->count; ++i) if (strcmp(directory->keys[i].key, key) == 0) key_found = true;
    if (!key_found && directory->count >= RYZ_FS_GLOBAL_MAX_FILES) { result = RYZ_FS_TOO_MANY_FILES; goto done; }
    if (current.generation == UINT64_MAX) { result = RYZ_FS_RECOVERY_REQUIRED; goto done; }
    /* Leave pages for explicit Lua deletion and the independent script store's
     * journals. Deletion may use this reserve; normal writes may not. */
    result = ryz_fs_platform_reserve(HEADER_SIZE + request->size + COMMIT_SIZE + ACK_SIZE + RECOVERY_RESERVE_BYTES);
    if (result != RYZ_FS_OK) goto done;
    unsigned slot = current.exists ? 1U - current.slot : 0U;
    char path[PHYSICAL_LEN + 1U];
    physical_name(key, slot, true, path);
    result = ryz_fs_platform_remove(path);
    if (result != RYZ_FS_OK) goto done;
    physical_name(key, slot, false, path);
    result = ryz_fs_platform_remove(path);
    if (result != RYZ_FS_OK) goto done;
    build_header(scratch, app, request->name, current.generation + 1U, request->data, request->size, false);
    uint8_t header[HEADER_SIZE];
    memcpy(header, scratch, HEADER_SIZE);
    result = ryz_fs_platform_write_new(path, scratch, HEADER_SIZE + request->size);
    if (result != RYZ_FS_OK) goto done; /* No commit was attempted: old value remains authoritative. */
    size_t size = 0;
    result = ryz_fs_platform_read(path, scratch, RECORD_SIZE, &size);
    if (result != RYZ_FS_OK) goto done;
    if (size != HEADER_SIZE + request->size || memcmp(scratch, header, HEADER_SIZE) != 0 ||
        (request->size && memcmp(scratch + HEADER_SIZE, request->data, request->size) != 0)) { result = RYZ_FS_IO; goto done; }
    result = publish(key, slot, header);
    if (result != RYZ_FS_OK) goto done;
    result = ryz_fs_platform_append(path, HEADER_SIZE + request->size, "ACK1", ACK_SIZE);
    if (result != RYZ_FS_OK) { result = RYZ_FS_COMMIT_UNKNOWN; goto done; }
    size = 0;
    result = ryz_fs_platform_read(path, scratch, RECORD_SIZE, &size);
    if (result != RYZ_FS_OK || size != HEADER_SIZE + request->size + ACK_SIZE ||
        memcmp(scratch, header, HEADER_SIZE) != 0 ||
        (request->size && memcmp(scratch + HEADER_SIZE, request->data, request->size) != 0) ||
        memcmp(scratch + HEADER_SIZE + request->size, "ACK1", ACK_SIZE) != 0) result = RYZ_FS_COMMIT_UNKNOWN;
done:
    ryz_fs_platform_free(directory);
    ryz_fs_platform_free(scratch);
    return result;
}

ryz_fs_result_t ryz_app_fs_call(void *context, const char *app_id, const ryz_fs_request_t *request,
                               ryz_fs_response_t *response)
{
    (void)context;
    if (response == NULL || request == NULL) return RYZ_FS_IO;
    memset(response, 0, sizeof(*response));
    if (!valid_name(app_id, RYZ_FS_APP_ID_MAX, true)) return RYZ_FS_INVALID_APP;
    if ((unsigned)request->op > (unsigned)RYZ_FS_INFO) return RYZ_FS_UNAVAILABLE;
    if (request->op != RYZ_FS_LIST && request->op != RYZ_FS_INFO &&
        !valid_name(request->name, RYZ_FS_NAME_MAX, false)) return RYZ_FS_INVALID_NAME;
    if (request->op == RYZ_FS_WRITE && (request->size > RYZ_FS_MAX_FILE_BYTES ||
        (request->size != 0 && request->data == NULL))) return RYZ_FS_TOO_LARGE;
    if (request->op == RYZ_FS_LIST && (request->entries == NULL || request->entries_capacity < RYZ_FS_MAX_FILES)) return RYZ_FS_TOO_MANY_FILES;
    if (atomic_flag_test_and_set(&s_busy)) return RYZ_FS_BUSY;
    ryz_fs_result_t result = execute(app_id, request, response);
    atomic_flag_clear(&s_busy);
    if (result != RYZ_FS_OK) memset(response, 0, sizeof(*response));
    return result;
}
