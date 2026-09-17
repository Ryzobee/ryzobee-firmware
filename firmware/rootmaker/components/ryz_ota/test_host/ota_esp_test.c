#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ota_sdk.h"
#include "ryz_ota.h"

static TaskFunction_t s_entry;
static void *s_argument;
static unsigned s_starts, s_deleted, s_begins, s_aborts, s_finishes;
static bool s_wrong_project, s_check_abort_window, s_handle_live;
static bool s_mutex_fail, s_task_fail, s_description_missing;
static bool s_incomplete, s_begin_null_success, s_begin_nonnull_error;
static unsigned s_mutex_created, s_mutex_deleted;
static esp_err_t s_begin_error, s_desc_error, s_perform_error, s_abort_error, s_finish_error;
typedef enum { HOLD_NONE, HOLD_INIT, HOLD_BEGIN, HOLD_DESC, HOLD_PERFORM,
               HOLD_FINISH, HOLD_VERIFY_LOCK } hold_at_t;
static hold_at_t s_hold_at;
static bool s_arm_verify_lock, s_release_hold, s_hold_waiting;
static pthread_t s_hold_thread;
static _Thread_local bool s_is_hold_thread;
static pthread_mutex_t s_gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_changed = PTHREAD_COND_INITIALIZER;
static void *s_handle;
static const esp_app_desc_t s_running = {"ryzobee_rootmaker", "0.7.0"};
static esp_partition_t s_partition = {
    .type = ESP_PARTITION_TYPE_APP, .subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0,
    .address = 0x10000, .size = 0x300000, .erase_size = 0x1000, .label = "ota_0",
};
static esp_partition_t s_ota0, s_ota1, s_otadata;
static bool s_missing_a, s_missing_b, s_missing_data, s_running_missing;
static esp_ota_img_states_t s_image_state = ESP_OTA_IMG_VALID;
static esp_err_t s_state_error, s_mark_error, s_readback_error;
static bool s_mark_leaves_pending, s_mark_changes_partition;
static unsigned s_state_reads, s_find_calls, s_mark_calls;

static ryz_ota_snapshot_t snapshot(void)
{
    ryz_ota_snapshot_t value;
    assert(ryz_ota_get_snapshot(&value) == ESP_OK);
    return value;
}

static void hold_at(hold_at_t stage)
{
    if (s_hold_at != stage) return;
    assert(ryz_ota_network_hold(true) == ESP_OK);
    if (stage == HOLD_INIT) return;
    ryz_ota_snapshot_t value = snapshot();
    assert(value.network_held && value.worker_active && !value.cleanup_confirmed);
    assert(value.state == (stage == HOLD_FINISH ? RYZ_OTA_VERIFYING : RYZ_OTA_CANCELLING));
    assert(value.cancel_requested == (stage != HOLD_FINISH));
    assert(ryz_ota_start_update() == ESP_ERR_INVALID_STATE);
}

