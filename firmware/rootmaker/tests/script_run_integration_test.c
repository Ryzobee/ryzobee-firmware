#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L

/* Reuse the real-runtime Host owner/channel and BSP spies, not a copied Lua
 * implementation. The original test main remains callable but is not run. */
#define main runtime_owner_test_main
#include "runtime_owner_test.c"
#undef main

#include "workbench_file_rpc.h"
#include "workbench_job_start.h"
#include "ryz_script_store.h"
#include "ryz_script_store_platform.h"
#include "script_store_host.h"

#include <limits.h>

static const char integration_boot[] = "run-boot";
static const char selected_source[] = RAW_IO "print('SELECTED_A')";
static const char changed_source[] = "error('CHANGED_B_MUST_NOT_EXECUTE')";

typedef struct {
    pthread_mutex_t mutex;
    pthread_t lock_owner;
    bool locked, gateway_ready, queue_ready, writer_active;
    unsigned next_job, enqueues, notifications, callbacks, callback_acks;
    unsigned write_begins, write_ends, write_rejections;
    pthread_t writer_owner;
    script_job_t *current, *queued;
    ryz_workbench_job_start_context_t start;
    ryz_workbench_write_guard_t guard;
    pthread_mutex_t callback_mutex;
    pthread_cond_t callback_condition;
    bool pause_callback, callback_entered, callback_released;
} launch_fixture_t;

/* Only this executable renames the existing POSIX Host adapter delegate at
 * compile time. Production Store still enters its real platform API; after
 * a bounded rendezvous below, the unchanged Host rename performs the I/O. */
esp_err_t script_run_host_rename(const char *from, const char *to);
static struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool armed, entered, released;
    const char *from, *to;
    launch_fixture_t *launch;
} commit_gate = {.mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER};

