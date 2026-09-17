#include "workbench_lifecycle.h"
#include <stddef.h>

void ryz_workbench_complete_job(const ryz_workbench_completion_t *completion)
{
    if (!completion || !completion->restore_owner_state ||
        !completion->publish_completion) {
        return;
    }
    completion->restore_owner_state(completion->context);
    completion->publish_completion(completion->context);
}

bool ryz_workbench_cancel_render(void *context)
{
    ryz_workbench_render_guard_t *guard = context;
    if (!guard || !guard->pending_job) return false;
    if (!guard->pending_job(guard->context)) return false;
    guard->cancel_observed = true;
    return true;
}

bool ryz_workbench_render_was_cancelled(
    const ryz_workbench_render_guard_t *guard, bool cancelled_out)
{
    return guard && guard->cancel_observed && cancelled_out;
}
