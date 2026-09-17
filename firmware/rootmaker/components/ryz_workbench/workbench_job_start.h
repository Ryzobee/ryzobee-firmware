#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "lua_runtime.h"
#include "workbench_tools.h"

#define RYZ_WORKBENCH_JOB_LOG_BYTES 4096

/* Large immutable/result buffers do not participate in cross-task atomics.
 * ESP builds keep this payload in PSRAM so retaining one completed Job cannot
 * consume the internal heap needed to admit its successor. */
typedef struct {
    char log[RYZ_WORKBENCH_JOB_LOG_BYTES];
    ryz_lua_result_t result;
} script_job_payload_t;

/* Workbench-private job ownership. Control fields and the cancellation atomic
 * remain in internal RAM. The source is an independently owned PSRAM copy,
 * not a borrowed Store snapshot. Only the runtime owner may destroy a
 * published job. All source allocations remain compatible with free(). */
typedef struct {
    char id[32], name[41], sha256[65];
    char *source, *reply_id;
    script_job_payload_t *payload;
    uint32_t timeout_ms;
    int64_t started_us;
    atomic_bool cancel;
    bool active, legacy;
    size_t log_length;
    uint32_t sequence, dropped_bytes;
    /* Worker-only until completion queue handoff; active diagnostics use the
     * synchronized Workbench tools cache, never these mutable fields. */
    ryz_workbench_tools_job_t tools;
} script_job_t;

typedef struct {
    void *context;
    const char *boot_id;
    unsigned *next_job;
    script_job_t **current;
    void (*lock)(void *context);
    void (*unlock)(void *context);
    bool (*ready)(void *context);
    /* Under lock. false guarantees no job was published to the queue. */
    bool (*enqueue)(void *context, script_job_t *job);
    void (*notify)(void *context);
    int64_t (*now_us)(void *context);
} ryz_workbench_job_start_context_t;

/* Private immutable CPU-only source ownership boundary. The caller supplies
 * the verified byte length excluding NUL. ESP uses only PSRAM (no internal
 * fallback); Host uses malloc. The returned NUL-terminated copy belongs to
 * the caller until job admission consumes it, and is released with free(). */
char *ryz_workbench_source_copy(const char *source, size_t length);

/* Always consumes independently owned source, including on rejection. Complete ACK
 * allocation and final busy/gateway checks precede queue publication. A NULL
 * reply is allocation failure or, for legacy success, reply-on-completion.
 * Locks guard current/counter as well as enqueue. No filesystem re-read. */
cJSON *ryz_workbench_job_start(const ryz_workbench_job_start_context_t *context,
                              const char *id, const char *name, char *source,
                              uint32_t timeout_ms, bool legacy);
void ryz_workbench_job_destroy(script_job_t *job);
