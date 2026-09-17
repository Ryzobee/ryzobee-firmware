/* Real Workbench restart + OTA core + Store with POSIX temporary files.
 * SDK reboot is an observation point that deliberately returns; this is not
 * device reboot, SPIFFS power-loss durability, or peripheral teardown proof. */
#define _POSIX_C_SOURCE 200809L

#include "workbench_restart.h"
#include "ryz_ota.h"
#include "ryz_script_store.h"
#include "fake_platform.h"
#include "script_store_host.h"

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t job_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool job_active, writer_active;
static unsigned write_begins, write_ends, write_rejections, status_calls;
static bool observe_reboot;
static bool status_override;
static esp_err_t status_error;
static ryz_script_store_status_t status_value;

/* Only the standalone test's Store object renames this symbol. Normal calls
 * still execute the real public Store status; no filesystem logic is copied. */
esp_err_t restart_real_store_status(ryz_script_store_status_t *out);
esp_err_t ryz_script_store_status(ryz_script_store_status_t *out)
{
    ++status_calls;
    /* Explicitly scoped contract fault injection. The independent real Store
     * scenarios below leave this disabled, including journal and lock tests. */
    if (status_override) {
        *out = status_value;
        return status_error;
    }
    return restart_real_store_status(out);
}

static bool writer_begin(void *context)
{
    assert(context == &job_mutex);
    if (pthread_mutex_trylock(&job_mutex) != 0) return false;
    const bool accepted = !job_active && !writer_active;
    if (accepted) { writer_active = true; ++write_begins; }
    else ++write_rejections;
    assert(pthread_mutex_unlock(&job_mutex) == 0);
    return accepted;
}

static void writer_end(void *context)
{
    assert(context == &job_mutex);
    assert(pthread_mutex_lock(&job_mutex) == 0);
    assert(writer_active);
    writer_active = false;
    ++write_ends;
    assert(pthread_mutex_unlock(&job_mutex) == 0);
}

static const ryz_workbench_write_guard_t guard = {
    .context = &job_mutex, .try_begin_write = writer_begin, .end_write = writer_end,
};

/* The production final Job admission uses the same mutex and writer flag.
 * This is only that typed admission model, not an executing Lua VM. */
static bool job_try_begin(void)
{
    assert(pthread_mutex_lock(&job_mutex) == 0);
    const bool accepted = !job_active && !writer_active;
    if (accepted) job_active = true;
    assert(pthread_mutex_unlock(&job_mutex) == 0);
    return accepted;
}

static void *competing_admission(void *unused)
{
    (void)unused;
    assert(!job_try_begin());
    assert(!writer_begin(&job_mutex));
    return NULL;
}

void restart_delegate_reboot(void);
void ryz_ota_platform_reboot(void)
{
    if (observe_reboot) {
        assert(writer_active && write_begins == write_ends + 1U);
        /* The logical reservation excludes both callers without holding the
         * Job mutex across Store I/O, OTA inspection or the platform call. */
        assert(pthread_mutex_trylock(&job_mutex) == 0);
        assert(!job_active && writer_active);
        assert(pthread_mutex_unlock(&job_mutex) == 0);
        pthread_t contender;
        assert(pthread_create(&contender, NULL, competing_admission, NULL) == 0);
        assert(pthread_join(contender, NULL) == 0);
    }
    restart_delegate_reboot();
}

static uint32_t ready_ota(void)
{
    fake_ota_platform_reset();
    assert(ryz_ota_init() == ESP_OK);
    assert(ryz_ota_set_prerequisites(true, true) == ESP_OK);
    assert(ryz_ota_start_update() == ESP_OK);
    assert(fake_ota_platform_emit_candidate("ryzobee_rootmaker", "0.8.0", 1000) == ESP_OK);
    assert(fake_ota_platform_emit_progress(1000, 1000) == ESP_OK);
    assert(fake_ota_platform_emit_verifying() == ESP_OK);
    assert(fake_ota_platform_emit_completed() == ESP_OK);
    ryz_ota_snapshot_t snapshot;
    assert(ryz_ota_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RYZ_OTA_READY_TO_REBOOT && !snapshot.worker_active);
    assert(snapshot.cleanup_confirmed && snapshot.attempt_id);
    return snapshot.attempt_id;
}

