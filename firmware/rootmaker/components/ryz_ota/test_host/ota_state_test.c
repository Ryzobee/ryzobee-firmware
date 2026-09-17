#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "fake_platform.h"
#include "ryz_ota.h"

static esp_err_t allow_reboot(void *context)
{
    (void)context;
    return ESP_OK;
}

static void end_reboot(void *context)
{
    (void)context;
}

static const ryz_ota_reboot_guard_t allow_guard = {
    .try_begin = allow_reboot,
    .end = end_reboot,
};

static void test_initial_state(void)
{
    fake_ota_platform_reset();

    ryz_ota_snapshot_t snapshot = {0};
    assert(ryz_ota_get_snapshot(NULL) == ESP_ERR_INVALID_ARG);
    assert(ryz_ota_get_snapshot(&snapshot) == ESP_ERR_INVALID_STATE);

    assert(ryz_ota_init() == ESP_OK);
    assert(ryz_ota_init() == ESP_OK);
    assert(fake_ota_platform_init_count() == 1);
    assert(ryz_ota_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RYZ_OTA_WAITING_PREREQUISITES);
    assert(snapshot.configured);
    assert(!snapshot.network_ready);
    assert(!snapshot.clock_valid);
    assert(!snapshot.reboot_required);
    assert(!snapshot.reboot_pending);
    assert(snapshot.revision == 1);
    assert(!snapshot.network_held && !snapshot.worker_active);
    assert(snapshot.cleanup_confirmed && snapshot.cleanup_error == ESP_OK);
    assert(snapshot.attempt_id == 0);

}

static void test_running_metadata_is_copied_at_boot(void)
{
    fake_ota_platform_reset();
    ryz_ota_running_info_t info = {
        .running_label = "real_ota_slot",
        .running_subtype = 0x11,
        .partition_known = true,
        .state = RYZ_OTA_IMAGE_STATE_VALID,
        .state_known = true,
        .ab_slots_known = true,
        .ab_slots = true,
    };
    fake_ota_platform_set_running_info(&info);
    assert(ryz_ota_init() == ESP_OK);
    ryz_ota_snapshot_t value;
    assert(ryz_ota_get_snapshot(&value) == ESP_OK);
    assert(value.running_info.partition_known);
    assert(strcmp(value.running_info.running_label, "real_ota_slot") == 0);
    assert(value.running_info.running_subtype == 0x11);
    assert(value.running_info.state_known && value.running_info.state == RYZ_OTA_IMAGE_STATE_VALID);
    assert(value.running_info.state_error == ESP_OK);
    assert(value.running_info.ab_slots_known && value.running_info.ab_slots);
    ryz_ota_running_info_t initial = value.running_info;
    assert(ryz_ota_set_prerequisites(true, true) == ESP_OK);
    assert(ryz_ota_start_update() == ESP_OK);
    assert(ryz_ota_get_snapshot(&value) == ESP_OK);
    uint32_t previous = value.attempt_id;
    assert(fake_ota_platform_emit_candidate("ryzobee_rootmaker", "0.8.0", 1000) == ESP_OK);
    assert(ryz_ota_get_snapshot(&value) == ESP_OK);
    assert(memcmp(&value.running_info, &initial, sizeof(initial)) == 0);
    assert(fake_ota_platform_emit_failed(ESP_FAIL) == ESP_OK);
    assert(ryz_ota_start_update() == ESP_OK);
    assert(fake_ota_platform_emit_candidate_for_attempt(
        previous, "another_product", "STALE", 9999) == ESP_ERR_INVALID_STATE);
    assert(ryz_ota_get_snapshot(&value) == ESP_OK);
    assert(value.attempt_id == previous + 1 && value.state == RYZ_OTA_STARTING);
    assert(memcmp(&value.running_info, &initial, sizeof(initial)) == 0);
    assert(fake_ota_platform_emit_candidate("ryzobee_rootmaker", "0.9.0", 1000) == ESP_OK);
    assert(fake_ota_platform_emit_verifying() == ESP_OK);
    assert(fake_ota_platform_emit_completed() == ESP_OK);
    assert(ryz_ota_get_snapshot(&value) == ESP_OK);
    assert(value.state == RYZ_OTA_READY_TO_REBOOT);
    assert(strcmp(value.candidate_version, "0.9.0") == 0);
    assert(strcmp(value.running_version, "0.7.0") == 0);
    assert(memcmp(&value.running_info, &initial, sizeof(initial)) == 0);
}