static void integration_wait(pthread_cond_t *condition, pthread_mutex_t *mutex)
{
    struct timespec deadline;
    CHECK(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 5;
    CHECK(pthread_cond_timedwait(condition, mutex, &deadline) == 0);
}

esp_err_t ryz_script_store_platform_rename(const char *from, const char *to)
{
    CHECK(pthread_mutex_lock(&commit_gate.mutex) == 0);
    if (commit_gate.armed && strcmp(strrchr(from, '/') + 1, commit_gate.from) == 0 &&
        strcmp(strrchr(to, '/') + 1, commit_gate.to) == 0) {
        commit_gate.armed = false;
        launch_fixture_t *launch = commit_gate.launch;
        /* The writer lease survives Store I/O, but the shared Job mutex must
         * be free so final Job admission and another writer can reject. */
        CHECK(pthread_mutex_trylock(&launch->mutex) == 0);
        CHECK(!launch->locked && launch->writer_active && launch->current == NULL);
        CHECK(launch->write_begins == 1U && launch->write_ends == 0U);
        CHECK(pthread_equal(launch->writer_owner, pthread_self()));
        CHECK(pthread_mutex_unlock(&launch->mutex) == 0);
        commit_gate.entered = true;
        CHECK(pthread_cond_broadcast(&commit_gate.condition) == 0);
        while (!commit_gate.released) integration_wait(&commit_gate.condition, &commit_gate.mutex);
    }
    CHECK(pthread_mutex_unlock(&commit_gate.mutex) == 0);
    return script_run_host_rename(from, to);
}

static void commit_gate_arm(launch_fixture_t *launch, bool putting)
{
    CHECK(pthread_mutex_lock(&commit_gate.mutex) == 0);
    commit_gate.armed = true;
    commit_gate.entered = commit_gate.released = false;
    commit_gate.from = putting ? ".ryz-new" : "picked.lua";
    commit_gate.to = putting ? "picked.lua" : ".ryz-old";
    commit_gate.launch = launch;
    CHECK(pthread_mutex_unlock(&commit_gate.mutex) == 0);
}

static void commit_gate_wait(void)
{
    CHECK(pthread_mutex_lock(&commit_gate.mutex) == 0);
    while (!commit_gate.entered) integration_wait(&commit_gate.condition, &commit_gate.mutex);
    CHECK(pthread_mutex_unlock(&commit_gate.mutex) == 0);
}

static void commit_gate_release(void)
{
    CHECK(pthread_mutex_lock(&commit_gate.mutex) == 0);
    CHECK(commit_gate.entered);
    commit_gate.released = true;
    CHECK(pthread_cond_broadcast(&commit_gate.condition) == 0);
    CHECK(pthread_mutex_unlock(&commit_gate.mutex) == 0);
}

static void launch_lock(void *context)
{
    launch_fixture_t *launch = context;
    CHECK(pthread_mutex_lock(&launch->mutex) == 0);
    CHECK(!launch->locked);
    launch->locked = true;
    launch->lock_owner = pthread_self();
}

static void launch_locked(const launch_fixture_t *launch)
{
    CHECK(launch->locked && pthread_equal(launch->lock_owner, pthread_self()));
}

static void launch_unlock(void *context)
{
    launch_fixture_t *launch = context;
    launch_locked(launch);
    launch->locked = false;
    CHECK(pthread_mutex_unlock(&launch->mutex) == 0);
}

static bool launch_ready(void *context)
{
    launch_fixture_t *launch = context;
    launch_locked(launch);
    return launch->gateway_ready && !launch->writer_active;
}

static bool launch_begin_write(void *context)
{
    launch_fixture_t *launch = context;
    launch_lock(launch);
    bool accepted = launch->current == NULL && !launch->writer_active;
    if (accepted) {
        launch->writer_active = true;
        launch->writer_owner = pthread_self();
        ++launch->write_begins;
    } else ++launch->write_rejections;
    launch_unlock(launch);
    return accepted;
}

static void launch_end_write(void *context)
{
    launch_fixture_t *launch = context;
    launch_lock(launch);
    CHECK(launch->writer_active && pthread_equal(launch->writer_owner, pthread_self()));
    launch->writer_active = false;
    ++launch->write_ends;
    launch_unlock(launch);
}

static bool launch_enqueue(void *context, script_job_t *job)
{
    launch_fixture_t *launch = context;
    launch_locked(launch);
    CHECK(launch->current == job && launch->queued == NULL);
    if (!launch->queue_ready) return false;
    launch->queued = job;
    ++launch->enqueues;
    return true;
}

static void launch_notify(void *context)
{
    launch_fixture_t *launch = context;
    CHECK(pthread_mutex_trylock(&launch->mutex) == 0);
    CHECK(!launch->locked && launch->queued != NULL);
    ++launch->notifications;
    CHECK(pthread_mutex_unlock(&launch->mutex) == 0);
}

static int64_t launch_now(void *context)
{
    CHECK(context != NULL);
    return INT64_C(123456789);
}

static void launch_init(launch_fixture_t *launch)
{
    memset(launch, 0, sizeof(*launch));
    CHECK(pthread_mutex_init(&launch->mutex, NULL) == 0);
    CHECK(pthread_mutex_init(&launch->callback_mutex, NULL) == 0);
    CHECK(pthread_cond_init(&launch->callback_condition, NULL) == 0);
    launch->gateway_ready = true;
    launch->queue_ready = true;
    launch->guard = (ryz_workbench_write_guard_t){
        .context = launch, .try_begin_write = launch_begin_write, .end_write = launch_end_write,
    };
    launch->start = (ryz_workbench_job_start_context_t){
        .context = launch, .boot_id = integration_boot,
        .next_job = &launch->next_job, .current = &launch->current,
        .lock = launch_lock, .unlock = launch_unlock, .ready = launch_ready,
        .enqueue = launch_enqueue, .notify = launch_notify, .now_us = launch_now,
    };
}

static void release_queued(launch_fixture_t *launch)
{
    launch_lock(launch);
    script_job_t *job = launch->queued;
    CHECK(launch->current == job);
    launch->current = NULL;
    launch->queued = NULL;
    launch_unlock(launch);
    ryz_workbench_job_destroy(job);
}

static void launch_destroy(launch_fixture_t *launch)
{
    CHECK(!launch->writer_active && launch->write_begins == launch->write_ends);
    release_queued(launch);
    CHECK(pthread_cond_destroy(&launch->callback_condition) == 0);
    CHECK(pthread_mutex_destroy(&launch->callback_mutex) == 0);
    CHECK(pthread_mutex_destroy(&launch->mutex) == 0);
}

static const cJSON *field(const cJSON *value, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(value, name);
    CHECK(item != NULL);
    return item;
}

static void string_is(const cJSON *value, const char *name, const char *expected)
{
    const cJSON *item = field(value, name);
    CHECK(cJSON_IsString(item) && strcmp(item->valuestring, expected) == 0);
}

static void complete_ack(const cJSON *reply, const script_job_t *job)
{
    CHECK(cJSON_IsObject(reply) && cJSON_IsTrue(field(reply, "ok")));
    string_is(reply, "id", job->reply_id);
    string_is(reply, "output", "Lua job started");
    const cJSON *value = field(reply, "job");
    CHECK(cJSON_IsObject(value));
    string_is(value, "job_id", job->id);
    string_is(value, "name", job->name);
    string_is(value, "sha256", job->sha256);
    string_is(value, "state", "running");
    CHECK(cJSON_IsFalse(field(value, "stop_requested")));
    const char *counters[] = {"elapsed_ms", "log_next_seq", "dropped_bytes"};
    for (size_t i = 0U; i < sizeof(counters) / sizeof(counters[0]); ++i) {
        const cJSON *counter = field(value, counters[i]);
        CHECK(cJSON_IsNumber(counter) && counter->valuedouble == 0);
    }
}

static cJSON *start_snapshot(void *context, const char *id,
                             const ryz_script_store_snapshot_t *snapshot)
{
    launch_fixture_t *launch = context;
    CHECK(snapshot != NULL && snapshot->source != NULL);
    ++launch->callbacks;
    CHECK(pthread_mutex_lock(&launch->callback_mutex) == 0);
    if (launch->pause_callback) {
        launch->callback_entered = true;
        CHECK(pthread_cond_broadcast(&launch->callback_condition) == 0);
        while (!launch->callback_released)
            CHECK(pthread_cond_wait(&launch->callback_condition, &launch->callback_mutex) == 0);
    }
    CHECK(pthread_mutex_unlock(&launch->callback_mutex) == 0);
    char *owned = malloc(snapshot->entry.bytes + 1U);
    CHECK(owned != NULL);
    memcpy(owned, snapshot->source, snapshot->entry.bytes + 1U);
    const unsigned before = launch->enqueues;
    cJSON *reply = ryz_workbench_job_start(&launch->start, id, snapshot->entry.name,
                                          owned, 0U, false);
    if (launch->enqueues != before) {
        CHECK(launch->queued->timeout_ms == 0U);
        complete_ack(reply, launch->queued);
        ++launch->callback_acks;
    }
    return reply;
}

static void save_script(const char *name, const char *source, char sha[65])
{
    ryz_script_store_mutation_t result;
    CHECK(ryz_script_store_put(name, source, strlen(source), NULL, &result) == ESP_OK);
    CHECK(result.commit == RYZ_SCRIPT_STORE_COMMITTED && !result.recovery_required);
    strcpy(sha, result.sha256);
}

static cJSON *run_request(const char *sha)
{
    cJSON *request = cJSON_CreateObject();
    CHECK(request != NULL);
    CHECK(cJSON_AddStringToObject(request, "id", "run-request"));
    CHECK(cJSON_AddStringToObject(request, "op", "scripts"));
    CHECK(cJSON_AddStringToObject(request, "schema", "ryz-script-store/1"));
    CHECK(cJSON_AddStringToObject(request, "boot_id", integration_boot));
    CHECK(cJSON_AddStringToObject(request, "action", "run"));
    CHECK(cJSON_AddStringToObject(request, "name", "picked.lua"));
    CHECK(cJSON_AddStringToObject(request, "sha256", sha));
    return request;
}

static void test_selected_snapshot_executes(void)
{
    launch_fixture_t launch;
    launch_init(&launch);
    char selected_sha[65], changed_sha[65];
    save_script("picked.lua", selected_source, selected_sha);
    cJSON *request = run_request(selected_sha);
    cJSON *reply = ryz_workbench_file_rpc(request, integration_boot, false, start_snapshot, &launch, &launch.guard);
    CHECK(launch.enqueues == 1U && launch.notifications == 1U && launch.callback_acks == 1U);
    complete_ack(reply, launch.queued);
    string_is(reply, "schema", "ryz-script-store/1");
    string_is(reply, "boot_id", integration_boot);
    CHECK(strcmp(launch.queued->source, selected_source) == 0);
    CHECK(strcmp(launch.queued->sha256, selected_sha) == 0);
    save_script("picked.lua", changed_source, changed_sha);
    CHECK(strcmp(selected_sha, changed_sha) != 0);
    ryz_script_store_snapshot_t disk;
    CHECK(ryz_script_store_get("picked.lua", &disk) == ESP_OK);
    CHECK(strcmp(disk.source, changed_source) == 0);
    ryz_script_store_snapshot_free(&disk);
    /* RPC has released its snapshot and disk now contains a different script.
     * Execute the source the real job_start published, through real Lua. */
    run(launch.queued->source, launch.queued->timeout_ms, MODE_NORMAL);
    phase("done");
    CHECK(fixture.clear_calls == 1U && fixture.rect_calls == 1U && fixture.text_calls == 1U);
    CHECK(fixture.touch_reads == 1U && fixture.show_calls == 1U && fixture.output_bytes > 0U);
    cJSON_Delete(reply);
    cJSON_Delete(request);
    launch_destroy(&launch);
}

static void replace_string(cJSON *object, const char *name, const char *value)
{
    cJSON *replacement = cJSON_CreateString(value);
    CHECK(replacement != NULL);
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(object, name, replacement));
}

