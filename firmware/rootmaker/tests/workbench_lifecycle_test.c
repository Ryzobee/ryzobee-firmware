#include "workbench_lifecycle.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum {
    STOP_BASELINE_MS = 173,
    STOP_DEADLINE_MS = 500,
};

typedef struct {
    bool active;
} job_t;

typedef struct {
    unsigned now_ms;
    unsigned published_ms;
    unsigned render_calls;
    char order[3];
    size_t steps;
    bool ui_state_restored;
    bool render_dirty;
    bool reply_after_restore;
    bool next_job_accepted_from_reply;
    job_t first;
    job_t second;
    job_t *current;
    job_t *recent;
} fixture_t;

static void record(fixture_t *fixture, char step)
{
    if (fixture->steps < sizeof(fixture->order) - 1) {
        fixture->order[fixture->steps++] = step;
        fixture->order[fixture->steps] = '\0';
    }
}

static bool start_job(fixture_t *fixture, job_t *job)
{
    if (fixture->current) return false;
    job->active = true;
    fixture->current = job;
    return true;
}

static bool fixture_has_pending_job(void *context)
{
    fixture_t *fixture = context;
    return fixture->current != NULL;
}

static void restore_ui_state(void *context)
{
    fixture_t *fixture = context;
    record(fixture, 'R');
    fixture->ui_state_restored = true;
    fixture->render_dirty = true;
    /* The synchronous completion path must not paint the LCD. */
}

static void publish_completion(void *context)
{
    fixture_t *fixture = context;
    record(fixture, 'P');
    fixture->published_ms = fixture->now_ms;
    fixture->reply_after_restore = fixture->ui_state_restored;
    fixture->current->active = false;
    fixture->recent = fixture->current;
    fixture->current = NULL;
    /* A completion reply/event must once again mean the next request can run. */
    fixture->next_job_accepted_from_reply =
        start_job(fixture, &fixture->second);
}

int main(void)
{
    fixture_t fixture = {
        .now_ms = STOP_BASELINE_MS,
    };
    if (!start_job(&fixture, &fixture.first)) return 1;
    const ryz_workbench_completion_t completion = {
        .context = &fixture,
        .restore_owner_state = restore_ui_state,
        .publish_completion = publish_completion,
    };
    ryz_workbench_complete_job(&completion);

    ryz_workbench_render_guard_t guard = {
        .context = &fixture,
        .pending_job = fixture_has_pending_job,
    };
    const bool deferred_render_cancelled =
        ryz_workbench_cancel_render(&guard);
    const bool deferred_render_should_yield =
        ryz_workbench_render_was_cancelled(&guard, true);
    const bool drain_timeout_should_yield =
        ryz_workbench_render_was_cancelled(&guard, false);

    if (strcmp(fixture.order, "RP") != 0 ||
        !fixture.reply_after_restore ||
        !fixture.next_job_accepted_from_reply ||
        fixture.current != &fixture.second || fixture.recent != &fixture.first ||
        fixture.render_calls != 0 || !fixture.render_dirty ||
        !deferred_render_cancelled || !guard.cancel_observed ||
        !deferred_render_should_yield || drain_timeout_should_yield ||
        fixture.published_ms >= STOP_DEADLINE_MS) {
        fprintf(stderr,
                "WORKBENCH_LIFECYCLE_FAIL order=%s reply_after_restore=%d "
                "next_accepted=%d renders=%u published_ms=%u\n",
                fixture.order, fixture.reply_after_restore,
                fixture.next_job_accepted_from_reply, fixture.render_calls,
                fixture.published_ms);
        return 1;
    }

    fixture.current = NULL;
    guard.cancel_observed = false;
    if (ryz_workbench_cancel_render(&guard) || guard.cancel_observed) {
        fputs("WORKBENCH_LIFECYCLE_FAIL idle render was cancelled\n", stderr);
        return 1;
    }
    puts("WORKBENCH_LIFECYCLE_PASS: restore state, publish ready, defer LCD render");
    return 0;
}