static void test_prerequisite_gating(void)
{
    fake_ota_platform_reset();
    assert(ryz_ota_set_prerequisites(true, true) == ESP_ERR_INVALID_STATE);
    assert(ryz_ota_start_update() == ESP_ERR_INVALID_STATE);
    assert(ryz_ota_init() == ESP_OK);

    ryz_ota_snapshot_t snapshot = {0};
    assert(ryz_ota_start_update() == ESP_ERR_INVALID_STATE);
    assert(fake_ota_platform_start_count() == 0);

    assert(ryz_ota_set_prerequisites(true, false) == ESP_OK);
    assert(ryz_ota_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RYZ_OTA_WAITING_PREREQUISITES);
    assert(snapshot.network_ready);
    assert(!snapshot.clock_valid);
    assert(ryz_ota_start_update() == ESP_ERR_INVALID_STATE);
    assert(fake_ota_platform_start_count() == 0);

    assert(ryz_ota_set_prerequisites(true, true) == ESP_OK);
    assert(ryz_ota_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RYZ_OTA_IDLE);
    assert(snapshot.network_ready);
    assert(snapshot.clock_valid);

    assert(ryz_ota_start_update() == ESP_OK);
    assert(fake_ota_platform_start_count() == 1);
    assert(ryz_ota_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RYZ_OTA_STARTING);
    assert(snapshot.downloaded_bytes == 0);
    assert(snapshot.total_bytes == 0);
    assert(snapshot.candidate_version[0] == '\0');
    assert(ryz_ota_start_update() == ESP_ERR_INVALID_STATE);
    assert(fake_ota_platform_start_count() == 1);
}

static void test_candidate_and_monotonic_progress(void)
{
    fake_ota_platform_reset();
    assert(ryz_ota_init() == ESP_OK);
    assert(fake_ota_platform_emit_candidate(
               "ryzobee_rootmaker", "0.8.0", 1000) == ESP_ERR_INVALID_STATE);
    assert(ryz_ota_set_prerequisites(true, true) == ESP_OK);
    assert(ryz_ota_start_update() == ESP_OK);

    assert(fake_ota_platform_emit_candidate(
               "ryzobee_rootmaker", "0.8.0", 1000) == ESP_OK);
    ryz_ota_snapshot_t snapshot = {0};
    assert(ryz_ota_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RYZ_OTA_DOWNLOADING);
    assert(strcmp(snapshot.candidate_version, "0.8.0") == 0);
    assert(snapshot.downloaded_bytes == 0);
    assert(snapshot.total_bytes == 1000);

    assert(fake_ota_platform_emit_progress(400, 1000) == ESP_OK);
    assert(fake_ota_platform_emit_progress(350, 900) == ESP_OK);
    assert(ryz_ota_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.downloaded_bytes == 400);
    assert(snapshot.total_bytes == 1000);

    assert(fake_ota_platform_emit_progress(1100, 1200) == ESP_OK);
    assert(ryz_ota_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.downloaded_bytes == 1100);
    assert(snapshot.total_bytes == 1200);
    assert(!snapshot.reboot_required);

    assert(fake_ota_platform_emit_verifying() == ESP_OK);
    assert(ryz_ota_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RYZ_OTA_VERIFYING);
    assert(ryz_ota_cancel_update() == ESP_ERR_INVALID_STATE);
    assert(fake_ota_platform_cancel_count() == 0);
    assert(fake_ota_platform_emit_completed() == ESP_OK);
    assert(ryz_ota_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RYZ_OTA_READY_TO_REBOOT);
    assert(snapshot.reboot_required);
    assert(snapshot.last_error == ESP_OK);
    assert(ryz_ota_start_update() == ESP_ERR_INVALID_STATE);
    /* Returning from the platform restart is failure, not reboot success. */
    assert(ryz_ota_reboot_to_updated_image(snapshot.attempt_id, &allow_guard) == ESP_FAIL);
    assert(fake_ota_platform_reboot_count() == 1);
}

static void test_candidate_rejection(const char *project_name,
                                     const char *version,
                                     esp_err_t expected_error)
{
    fake_ota_platform_reset();
    assert(ryz_ota_init() == ESP_OK);
    assert(ryz_ota_set_prerequisites(true, true) == ESP_OK);
    assert(ryz_ota_start_update() == ESP_OK);
    assert(fake_ota_platform_emit_candidate(project_name, version, 1000) ==
           expected_error);

    ryz_ota_snapshot_t snapshot = {0};
    assert(ryz_ota_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RYZ_OTA_FAILED);
    assert(snapshot.last_error == expected_error);
    assert(snapshot.candidate_version[0] == '\0');
    assert(!snapshot.reboot_required);
}

static void test_cooperative_cancellation(void)
{
    fake_ota_platform_reset();
    assert(ryz_ota_cancel_update() == ESP_ERR_INVALID_STATE);
    assert(ryz_ota_init() == ESP_OK);
    assert(ryz_ota_set_prerequisites(true, true) == ESP_OK);
    assert(ryz_ota_start_update() == ESP_OK);
    assert(fake_ota_platform_emit_candidate(
               "ryzobee_rootmaker", "0.8.0", 1000) == ESP_OK);

    assert(ryz_ota_cancel_update() == ESP_OK);
    assert(ryz_ota_cancel_update() == ESP_OK);
    assert(fake_ota_platform_cancel_count() == 1);
    ryz_ota_snapshot_t snapshot = {0};
    assert(ryz_ota_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RYZ_OTA_CANCELLING);
    assert(snapshot.cancel_requested);
    assert(ryz_ota_start_update() == ESP_ERR_INVALID_STATE);
    assert(fake_ota_platform_emit_progress(500, 1000) ==
           ESP_ERR_INVALID_STATE);

    assert(fake_ota_platform_emit_cancelled() == ESP_OK);
    assert(ryz_ota_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RYZ_OTA_CANCELLED);
    assert(!snapshot.cancel_requested);
    assert(!snapshot.reboot_required);

    assert(ryz_ota_start_update() == ESP_OK);
    assert(fake_ota_platform_start_count() == 2);
    assert(ryz_ota_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RYZ_OTA_STARTING);
    assert(snapshot.candidate_version[0] == '\0');
}