static void rejected_reply(cJSON *reply, const char *contains)
{
    CHECK(cJSON_IsObject(reply) && cJSON_IsFalse(field(reply, "ok")));
    const cJSON *error = field(reply, "error");
    CHECK(cJSON_IsString(error) && strstr(error->valuestring, contains) != NULL);
}

static void test_rpc_rejections(void)
{
    launch_fixture_t launch;
    launch_init(&launch);
    char sha[65], changed_sha[65];
    save_script("picked.lua", selected_source, sha);
    static const struct { const char *key, *value; } invalid[] = {
        {"schema", "ryz-script-store/999"}, {"boot_id", "previous-boot"},
        {"sha256", ""}, {"sha256", "not-a-sha256"},
        {"sha256", "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"},
        {"name", "../picked.lua"},
    };
    for (size_t i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        cJSON *request = run_request(sha);
        replace_string(request, invalid[i].key, invalid[i].value);
        cJSON *reply = ryz_workbench_file_rpc(request, integration_boot, false, start_snapshot, &launch, &launch.guard);
        rejected_reply(reply, "");
        CHECK(launch.queued == NULL && launch.callbacks == 0U);
        cJSON_Delete(reply);
        cJSON_Delete(request);
    }
    cJSON *request = run_request(sha);
    cJSON *reply = ryz_workbench_file_rpc(request, integration_boot, false, NULL, &launch, &launch.guard);
    rejected_reply(reply, "unavailable");
    cJSON_Delete(reply);
    reply = ryz_workbench_file_rpc(request, integration_boot, true, start_snapshot, &launch, &launch.guard);
    rejected_reply(reply, "busy");
    cJSON_Delete(reply);
    save_script("picked.lua", changed_source, changed_sha);
    reply = ryz_workbench_file_rpc(request, integration_boot, false, start_snapshot, &launch, &launch.guard);
    rejected_reply(reply, "changed");
    cJSON_Delete(reply);
    CHECK(launch.callbacks == 0U && launch.current == NULL && launch.queued == NULL);
    /* Obtain a real committed-but-cleanup-pending Store state, not a fake
     * status structure, then prove even matching source cannot start. */
    script_store_host_fault(SCRIPT_HOST_REMOVE_BEFORE, ".ryz-old", NULL, 0U, 1U);
    ryz_script_store_mutation_t mutation;
    CHECK(ryz_script_store_put("picked.lua", selected_source, strlen(selected_source),
                               NULL, &mutation) == ESP_FAIL);
    CHECK(mutation.commit == RYZ_SCRIPT_STORE_COMMITTED && mutation.recovery_required);
    CHECK(script_store_host_fault_hits() == 1U && strcmp(mutation.sha256, sha) == 0);
    reply = ryz_workbench_file_rpc(request, integration_boot, false, start_snapshot, &launch, &launch.guard);
    rejected_reply(reply, "recovery");
    CHECK(launch.callbacks == 0U && launch.enqueues == 0U && launch.notifications == 0U);
    cJSON_Delete(reply);
    cJSON_Delete(request);
    script_store_host_clear_fault();
    CHECK(ryz_script_store_recover(&mutation) == ESP_OK);
    launch_destroy(&launch);
}