static void busy_rejects_before_store(uint32_t attempt)
{
    job_active = true;
    size_t before = script_store_host_platform_calls();
    esp_err_t error = ryz_workbench_restart_updated(attempt, &guard);
    fprintf(stderr, "busy restart: result=%d reboots=%u status_calls=%u\n",
            error, fake_ota_platform_reboot_count(), status_calls);
    assert(error == ESP_ERR_TIMEOUT);
    assert(fake_ota_platform_reboot_count() == 0);
    assert(!status_calls && !write_begins && !write_ends);
    assert(script_store_host_platform_calls() == before);
    job_active = false;
    writer_active = true; /* A different writer owns this lease. */
    assert(ryz_workbench_restart_updated(attempt, &guard) == ESP_ERR_TIMEOUT);
    assert(writer_active && !write_begins && !write_ends && !status_calls);
    writer_active = false;
    assert(pthread_mutex_lock(&job_mutex) == 0);
    assert(ryz_workbench_restart_updated(attempt, &guard) == ESP_ERR_TIMEOUT);
    assert(pthread_mutex_unlock(&job_mutex) == 0);
    assert(!write_begins && !write_ends && !status_calls);
    assert(script_store_host_platform_calls() == before);
}

static void malformed_and_stale(uint32_t attempt)
{
    const ryz_workbench_write_guard_t missing_begin = {.end_write = writer_end};
    const ryz_workbench_write_guard_t missing_end = {.try_begin_write = writer_begin};
    size_t before = script_store_host_platform_calls();
    assert(ryz_workbench_restart_updated(attempt, NULL) == ESP_ERR_INVALID_ARG);
    assert(ryz_workbench_restart_updated(attempt, &missing_begin) == ESP_ERR_INVALID_ARG);
    assert(ryz_workbench_restart_updated(attempt, &missing_end) == ESP_ERR_INVALID_ARG);
    assert(ryz_workbench_restart_updated(0, &guard) == ESP_ERR_INVALID_ARG);
    assert(ryz_workbench_restart_updated(attempt + 1U, &guard) == ESP_ERR_INVALID_STATE);
    assert(!status_calls && !write_begins && !write_ends);
    assert(!fake_ota_platform_reboot_count());
    assert(script_store_host_platform_calls() == before);
}

static void clean_reboot_return_releases(uint32_t attempt)
{
    observe_reboot = true;
    size_t before = script_store_host_platform_calls();
    assert(ryz_workbench_restart_updated(attempt, &guard) == ESP_FAIL);
    assert(fake_ota_platform_reboot_count() == 1U);
    assert(status_calls == 1U && write_begins == 1U && write_ends == 1U);
    assert(!writer_active && !job_active && write_rejections == 1U);
    assert(script_store_host_platform_calls() == before);
    ryz_ota_snapshot_t ota;
    assert(ryz_ota_get_snapshot(&ota) == ESP_OK && !ota.reboot_pending);
    assert(ota.state == RYZ_OTA_READY_TO_REBOOT && ota.attempt_id == attempt);
    assert(job_try_begin());
    job_active = false;
    assert(writer_begin(&job_mutex));
    writer_end(&job_mutex);
}

static void status_contract_failures(uint32_t attempt)
{
    const struct {
        ryz_script_store_status_t status;
        esp_err_t result, expected;
    } cases[] = {
        {{.ready = false}, ESP_OK, ESP_ERR_INVALID_STATE},
        {{.ready = true, .recovery_required = true}, ESP_OK, ESP_ERR_INVALID_STATE},
        {{.ready = true, .legacy_backup_present = true}, ESP_OK, ESP_ERR_INVALID_STATE},
        {{.ready = true, .recovery_error = ESP_FAIL}, ESP_OK, ESP_FAIL},
        {{.ready = true}, ESP_ERR_TIMEOUT, ESP_ERR_TIMEOUT},
        {{.ready = true}, ESP_ERR_INVALID_RESPONSE, ESP_ERR_INVALID_RESPONSE},
        {{.ready = true}, ESP_FAIL, ESP_FAIL},
    };
    status_override = true;
    size_t before = script_store_host_platform_calls();
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        status_value = cases[i].status;
        status_error = cases[i].result;
        unsigned begins = write_begins, ends = write_ends;
        assert(ryz_workbench_restart_updated(attempt, &guard) == cases[i].expected);
        assert(write_begins == begins + 1U && write_ends == ends + 1U);
        assert(!writer_active && !fake_ota_platform_reboot_count());
        assert(job_try_begin());
        job_active = false;
    }
    assert(script_store_host_platform_calls() == before);
    assert(status_calls == sizeof(cases) / sizeof(cases[0]));
}

static void store_uninitialized(uint32_t attempt)
{
    size_t before = script_store_host_platform_calls();
    assert(ryz_workbench_restart_updated(attempt, &guard) == ESP_ERR_INVALID_STATE);
    assert(write_begins == 1U && write_ends == 1U && !writer_active);
    assert(!fake_ota_platform_reboot_count());
    assert(script_store_host_platform_calls() == before);
}

static void *read_while_hash_held(void *unused)
{
    (void)unused;
    ryz_script_store_snapshot_t source;
    assert(ryz_script_store_get("keep.lua", &source) == ESP_OK);
    assert(!strcmp(source.source, "return 1"));
    ryz_script_store_snapshot_free(&source);
    return NULL;
}