static void test_candidate_failure_still_owns_worker(void)
{
    fake_ota_platform_reset();
    assert(ryz_ota_init() == ESP_OK);
    assert(ryz_ota_set_prerequisites(true, true) == ESP_OK);
    assert(ryz_ota_start_update() == ESP_OK);
    /* The real Adapter is still inside its candidate callback here: rejection
     * changes the visible phase before it can abort its HTTPS handle. */
    assert(fake_ota_platform_emit_candidate("another_product", "0.8.0", 1000) ==
           ESP_ERR_INVALID_RESPONSE);
    ryz_ota_snapshot_t value = {0};
    assert(ryz_ota_get_snapshot(&value) == ESP_OK);
    assert(value.state == RYZ_OTA_FAILED);
    assert(value.worker_active && !value.cleanup_confirmed);
    uint32_t attempt = value.attempt_id;
    esp_err_t second = ryz_ota_start_update();
    assert(second == ESP_ERR_INVALID_STATE);
    assert(fake_ota_platform_start_count() == 1);
    assert(fake_ota_platform_emit_failed(ESP_FAIL) == ESP_OK);
    assert(ryz_ota_get_snapshot(&value) == ESP_OK);
    assert(!value.worker_active && value.cleanup_confirmed);
    assert(value.last_error == ESP_ERR_INVALID_RESPONSE);
    assert(ryz_ota_start_update() == ESP_OK);
    assert(ryz_ota_get_snapshot(&value) == ESP_OK);
    assert(value.attempt_id == attempt + 1);
}

static ryz_ota_snapshot_t snapshot(void)
{
    ryz_ota_snapshot_t value;
    assert(ryz_ota_get_snapshot(&value) == ESP_OK);
    return value;
}

static void ready(void)
{
    fake_ota_platform_reset();
    assert(ryz_ota_init() == ESP_OK);
    assert(ryz_ota_set_prerequisites(true, true) == ESP_OK);
}

typedef struct {
    unsigned begins;
    unsigned ends;
    bool held;
    esp_err_t result;
    uint32_t attempt;
    unsigned block_at;
    bool entered;
    bool proceed;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
} reboot_guard_fixture_t;

enum { BLOCK_BEGIN = 1, BLOCK_REBOOT, BLOCK_END };

static void fixture_pause(reboot_guard_fixture_t *fixture, unsigned point)
{
    if (fixture->block_at != point) return;
    assert(pthread_mutex_lock(&fixture->mutex) == 0);
    fixture->entered = true;
    assert(pthread_cond_broadcast(&fixture->condition) == 0);
    while (!fixture->proceed)
        assert(pthread_cond_wait(&fixture->condition, &fixture->mutex) == 0);
    assert(pthread_mutex_unlock(&fixture->mutex) == 0);
}

static esp_err_t fixture_begin(void *context)
{
    reboot_guard_fixture_t *fixture = context;
    ++fixture->begins;
    /* This copy would deadlock with the real non-recursive pthread mutex if
     * OTA invoked the wider-system guard while holding its snapshot lock. */
    ryz_ota_snapshot_t value = snapshot();
    assert(value.reboot_pending && value.attempt_id == fixture->attempt);
    assert(value.state == RYZ_OTA_READY_TO_REBOOT);
    fixture_pause(fixture, BLOCK_BEGIN);
    fixture->held = fixture->result == ESP_OK;
    return fixture->result;
}

static void fixture_end(void *context)
{
    reboot_guard_fixture_t *fixture = context;
    assert(fixture->held);
    assert(snapshot().reboot_pending);
    fixture_pause(fixture, BLOCK_END);
    fixture->held = false;
    ++fixture->ends;
}

static ryz_ota_reboot_guard_t fixture_guard(reboot_guard_fixture_t *fixture)
{
    return (ryz_ota_reboot_guard_t){
        .context = fixture,
        .try_begin = fixture_begin,
        .end = fixture_end,
    };
}

static void complete_update(void)
{
    assert(ryz_ota_start_update() == ESP_OK);
    assert(fake_ota_platform_emit_candidate("ryzobee_rootmaker", "0.8.0", 1000) == ESP_OK);
    assert(fake_ota_platform_emit_verifying() == ESP_OK);
    assert(fake_ota_platform_emit_completed() == ESP_OK);
}

