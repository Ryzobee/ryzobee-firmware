#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "ryz_script_store.h"
#include "workbench_write_guard.h"

/* Borrows the immutable, SHA-checked snapshot only for the duration of the
 * call. The runtime must copy/adopt these bytes, never reopen by name, and
 * repeat final owner admission. A false reply guarantees no job was started;
 * NULL is an unknown acknowledgement, not permission to retry. */
typedef cJSON *(*ryz_workbench_run_script_fn)(
    void *context, const char *id, const ryz_script_store_snapshot_t *snapshot);

/* Serial Adapter for the shared script Store. Existing list/get/put/remove
 * success replies retain their wire shape. The scripts operation is separately
 * versioned and scoped by boot_id, not additions to the old list entries.
 *
 * The transport must reject embedded JSON NULs before this call: cJSON strings
 * are NUL-terminated and cannot represent their original encoded byte length.
 * This synchronous call performs storage I/O; never call it from the UI Owner.
 * All legacy/versioned put/remove calls require a complete write guard. The
 * basic reply envelope and parameters are prepared before begin; one successful
 * begin is paired with one end after Store mutation and before result JSON.
 * Reads and prepared runs never take this lease and retain runtime_busy policy.
 * The returned JSON is caller-owned; NULL means reply allocation failed.
 */
bool ryz_workbench_file_rpc_supports(const char *operation);
cJSON *ryz_workbench_file_rpc(const cJSON *request, const char *boot_id,
                             bool runtime_busy, ryz_workbench_run_script_fn run,
                             void *run_context, const ryz_workbench_write_guard_t *guard);
