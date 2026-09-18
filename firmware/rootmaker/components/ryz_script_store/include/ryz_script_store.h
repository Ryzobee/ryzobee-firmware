#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RYZ_SCRIPT_STORE_NAME_MAX 40U
#define RYZ_SCRIPT_STORE_SOURCE_MAX 16384U
#define RYZ_SCRIPT_STORE_INDEX_MAX 256U
#define RYZ_SCRIPT_STORE_PAGE_MAX 16U

typedef struct {
    char name[RYZ_SCRIPT_STORE_NAME_MAX + 1U];
    size_t bytes;
    bool protected_file;
} ryz_script_store_entry_t;

/* UTC Unix seconds, independently 0 when unknown. Historical files are not
 * backfilled. Values are persisted with source identity, not SPIFFS mtime.
 * Supported calendar range: 2024-01-01 through 9999-12-31 (inclusive). */
typedef struct {
    int64_t created_unix;
    int64_t modified_unix;
} ryz_script_store_times_t;

typedef struct {
    ryz_script_store_entry_t entry;
    char sha256[65];
    ryz_script_store_times_t times;
    char *source; /* Owned, length=entry.bytes, with an extra trailing NUL. */
} ryz_script_store_snapshot_t;

typedef struct {
    ryz_script_store_entry_t entry;
    char sha256[65];
    ryz_script_store_times_t times;
} ryz_script_store_description_t;

typedef struct {
    uint32_t revision;
    size_t offset;
    size_t total;
    size_t count;
    ryz_script_store_entry_t entries[RYZ_SCRIPT_STORE_PAGE_MAX];
} ryz_script_store_page_t;

typedef enum {
    RYZ_SCRIPT_STORE_NOT_COMMITTED = 0,
    RYZ_SCRIPT_STORE_COMMITTED,
    RYZ_SCRIPT_STORE_COMMIT_UNKNOWN,
} ryz_script_store_commit_t;

typedef struct {
    ryz_script_store_commit_t commit;
    esp_err_t cleanup_error;
    bool recovery_required;
    uint32_t revision;
    size_t bytes;
    char sha256[65];
} ryz_script_store_mutation_t;

typedef struct {
    size_t total; /* Frozen regular-Lua target count, set after a complete scan. */
    size_t removed; /* Only individually confirmed COMMITTED deletions. */
    bool complete; /* All frozen targets visibly absent; cleanup may still fail. */
    bool outcome_unknown; /* Current file's commit could not be established. */
    esp_err_t cleanup_error;
    bool recovery_required;
    uint32_t revision;
    char failed_name[RYZ_SCRIPT_STORE_NAME_MAX + 1U];
} ryz_script_store_bulk_t;

typedef struct {
    bool ready;
    bool recovery_required;
    bool legacy_backup_present;
    esp_err_t recovery_error;
    uint32_t revision;
    bool capacity_valid;
    size_t total_bytes;
    size_t used_bytes;
    esp_err_t capacity_error;
} ryz_script_store_status_t;

/**
 * One mounted directory, with script access mediated by this Store. The trusted
 * app-data Store uses a separate hidden namespace on the same mounted volume.
 * Init validates the directory and performs recovery; repeating the SAME root
 * is allowed. A different root is rejected after initialization. No mounting,
 * formatting, device operation or user boot migration is performed here.
 *
 * All operations take one nonblocking exclusive lock; contention returns
 * ESP_ERR_TIMEOUT, never waits on a user script. Snapshot data outlives the
 * lock and is released with snapshot_free. Callers must independently arbitrate
 * application execution if their policy forbids edits while an app is running.
 *
 * Recovery errors leave reads/status available but block mutations. Legacy
 * .upload.bak/.upload.tmp have no target identity: preserve them and require
 * explicit external reconciliation, never guess their destination.
 * A failed directory check leaves ready=false and get/list return INVALID_STATE;
 * status itself remains available. revision starts at 1 and never wraps.
 * INVALID_ARG=bad arguments; INVALID_SIZE=source/index/filesystem name limit;
 * NOT_SUPPORTED=nonregular target; NOT_FOUND=missing file;
 * INVALID_STATE=CAS/revision conflict or recovery needed; NO_MEM=allocation or
 * capacity reserve unavailable. Other errors describe platform I/O failures.
 */
esp_err_t ryz_script_store_init(const char *base_path,
                               ryz_script_store_mutation_t *out_recovery);
bool ryz_script_store_valid_name(const char *name);
/** Validate immutable source bytes and hash them; independent of Store init/lock. */
esp_err_t ryz_script_store_source_sha256(const char *source, size_t length,
                                        char out_sha256[65]);