static void fixture_reboot(void *context)
{
    reboot_guard_fixture_t *fixture = context;
    assert(fixture->held && fixture->begins == 1 && fixture->ends == 0);
    assert(snapshot().reboot_pending);
    fixture_pause(fixture, BLOCK_REBOOT);
}

static void assert_reboot_rejected(uint32_t attempt, reboot_guard_fixture_t *fixture)
{
    ryz_ota_reboot_guard_t guard = fixture_guard(fixture);
    ryz_ota_snapshot_t before = snapshot();
    assert(ryz_ota_reboot_to_updated_image(attempt, &guard) == ESP_ERR_INVALID_STATE);
    assert(!fixture->begins && !fixture->ends);
    assert(fake_ota_platform_reboot_count() == 0);
    assert(snapshot().revision == before.revision);
    assert(!snapshot().reboot_pending);
}

static void test_reboot_arguments(void)
{
    assert(ryz_ota_reboot_to_updated_image(1, &allow_guard) == ESP_ERR_INVALID_STATE);
    assert(ryz_ota_reboot_to_updated_image(0, &allow_guard) == ESP_ERR_INVALID_ARG);
    ready();
    complete_update();
    ryz_ota_snapshot_t before = snapshot();
    reboot_guard_fixture_t fixture = {.attempt = before.attempt_id};
    ryz_ota_reboot_guard_t guard = fixture_guard(&fixture);
    assert(ryz_ota_reboot_to_updated_image(0, &guard) == ESP_ERR_INVALID_ARG);
    assert(ryz_ota_reboot_to_updated_image(before.attempt_id, NULL) == ESP_ERR_INVALID_ARG);
    guard.try_begin = NULL;
    assert(ryz_ota_reboot_to_updated_image(before.attempt_id, &guard) == ESP_ERR_INVALID_ARG);
    guard = fixture_guard(&fixture);
    guard.end = NULL;
    assert(ryz_ota_reboot_to_updated_image(before.attempt_id, &guard) == ESP_ERR_INVALID_ARG);
    guard.try_begin = NULL;
    assert(ryz_ota_reboot_to_updated_image(before.attempt_id, &guard) == ESP_ERR_INVALID_ARG);
    assert(!fixture.begins && !fixture.ends && !snapshot().reboot_pending);
    assert(snapshot().revision == before.revision);
    assert(fake_ota_platform_reboot_count() == 0);
}

static void test_reboot_exact_attempt(void)
{
    ready();
    assert(ryz_ota_start_update() == ESP_OK);
    uint32_t old_attempt = snapshot().attempt_id;
    assert(fake_ota_platform_emit_failed(ESP_FAIL) == ESP_OK);
    complete_update();
    uint32_t current = snapshot().attempt_id;
    assert(current == old_attempt + 1);
    reboot_guard_fixture_t fixture = {.attempt = current};
    assert_reboot_rejected(old_attempt, &fixture);
    assert_reboot_rejected(current + 1, &fixture);
    assert_reboot_rejected(UINT32_MAX, &fixture);
    ryz_ota_reboot_guard_t guard = fixture_guard(&fixture);
    assert(ryz_ota_reboot_to_updated_image(current, &guard) == ESP_FAIL);
    assert(fixture.begins == 1 && fixture.ends == 1);
    assert(fake_ota_platform_reboot_count() == 1);
    assert(!snapshot().reboot_pending && snapshot().attempt_id == current);
}

static void test_reboot_cleanup_gates(void)
{
    ready();
    reboot_guard_fixture_t fixture = {.attempt = 1};
    assert_reboot_rejected(1, &fixture);
    assert(ryz_ota_start_update() == ESP_OK);
    fixture.attempt = snapshot().attempt_id;
    assert_reboot_rejected(fixture.attempt, &fixture);
    assert(fake_ota_platform_emit_candidate("ryzobee_rootmaker", "0.8.0", 1000) == ESP_OK);
    assert_reboot_rejected(fixture.attempt, &fixture);
    assert(fake_ota_platform_emit_verifying() == ESP_OK);
    assert_reboot_rejected(fixture.attempt, &fixture);
    assert(fake_ota_platform_emit_completed_with_cleanup(false, ESP_FAIL) == ESP_ERR_INVALID_STATE);
    assert(snapshot().worker_active && !snapshot().cleanup_confirmed);
    assert_reboot_rejected(fixture.attempt, &fixture);
    assert(fake_ota_platform_emit_completed_with_cleanup(true, ESP_FAIL) == ESP_OK);
    ryz_ota_snapshot_t value = snapshot();
    assert(value.state == RYZ_OTA_READY_TO_REBOOT && value.reboot_required);
    assert(!value.worker_active && value.cleanup_confirmed && value.cleanup_error == ESP_FAIL);
    assert_reboot_rejected(fixture.attempt, &fixture);
}