static void store_contention(uint32_t attempt)
{
    ryz_script_store_mutation_t result;
    assert(ryz_script_store_put("keep.lua", "return 1", 8U, NULL, &result) == ESP_OK);
    script_store_host_sha_gate_arm();
    pthread_t reader;
    assert(pthread_create(&reader, NULL, read_while_hash_held, NULL) == 0);
    script_store_host_sha_gate_wait();
    size_t before = script_store_host_platform_calls();
    assert(ryz_workbench_restart_updated(attempt, &guard) == ESP_ERR_TIMEOUT);
    assert(write_begins == 1U && write_ends == 1U && !writer_active);
    assert(!fake_ota_platform_reboot_count());
    assert(script_store_host_platform_calls() == before);
    script_store_host_sha_gate_release();
    assert(pthread_join(reader, NULL) == 0);
    observe_reboot = true;
    assert(ryz_workbench_restart_updated(attempt, &guard) == ESP_FAIL);
    assert(fake_ota_platform_reboot_count() == 1U && write_ends == 2U);
}

static void cleanup_failure(uint32_t attempt)
{
    ryz_script_store_mutation_t result;
    assert(ryz_script_store_put("keep.lua", "return 1", 8U, NULL, &result) == ESP_OK);
    script_store_host_fault(SCRIPT_HOST_REMOVE_BEFORE, ".ryz-old", NULL, 0U, 1U);
    assert(ryz_script_store_put("keep.lua", "return 2", 8U, NULL, &result) == ESP_FAIL);
    assert(result.commit == RYZ_SCRIPT_STORE_COMMITTED && result.recovery_required);
    size_t before = script_store_host_platform_calls();
    assert(ryz_workbench_restart_updated(attempt, &guard) == ESP_FAIL);
    assert(!fake_ota_platform_reboot_count() && !writer_active);
    assert(write_begins == 1U && write_ends == 1U);
    assert(script_store_host_platform_calls() == before); /* No implicit recovery. */
    script_store_host_clear_fault();
    assert(ryz_script_store_recover(&result) == ESP_OK);
    assert(!result.recovery_required);
    observe_reboot = true;
    assert(ryz_workbench_restart_updated(attempt, &guard) == ESP_FAIL);
    assert(fake_ota_platform_reboot_count() == 1U && write_ends == 2U);
    ryz_script_store_snapshot_t source;
    assert(ryz_script_store_get("keep.lua", &source) == ESP_OK);
    assert(!strcmp(source.source, "return 2"));
    ryz_script_store_snapshot_free(&source);
}

static void legacy_preserved(uint32_t attempt, const char *root)
{
    char path[512];
    int length = snprintf(path, sizeof(path), "%s/.upload.bak", root);
    assert(length > 0 && (size_t)length < sizeof(path));
    FILE *file = fopen(path, "wb");
    assert(file && fwrite("old", 1U, 3U, file) == 3U && fclose(file) == 0);
    ryz_script_store_mutation_t recovery;
    assert(ryz_script_store_init(root, &recovery) == ESP_ERR_INVALID_STATE);
    size_t before = script_store_host_platform_calls();
    assert(ryz_workbench_restart_updated(attempt, &guard) == ESP_ERR_INVALID_STATE);
    assert(!fake_ota_platform_reboot_count() && !writer_active);
    assert(write_begins == 1U && write_ends == 1U);
    assert(script_store_host_platform_calls() == before);
    file = fopen(path, "rb");
    char bytes[4] = {0};
    assert(file && fread(bytes, 1U, 3U, file) == 3U && fclose(file) == 0);
    assert(!strcmp(bytes, "old"));
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    script_store_host_configure(argv[2]);
    ryz_script_store_mutation_t recovery;
    if (strcmp(argv[1], "uninitialized"))
        assert(ryz_script_store_init(argv[2], &recovery) == ESP_OK);
    uint32_t attempt = ready_ota();
    if (!strcmp(argv[1], "busy")) busy_rejects_before_store(attempt);
    else if (!strcmp(argv[1], "arguments")) malformed_and_stale(attempt);
    else if (!strcmp(argv[1], "return")) clean_reboot_return_releases(attempt);
    else if (!strcmp(argv[1], "status")) status_contract_failures(attempt);
    else if (!strcmp(argv[1], "uninitialized")) store_uninitialized(attempt);
    else if (!strcmp(argv[1], "contention")) store_contention(attempt);
    else if (!strcmp(argv[1], "cleanup")) cleanup_failure(attempt);
    else if (!strcmp(argv[1], "legacy")) legacy_preserved(attempt, argv[2]);
    else assert(!"unknown case");
    printf("WORKBENCH_RESTART_PASS %s\n", argv[1]);
    return 0;
}