typedef struct {
    launch_fixture_t *launch;
    const cJSON *request;
    cJSON *reply;
} rpc_thread_t;

static void *rpc_thread(void *context)
{
    rpc_thread_t *request = context;
    request->reply = ryz_workbench_file_rpc(request->request, integration_boot,
                                           false, start_snapshot, request->launch, &request->launch->guard);
    return NULL;
}

static void wait_for_snapshot(launch_fixture_t *launch)
{
    CHECK(pthread_mutex_lock(&launch->callback_mutex) == 0);
    while (!launch->callback_entered)
        CHECK(pthread_cond_wait(&launch->callback_condition, &launch->callback_mutex) == 0);
    CHECK(pthread_mutex_unlock(&launch->callback_mutex) == 0);
}

static void release_snapshot(launch_fixture_t *launch)
{
    CHECK(pthread_mutex_lock(&launch->callback_mutex) == 0);
    launch->callback_released = true;
    CHECK(pthread_cond_broadcast(&launch->callback_condition) == 0);
    CHECK(pthread_mutex_unlock(&launch->callback_mutex) == 0);
}

static void test_final_admission(void)
{
    char sha[65];
    save_script("picked.lua", selected_source, sha);
    for (unsigned kind = 0U; kind < 2U; ++kind) {
        launch_fixture_t launch;
        launch_init(&launch);
        launch.pause_callback = true;
        cJSON *request = run_request(sha);
        rpc_thread_t invocation = {.launch = &launch, .request = request};
        pthread_t thread;
        CHECK(pthread_create(&thread, NULL, rpc_thread, &invocation) == 0);
        wait_for_snapshot(&launch);
        CHECK(launch.callbacks == 1U && launch.enqueues == 0U);
        cJSON *competing_ack = NULL;
        if (kind == 0U) {
            /* A real second job wins while the first RPC has a valid borrowed
             * snapshot but has not yet attempted final runtime admission. */
            competing_ack = ryz_workbench_job_start(&launch.start, "competing",
                "other.lua", strdup("print('other')"), 2000U, false);
            CHECK(launch.enqueues == 1U);
            complete_ack(competing_ack, launch.queued);
        } else {
            launch_lock(&launch);
            launch.gateway_ready = false;
            launch_unlock(&launch);
        }
        release_snapshot(&launch);
        CHECK(pthread_join(thread, NULL) == 0);
        rejected_reply(invocation.reply, kind == 0U ? "busy" : "unavailable");
        CHECK(launch.callback_acks == 0U);
        CHECK(launch.enqueues == (kind == 0U ? 1U : 0U));
        CHECK(launch.notifications == launch.enqueues);
        CHECK(launch.next_job == launch.enqueues);
        if (kind == 0U) CHECK(strcmp(launch.queued->name, "other.lua") == 0);
        cJSON_Delete(competing_ack);
        cJSON_Delete(invocation.reply);
        cJSON_Delete(request);
        launch_destroy(&launch);
    }
}

