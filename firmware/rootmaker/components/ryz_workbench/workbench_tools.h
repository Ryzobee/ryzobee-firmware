#pragma once

#include "cJSON.h"
#include "ryz_tools.h"

#define RYZ_WORKBENCH_TOOLS_RETIRED_MAX (RYZ_TOOL_COUNT + 1)
#define RYZ_WORKBENCH_TOOLS_OBSERVATION_US 500000

typedef enum {
    RYZ_WB_TOOLS_UNUSED, RYZ_WB_TOOLS_ACTIVE, RYZ_WB_TOOLS_PENDING,
    RYZ_WB_TOOLS_FAILED, RYZ_WB_TOOLS_CLEAN,
} ryz_workbench_tools_phase_t;
typedef struct {
    uint32_t session_id;
    ryz_workbench_tools_phase_t phase;
    bool observation_valid, timed_out, retirement_recorded;
    uint8_t remaining_mask, pending_mask, failed_mask;
    esp_err_t error, errors[RYZ_TOOL_COUNT];
    int64_t retired_us, observed_us;
} ryz_workbench_tools_report_t;
/* Worker-owned until completion queue handoff. Never read this mutable object
 * from another task while Lua runs; use the synchronized report lookup. */
typedef struct {
    char job_id[32];
    uint32_t session_id;
    bool finished;
    ryz_workbench_tools_report_t final;
} ryz_workbench_tools_job_t;

void ryz_workbench_tools_begin(ryz_workbench_tools_job_t *job, const char *job_id);
int ryz_workbench_tools_call(ryz_workbench_tools_job_t *job,
    const ryz_tool_request_t *request, ryz_tool_reply_t *reply);
/* One non-waiting close pass. Retention is reserved BEFORE first broker open;
 * therefore finish cannot lose a session when the broker gate is busy.
 * False means our observation gate was busy: the worker must yield and retry
 * BEFORE publishing its job. No JSON, driver waits, or caller callbacks. */
bool ryz_workbench_tools_finish(ryz_workbench_tools_job_t *job, int64_t now_us);
/* Existing RX task calls this even with no traffic. At most four progress
 * passes; failed commands are never automatically retried. 500ms only marks
 * an observation timeout; pending sessions remain tracked and keep polling. */
void ryz_workbench_tools_tick(int64_t now_us);
/* Pure bounded cached copy under a try-once gate. final is optional and only
 * legal after queue handoff. An absent former retired session means confirmed
 * clean only when retirement_recorded is true; only successful close removes
 * a tracking record. Missing-record invariant failures never become clean. */
esp_err_t ryz_workbench_tools_report(const char *job_id,
    const ryz_workbench_tools_report_t *final, ryz_workbench_tools_report_t *out);
bool ryz_workbench_tools_clean(const ryz_workbench_tools_report_t *report);
/* Additive diagnostics: caller must check NULL. No native/broker calls. */
cJSON *ryz_workbench_tools_report_json(const ryz_workbench_tools_report_t *report);
/* tools status is read-only and boot optional. retry_close requires exact
 * boot_id + existing retired numeric session_id. Full ACK is allocated before
 * retry mutation; no JSON operation occurs under the admission gate. */
cJSON *ryz_workbench_tools_rpc(const cJSON *request, const char *boot_id);
