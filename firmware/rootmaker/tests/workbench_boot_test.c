/* Real boot gate + Job admission + Store source validation. No Lua execution,
 * OTA flash operation, filesystem initialization or device I/O is performed. */
#include "workbench_boot.h"
#include "workbench_job_start.h"
#include "ryz_script_store.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    pthread_mutex_t mutex;
    bool locked;
    ryz_workbench_boot_result_t boot;
    esp_err_t confirm_error;
    unsigned confirmations, ready_calls, enqueues, notifications, next_job;
    script_job_t *current, *queued;
} fixture_t;

static esp_err_t confirm_image(void *context)
{
    fixture_t *fixture = context;
    fixture->confirmations++;
    return fixture->confirm_error;
}

static void lock(void *context)
{
    fixture_t *fixture = context;
    assert(pthread_mutex_lock(&fixture->mutex) == 0);
    assert(!fixture->locked);
    fixture->locked = true;
}

static void unlock(void *context)
{
    fixture_t *fixture = context;
    assert(fixture->locked);
    fixture->locked = false;
    assert(pthread_mutex_unlock(&fixture->mutex) == 0);
}

static bool ready(void *context)
{
    fixture_t *fixture = context;
    assert(fixture->locked);
    fixture->ready_calls++;
    return fixture->boot.runtime_ready;
}

static bool enqueue(void *context, script_job_t *job)
{
    fixture_t *fixture = context;
    assert(fixture->locked && fixture->current == job);
    assert(fixture->queued == NULL);
    fixture->enqueues++;
    fixture->queued = job;
    return true;
}

static void notify(void *context)
{
    fixture_t *fixture = context;
    assert(!fixture->locked && fixture->current == fixture->queued);
    fixture->notifications++;
}

static int64_t now_us(void *context)
{
    (void)context;
    return INT64_C(1000000);
}

static void initialize(fixture_t *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    assert(pthread_mutex_init(&fixture->mutex, NULL) == 0);
}

static cJSON *start(fixture_t *fixture, bool legacy)
{
    ryz_workbench_job_start_context_t context = {
        .context = fixture, .boot_id = "boot-fixture",
        .next_job = &fixture->next_job, .current = &fixture->current,
        .lock = lock, .unlock = unlock, .ready = ready,
        .enqueue = enqueue, .notify = notify, .now_us = now_us,
    };
    char *source = strdup("print('boot ready')");
    assert(source != NULL);
    return ryz_workbench_job_start(&context, "request", "demo.lua", source,
                                   0U, legacy);
}

static ryz_workbench_boot_inputs_t all_ready(void)
{
    return (ryz_workbench_boot_inputs_t){
        .services_ready = true, .font_ready = true, .touch_ready = true,
        .ui_active = true, .render_dirty = false,
    };
}

static void assert_rejected(fixture_t *fixture, bool legacy)
{
    const unsigned ready_before = fixture->ready_calls;
    cJSON *reply = start(fixture, legacy);
    assert(fixture->current == NULL && fixture->queued == NULL);
    assert(fixture->enqueues == 0U && fixture->notifications == 0U);
    assert(fixture->next_job == 0U && fixture->ready_calls == ready_before + 1U);
    assert(reply != NULL && cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(reply, "ok")));
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(reply, "error");
    assert(cJSON_IsString(error) && strstr(error->valuestring, "runtime unavailable"));
    cJSON_Delete(reply);
}

static void release_job(fixture_t *fixture)
{
    assert(fixture->current != NULL && fixture->queued == fixture->current);
    script_job_t *job = fixture->current;
    fixture->current = NULL;
    fixture->queued = NULL;
    ryz_workbench_job_destroy(job);
}

static void failed_confirmation_blocks_job(void)
{
    fixture_t fixture;
    initialize(&fixture);
    fixture.confirm_error = ESP_FAIL;
    ryz_workbench_boot_t state = {0};
    ryz_workbench_boot_inputs_t inputs = all_ready();
    fixture.boot = ryz_workbench_boot_step(&state, &inputs, INT64_C(1000000),
                                           confirm_image, &fixture);
    assert(fixture.confirmations == 1U && fixture.boot.attempted);
    assert(fixture.boot.error == ESP_FAIL && !state.confirmed);
    cJSON *reply = start(&fixture, false);
    /* Tracer: the former services-only gate publishes a real Job here. */
    assert(fixture.current == NULL && fixture.queued == NULL);
    assert(fixture.enqueues == 0U && fixture.notifications == 0U);
    assert(fixture.next_job == 0U && fixture.ready_calls == 1U);
    assert(reply != NULL && cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(reply, "ok")));
    assert(!fixture.boot.runtime_ready);
    cJSON_Delete(reply);
    assert_rejected(&fixture, true);
    assert(pthread_mutex_destroy(&fixture.mutex) == 0);
}