static void *hold_concurrently(void *unused)
{
    (void)unused;
    s_is_hold_thread = true;
    assert(ryz_ota_network_hold(true) == ESP_OK);
    return NULL;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    if (s_mutex_fail) return NULL;
    SemaphoreHandle_t value = malloc(sizeof(*value));
    assert(value && pthread_mutex_init(value, NULL) == 0);
    ++s_mutex_created;
    return value;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t wait)
{
    (void)wait;
    if (s_is_hold_thread) {
        /* network_hold has already latched atomic intent before this seam. */
        assert(pthread_mutex_lock(&s_gate) == 0);
        s_hold_waiting = true;
        assert(pthread_cond_broadcast(&s_changed) == 0);
        while (!s_release_hold) assert(pthread_cond_wait(&s_changed, &s_gate) == 0);
        assert(pthread_mutex_unlock(&s_gate) == 0);
    } else if (s_arm_verify_lock) {
        s_arm_verify_lock = false;
        assert(pthread_create(&s_hold_thread, NULL, hold_concurrently, NULL) == 0);
        assert(pthread_mutex_lock(&s_gate) == 0);
        while (!s_hold_waiting) assert(pthread_cond_wait(&s_changed, &s_gate) == 0);
        assert(pthread_mutex_unlock(&s_gate) == 0);
    }
    assert(pthread_mutex_lock(mutex) == 0);
    return pdPASS;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex)
{
    assert(pthread_mutex_unlock(mutex) == 0);
    if (!s_is_hold_thread && s_hold_waiting && !s_release_hold) {
        assert(pthread_mutex_lock(&s_gate) == 0);
        s_release_hold = true;
        assert(pthread_cond_broadcast(&s_changed) == 0);
        assert(pthread_mutex_unlock(&s_gate) == 0);
        assert(pthread_join(s_hold_thread, NULL) == 0);
    }
    return pdPASS;
}
void vSemaphoreDelete(SemaphoreHandle_t mutex)
{ assert(pthread_mutex_destroy(mutex) == 0); free(mutex); ++s_mutex_deleted; }
BaseType_t xTaskCreate(TaskFunction_t entry, const char *name, uint32_t stack,
                      void *argument, unsigned priority, TaskHandle_t *handle)
{
    assert(!s_entry && !strcmp(name, "ryz-https-ota"));
    assert(stack == 8192 && priority == 5 && !handle && argument);
    ++s_starts;
    if (s_task_fail) return pdFALSE;
    s_entry = entry; s_argument = argument;
    return pdPASS;
}
void vTaskDelete(TaskHandle_t handle) { assert(!handle); ++s_deleted; }
void esp_crt_bundle_attach(void) {}
void esp_restart(void) { assert(!"unexpected reboot"); }
const esp_app_desc_t *esp_app_get_description(void)
{ hold_at(HOLD_INIT); return s_description_missing ? NULL : &s_running; }
const esp_partition_t *esp_ota_get_running_partition(void)
{ return s_running_missing ? NULL : &s_partition; }
const esp_partition_t *esp_partition_find_first(
    esp_partition_type_t type, esp_partition_subtype_t subtype, const char *label)
{
    assert(label == NULL);
    ++s_find_calls;
    if (type == ESP_PARTITION_TYPE_APP && subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0)
        return s_missing_a ? NULL : &s_ota0;
    if (type == ESP_PARTITION_TYPE_APP && subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1)
        return s_missing_b ? NULL : &s_ota1;
    assert(type == ESP_PARTITION_TYPE_DATA && subtype == ESP_PARTITION_SUBTYPE_DATA_OTA);
    return s_missing_data ? NULL : &s_otadata;
}
esp_err_t esp_ota_get_state_partition(const esp_partition_t *partition, esp_ota_img_states_t *out)
{
    assert(partition == &s_partition);
    ++s_state_reads;
    if (s_mark_calls && s_readback_error != ESP_OK) return s_readback_error;
    if (s_state_error == ESP_OK) *out = s_image_state;
    return s_state_error;
}
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void)
{
    ++s_mark_calls;
    if (s_mark_error != ESP_OK) return s_mark_error;
    if (!s_mark_leaves_pending) s_image_state = ESP_OTA_IMG_VALID;
    if (s_mark_changes_partition) s_partition.address += 0x10000;
    return ESP_OK;
}
esp_err_t esp_https_ota_begin(const esp_https_ota_config_t *config, esp_https_ota_handle_t *out)
{
    assert(!strcmp(config->http_config->url, "https://ota.invalid/fixture.bin"));
    assert(config->http_config->disable_auto_redirect && config->http_config->crt_bundle_attach);
    assert(!s_handle_live);
    ++s_begins;
    hold_at(HOLD_BEGIN);
    if ((s_begin_error != ESP_OK && !s_begin_nonnull_error) || s_begin_null_success) {
        *out = NULL;
        return s_begin_error;
    }
    s_handle = malloc(1); assert(s_handle);
    *out = s_handle; s_handle_live = true;
    return s_begin_error;
}
static void valid_handle(void *handle) { assert(s_handle_live && handle == s_handle); }
esp_err_t esp_https_ota_get_img_desc(esp_https_ota_handle_t handle, esp_app_desc_t *out)
{
    valid_handle(handle);
    hold_at(HOLD_DESC);
    *out = (esp_app_desc_t){"ryzobee_rootmaker", "0.8.0"};
    if (s_wrong_project) strcpy(out->project_name, "another_product");
    return s_desc_error;
}
int esp_https_ota_get_image_size(esp_https_ota_handle_t handle) { valid_handle(handle); return 1000; }
int esp_https_ota_get_image_len_read(esp_https_ota_handle_t handle) { valid_handle(handle); return 1000; }
esp_err_t esp_https_ota_perform(esp_https_ota_handle_t handle)
{ valid_handle(handle); hold_at(HOLD_PERFORM); return s_perform_error; }
bool esp_https_ota_is_complete_data_received(esp_https_ota_handle_t handle)
{
    valid_handle(handle);
    if (s_hold_at == HOLD_VERIFY_LOCK) s_arm_verify_lock = true;
    return !s_incomplete;
}
esp_err_t esp_https_ota_abort(esp_https_ota_handle_t handle)
{
    valid_handle(handle); ++s_aborts;
    if (s_check_abort_window) {
        ryz_ota_snapshot_t state; assert(ryz_ota_get_snapshot(&state) == ESP_OK);
        assert(state.state == RYZ_OTA_FAILED && state.worker_active && !state.cleanup_confirmed);
        esp_err_t second = ryz_ota_start_update();
        assert(second == ESP_ERR_INVALID_STATE && s_starts == 1);
    }
    free(s_handle); s_handle = NULL; s_handle_live = false;
    return s_abort_error;
}
esp_err_t esp_https_ota_finish(esp_https_ota_handle_t handle)
{
    valid_handle(handle); ++s_finishes;
    hold_at(HOLD_FINISH);
    free(s_handle); s_handle = NULL; s_handle_live = false;
    return s_finish_error;
}
static void run_worker(void)
{
    TaskFunction_t entry = s_entry; void *argument = s_argument;
    assert(entry); s_entry = NULL; s_argument = NULL;
    unsigned deleted = s_deleted;
    entry(argument);
    assert(s_deleted == deleted + 1);
}