/** Copy-only, no filesystem calls. Capacity is refreshed by init/mutate/recover. */
esp_err_t ryz_script_store_status(ryz_script_store_status_t *out_status);
/** Worker-side refresh after other trusted users of the same SPIFFS volume
 * change files. Does not change the script index/revision or run recovery.
 * status() remains a copy-only, non-I/O operation for UI callers. */
esp_err_t ryz_script_store_refresh_capacity(void);
/** ESP volume-wide descriptor-fault gate shared with app data storage.
 * A failed close can leak a VFS descriptor; only reboot clears this gate. */
bool ryz_script_store_io_healthy(void);
void ryz_script_store_latch_io_fault(void);
esp_err_t ryz_script_store_recover(ryz_script_store_mutation_t *out_result);

/**
 * Stable ASCII strcmp order. limit=1..PAGE_MAX; expected_revision=0 accepts
 * the current index, otherwise a changed revision returns INVALID_STATE.
 * More than INDEX_MAX valid regular scripts returns INVALID_SIZE, not a
 * silently truncated catalog. Nonregular entries are not listed.
 */
esp_err_t ryz_script_store_list(size_t offset, size_t limit,
                               uint32_t expected_revision,
                               ryz_script_store_page_t *out_page);
/** Regular, nonempty, NUL-free 1..16384 byte source; owns an immutable copy. */
esp_err_t ryz_script_store_get(const char *name,
                              ryz_script_store_snapshot_t *out_snapshot);
/** Raw regular-file identity, including empty/NUL/oversize files, no source
 * allocation. Streaming SHA permits safe CAS repair/removal of invalid apps. */
esp_err_t ryz_script_store_describe(const char *name,
                                   ryz_script_store_description_t *out_description);
void ryz_script_store_snapshot_free(ryz_script_store_snapshot_t *snapshot);

/**
 * previous_sha256: NULL=unconditional; ""=must not exist; 64 lowercase hex=
 * compare the old source SHA. Mismatch returns INVALID_STATE. boot.lua is an
 * ordinary user file; system diagnostics must not reside in this namespace.
 * Names follow 1..36 [A-Za-z0-9_-] plus .lua. An underlying filesystem may
 * have a LOWER name limit; its rejection is returned, never shortened.
 *
 * Always inspect out_result, including on error: COMMITTED with a cleanup
 * error means the visible mutation happened and must NOT be blindly retried;
 * COMMIT_UNKNOWN requires recovery before its outcome is known. Logical
 * commit/verified recovery is NOT a guarantee of SPIFFS power-loss atomicity.
 * Trusted timestamps are journaled with the content; uncertain recovery hides
 * timestamps. A committed source with a timestamp/cleanup failure still returns
 * an error and requires recovery. No synthetic application metadata is supplied.
 */
esp_err_t ryz_script_store_put(const char *name, const char *source, size_t length,
                              const char *previous_sha256,
                              ryz_script_store_mutation_t *out_result);
esp_err_t ryz_script_store_remove(const char *name,
                                 const char *previous_sha256,
                                 ryz_script_store_mutation_t *out_result);

/** Copy the selected, valid source to user boot.lua under ONE Store lock.
 * expected_revision must be nonzero and current; source_sha256 must exactly
 * match the source read in that transaction. A self-copy or identical target
 * is COMMITTED without writes or a revision increment. No Lua is executed and
 * no autorun/restart policy is established by this storage-only operation. */
esp_err_t ryz_script_store_copy_boot(const char *source_name,
                                    const char *source_sha256,
                                    uint32_t expected_revision,
                                    ryz_script_store_mutation_t *out_result);

/** Delete the current <=INDEX_MAX valid-name regular Lua files, including
 * user boot.lua and empty/NUL/oversize sources. A nonzero matching revision is
 * mandatory. Freeze the sorted target list and execute individually journaled
 * removes under ONE lock. First error/unknown/cleanup failure stops the batch;
 * confirmed commits remain deleted, including a COMMITTED + error final item.
 * complete concerns visible deletion, NOT clean recovery or batch atomicity.
 * Cancellation before this call has no effects; after admission no implicit
 * cancellation/rollback occurs. Reboot recovers only the in-flight single-file
 * journal and NEVER resumes the remaining batch. Inspect every output on error.
 * Run on a filesystem worker, not the UI Owner. Callers separately exclude
 * application starts and other user work while this operation is executing. */
esp_err_t ryz_script_store_delete_all(uint32_t expected_revision,
                                     ryz_script_store_bulk_t *out_result);

#ifdef __cplusplus
}
#endif