static void initialization_gates(void)
{
    /* A running service task is not a completed service initialization. The
     * public decision deliberately consumes services_ready, not task status. */
    for (unsigned missing = 0U; missing < 5U; ++missing) {
        fixture_t fixture;
        initialize(&fixture);
        ryz_workbench_boot_t state = {0};
        ryz_workbench_boot_inputs_t inputs = all_ready();
        switch (missing) {
        case 0: inputs.services_ready = false; break;
        case 1: inputs.font_ready = false; break;
        case 2: inputs.touch_ready = false; break;
        case 3: inputs.ui_active = false; break;
        case 4: inputs.render_dirty = true; break;
        default: assert(false);
        }
        fixture.boot = ryz_workbench_boot_step(&state, &inputs, INT64_C(1000000),
                                               confirm_image, &fixture);
        assert(!fixture.boot.runtime_ready && !fixture.boot.attempted);
        assert(fixture.boot.error == ESP_OK && !state.confirmed);
        assert(state.retry_at_us == 0 && fixture.confirmations == 0U);
        assert_rejected(&fixture, false);
        assert_rejected(&fixture, true);

        inputs = all_ready();
        fixture.boot = ryz_workbench_boot_step(&state, &inputs, INT64_C(1000001),
                                               confirm_image, &fixture);
        assert(fixture.boot.runtime_ready && fixture.boot.attempted);
        assert(fixture.boot.error == ESP_OK && state.confirmed);
        assert(fixture.confirmations == 1U);
        assert(pthread_mutex_destroy(&fixture.mutex) == 0);
    }
}

static void retry_and_recovery(void)
{
    fixture_t fixture;
    initialize(&fixture);
    ryz_workbench_boot_t state = {0};
    ryz_workbench_boot_inputs_t inputs = all_ready();
    fixture.confirm_error = ESP_ERR_TIMEOUT;
    fixture.boot = ryz_workbench_boot_step(&state, &inputs, INT64_C(1000000),
                                           confirm_image, &fixture);
    assert(!state.confirmed && state.retry_at_us == INT64_C(6000000));
    assert(fixture.boot.attempted && fixture.boot.error == ESP_ERR_TIMEOUT);
    assert(!fixture.boot.runtime_ready && fixture.confirmations == 1U);
    assert_rejected(&fixture, false);

    const int64_t early[] = {INT64_C(1000000), INT64_C(1000001), INT64_C(5999999)};
    for (size_t index = 0U; index < sizeof(early) / sizeof(early[0]); ++index) {
        fixture.boot = ryz_workbench_boot_step(&state, &inputs, early[index],
                                               confirm_image, &fixture);
        assert(!fixture.boot.runtime_ready && !fixture.boot.attempted);
        assert(fixture.confirmations == 1U && !state.confirmed);
        assert(state.retry_at_us == INT64_C(6000000));
        assert_rejected(&fixture, index % 2U == 0U);
    }

    fixture.confirm_error = ESP_FAIL;
    fixture.boot = ryz_workbench_boot_step(&state, &inputs, INT64_C(6000000),
                                           confirm_image, &fixture);
    assert(fixture.boot.attempted && fixture.boot.error == ESP_FAIL);
    assert(!fixture.boot.runtime_ready && fixture.confirmations == 2U);
    assert(state.retry_at_us == INT64_C(11000000));
    assert_rejected(&fixture, true);

    fixture.confirm_error = ESP_OK;
    fixture.boot = ryz_workbench_boot_step(&state, &inputs, INT64_C(10999999),
                                           confirm_image, &fixture);
    assert(!fixture.boot.runtime_ready && !fixture.boot.attempted);
    assert(fixture.confirmations == 2U);
    assert_rejected(&fixture, false);
    /* A retry deadline alone does not bypass the screen-ready condition. */
    inputs.render_dirty = true;
    fixture.boot = ryz_workbench_boot_step(&state, &inputs, INT64_C(11000000),
                                           confirm_image, &fixture);
    assert(!fixture.boot.runtime_ready && !fixture.boot.attempted);
    assert(fixture.confirmations == 2U && state.retry_at_us == INT64_C(11000000));
    inputs.render_dirty = false;
    fixture.boot = ryz_workbench_boot_step(&state, &inputs, INT64_C(11000001),
                                           confirm_image, &fixture);
    assert(fixture.boot.runtime_ready && fixture.boot.attempted && state.confirmed);
    assert(fixture.boot.error == ESP_OK && fixture.confirmations == 3U);
    cJSON *reply = start(&fixture, false);
    assert(reply != NULL && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply, "ok")));
    assert(fixture.next_job == 1U && fixture.enqueues == 1U && fixture.notifications == 1U);
    cJSON_Delete(reply);
    release_job(&fixture);
    assert(pthread_mutex_destroy(&fixture.mutex) == 0);
}