static void test_reboot_failed_active_and_unknown_cleanup(void)
{
    ready();
    assert(ryz_ota_start_update() == ESP_OK);
    reboot_guard_fixture_t fixture = {.attempt = snapshot().attempt_id};
    assert(fake_ota_platform_emit_candidate("wrong_product", "0.8.0", 1000) == ESP_ERR_INVALID_RESPONSE);
    assert(snapshot().state == RYZ_OTA_FAILED && snapshot().worker_active);
    assert_reboot_rejected(fixture.attempt, &fixture);
    assert(fake_ota_platform_emit_terminal(fixture.attempt, false, ESP_FAIL) == ESP_OK);
    assert(!snapshot().worker_active && !snapshot().cleanup_confirmed);
    assert_reboot_rejected(fixture.attempt, &fixture);
}

static void test_reboot_pending_confirmation(void)
{
    fake_ota_platform_reset();
    fake_ota_platform_set_pending_confirmation(true);
    assert(ryz_ota_init() == ESP_OK);
    assert(ryz_ota_set_prerequisites(true, true) == ESP_OK);
    reboot_guard_fixture_t fixture = {.attempt = 1};
    assert(snapshot().pending_confirmation);
    assert_reboot_rejected(1, &fixture);
    assert(ryz_ota_start_update() == ESP_ERR_INVALID_STATE);
    /* A real pending-confirmation image cannot create a READY attempt until
     * confirmed; exercise that public boundary instead of mutating core state. */
    assert(ryz_ota_confirm_running_image_healthy() == ESP_OK);
    complete_update();
    fixture.attempt = snapshot().attempt_id;
    ryz_ota_reboot_guard_t guard = fixture_guard(&fixture);
    assert(ryz_ota_reboot_to_updated_image(fixture.attempt, &guard) == ESP_FAIL);
    assert(fixture.begins == 1 && fixture.ends == 1);
}

typedef struct {
    uint32_t attempt;
    const ryz_ota_reboot_guard_t *guard;
    esp_err_t result;
} reboot_thread_t;

static void *reboot_thread(void *context)
{
    reboot_thread_t *thread = context;
    thread->result = ryz_ota_reboot_to_updated_image(thread->attempt, thread->guard);
    return NULL;
}

static void test_reboot_reservation_survives_network_hold(unsigned point, bool reject)
{
    ready();
    complete_update();
    uint32_t attempt = snapshot().attempt_id;
    reboot_guard_fixture_t fixture = {
        .attempt = attempt,
        .result = reject ? ESP_ERR_NO_MEM : ESP_OK,
        .block_at = point,
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .condition = PTHREAD_COND_INITIALIZER,
    };
    ryz_ota_reboot_guard_t guard = fixture_guard(&fixture);
    fake_ota_platform_set_reboot_hook(fixture_reboot, &fixture);
    reboot_thread_t thread = {.attempt = attempt, .guard = &guard, .result = ESP_OK};
    pthread_t worker;
    assert(pthread_create(&worker, NULL, reboot_thread, &thread) == 0);
    assert(pthread_mutex_lock(&fixture.mutex) == 0);
    while (!fixture.entered)
        assert(pthread_cond_wait(&fixture.condition, &fixture.mutex) == 0);
    assert(pthread_mutex_unlock(&fixture.mutex) == 0);

    assert(snapshot().reboot_pending);
    assert(ryz_ota_network_hold(true) == ESP_OK);
    ryz_ota_snapshot_t held = snapshot();
    assert(held.reboot_pending && held.network_held && held.attempt_id == attempt);
    assert(held.state == RYZ_OTA_READY_TO_REBOOT && !held.worker_active);
    assert(ryz_ota_set_prerequisites(false, false) == ESP_OK);
    assert(snapshot().reboot_pending);
    assert(ryz_ota_network_hold(false) == ESP_OK);
    assert(ryz_ota_set_prerequisites(true, true) == ESP_OK);
    assert(snapshot().reboot_pending && !snapshot().network_held);
    assert(ryz_ota_start_update() == ESP_ERR_INVALID_STATE);
    assert(fake_ota_platform_start_count() == 1);
    assert(ryz_ota_cancel_update() == ESP_ERR_INVALID_STATE);
    reboot_guard_fixture_t duplicate = {.attempt = attempt};
    ryz_ota_reboot_guard_t duplicate_guard = fixture_guard(&duplicate);
    assert(ryz_ota_reboot_to_updated_image(attempt, &duplicate_guard) == ESP_ERR_INVALID_STATE);
    assert(!duplicate.begins && !duplicate.ends);
    assert(snapshot().reboot_pending);
    assert(fixture.begins == 1 && fixture.ends == 0);

    assert(pthread_mutex_lock(&fixture.mutex) == 0);
    fixture.proceed = true;
    assert(pthread_cond_broadcast(&fixture.condition) == 0);
    assert(pthread_mutex_unlock(&fixture.mutex) == 0);
    assert(pthread_join(worker, NULL) == 0);
    assert(thread.result == (reject ? ESP_ERR_NO_MEM : ESP_FAIL));
    assert(fixture.begins == 1 && fixture.ends == (reject ? 0U : 1U));
    assert(!fixture.held && !snapshot().reboot_pending);
    assert(snapshot().state == RYZ_OTA_READY_TO_REBOOT && snapshot().reboot_required);
    assert(snapshot().attempt_id == attempt);
    assert(strcmp(snapshot().candidate_version, "0.8.0") == 0);
    assert(fake_ota_platform_reboot_count() == (reject ? 0U : 1U));
    fake_ota_platform_set_reboot_hook(NULL, NULL);
    assert(pthread_cond_destroy(&fixture.condition) == 0);
    assert(pthread_mutex_destroy(&fixture.mutex) == 0);
}