static void assert_no_metadata_io(void)
{
    unsigned reads = s_state_reads, finds = s_find_calls;
    ryz_ota_running_info_t before = snapshot().running_info;
    for (unsigned i = 0; i < 8; ++i) {
        assert(ryz_ota_init() == ESP_OK);
        ryz_ota_snapshot_t value = snapshot();
        assert(memcmp(&value.running_info, &before, sizeof(before)) == 0);
    }
    assert(s_state_reads == reads && s_find_calls == finds);
    if (!snapshot().pending_confirmation) {
        assert(ryz_ota_confirm_running_image_healthy() == ESP_OK);
        ryz_ota_snapshot_t value = snapshot();
        assert(memcmp(&value.running_info, &before, sizeof(before)) == 0);
        assert(s_state_reads == reads && s_find_calls == finds && s_mark_calls == 0);
    }
}

static void metadata_case(const char *name)
{
    ryz_ota_image_state_t expected = RYZ_OTA_IMAGE_STATE_VALID;
    esp_err_t init_error = ESP_OK;
    bool expected_known = true, ab_known = true, ab = true;
    if (!strcmp(name, "meta-new")) { s_image_state = ESP_OTA_IMG_NEW; expected = RYZ_OTA_IMAGE_STATE_NEW; }
    else if (!strcmp(name, "meta-pending")) { s_image_state = ESP_OTA_IMG_PENDING_VERIFY; expected = RYZ_OTA_IMAGE_STATE_PENDING_VERIFY; }
    else if (!strcmp(name, "meta-valid")) {}
    else if (!strcmp(name, "meta-invalid")) { s_image_state = ESP_OTA_IMG_INVALID; expected = RYZ_OTA_IMAGE_STATE_INVALID; }
    else if (!strcmp(name, "meta-aborted")) { s_image_state = ESP_OTA_IMG_ABORTED; expected = RYZ_OTA_IMAGE_STATE_ABORTED; }
    else if (!strcmp(name, "meta-undefined")) { s_image_state = ESP_OTA_IMG_UNDEFINED; expected = RYZ_OTA_IMAGE_STATE_UNDEFINED; }
    else if (!strcmp(name, "meta-factory")) {
        s_partition.subtype = ESP_PARTITION_SUBTYPE_APP_FACTORY;
        s_partition.address = 0x710000;
        strcpy(s_partition.label, "factory");
        s_state_error = ESP_ERR_NOT_SUPPORTED;
        expected_known = false;
    } else if (!strcmp(name, "meta-label16")) {
        strcpy(s_partition.label, "0123456789abcdef");
    } else if (!strcmp(name, "meta-no-state")) {
        s_state_error = ESP_ERR_NOT_FOUND; expected_known = false;
    } else if (!strcmp(name, "meta-state-unsupported")) {
        s_state_error = ESP_ERR_NOT_SUPPORTED; expected_known = false;
    } else if (!strcmp(name, "meta-state-error")) {
        s_state_error = init_error = ESP_FAIL;
    } else if (!strcmp(name, "meta-state-malformed")) {
        s_image_state = (esp_ota_img_states_t)0x55;
        init_error = ESP_ERR_INVALID_RESPONSE;
    } else if (!strcmp(name, "meta-running-missing")) {
        s_running_missing = true; init_error = ESP_ERR_INVALID_STATE;
    } else if (!strcmp(name, "meta-running-malformed")) {
        s_partition.address = 1; init_error = ESP_ERR_INVALID_RESPONSE;
    } else if (!strcmp(name, "meta-ab-missing-a")) { s_missing_a = true; ab = false; }
    else if (!strcmp(name, "meta-ab-missing-b")) { s_missing_b = true; ab = false; }
    else if (!strcmp(name, "meta-ab-missing-data")) { s_missing_data = true; ab = false; }
    else {
        ab_known = ab = false;
        if (!strcmp(name, "meta-ab-type")) s_ota1.type = ESP_PARTITION_TYPE_DATA;
        else if (!strcmp(name, "meta-ab-subtype")) s_ota1.subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0;
        else if (!strcmp(name, "meta-ab-address")) s_ota1.address += 1;
        else if (!strcmp(name, "meta-ab-size")) s_ota1.size = 0;
        else if (!strcmp(name, "meta-ab-data-size")) s_otadata.size = 0x1000;
        else if (!strcmp(name, "meta-ab-overflow")) { s_ota1.address = 0xffff0000; s_ota1.size = 0x20000; }
        else if (!strcmp(name, "meta-ab-overlap")) s_ota1.address = s_ota0.address;
        else if (!strcmp(name, "meta-ab-chip")) s_ota1.flash_chip = &s_ota1;
        else if (!strcmp(name, "meta-ab-erase")) s_otadata.erase_size = 0;
        else if (!strcmp(name, "meta-ab-label")) memset(s_ota1.label, 'X', sizeof(s_ota1.label));
        else assert(!"unknown metadata scenario");
    }
    esp_err_t error = ryz_ota_init();
    assert(error == init_error);
    if (init_error != ESP_OK) {
        ryz_ota_snapshot_t missing;
        assert(ryz_ota_get_snapshot(&missing) == ESP_ERR_INVALID_STATE);
        assert(s_mutex_created == 1 && s_mutex_deleted == 1);
        s_state_error = ESP_OK; s_image_state = ESP_OTA_IMG_VALID;
        s_running_missing = false; s_partition = s_ota0;
        assert(ryz_ota_init() == ESP_OK);
        assert(snapshot().running_info.state_known);
        assert(snapshot().running_info.state == RYZ_OTA_IMAGE_STATE_VALID);
        assert(s_mutex_created == 2 && s_mutex_deleted == 1);
        assert_no_metadata_io();
        return;
    }
    ryz_ota_snapshot_t value = snapshot();
    assert(value.running_info.partition_known);
    assert(!strcmp(value.running_info.running_label, s_partition.label));
    assert(value.running_info.running_subtype == (uint8_t)s_partition.subtype);
    assert(value.running_info.state_known == expected_known);
    assert(value.running_info.state == (expected_known ? expected : RYZ_OTA_IMAGE_STATE_UNDEFINED));
    assert(value.running_info.state_error == s_state_error);
    assert(value.running_info.ab_slots_known == ab_known && value.running_info.ab_slots == ab);
    assert(value.pending_confirmation == (expected_known && expected == RYZ_OTA_IMAGE_STATE_PENDING_VERIFY));
    assert(s_state_reads == 1 && s_find_calls == 3);
    assert_no_metadata_io();
}

