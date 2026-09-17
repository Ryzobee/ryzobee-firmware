#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ryz_script_metadata.h"
#include "ryz_script_store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { RYZ_APPS_VIEW_NONE, RYZ_APPS_VIEW_CATALOG,
               RYZ_APPS_VIEW_DETAIL } ryz_apps_view_t;
typedef enum { RYZ_APPS_IDLE, RYZ_APPS_LOADING, RYZ_APPS_READY,
               RYZ_APPS_EMPTY, RYZ_APPS_FAILED, RYZ_APPS_STALE } ryz_apps_state_t;

typedef struct {
    ryz_script_store_description_t file;
    ryz_script_metadata_t metadata;
    bool source_valid; /* Only size/NUL validity, NOT Lua syntax or safety. */
    esp_err_t source_error;
    uint32_t revision;
} ryz_apps_detail_t;

typedef struct {
    uint64_t request_id;
    uint64_t completed_id;
    ryz_apps_view_t view;
    ryz_apps_state_t state;
    esp_err_t error;
    bool store_status_valid; /* False means unknown, not offline. */
    bool store_ready;
    bool recovery_required;
    /* Meaningful only for READY/EMPTY. Never run/remove from LOADING/STALE
     * data. No source bytes, filesystem pointers or fabricated timestamps. */
    ryz_script_store_page_t page;
    ryz_apps_detail_t detail;
} ryz_apps_snapshot_t;

/** Start one boot-lifetime storage reader. Task-creation failure is retryable;
 * success means the reader exists, not that storage is ready. No UI, display,
 * Lua execution or filesystem mutation is performed by this Module. */
esp_err_t ryz_apps_start(void);

/** These calls only copy bounded request/cache data under a short lock. They
 * never wait for filesystem work. One in-flight and one latest desired read;
 * newer requests supersede older ones, whose results are discarded.
 * Request IDs are nonzero, boot-local, never wrap. The UI must ALSO compare
 * its navigation token before using results; this reader never navigates.
 *
 * Page: limit=1..STORE_PAGE_MAX, offset<=STORE_INDEX_MAX, revision=0 for a
 * fresh page or the prior page revision for pagination. A revision mismatch
 * is STALE (not an empty page). Explicit out-of-range offsets fail; automatic
 * refresh after deletion may return offset 0 if the old page no longer exists.
 * Detail: a valid name and nonzero revision from the displayed catalog.
 * Successful invalid-source details retain raw identity for later CAS repair.
 * Startup/task errors and bad args are synchronous; read errors are async.
 */
esp_err_t ryz_apps_request_page(size_t offset, size_t limit,
                               uint32_t expected_revision, uint64_t *out_id);
esp_err_t ryz_apps_request_detail(const char *name, uint32_t expected_revision,
                                 uint64_t *out_id);

/** Invalidate the current view immediately, even if a read is in flight.
 * Does not stop a filesystem call already running; its result is discarded. */
esp_err_t ryz_apps_close(void);

/** Copy-only. All non-OK returns clear a non-NULL output. While open, the
 * reader observes Store status without rescanning each UI frame. Changed
 * revisions refresh catalogs automatically, but invalidate selected details
 * until explicitly reselected: a changed file must never be silently armed.
 * An unready/recovery state invalidates data even at the same revision.
 * Errors do not auto-spin on storage: refresh/reselect explicitly retries. */
esp_err_t ryz_apps_get_snapshot(ryz_apps_snapshot_t *out);

#ifdef __cplusplus
}
#endif