static void test_reboot_guard_rejection_and_explicit_retry(void)
{
    ready();
    complete_update();
    ryz_ota_snapshot_t before = snapshot();
    reboot_guard_fixture_t fixture = {
        .result = ESP_ERR_INVALID_STATE,
        .attempt = before.attempt_id,
    };
    ryz_ota_reboot_guard_t guard = fixture_guard(&fixture);
    assert(ryz_ota_reboot_to_updated_image(before.attempt_id, &guard) == ESP_ERR_INVALID_STATE);
    assert(fixture.begins == 1 && fixture.ends == 0 && !fixture.held);
    assert(fake_ota_platform_reboot_count() == 0);
    ryz_ota_snapshot_t after = snapshot();
    assert(!after.reboot_pending && after.reboot_required);
    assert(after.state == RYZ_OTA_READY_TO_REBOOT);
    assert(after.attempt_id == before.attempt_id);
    assert(strcmp(after.candidate_version, before.candidate_version) == 0);
    assert(after.revision > before.revision);

    fixture.result = ESP_OK;
    assert(ryz_ota_reboot_to_updated_image(before.attempt_id, &guard) == ESP_FAIL);
    assert(fixture.begins == 2 && fixture.ends == 1 && !fixture.held);
    assert(fake_ota_platform_reboot_count() == 1);
    after = snapshot();
    assert(!after.reboot_pending && after.reboot_required);
    assert(after.state == RYZ_OTA_READY_TO_REBOOT);
    assert(strcmp(after.candidate_version, before.candidate_version) == 0);
    /* Neither failure silently retries the platform call. */
    assert(fake_ota_platform_reboot_count() == 1);
}

static void test_preinit_hold(void)
{
    assert(ryz_ota_network_hold(true) == ESP_OK);
    ready();
    ryz_ota_snapshot_t value = snapshot();
    assert(value.network_held && value.cleanup_confirmed && !value.worker_active);
    assert(value.state == RYZ_OTA_WAITING_PREREQUISITES);
    assert(ryz_ota_start_update() == ESP_ERR_INVALID_STATE);
    assert(ryz_ota_network_hold(false) == ESP_OK);
    assert(snapshot().state == RYZ_OTA_IDLE);
    assert(fake_ota_platform_start_count() == 0);
    assert(ryz_ota_start_update() == ESP_OK);
}

static void test_hold_cancel(bool downloading)
{
    ready();
    assert(ryz_ota_start_update() == ESP_OK);
    if (downloading) assert(fake_ota_platform_emit_candidate(
        "ryzobee_rootmaker", "0.8.0", 1000) == ESP_OK);
    assert(ryz_ota_network_hold(true) == ESP_OK);
    assert(ryz_ota_network_hold(true) == ESP_OK);
    ryz_ota_snapshot_t value = snapshot();
    assert(value.network_held && value.worker_active && !value.cleanup_confirmed);
    assert(value.state == RYZ_OTA_CANCELLING && value.cancel_requested);
    assert(fake_ota_platform_cancel_count() == 1);
    assert(ryz_ota_network_hold(false) == ESP_OK);
    assert(ryz_ota_start_update() == ESP_ERR_INVALID_STATE);
    assert(fake_ota_platform_emit_cancelled() == ESP_OK);
    value = snapshot();
    assert(!value.worker_active && value.cleanup_confirmed);
    assert(value.state == RYZ_OTA_CANCELLED);
    assert(fake_ota_platform_start_count() == 1);
    assert(ryz_ota_start_update() == ESP_OK);
}

static void test_hold_verifying(void)
{
    ready();
    assert(ryz_ota_start_update() == ESP_OK);
    assert(fake_ota_platform_emit_candidate("ryzobee_rootmaker", "0.8.0", 1000) == ESP_OK);
    assert(fake_ota_platform_emit_verifying() == ESP_OK);
    assert(ryz_ota_network_hold(true) == ESP_OK);
    ryz_ota_snapshot_t value = snapshot();
    assert(value.state == RYZ_OTA_VERIFYING && value.worker_active);
    assert(!value.cleanup_confirmed && !value.cancel_requested);
    assert(value.network_held && fake_ota_platform_cancel_count() == 0);
    assert(fake_ota_platform_emit_completed() == ESP_OK);
    value = snapshot();
    assert(value.network_held && value.cleanup_confirmed && !value.worker_active);
    assert(value.state == RYZ_OTA_READY_TO_REBOOT);
}