static void test_job_start_modes(void)
{
    launch_fixture_t launch;
    launch_init(&launch);
    launch.queue_ready = false;
    cJSON *reply = ryz_workbench_job_start(&launch.start, "queue-reject", "picked.lua",
                                          strdup(selected_source), 2000U, false);
    rejected_reply(reply, "queue");
    CHECK(launch.current == NULL && launch.queued == NULL && launch.next_job == 0U);
    CHECK(launch.enqueues == 0U && launch.notifications == 0U);
    cJSON_Delete(reply);
    launch.queue_ready = true;
    reply = ryz_workbench_job_start(&launch.start, "legacy-reply-later", "picked.lua",
                                    strdup(selected_source), 2000U, true);
    CHECK(reply == NULL && launch.queued != NULL && launch.queued->legacy);
    CHECK(launch.enqueues == 1U && launch.notifications == 1U && launch.next_job == 1U);
    CHECK(strcmp(launch.queued->reply_id, "legacy-reply-later") == 0);
    CHECK(launch.queued->started_us == INT64_C(123456789));
    run(launch.queued->source, launch.queued->timeout_ms, MODE_NORMAL);
    phase("done");
    release_queued(&launch);
    launch.next_job = UINT_MAX - 1U;
    reply = ryz_workbench_job_start(&launch.start, "last-identity", "picked.lua",
                                    strdup(selected_source), 2000U, false);
    complete_ack(reply, launch.queued);
    CHECK(launch.next_job == UINT_MAX);
    CHECK(strcmp(launch.queued->id, "run-boot-4294967295") == 0);
    cJSON_Delete(reply);
    release_queued(&launch);
    unsigned enqueues = launch.enqueues;
    reply = ryz_workbench_job_start(&launch.start, "exhausted", "picked.lua",
                                    strdup(selected_source), 2000U, false);
    rejected_reply(reply, "exhausted");
    CHECK(launch.next_job == UINT_MAX && launch.enqueues == enqueues);
    CHECK(launch.current == NULL && launch.queued == NULL);
    cJSON_Delete(reply);
    launch_destroy(&launch);
}

static int json_budget = -1;
static size_t json_live;

static void *budget_malloc(size_t bytes)
{
    if (json_budget == 0) return NULL;
    if (json_budget > 0) --json_budget;
    void *allocation = malloc(bytes);
    if (allocation) ++json_live;
    return allocation;
}

static void budget_free(void *allocation)
{
    if (allocation) { CHECK(json_live > 0U); --json_live; }
    free(allocation);
}

static void budget_hooks(void)
{
    CHECK(json_live == 0U);
    json_budget = -1;
    cJSON_Hooks hooks = {.malloc_fn = budget_malloc, .free_fn = budget_free};
    cJSON_InitHooks(&hooks);
}

static void test_complete_ack_before_enqueue(void)
{
    budget_hooks();
    unsigned rejected = 0U, started = 0U;
    for (int budget = 0; budget < 128 && started < 2U; ++budget) {
        launch_fixture_t launch;
        launch_init(&launch);
        json_budget = budget;
        cJSON *reply = ryz_workbench_job_start(&launch.start, "budget-ack", "picked.lua",
                                              strdup(selected_source), 2000U, false);
        json_budget = -1;
        if (launch.queued) {
            complete_ack(reply, launch.queued);
            CHECK(launch.enqueues == 1U && launch.notifications == 1U && launch.next_job == 1U);
            ++started;
        } else {
            CHECK(reply == NULL && launch.current == NULL);
            CHECK(launch.enqueues == 0U && launch.notifications == 0U && launch.next_job == 0U);
            ++rejected;
        }
        cJSON_Delete(reply);
        launch_destroy(&launch);
        CHECK(json_live == 0U);
    }
    CHECK(rejected > 20U && started == 2U);
    cJSON_InitHooks(NULL);
}