static void confirmed_admission_survives_lua_display(void)
{
    fixture_t fixture;
    initialize(&fixture);
    ryz_workbench_boot_t state = {0};
    ryz_workbench_boot_inputs_t inputs = all_ready();
    /* No network/ONLINE property exists in this contract: offline but ready
     * services are sufficient. This does not simulate service initialization. */
    fixture.boot = ryz_workbench_boot_step(&state, &inputs, 0, confirm_image, &fixture);
    assert(fixture.boot.attempted && fixture.boot.runtime_ready && state.confirmed);
    assert(fixture.boot.error == ESP_OK && fixture.confirmations == 1U);
    char source_sha[65];
    const char *source = "print('boot ready')";
    assert(ryz_script_store_source_sha256(source, strlen(source), source_sha) == ESP_OK);
    cJSON *reply = start(&fixture, false);
    assert(reply != NULL && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply, "ok")));
    const cJSON *job = cJSON_GetObjectItemCaseSensitive(reply, "job");
    assert(cJSON_IsObject(job));
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(job, "job_id")),
                  "boot-fixture-1") == 0);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(job, "sha256")),
                  source_sha) == 0);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(job, "state")),
                  "running") == 0);
    assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(job, "stop_requested")));
    assert(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(job, "elapsed_ms")));
    assert(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(job, "log_next_seq")));
    assert(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(job, "dropped_bytes")));
    assert(fixture.current != NULL && !fixture.current->legacy);
    assert(strcmp(fixture.current->source, source) == 0);
    assert(strcmp(fixture.current->reply_id, "request") == 0);
    assert(fixture.current->timeout_ms == 0U && fixture.current->started_us == INT64_C(1000000));
    assert(!atomic_load(&fixture.current->cancel) && fixture.current->active);
    cJSON_Delete(reply);

    /* Lua may now own the display. This milestone must not be undone, and the
     * confirmation callback must not run again, even if it would now fail. */
    fixture.confirm_error = ESP_FAIL;
    inputs = (ryz_workbench_boot_inputs_t){.render_dirty = true};
    for (unsigned pass = 0U; pass < 3U; ++pass) {
        fixture.boot = ryz_workbench_boot_step(&state, &inputs,
            INT64_C(1000000) * (int64_t)(pass + 1U), confirm_image, &fixture);
        assert(fixture.boot.runtime_ready && !fixture.boot.attempted);
        assert(fixture.boot.error == ESP_OK && fixture.confirmations == 1U);
    }
    /* An already active Job remains independently protected by job_start. */
    reply = start(&fixture, true);
    assert(reply != NULL && cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(reply, "ok")));
    assert(fixture.next_job == 1U && fixture.enqueues == 1U);
    cJSON_Delete(reply);
    release_job(&fixture);

    reply = start(&fixture, true);
    assert(reply == NULL); /* Legacy success replies only after execution. */
    assert(fixture.current != NULL && fixture.current == fixture.queued);
    assert(fixture.current->legacy && strcmp(fixture.current->id, "boot-fixture-2") == 0);
    assert(strcmp(fixture.current->source, source) == 0);
    assert(strcmp(fixture.current->sha256, source_sha) == 0);
    assert(fixture.current->timeout_ms == 0U && fixture.current->active);
    assert(fixture.next_job == 2U && fixture.enqueues == 2U && fixture.notifications == 2U);
    assert(fixture.ready_calls == 2U && fixture.confirmations == 1U);
    release_job(&fixture);
    assert(pthread_mutex_destroy(&fixture.mutex) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "failed_confirmation") == 0) failed_confirmation_blocks_job();
    else if (strcmp(argv[1], "gates") == 0) initialization_gates();
    else if (strcmp(argv[1], "retry") == 0) retry_and_recovery();
    else if (strcmp(argv[1], "confirmed") == 0) confirmed_admission_survives_lua_display();
    else assert(!"unknown test case");
    printf("WORKBENCH_BOOT_PASS %s\n", argv[1]);
    return 0;
}