static void test_hold_cancel_error(void)
{
    ready();
    assert(ryz_ota_start_update() == ESP_OK);
    fake_ota_platform_set_cancel_result(ESP_FAIL);
    assert(ryz_ota_network_hold(true) == ESP_OK);
    ryz_ota_snapshot_t value = snapshot();
    assert(value.state == RYZ_OTA_FAILED && value.last_error == ESP_FAIL);
    assert(value.network_held && value.worker_active && !value.cleanup_confirmed);
    assert(ryz_ota_network_hold(false) == ESP_OK);
    assert(ryz_ota_start_update() == ESP_ERR_INVALID_STATE);
    assert(fake_ota_platform_emit_failed(ESP_ERR_INVALID_RESPONSE) == ESP_OK);
    value = snapshot();
    assert(!value.worker_active && value.cleanup_confirmed && value.last_error == ESP_FAIL);
    assert(ryz_ota_start_update() == ESP_OK);
}

static void test_old_attempt_cannot_release_new_worker(void)
{
    ready();
    assert(ryz_ota_start_update() == ESP_OK);
    uint32_t previous = snapshot().attempt_id;
    assert(fake_ota_platform_emit_failed(ESP_FAIL) == ESP_OK);
    assert(ryz_ota_start_update() == ESP_OK);
    ryz_ota_snapshot_t before = snapshot();
    assert(before.attempt_id == previous + 1);
    assert(fake_ota_platform_emit_terminal(previous, true, ESP_OK) == ESP_ERR_INVALID_STATE);
    assert(fake_ota_platform_emit_terminal(0, true, ESP_OK) == ESP_ERR_INVALID_STATE);
    ryz_ota_snapshot_t after = snapshot();
    assert(after.revision == before.revision && after.attempt_id == before.attempt_id);
    assert(after.state == RYZ_OTA_STARTING && after.worker_active && !after.cleanup_confirmed);
}

static void test_unconfirmed_cleanup_fails_closed(void)
{
    ready();
    assert(ryz_ota_start_update() == ESP_OK);
    uint32_t attempt = snapshot().attempt_id;
    assert(fake_ota_platform_emit_terminal(attempt, false, ESP_ERR_INVALID_STATE) == ESP_OK);
    ryz_ota_snapshot_t value = snapshot();
    assert(value.state == RYZ_OTA_FAILED && !value.worker_active && !value.cleanup_confirmed);
    assert(value.cleanup_error == ESP_ERR_INVALID_STATE);
    assert(ryz_ota_network_hold(true) == ESP_OK);
    assert(ryz_ota_network_hold(false) == ESP_OK);
    assert(ryz_ota_start_update() == ESP_ERR_INVALID_STATE);
    assert(fake_ota_platform_emit_terminal(attempt, true, ESP_OK) == ESP_ERR_INVALID_STATE);
    assert(!snapshot().cleanup_confirmed);
}

static void test_failures_are_visible_and_retryable(void)
{
    fake_ota_platform_reset();
    assert(ryz_ota_init() == ESP_OK);
    assert(ryz_ota_set_prerequisites(true, true) == ESP_OK);

    fake_ota_platform_set_start_result(ESP_FAIL);
    assert(ryz_ota_start_update() == ESP_FAIL);
    ryz_ota_snapshot_t snapshot = {0};
    assert(ryz_ota_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RYZ_OTA_FAILED);
    assert(snapshot.last_error == ESP_FAIL);

    fake_ota_platform_set_start_result(ESP_OK);
    assert(ryz_ota_start_update() == ESP_OK);
    assert(fake_ota_platform_emit_failed(ESP_ERR_INVALID_RESPONSE) == ESP_OK);
    assert(ryz_ota_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RYZ_OTA_FAILED);
    assert(snapshot.last_error == ESP_ERR_INVALID_RESPONSE);

    assert(ryz_ota_start_update() == ESP_OK);
    fake_ota_platform_set_cancel_result(ESP_FAIL);
    assert(ryz_ota_cancel_update() == ESP_FAIL);
    assert(ryz_ota_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RYZ_OTA_FAILED);
    assert(snapshot.last_error == ESP_FAIL);
    assert(!snapshot.cancel_requested);
}

static void test_disabled_configuration_and_boot_confirmation(void)
{
    fake_ota_platform_reset();
    fake_ota_platform_set_configured(false);
    fake_ota_platform_set_pending_confirmation(true);
    assert(ryz_ota_confirm_running_image_healthy() == ESP_ERR_INVALID_STATE);
    assert(ryz_ota_init() == ESP_OK);

    ryz_ota_snapshot_t snapshot = {0};
    assert(ryz_ota_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RYZ_OTA_DISABLED);
    assert(!snapshot.configured);
    assert(snapshot.pending_confirmation);
    assert(ryz_ota_set_prerequisites(true, true) == ESP_OK);
    assert(ryz_ota_start_update() == ESP_ERR_NOT_SUPPORTED);
    assert(fake_ota_platform_start_count() == 0);
    assert(ryz_ota_reboot_to_updated_image(1, &allow_guard) == ESP_ERR_INVALID_STATE);

    assert(ryz_ota_confirm_running_image_healthy() == ESP_OK);
    assert(fake_ota_platform_mark_valid_count() == 1);
    assert(ryz_ota_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RYZ_OTA_DISABLED);
    assert(!snapshot.pending_confirmation);
    assert(snapshot.last_error == ESP_OK);
    assert(ryz_ota_confirm_running_image_healthy() == ESP_OK);
    assert(fake_ota_platform_mark_valid_count() == 1);
}