static void test_version_ack_failure_preserves_job(void)
{
    char sha[65];
    save_script("picked.lua", selected_source, sha);
    budget_hooks();
    unsigned not_started = 0U, unknown_started = 0U, acknowledged = 0U;
    for (int budget = 0; budget < 160 && acknowledged < 2U; ++budget) {
        launch_fixture_t launch;
        launch_init(&launch);
        cJSON *request = run_request(sha);
        json_budget = budget;
        cJSON *reply = ryz_workbench_file_rpc(request, integration_boot, false, start_snapshot, &launch, &launch.guard);
        json_budget = -1;
        if (launch.queued) {
            CHECK(launch.enqueues == 1U && launch.notifications == 1U && launch.callback_acks == 1U);
            CHECK(strcmp(launch.queued->source, selected_source) == 0);
            CHECK(strcmp(launch.queued->sha256, sha) == 0);
            if (reply) {
                complete_ack(reply, launch.queued);
                string_is(reply, "schema", "ryz-script-store/1");
                string_is(reply, "boot_id", integration_boot);
                ++acknowledged;
            } else {
                /* Actual job_start already returned a complete ACK to RPC;
                 * only schema/boot envelope allocation has failed now. */
                if (unknown_started == 0U) {
                    run(launch.queued->source, launch.queued->timeout_ms, MODE_NORMAL);
                    phase("done");
                }
                ++unknown_started;
            }
        } else {
            CHECK(reply == NULL && launch.current == NULL && launch.callback_acks == 0U);
            CHECK(launch.notifications == 0U && launch.next_job == 0U);
            ++not_started;
        }
        cJSON_Delete(reply);
        cJSON_Delete(request);
        launch_destroy(&launch);
        CHECK(json_live == 0U);
    }
    CHECK(not_started > 20U && unknown_started > 0U && acknowledged == 2U);
    cJSON_InitHooks(NULL);
}

static cJSON *mutation_request(bool putting, bool versioned, const char *previous)
{
    cJSON *request = cJSON_CreateObject();
    CHECK(request != NULL && cJSON_AddStringToObject(request, "id", "write-request"));
    CHECK(cJSON_AddStringToObject(request, "op", versioned ? "scripts" : putting ? "put" : "remove"));
    CHECK(cJSON_AddStringToObject(request, "name", "picked.lua"));
    if (versioned) {
        CHECK(cJSON_AddStringToObject(request, "schema", "ryz-script-store/1"));
        CHECK(cJSON_AddStringToObject(request, "boot_id", integration_boot));
        CHECK(cJSON_AddStringToObject(request, "action", putting ? "put" : "remove"));
        CHECK(cJSON_AddStringToObject(request, "previous_sha256", previous));
    }
    if (putting) {
        char sha[65];
        CHECK(ryz_script_store_source_sha256(changed_source, strlen(changed_source), sha) == ESP_OK);
        CHECK(cJSON_AddStringToObject(request, "source", changed_source));
        CHECK(cJSON_AddStringToObject(request, "sha256", sha));
    }
    return request;
}

static void test_existing_job_rejects_writer(void)
{
    for (unsigned kind = 0U; kind < 4U; ++kind) {
        launch_fixture_t launch;
        launch_init(&launch);
        char sha[65];
        save_script("picked.lua", selected_source, sha);
        cJSON *ack = ryz_workbench_job_start(&launch.start, "current-job", "picked.lua",
                                            strdup(selected_source), 0U, false);
        complete_ack(ack, launch.queued);
        cJSON *request = mutation_request(kind % 2U == 0U, kind >= 2U, sha);
        /* The caller's earlier busy sample is false. The shared lease must
         * still reject against the real job_start publication under its lock. */
        cJSON *reply = ryz_workbench_file_rpc(request, integration_boot, false,
                                              start_snapshot, &launch, &launch.guard);
        rejected_reply(reply, "busy");
        CHECK(!launch.writer_active && launch.write_begins == 0U && launch.write_ends == 0U);
        CHECK(launch.write_rejections == 1U && launch.enqueues == 1U);
        ryz_script_store_snapshot_t disk;
        CHECK(ryz_script_store_get("picked.lua", &disk) == ESP_OK);
        CHECK(strcmp(disk.source, selected_source) == 0 && strcmp(disk.sha256, sha) == 0);
        ryz_script_store_snapshot_free(&disk);
        cJSON_Delete(reply);
        release_queued(&launch);
        reply = ryz_workbench_file_rpc(request, integration_boot, false,
                                       start_snapshot, &launch, &launch.guard);
        CHECK(cJSON_IsTrue(field(reply, "ok")));
        CHECK(!launch.writer_active && launch.write_begins == 1U && launch.write_ends == 1U);
        cJSON_Delete(reply);
        cJSON_Delete(request);
        cJSON_Delete(ack);
        launch_destroy(&launch);
    }
}

