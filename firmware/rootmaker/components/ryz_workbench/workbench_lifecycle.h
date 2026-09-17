#pragma once

#include <stdbool.h>

typedef void (*ryz_workbench_lifecycle_step_t)(void *context);
typedef bool (*ryz_workbench_pending_job_fn)(void *context);

typedef struct {
    void *context;
    ryz_workbench_lifecycle_step_t restore_owner_state;
    ryz_workbench_lifecycle_step_t publish_completion;
} ryz_workbench_completion_t;

typedef struct {
    void *context;
    ryz_workbench_pending_job_fn pending_job;
    bool cancel_observed;
} ryz_workbench_render_guard_t;

void ryz_workbench_complete_job(const ryz_workbench_completion_t *completion);
bool ryz_workbench_cancel_render(void *context);
bool ryz_workbench_render_was_cancelled(
    const ryz_workbench_render_guard_t *guard, bool cancelled_out);
