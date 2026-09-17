#pragma once

#include "ryz_i2c_scan.h"
#include "ryz_monitor.h"
#include "ryz_rgb.h"
#include "ryz_tool_call.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t session_id;
    uint8_t remaining_mask, pending_mask, failed_mask;
    esp_err_t errors[RYZ_TOOL_COUNT];
} ryz_tools_close_result_t;

typedef struct {
    uint32_t current_session;
    struct {
        uint32_t owner_session, operation_id;
        bool closing, cleanup_requested, failed;
        esp_err_t cleanup_error;
    } tools[RYZ_TOOL_COUNT];
} ryz_tools_snapshot_t;

/* C-owned per-job session. One current caller, fixed three tool slots; no
 * allocation, worker or hardware start. IDs never wrap. A retired session
 * with failed cleanup may retain slots while another ordinary job runs.
 * Never derive privilege from a factory filename or accept this ID from Lua. */
esp_err_t ryz_tools_open(uint32_t *out_session);
/* The first valid call on a tool claims its software lease only after its
 * core is inactive AND has no held resources, except healthy completed RGB
 * allocations may be parked and reused by the next successful submit. A
 * status-only caller never owns that historical request/cleanup. Even status claims a lease;
 * it cannot expose/adopt another controller's active capture. Cached status
 * before core startup returns started=false and default configuration.
 * All error outputs are cleared. A successful admission records the exact
 * native operation/session ID before releasing the shared write gate. */
esp_err_t ryz_tools_call(uint32_t session, const ryz_tool_request_t *request,
                         ryz_tool_reply_t *out_reply);

/* Revoke further calls, then make one bounded cleanup progress pass. This
 * only submits native cancellation/cleanup and reads caches, never waits on
 * hardware. OK means no owned slots remain; NOT_FINISHED means pending;
 * FAIL means explicit failed cleanup remains. NOT_FINISHED with session_id=0
 * means the admission gate was busy and no close progress was made.
 * Call again to observe pending
 * work. Failed work is not automatically resubmitted. Output remains valid
 * for these three outcomes; other failures clear it.
 * A caller MUST arrange continued polling after an observation timeout and
 * preserve/report remaining ownership. Never publish a false clean job.
 * Old session close can only touch slots still owned by that same session. */
esp_err_t ryz_tools_close(uint32_t session, ryz_tools_close_result_t *out);
/* Explicit trusted recovery, only for a closed/retired session. Resets failed
 * submissions for a later close progress pass; no operation is submitted here. */
esp_err_t ryz_tools_retry_close(uint32_t session);
/* Cached broker ownership, no tool-core calls. NOT_FINISHED on contention. */
esp_err_t ryz_tools_get_snapshot(ryz_tools_snapshot_t *out);

/* Existing trusted RPC writers MUST enter here; readers may read core caches.
 * The callback may only synchronously validate/admit a core command, no JSON,
 * device work or reentry. One non-waiting gate covers check + callback. The
 * original result is returned without a fallible post-success release.
 * Unowned is not proof of native idle; asynchronous activity is checked anew
 * by app claim. Raw C cores remain private to trusted adapters/this Module. */
esp_err_t ryz_tools_external(ryz_tool_t tool, esp_err_t (*admit)(void *), void *context);

#ifdef __cplusplus
}
#endif