static void confirmation_case(const char *name)
{
    s_image_state = ESP_OTA_IMG_PENDING_VERIFY;
    assert(ryz_ota_init() == ESP_OK);
    assert(snapshot().pending_confirmation);
    esp_err_t expected = ESP_OK;
    unsigned expected_mark_calls = 1;
    bool changed_before = false;
    if (!strcmp(name, "confirm-real-success")) {}
    else if (!strcmp(name, "confirm-real-write-error")) s_mark_error = expected = ESP_FAIL;
    else if (!strcmp(name, "confirm-real-readback-error")) s_readback_error = expected = ESP_FAIL;
    else if (!strcmp(name, "confirm-real-readback-missing")) s_readback_error = expected = ESP_ERR_NOT_FOUND;
    else if (!strcmp(name, "confirm-real-readback-pending")) {
        s_mark_leaves_pending = true; expected = ESP_ERR_INVALID_STATE;
    } else if (!strcmp(name, "confirm-real-changed-after")) {
        s_mark_changes_partition = true; expected = ESP_ERR_INVALID_STATE;
    } else {
        expected_mark_calls = 0;
        if (!strcmp(name, "confirm-real-already-valid")) s_image_state = ESP_OTA_IMG_VALID;
        else if (!strcmp(name, "confirm-real-before-missing")) s_state_error = expected = ESP_ERR_NOT_FOUND;
        else if (!strcmp(name, "confirm-real-before-unsupported")) s_state_error = expected = ESP_ERR_NOT_SUPPORTED;
        else if (!strcmp(name, "confirm-real-before-invalid")) {
            s_image_state = ESP_OTA_IMG_INVALID; expected = ESP_ERR_INVALID_STATE;
        } else if (!strcmp(name, "confirm-real-changed-before")) {
            s_partition.address += 0x10000; changed_before = true;
            expected = ESP_ERR_INVALID_STATE;
        } else assert(!"unknown confirmation scenario");
    }
    unsigned reads_before = s_state_reads;
    assert(ryz_ota_confirm_running_image_healthy() == expected);
    assert(s_mark_calls == expected_mark_calls);
    if (changed_before) assert(s_state_reads == reads_before);
    ryz_ota_snapshot_t value = snapshot();
    assert(value.pending_confirmation == (expected != ESP_OK));
    assert(value.last_error == expected);
    assert(!strcmp(value.running_info.running_label, "ota_0"));
    assert(value.running_info.running_subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0);
    assert(value.running_info.ab_slots_known && value.running_info.ab_slots);
    if (expected == ESP_OK) {
        assert(value.running_info.state_known && value.running_info.state == RYZ_OTA_IMAGE_STATE_VALID);
        assert(value.running_info.state_error == ESP_OK);
    } else {
        assert(!value.running_info.state_known && value.running_info.state == RYZ_OTA_IMAGE_STATE_UNDEFINED);
        assert(value.running_info.state_error == expected);
        assert(ryz_ota_set_prerequisites(true, true) == ESP_OK);
        assert(ryz_ota_start_update() == ESP_ERR_INVALID_STATE && s_starts == 0);
        /* Explicit retry must use a fresh real observation. A write/readback
         * error never auto-confirms; an already VALID readback needs no write. */
        s_state_error = s_mark_error = s_readback_error = ESP_OK;
        s_mark_leaves_pending = s_mark_changes_partition = false;
        s_partition = s_ota0; s_image_state = ESP_OTA_IMG_PENDING_VERIFY;
        assert(ryz_ota_confirm_running_image_healthy() == ESP_OK);
        value = snapshot();
        assert(!value.pending_confirmation && value.running_info.state_known);
        assert(value.running_info.state == RYZ_OTA_IMAGE_STATE_VALID);
        assert(s_mark_calls == expected_mark_calls + 1);
    }
    unsigned reads = s_state_reads, calls = s_mark_calls, finds = s_find_calls;
    assert(ryz_ota_confirm_running_image_healthy() == ESP_OK);
    assert(s_state_reads == reads && s_mark_calls == calls && s_find_calls == finds);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *scenario = argv[1];
    s_ota0 = s_partition;
    s_ota1 = (esp_partition_t){
        .type = ESP_PARTITION_TYPE_APP, .subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1,
        .address = 0x310000, .size = 0x300000, .erase_size = 0x1000, .label = "ota_1",
    };
    s_otadata = (esp_partition_t){
        .type = ESP_PARTITION_TYPE_DATA, .subtype = ESP_PARTITION_SUBTYPE_DATA_OTA,
        .address = 0xd000, .size = 0x2000, .erase_size = 0x1000, .label = "otadata",
    };
    if (!strncmp(scenario, "meta-", 5) || !strncmp(scenario, "confirm-real-", 13)) {
        if (!strncmp(scenario, "meta-", 5)) metadata_case(scenario);
        else confirmation_case(scenario);
        puts("RYZ_OTA_ESP_PASS");
        return 0;
    }
    if (!strcmp(scenario, "running-info")) {
        assert(ryz_ota_init() == ESP_OK);
        ryz_ota_snapshot_t value = snapshot();
        assert(value.running_info.state_known);
        assert(value.running_info.state == RYZ_OTA_IMAGE_STATE_VALID);
        assert(value.running_info.partition_known);
        assert(!strcmp(value.running_info.running_label, "ota_0"));
        assert(value.running_info.running_subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0);
        assert(value.running_info.ab_slots_known && value.running_info.ab_slots);
        assert(s_state_reads == 1 && s_find_calls == 3);
        puts("RYZ_OTA_ESP_PASS");
        return 0;
    }
    if (!strcmp(scenario, "candidate-abort-window")) s_wrong_project = s_check_abort_window = true;
    else if (!strcmp(scenario, "abort-error")) { s_wrong_project = true; s_abort_error = ESP_FAIL; }
    else if (!strcmp(scenario, "begin-error")) s_begin_error = ESP_FAIL;
    else if (!strcmp(scenario, "begin-null-success")) s_begin_null_success = true;
    else if (!strcmp(scenario, "begin-unknown")) { s_begin_error = ESP_FAIL; s_begin_nonnull_error = true; }
    else if (!strcmp(scenario, "desc-error")) s_desc_error = ESP_ERR_INVALID_RESPONSE;
    else if (!strcmp(scenario, "perform-error")) s_perform_error = ESP_FAIL;
    else if (!strcmp(scenario, "incomplete")) s_incomplete = true;
    else if (!strcmp(scenario, "finish-error")) s_finish_error = ESP_ERR_INVALID_RESPONSE;
    else if (!strcmp(scenario, "hold-init")) s_hold_at = HOLD_INIT;
    else if (!strcmp(scenario, "hold-begin")) s_hold_at = HOLD_BEGIN;
    else if (!strcmp(scenario, "hold-desc")) s_hold_at = HOLD_DESC;
    else if (!strcmp(scenario, "hold-perform")) s_hold_at = HOLD_PERFORM;
    else if (!strcmp(scenario, "hold-finish")) s_hold_at = HOLD_FINISH;
    else if (!strcmp(scenario, "hold-verify-lock")) s_hold_at = HOLD_VERIFY_LOCK;
    else if (!strcmp(scenario, "init-mutex-failure")) {
        s_mutex_fail = true;
        assert(ryz_ota_network_hold(true) == ESP_OK);
        assert(ryz_ota_init() == ESP_ERR_NO_MEM);
        assert(s_mutex_created == 0);
        s_mutex_fail = false;
    } else if (!strcmp(scenario, "init-description-failure")) {
        s_description_missing = true;
        assert(ryz_ota_init() == ESP_ERR_INVALID_STATE);
        assert(s_mutex_created == 1 && s_mutex_deleted == 1);
        s_description_missing = false;
    } else assert(!strcmp(scenario, "success") || !strcmp(scenario, "task-failure") ||
                  !strcmp(scenario, "hold-queued"));
    assert(ryz_ota_init() == ESP_OK);
    assert(ryz_ota_set_prerequisites(true, true) == ESP_OK);
    if (s_hold_at == HOLD_INIT || !strcmp(scenario, "init-mutex-failure")) {
        assert(snapshot().network_held && snapshot().cleanup_confirmed);
        assert(ryz_ota_start_update() == ESP_ERR_INVALID_STATE && s_starts == 0);
        assert(ryz_ota_network_hold(false) == ESP_OK);
        assert(s_starts == 0);
    }
    if (!strcmp(scenario, "task-failure")) {
        s_task_fail = true;
        assert(ryz_ota_start_update() == ESP_ERR_NO_MEM);
        ryz_ota_snapshot_t failed = snapshot();
        assert(failed.state == RYZ_OTA_FAILED && !failed.worker_active && failed.cleanup_confirmed);
        assert(failed.last_error == ESP_ERR_NO_MEM && failed.attempt_id == 1 && !s_entry && !s_begins);
        s_task_fail = false;
    }
    assert(ryz_ota_start_update() == ESP_OK);
    assert(snapshot().worker_active && !snapshot().cleanup_confirmed);
    ryz_ota_running_info_t initial_running_info = snapshot().running_info;
    if (!strcmp(scenario, "hold-queued")) assert(ryz_ota_network_hold(true) == ESP_OK);
    run_worker();
    ryz_ota_snapshot_t result = snapshot();
    assert(memcmp(&result.running_info, &initial_running_info, sizeof(initial_running_info)) == 0);
    assert(!result.worker_active);
    if (s_begin_nonnull_error) {
        assert(result.state == RYZ_OTA_FAILED && !result.cleanup_confirmed);
        assert(result.cleanup_error == ESP_ERR_INVALID_STATE && s_handle_live);
        assert(!s_aborts && !s_finishes && ryz_ota_start_update() == ESP_ERR_INVALID_STATE);
        /* Fixture deliberately violated the SDK contract; reclaim the test
         * allocation here, never pretend production knew it was releasable. */
        free(s_handle); s_handle = NULL; s_handle_live = false;
    } else {
        assert(result.cleanup_confirmed && !s_handle_live);
        assert(result.cleanup_error == s_abort_error);
        bool cancelled = !strcmp(scenario, "hold-queued") || s_hold_at == HOLD_BEGIN ||
            s_hold_at == HOLD_DESC || s_hold_at == HOLD_PERFORM || s_hold_at == HOLD_VERIFY_LOCK;
        bool failed = s_wrong_project || s_begin_error || s_begin_null_success || s_desc_error ||
            s_perform_error || s_incomplete || s_finish_error;
        assert(result.state == (cancelled ? RYZ_OTA_CANCELLED : failed ? RYZ_OTA_FAILED : RYZ_OTA_READY_TO_REBOOT));
        if (s_wrong_project) assert(result.last_error == ESP_ERR_INVALID_RESPONSE);
        if (s_finish_error) assert(result.last_error == s_finish_error);
        if (s_hold_at == HOLD_VERIFY_LOCK) assert(s_hold_waiting && s_release_hold && s_aborts == 1 && !s_finishes);
        if (s_hold_at == HOLD_FINISH) assert(result.network_held && s_finishes == 1 && !s_aborts);
        if (!strcmp(scenario, "hold-queued")) assert(!s_begins && !s_aborts && !s_finishes);
        else if (s_begin_error || s_begin_null_success) assert(s_begins == 1 && !s_aborts && !s_finishes);
        else assert(s_begins == 1 && s_aborts + s_finishes == 1);
        if (s_abort_error || s_finish_error) {
            /* SDK consumed the handle despite the error: explicit retry may
             * begin anew and must never re-abort the old allocation. */
            s_wrong_project = false; s_abort_error = s_finish_error = ESP_OK;
            assert(ryz_ota_start_update() == ESP_OK);
            run_worker();
            assert(snapshot().state == RYZ_OTA_READY_TO_REBOOT && !s_handle_live);
            assert(s_begins == 2 && s_aborts + s_finishes == 2);
        }
    }
    puts("RYZ_OTA_ESP_PASS");
    return 0;
}