static void test_store_commit_rejects_prepared_job(bool putting)
{
    launch_fixture_t launch;
    launch_init(&launch);
    char sha[65];
    save_script("picked.lua", selected_source, sha);
    launch.pause_callback = true;
    cJSON *run = run_request(sha);
    rpc_thread_t reader = {.launch = &launch, .request = run};
    pthread_t reader_thread, writer_thread;
    CHECK(pthread_create(&reader_thread, NULL, rpc_thread, &reader) == 0);
    wait_for_snapshot(&launch); /* Real Store snapshot is prepared, not admitted. */

    cJSON *write = mutation_request(putting, true, sha);
    rpc_thread_t writer = {.launch = &launch, .request = write};
    commit_gate_arm(&launch, putting);
    CHECK(pthread_create(&writer_thread, NULL, rpc_thread, &writer) == 0);
    commit_gate_wait();
    release_snapshot(&launch);
    CHECK(pthread_join(reader_thread, NULL) == 0);
    rejected_reply(reader.reply, "unavailable");
    CHECK(launch.current == NULL && launch.queued == NULL && launch.enqueues == 0U);
    CHECK(launch.callback_acks == 0U && launch.notifications == 0U && launch.next_job == 0U);

    cJSON *second = ryz_workbench_file_rpc(write, integration_boot, false,
                                            start_snapshot, &launch, &launch.guard);
    rejected_reply(second, "busy");
    CHECK(launch.write_rejections == 1U && launch.write_begins == 1U && launch.write_ends == 0U);
    commit_gate_release();
    CHECK(pthread_join(writer_thread, NULL) == 0);
    CHECK(cJSON_IsTrue(field(writer.reply, "ok")));
    string_is(writer.reply, "store_commit", "committed");
    CHECK(!launch.writer_active && launch.write_begins == 1U && launch.write_ends == 1U);
    ryz_script_store_snapshot_t disk;
    esp_err_t result = ryz_script_store_get("picked.lua", &disk);
    CHECK(result == (putting ? ESP_OK : ESP_ERR_NOT_FOUND));
    if (putting) {
        CHECK(strcmp(disk.source, changed_source) == 0);
        ryz_script_store_snapshot_free(&disk);
    }
    cJSON *next = ryz_workbench_job_start(&launch.start, "after-writer", "inline.lua",
                                         strdup(selected_source), 0U, false);
    complete_ack(next, launch.queued);
    CHECK(launch.enqueues == 1U && launch.notifications == 1U && launch.next_job == 1U);
    cJSON_Delete(next);
    cJSON_Delete(second);
    cJSON_Delete(writer.reply);
    cJSON_Delete(reader.reply);
    cJSON_Delete(write);
    cJSON_Delete(run);
    launch_destroy(&launch);
}

static void job_after_writer(launch_fixture_t *launch)
{
    CHECK(!launch->writer_active && launch->write_begins == launch->write_ends);
    cJSON *ack = ryz_workbench_job_start(&launch->start, "after-write-result", "inline.lua",
                                        strdup(selected_source), 0U, false);
    complete_ack(ack, launch->queued);
    CHECK(launch->enqueues == 1U && launch->notifications == 1U);
    cJSON_Delete(ack);
    release_queued(launch);
}

static void test_store_failure_returns_lease(void)
{
    for (unsigned kind = 0U; kind < 5U; ++kind) {
        launch_fixture_t launch;
        launch_init(&launch);
        char sha[65];
        save_script("picked.lua", selected_source, sha);
        bool putting = kind < 3U;
        cJSON *request = mutation_request(putting, true, sha);
        if (kind == 0U) {
            script_store_host_fault(SCRIPT_HOST_WRITE_PARTIAL, ".ryz-new", NULL, 0U, 1U);
        } else if (kind == 1U) {
            script_store_host_fault(SCRIPT_HOST_RENAME_BEFORE, ".ryz-new", "picked.lua", 0U, 1U);
            script_store_host_add_fault(SCRIPT_HOST_RENAME_BEFORE, ".ryz-old", "picked.lua", 0U, 1U);
        } else if (kind == 3U) {
            script_store_host_fault(SCRIPT_HOST_RENAME_BEFORE, "picked.lua", ".ryz-old", 0U, 1U);
        } else {
            script_store_host_fault(SCRIPT_HOST_REMOVE_BEFORE, ".ryz-old", NULL, 0U, 1U);
        }
        cJSON *reply = ryz_workbench_file_rpc(request, integration_boot, false,
                                              start_snapshot, &launch, &launch.guard);
        bool committed = kind == 2U || kind == 4U;
        CHECK(cJSON_IsBool(field(reply, "ok")) && cJSON_IsTrue(field(reply, "ok")) == committed);
        string_is(reply, "store_commit", committed ? "committed" : kind == 1U ? "unknown" : "not_committed");
        CHECK(script_store_host_fault_hits() == (kind == 1U ? 2U : 1U));
        CHECK(launch.write_begins == 1U && launch.write_ends == 1U && !launch.writer_active);
        const bool recovering = kind == 1U || committed;
        CHECK(cJSON_IsTrue(field(reply, "store_recovery_required")) == recovering);
        /* An inline source may start after the lease is returned; this does
         * not claim the Store's UNKNOWN/recovery state has become healthy. */
        job_after_writer(&launch);
        script_store_host_clear_fault();
        ryz_script_store_mutation_t recovery;
        CHECK(ryz_script_store_recover(&recovery) == ESP_OK && !recovery.recovery_required);
        ryz_script_store_snapshot_t disk;
        esp_err_t error = ryz_script_store_get("picked.lua", &disk);
        CHECK(error == (kind == 4U ? ESP_ERR_NOT_FOUND : ESP_OK));
        if (error == ESP_OK) {
            CHECK(strcmp(disk.source, kind == 2U ? changed_source : selected_source) == 0);
            ryz_script_store_snapshot_free(&disk);
        }
        cJSON_Delete(reply);
        cJSON_Delete(request);
        launch_destroy(&launch);
    }
}

