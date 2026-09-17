#pragma once

#include "ryz_system_services.h"

/* Private single-owner state. Zero-initialize once in the existing system
 * task. No additional task, mutex, task-name list or boot-health dependency. */
typedef struct {
    bool attempted;
    bool baseline_valid;
    int64_t attempted_at_us;
    uint64_t baseline_at_us[2];
    uint64_t idle_us[2];
    ryz_system_metrics_snapshot_t snapshot;
} ryz_system_metrics_t;

/* Called by the system task; samples at most once per monotonic second.
 * A backward clock invalidates/rebases LOAD immediately. Copies on skipped
 * cycles preserve the actual sample timestamp rather than rejuvenating it. */
void ryz_system_metrics_poll(ryz_system_metrics_t *state,
                             ryz_system_metrics_snapshot_t *out);