static void test_boot_confirmation_failure_is_retryable(void)
{
    fake_ota_platform_reset();
    fake_ota_platform_set_pending_confirmation(true);
    fake_ota_platform_set_mark_valid_result(ESP_FAIL);
    assert(ryz_ota_init() == ESP_OK);
    assert(ryz_ota_confirm_running_image_healthy() == ESP_FAIL);

    ryz_ota_snapshot_t snapshot = {0};
    assert(ryz_ota_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.pending_confirmation);
    assert(snapshot.last_error == ESP_FAIL);
    fake_ota_platform_set_mark_valid_result(ESP_OK);
    assert(ryz_ota_confirm_running_image_healthy() == ESP_OK);
    assert(fake_ota_platform_mark_valid_count() == 2);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "initial") == 0) {
        test_initial_state();
    } else if (strcmp(argv[1], "running-info") == 0) {
        test_running_metadata_is_copied_at_boot();
    } else if (strcmp(argv[1], "gating") == 0) {
        test_prerequisite_gating();
    } else if (strcmp(argv[1], "progress") == 0) {
        test_candidate_and_monotonic_progress();
    } else if (strcmp(argv[1], "wrong-project") == 0) {
        test_candidate_rejection("another_product", "0.8.0",
                                 ESP_ERR_INVALID_RESPONSE);
    } else if (strcmp(argv[1], "same-version") == 0) {
        test_candidate_rejection("ryzobee_rootmaker", "0.7.0",
                                 ESP_ERR_INVALID_VERSION);
    } else if (strcmp(argv[1], "empty-version") == 0) {
        test_candidate_rejection("ryzobee_rootmaker", "",
                                 ESP_ERR_INVALID_VERSION);
    } else if (strcmp(argv[1], "cancel") == 0) {
        test_cooperative_cancellation();
    } else if (strcmp(argv[1], "candidate-pending-cleanup") == 0) {
        test_candidate_failure_still_owns_worker();
    } else if (strcmp(argv[1], "preinit-hold") == 0) {
        test_preinit_hold();
    } else if (strcmp(argv[1], "hold-starting") == 0) {
        test_hold_cancel(false);
    } else if (strcmp(argv[1], "hold-downloading") == 0) {
        test_hold_cancel(true);
    } else if (strcmp(argv[1], "hold-verifying") == 0) {
        test_hold_verifying();
    } else if (strcmp(argv[1], "hold-cancel-error") == 0) {
        test_hold_cancel_error();
    } else if (strcmp(argv[1], "old-attempt") == 0) {
        test_old_attempt_cannot_release_new_worker();
    } else if (strcmp(argv[1], "cleanup-unknown") == 0) {
        test_unconfirmed_cleanup_fails_closed();
    } else if (strcmp(argv[1], "failure") == 0) {
        test_failures_are_visible_and_retryable();
    } else if (strcmp(argv[1], "disabled") == 0) {
        test_disabled_configuration_and_boot_confirmation();
    } else if (strcmp(argv[1], "confirm-failure") == 0) {
        test_boot_confirmation_failure_is_retryable();
    } else if (strcmp(argv[1], "reboot-guard") == 0) {
        test_reboot_guard_rejection_and_explicit_retry();
    } else if (strcmp(argv[1], "reboot-arguments") == 0) {
        test_reboot_arguments();
    } else if (strcmp(argv[1], "reboot-attempt") == 0) {
        test_reboot_exact_attempt();
    } else if (strcmp(argv[1], "reboot-cleanup") == 0) {
        test_reboot_cleanup_gates();
    } else if (strcmp(argv[1], "reboot-failed-active") == 0) {
        test_reboot_failed_active_and_unknown_cleanup();
    } else if (strcmp(argv[1], "reboot-confirmation") == 0) {
        test_reboot_pending_confirmation();
    } else if (strcmp(argv[1], "reboot-begin-pending") == 0) {
        test_reboot_reservation_survives_network_hold(BLOCK_BEGIN, false);
    } else if (strcmp(argv[1], "reboot-begin-rejected") == 0) {
        test_reboot_reservation_survives_network_hold(BLOCK_BEGIN, true);
    } else if (strcmp(argv[1], "reboot-platform-pending") == 0) {
        test_reboot_reservation_survives_network_hold(BLOCK_REBOOT, false);
    } else if (strcmp(argv[1], "reboot-end-pending") == 0) {
        test_reboot_reservation_survives_network_hold(BLOCK_END, false);
    } else {
        assert(!"unknown scenario");
    }
    puts("RYZ_OTA_STATE_PASS");
    return 0;
}