static void test_mutation_ack_oom_returns_lease(bool putting)
{
    budget_hooks();
    unsigned before_mutation = 0U, unknown_committed = 0U, acknowledged = 0U;
    for (int budget = 0; budget < 128 && acknowledged < 2U; ++budget) {
        launch_fixture_t launch;
        launch_init(&launch);
        char sha[65];
        save_script("picked.lua", selected_source, sha);
        cJSON *request = mutation_request(putting, true, sha);
        json_budget = budget;
        cJSON *reply = ryz_workbench_file_rpc(request, integration_boot, false,
                                              start_snapshot, &launch, &launch.guard);
        json_budget = -1;
        CHECK(!launch.writer_active && launch.write_begins == launch.write_ends);
        ryz_script_store_snapshot_t disk;
        esp_err_t error = ryz_script_store_get("picked.lua", &disk);
        CHECK(error == ESP_OK || (!putting && error == ESP_ERR_NOT_FOUND));
        bool committed = putting ? strcmp(disk.source, changed_source) == 0 : error == ESP_ERR_NOT_FOUND;
        if (error == ESP_OK) {
            CHECK(strcmp(disk.source, committed ? changed_source : selected_source) == 0);
            ryz_script_store_snapshot_free(&disk);
        }
        if (reply) {
            CHECK(cJSON_IsTrue(field(reply, "ok")) && committed);
            string_is(reply, "store_commit", "committed");
            ++acknowledged;
        } else if (committed) {
            CHECK(launch.write_begins == 1U && launch.write_ends == 1U);
            ++unknown_committed; /* No ACK is NOT a definite uncommitted result. */
        } else {
            CHECK(launch.write_begins == 0U && launch.write_ends == 0U);
            ++before_mutation;
        }
        job_after_writer(&launch);
        cJSON_Delete(reply);
        cJSON_Delete(request);
        launch_destroy(&launch);
        CHECK(json_live == 0U);
    }
    CHECK(before_mutation > 0U && unknown_committed > 0U && acknowledged == 2U);
    cJSON_InitHooks(NULL);
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    script_store_host_configure(argv[2]);
    ryz_script_store_mutation_t recovery;
    CHECK(ryz_script_store_init(argv[2], &recovery) == ESP_OK);
    if (strcmp(argv[1], "selected") == 0) test_selected_snapshot_executes();
    else if (strcmp(argv[1], "rejections") == 0) test_rpc_rejections();
    else if (strcmp(argv[1], "admission") == 0) test_final_admission();
    else if (strcmp(argv[1], "modes") == 0) test_job_start_modes();
    else if (strcmp(argv[1], "ack_budget") == 0) test_complete_ack_before_enqueue();
    else if (strcmp(argv[1], "version_budget") == 0) test_version_ack_failure_preserves_job();
    else if (strcmp(argv[1], "writer_existing_job") == 0) test_existing_job_rejects_writer();
    else if (strcmp(argv[1], "writer_put_commit") == 0) test_store_commit_rejects_prepared_job(true);
    else if (strcmp(argv[1], "writer_remove_commit") == 0) test_store_commit_rejects_prepared_job(false);
    else if (strcmp(argv[1], "writer_faults") == 0) test_store_failure_returns_lease();
    else if (strcmp(argv[1], "writer_put_oom") == 0) test_mutation_ack_oom_returns_lease(true);
    else if (strcmp(argv[1], "writer_remove_oom") == 0) test_mutation_ack_oom_returns_lease(false);
    else { fprintf(stderr, "Unknown integration case: %s\n", argv[1]); return 2; }
    printf("SCRIPT_RUN_INTEGRATION_PASS %s\n", argv[1]);
    return 0;
}
