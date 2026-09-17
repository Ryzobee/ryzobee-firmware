#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "workbench_i2c_rpc.h"
#include "ryz_i2c_scan.h"
#include "tool_gate_test.h"

/* The scanner's frozen public dependency supplies deterministic outcomes.
 * This suite links the actual JSON Adapter and actual IDF cJSON. It does not
 * simulate a physical bus or claim to test the scanner worker. */
static ryz_i2c_scan_snapshot_t scan;
static esp_err_t start_error, cancel_error, snapshot_error, configure_error;
static unsigned starts, cancels, snapshots, configurations;
static uint32_t next_id, cancelled_id;
static uint32_t expected_config_revision, next_config_revision;
static ryz_i2c_scan_config_t submitted_config;
static bool successful_action;
static size_t allocations, live_allocations, fail_at, allocations_after_action;

typedef union {
    max_align_t alignment;
    size_t magic;
} allocation_header_t;

static void *json_malloc(size_t size)
{
    assert(!test_gate_active);
    ++allocations;
    if (successful_action) ++allocations_after_action;
    if (fail_at && allocations == fail_at) return NULL;
    allocation_header_t *allocation = malloc(sizeof(*allocation) + size);
    assert(allocation);
    allocation->magic = 0x52595aU;
    ++live_allocations;
    return allocation + 1;
}

static void json_free(void *pointer)
{
    assert(!test_gate_active);
    if (!pointer) return;
    allocation_header_t *allocation = (allocation_header_t *)pointer - 1;
    assert(allocation->magic == 0x52595aU && live_allocations);
    allocation->magic = 0;
    --live_allocations;
    free(allocation);
}

esp_err_t ryz_i2c_scan_start(uint32_t *out)
{
    assert(test_gate_active);
    assert(out);
    ++starts;
    *out = start_error == ESP_OK ? next_id : 0;
    if (start_error == ESP_OK) successful_action = true;
    return start_error;
}

esp_err_t ryz_i2c_scan_cancel(uint32_t token)
{
    assert(test_gate_active);
    ++cancels;
    cancelled_id = token;
    if (cancel_error == ESP_OK) successful_action = true;
    return cancel_error;
}

esp_err_t ryz_i2c_scan_get_snapshot(ryz_i2c_scan_snapshot_t *out)
{
    assert(!test_gate_active);
    ++snapshots;
    *out = scan;
    return snapshot_error;
}

esp_err_t ryz_i2c_scan_configure(const ryz_i2c_scan_config_t *config,
                                 uint32_t expected_revision, uint32_t *out_revision)
{
    assert(config && out_revision);
    assert(test_gate_active);
    ++configurations;
    submitted_config = *config;
    expected_config_revision = expected_revision;
    *out_revision = configure_error == ESP_OK ? next_config_revision : 0;
    if (configure_error == ESP_OK) {
        successful_action = true;
        scan.config = *config;
        scan.config_revision = next_config_revision;
    }
    return configure_error;
}

static void reset_calls(void)
{
    if (test_gate_error == ESP_OK) assert(test_gate_calls == starts + cancels + configurations);
    test_gate_reset_calls();
    starts = cancels = snapshots = configurations = 0;
    cancelled_id = 0;
    successful_action = false;
    allocations = fail_at = allocations_after_action = 0;
}

static void reset(void)
{
    reset_calls();
    test_gate_error = ESP_OK;
    memset(&scan, 0, sizeof(scan));
    scan.config = RYZ_I2C_SCAN_DEFAULT_CONFIG;
    scan.config_revision = 1;
    start_error = cancel_error = snapshot_error = configure_error = ESP_OK;
    next_id = 23;
    next_config_revision = 2;
}

static const cJSON *field(const cJSON *object, const char *name)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
    assert(value);
    return value;
}

static double number(const cJSON *object, const char *name)
{
    const cJSON *value = field(object, name);
    assert(cJSON_IsNumber(value));
    return value->valuedouble;
}

static void text_is(const cJSON *object, const char *name, const char *expected)
{
    const cJSON *value = field(object, name);
    assert(cJSON_IsString(value) && !strcmp(value->valuestring, expected));
}

static void bool_is(const cJSON *object, const char *name, bool expected)
{
    const cJSON *value = field(object, name);
    assert(cJSON_IsBool(value) && (cJSON_IsTrue(value) != 0) == expected);
}

static void envelope(const cJSON *value, bool ok)
{
    assert(cJSON_IsObject(value));
    bool_is(value, "ok", ok);
    text_is(value, "schema", "ryz-i2c-scan/2");
    text_is(value, "boot_id", "boot-a");
    (void)field(value, "id");
    (void)number(value, "error_code");
}

static cJSON *request(const char *text)
{
    cJSON *value = cJSON_Parse(text);
    assert(value);
    return value;
}

static cJSON *invoke(const char *text)
{
    cJSON *value = request(text);
    cJSON *result = ryz_workbench_i2c_rpc(value, "boot-a");
    cJSON_Delete(value);
    return result;
}

static const char *start_request =
    "{\"id\":\"request-1\",\"op\":\"i2c_scan\",\"action\":\"start\",\"boot_id\":\"boot-a\"}";
static const char *cancel_request =
    "{\"id\":\"request-1\",\"op\":\"i2c_scan\",\"action\":\"cancel\",\"boot_id\":\"boot-a\",\"scan_id\":23}";
static const char *status_request = "{\"op\":\"i2c_scan\",\"action\":\"status\"}";
static const char *configure_request = "{\"id\":\"config-1\",\"op\":\"i2c_scan\",\"action\":\"configure\",\"boot_id\":\"boot-a\",\"sda\":4,\"scl\":5,\"hz\":400000,\"expected_revision\":1}";

static void config_is(const cJSON *value, const char *name, int sda, int scl, uint32_t hz)
{
    const cJSON *config = field(value, name);
    assert(cJSON_IsObject(config));
    assert(number(config, "sda") == sda && number(config, "scl") == scl && number(config, "hz") == hz);
}

static void validation(void)
{
    assert(ryz_workbench_i2c_rpc_supports("i2c_scan"));
    assert(!ryz_workbench_i2c_rpc_supports(NULL));
    assert(!ryz_workbench_i2c_rpc_supports("i2c"));
    assert(!ryz_workbench_i2c_rpc_supports("I2C_SCAN"));
    const char *bad[] = {
        "null", "[]", "42", "{}", "{\"op\":\"i2c_scan\"}",
        "{\"op\":\"wrong\",\"action\":\"status\"}",
        "{\"op\":\"i2c_scan\",\"action\":\"unknown\"}",
        "{\"op\":\"i2c_scan\",\"action\":\"start\"}",
        "{\"op\":\"i2c_scan\",\"action\":\"status\",\"boot_id\":\"boot-b\"}",
        "{\"op\":\"i2c_scan\",\"action\":\"start\",\"boot_id\":\"boot-a-x\"}",
        "{\"op\":\"i2c_scan\",\"action\":\"status\",\"id\":9}",
        "{\"op\":9,\"action\":\"status\"}",
        "{\"op\":\"i2c_scan\",\"action\":null}",
        "{\"op\":\"i2c_scan\",\"action\":\"status\",\"boot_id\":null}",
        "{\"op\":\"i2c_scan\",\"action\":\"status\",\"scan_id\":1}",
        "{\"op\":\"i2c_scan\",\"op\":\"i2c_scan\",\"action\":\"status\"}",
        "{\"op\":\"i2c_scan\",\"action\":\"status\",\"action\":\"start\"}",
        "{\"op\":\"i2c_scan\",\"action\":\"status\",\"id\":\"x\",\"id\":\"x\"}",
        "{\"op\":\"i2c_scan\",\"action\":\"start\",\"boot_id\":\"boot-a\",\"boot_id\":\"boot-a\"}",
        "{\"op\":\"i2c_scan\",\"action\":\"cancel\",\"boot_id\":\"boot-a\"}",
        "{\"op\":\"i2c_scan\",\"action\":\"cancel\",\"boot_id\":\"boot-a\",\"scan_id\":1,\"scan_id\":1}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        reset();
        cJSON *value = invoke(bad[i]);
        envelope(value, false);
        assert(!starts && !cancels && !snapshots);
        cJSON_Delete(value);
    }
    const char *custom[] = {"sda", "scl", "hz", "bus", "schema", "unknown"};
    for (unsigned i = 0; i < sizeof(custom) / sizeof(custom[0]); ++i) {
        reset();
        cJSON *input = request(start_request);
        assert(cJSON_AddNumberToObject(input, custom[i], 41));
        cJSON *value = ryz_workbench_i2c_rpc(input, "boot-a");
        envelope(value, false);
        assert(!starts && !cancels && !snapshots);
        cJSON_Delete(value);
        cJSON_Delete(input);
    }
    const double invalid[] = {0, -1, 0.5, 1.5, 4294967296.0, INFINITY, NAN};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        reset();
        cJSON *input = request(cancel_request);
        /* The locked cJSON setter casts NaN to int. Populate the public DOM
         * value directly so UBSan reaches the Adapter's non-finite rejection,
         * not an unrelated undefined conversion in test construction. */
        cJSON_GetObjectItemCaseSensitive(input, "scan_id")->valuedouble = invalid[i];
        cJSON *value = ryz_workbench_i2c_rpc(input, "boot-a");
        envelope(value, false);
        assert(!starts && !cancels && !snapshots);
        cJSON_Delete(value);
        cJSON_Delete(input);
    }
    const char *wrong_tokens[] = {"\"23\"", "true", "null", "[]", "{}"};
    for (unsigned i = 0; i < sizeof(wrong_tokens) / sizeof(wrong_tokens[0]); ++i) {
        reset();
        cJSON *input = request(cancel_request);
        assert(cJSON_ReplaceItemInObjectCaseSensitive(input, "scan_id", request(wrong_tokens[i])));
        cJSON *value = ryz_workbench_i2c_rpc(input, "boot-a");
        envelope(value, false);
        assert(!starts && !cancels && !snapshots);
        cJSON_Delete(value);
        cJSON_Delete(input);
    }
    reset();
    cJSON *value = ryz_workbench_i2c_rpc(NULL, "boot-a");
    envelope(value, false);
    cJSON_Delete(value);
    cJSON *input = request(start_request);
    assert(!ryz_workbench_i2c_rpc(input, NULL));
    assert(!ryz_workbench_i2c_rpc(input, ""));
    assert(!starts && !cancels && !snapshots);
    cJSON_Delete(input);
}

static void mutations(void)
{
    reset();
    cJSON *value = invoke(start_request);
    envelope(value, true);
    text_is(value, "id", "request-1");
    text_is(value, "action", "start");
    bool_is(value, "accepted", true);
    assert(number(value, "scan_id") == 23);
    assert(starts == 1 && !cancels && !snapshots && !allocations_after_action);
    assert(!cJSON_GetObjectItemCaseSensitive(value, "phase"));
    cJSON_Delete(value);

    reset();
    value = invoke(cancel_request);
    envelope(value, true);
    text_is(value, "action", "cancel");
    bool_is(value, "accepted", true);
    assert(number(value, "scan_id") == 23 && cancelled_id == 23);
    assert(cancels == 1 && !starts && !snapshots && !allocations_after_action);
    assert(!cJSON_GetObjectItemCaseSensitive(value, "phase"));
    cJSON_Delete(value);

    const esp_err_t errors[] = {ESP_ERR_INVALID_STATE, ESP_ERR_TIMEOUT, ESP_ERR_NO_MEM, ESP_FAIL};
    for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        reset();
        start_error = errors[i];
        value = invoke(start_request);
        envelope(value, false);
        assert(number(value, "error_code") == errors[i]);
        assert(starts == 1 && !cancels && !snapshots);
        cJSON_Delete(value);
        reset();
        cancel_error = errors[i];
        value = invoke(cancel_request);
        envelope(value, false);
        assert(number(value, "error_code") == errors[i]);
        assert(cancels == 1 && !starts && !snapshots);
        cJSON_Delete(value);
    }
    reset();
    next_id = UINT32_MAX;
    value = invoke(start_request);
    envelope(value, true);
    assert(number(value, "scan_id") == 4294967295.0);
    cJSON_Delete(value);
    reset();
    value = invoke("{\"op\":\"i2c_scan\",\"action\":\"cancel\",\"boot_id\":\"boot-a\",\"scan_id\":4294967295}");
    envelope(value, true);
    assert(cancelled_id == UINT32_MAX && number(value, "scan_id") == 4294967295.0);
    cJSON_Delete(value);
    reset();
    value = invoke("{\"op\":\"i2c_scan\",\"action\":\"cancel\",\"boot_id\":\"boot-a\",\"scan_id\":1}");
    envelope(value, true);
    assert(cancelled_id == 1);
    cJSON_Delete(value);
}

static void mixed_snapshot(void)
{
    scan.config = (ryz_i2c_scan_config_t){4, 5, 400000};
    scan.config_revision = 4;
    scan.scan_config = RYZ_I2C_SCAN_DEFAULT_CONFIG;
    scan.scan_config_revision = 1;
    scan.scan_id = 31;
    scan.phase = RYZ_I2C_SCAN_RUNNING;
    scan.queued_ms = 9007199254740993LL;
    scan.started_ms = INT64_MAX;
    scan.finished_ms = 0;
    scan.completed_addresses = 5;
    scan.probe_calls = 9;
    scan.busy_responses = 4;
    scan.current_address = 13;
    scan.ack_count = scan.nack_count = scan.busy_count = scan.timeout_count = scan.error_count = 1;
    scan.results[8] = RYZ_I2C_SCAN_ACK;
    scan.results[9] = RYZ_I2C_SCAN_NACK;
    scan.results[10] = RYZ_I2C_SCAN_BUSY;
    scan.results[11] = RYZ_I2C_SCAN_TIMEOUT;
    scan.results[12] = RYZ_I2C_SCAN_ERROR;
    scan.errors[9] = ESP_ERR_NOT_FOUND;
    scan.errors[10] = ESP_ERR_NOT_FINISHED;
    scan.errors[11] = ESP_ERR_TIMEOUT;
    scan.errors[12] = ESP_FAIL;
    /* Reserved addresses can never escape the allowed result range. */
    scan.results[0] = scan.results[127] = RYZ_I2C_SCAN_ACK;
}

static void status(void)
{
    reset();
    mixed_snapshot();
    ryz_i2c_scan_snapshot_t before = scan;
    cJSON *value = invoke(status_request);
    envelope(value, true);
    text_is(value, "phase", "running");
    text_is(value, "queued_ms", "9007199254740993");
    text_is(value, "started_ms", "9223372036854775807");
    text_is(value, "finished_ms", "0");
    bool_is(value, "worker_running", true);
    bool_is(value, "empty", false);
    config_is(value, "config", 4, 5, 400000);
    config_is(value, "scan_config", 41, 40, 100000);
    assert(number(value, "config_revision") == 4 && number(value, "scan_config_revision") == 1);
    assert(!cJSON_GetObjectItemCaseSensitive(value, "bus") && !cJSON_GetObjectItemCaseSensitive(value, "sda"));
    bool_is(value, "resources_held", false);
    assert(number(value, "cleanup_error") == ESP_OK);
    assert(number(value, "scan_id") == 31 && number(value, "current_address") == 13);
    assert(number(value, "completed_addresses") == 5 && number(value, "probe_calls") == 9);
    assert(number(value, "busy_responses") == 4 && number(value, "unscanned_count") == 107);
    assert(number(value, "ack_count") == 1 && number(value, "nack_count") == 1);
    assert(number(value, "busy_count") == 1 && number(value, "timeout_count") == 1);
    assert(number(value, "error_count") == 1);
    const cJSON *results = field(value, "results");
    const char *names[] = {"ack", "nack", "busy", "timeout"};
    for (unsigned i = 0; i < 4; ++i) {
        const cJSON *array = field(results, names[i]);
        assert(cJSON_IsArray(array) && cJSON_GetArraySize(array) == 1);
        assert(cJSON_GetArrayItem(array, 0)->valuedouble == 8 + i);
    }
    const cJSON *errors = field(results, "error");
    assert(cJSON_GetArraySize(errors) == 1);
    assert(number(cJSON_GetArrayItem(errors, 0), "address") == 12);
    assert(number(cJSON_GetArrayItem(errors, 0), "code") == ESP_FAIL);
    const cJSON *unscanned = field(results, "unscanned");
    assert(cJSON_GetArraySize(unscanned) == 107);
    assert(cJSON_GetArrayItem(unscanned, 0)->valuedouble == 13);
    assert(cJSON_GetArrayItem(unscanned, 106)->valuedouble == 119);
    assert(!memcmp(&before, &scan, sizeof(scan)));
    assert(snapshots == 1 && !starts && !cancels);
    assert(!cJSON_GetObjectItemCaseSensitive(value, "model"));
    char *serialized = cJSON_PrintUnformatted(value);
    assert(serialized);
    cJSON *roundtrip = request(serialized);
    envelope(roundtrip, true);
    cJSON_Delete(roundtrip);
    cJSON_free(serialized);
    cJSON_Delete(value);

    const char *phases[] = {"idle", "queued", "running", "cancelling", "completed", "cancelled", "failed", "releasing"};
    for (unsigned i = 0; i < 8; ++i) {
        reset();
        scan.phase = (ryz_i2c_scan_phase_t)i;
        scan.error = i == RYZ_I2C_SCAN_FAILED ? ESP_FAIL : ESP_OK;
        value = invoke("{\"op\":\"i2c_scan\",\"action\":\"status\",\"boot_id\":\"boot-a\"}");
        envelope(value, true);
        text_is(value, "phase", phases[i]);
        assert(number(value, "error_code") == scan.error);
        cJSON_Delete(value);
    }
    reset();
    mixed_snapshot();
    snapshot_error = ESP_ERR_INVALID_STATE;
    value = invoke(status_request);
    envelope(value, true);
    text_is(value, "phase", "unstarted");
    bool_is(value, "worker_running", false);
    bool_is(value, "empty", false);
    assert(number(value, "scan_id") == 0 && number(value, "unscanned_count") == 112);
    assert(number(value, "error_code") == ESP_ERR_INVALID_STATE);
    config_is(value, "config", 41, 40, 100000);
    assert(number(value, "config_revision") == 1 && number(value, "scan_config_revision") == 0);
    assert(cJSON_IsNull(field(value, "scan_config")));
    bool_is(value, "resources_held", false);
    assert(number(value, "cleanup_error") == ESP_OK);
    assert(snapshots == 1 && !starts && !cancels);
    cJSON_Delete(value);
    const esp_err_t errors2[] = {ESP_ERR_TIMEOUT, ESP_FAIL, ESP_ERR_NO_MEM};
    for (unsigned i = 0; i < 3; ++i) {
        reset();
        snapshot_error = errors2[i];
        value = invoke(status_request);
        envelope(value, false);
        assert(number(value, "error_code") == errors2[i]);
        assert(!cJSON_GetObjectItemCaseSensitive(value, "empty"));
        assert(snapshots == 1 && !starts && !cancels);
        cJSON_Delete(value);
    }
    reset();
    scan.phase = (ryz_i2c_scan_phase_t)99;
    value = invoke(status_request);
    envelope(value, false);
    cJSON_Delete(value);
    reset();
    scan.results[8] = 255;
    value = invoke(status_request);
    envelope(value, false);
    cJSON_Delete(value);
}

static void all_nacks(void)
{
    reset();
    scan.scan_id = 7;
    scan.phase = RYZ_I2C_SCAN_COMPLETED;
    scan.completed_addresses = scan.nack_count = 112;
    scan.probe_calls = 112;
    for (unsigned address = 8; address <= 119; ++address) {
        scan.results[address] = RYZ_I2C_SCAN_NACK;
        scan.errors[address] = ESP_ERR_NOT_FOUND;
    }
}

static void empty_case(void)
{
    all_nacks();
    cJSON *value = invoke(status_request);
    envelope(value, true);
    bool_is(value, "empty", true);
    cJSON_Delete(value);
    for (unsigned i = 0; i <= RYZ_I2C_SCAN_ERROR; ++i) {
        if (i == RYZ_I2C_SCAN_NACK) continue;
        all_nacks();
        scan.results[33] = (uint8_t)i; /* Even inconsistent optimistic counters cannot fake empty. */
        value = invoke(status_request);
        bool_is(value, "empty", false);
        cJSON_Delete(value);
    }
    for (unsigned i = 0; i < 12; ++i) {
        all_nacks();
        switch (i) {
        case 0: scan.phase = RYZ_I2C_SCAN_CANCELLED; break;
        case 1: scan.phase = RYZ_I2C_SCAN_RUNNING; break;
        case 2: scan.error = ESP_FAIL; break;
        case 3: scan.completed_addresses = 111; break;
        case 4: scan.nack_count = 111; break;
        case 5: scan.ack_count = 1; break;
        case 6: scan.busy_count = 1; break;
        case 7: scan.timeout_count = 1; break;
        case 8: scan.error_count = 1; break;
        case 9: scan.resources_held = true; break;
        case 10: scan.cleanup_error = ESP_FAIL; break;
        default: snapshot_error = ESP_ERR_INVALID_STATE; break;
        }
        value = invoke(status_request);
        envelope(value, true);
        bool_is(value, "empty", false);
        cJSON_Delete(value);
    }
}

static void mutation_oom(void)
{
    for (unsigned cancel = 0; cancel < 2; ++cancel) {
        reset();
        cJSON *input = request(cancel ? cancel_request : start_request);
        const size_t input_live = live_allocations;
        reset_calls();
        cJSON *value = ryz_workbench_i2c_rpc(input, "boot-a");
        const size_t required = allocations;
        assert(value && !allocations_after_action);
        cJSON_Delete(value);
        assert(live_allocations == input_live);
        for (size_t allocation = 1; allocation <= required + 1; ++allocation) {
            reset_calls();
            fail_at = allocation;
            value = ryz_workbench_i2c_rpc(input, "boot-a");
            fail_at = 0;
            if (allocation <= required) {
                assert(!value && !starts && !cancels);
            } else {
                envelope(value, true);
                bool_is(value, "accepted", true);
                assert(number(value, "scan_id") == 23);
                assert((cancel ? cancels : starts) == 1 && !snapshots);
                assert(!allocations_after_action);
            }
            cJSON_Delete(value);
            assert(live_allocations == input_live);
        }
        cJSON_Delete(input);
        printf("MUTATION_OOM %s %zu points\n", cancel ? "cancel" : "start", required);
    }
}

static void status_oom(void)
{
    for (unsigned mode = 0; mode < 5; ++mode) {
        reset();
        cJSON *input = request(mode >= 3 ? (mode == 3 ? start_request : cancel_request) : status_request);
        if (mode == 0) {
            scan.phase = RYZ_I2C_SCAN_COMPLETED;
            scan.scan_config = RYZ_I2C_SCAN_DEFAULT_CONFIG;
            scan.scan_config_revision = 1;
            scan.error_count = scan.completed_addresses = 112;
            for (unsigned address = 8; address <= 119; ++address) {
                scan.results[address] = RYZ_I2C_SCAN_ERROR;
                scan.errors[address] = ESP_FAIL;
            }
        } else if (mode == 1) snapshot_error = ESP_ERR_INVALID_STATE;
        else if (mode == 2) snapshot_error = ESP_ERR_TIMEOUT;
        else if (mode == 3) start_error = ESP_ERR_TIMEOUT;
        else cancel_error = ESP_ERR_INVALID_STATE;
        const size_t input_live = live_allocations;
        reset_calls();
        cJSON *value = ryz_workbench_i2c_rpc(input, "boot-a");
        assert(value);
        const size_t required = allocations;
        cJSON_Delete(value);
        for (size_t allocation = 1; allocation <= required; ++allocation) {
            reset_calls();
            fail_at = allocation;
            value = ryz_workbench_i2c_rpc(input, "boot-a");
            fail_at = 0;
            assert(!value);
            assert(!successful_action && !allocations_after_action);
            assert(starts <= 1 && cancels <= 1 && snapshots <= 1);
            assert(live_allocations == input_live);
        }
        cJSON_Delete(input);
        printf("STATUS_FAILURE_OOM %u %zu points\n", mode, required);
    }
}

static void configuration(void)
{
    reset();
    cJSON *value = invoke(configure_request);
    envelope(value, true);
    text_is(value, "action", "configure");
    bool_is(value, "accepted", true);
    config_is(value, "config", 4, 5, 400000);
    assert(number(value, "config_revision") == 2);
    assert(configurations == 1 && !starts && !cancels && !snapshots && !allocations_after_action);
    assert(submitted_config.sda == 4 && submitted_config.scl == 5 && submitted_config.hz == 400000);
    assert(expected_config_revision == 1);
    assert(!cJSON_GetObjectItemCaseSensitive(value, "phase") && !cJSON_GetObjectItemCaseSensitive(value, "scan_id"));
    cJSON_Delete(value);

    const char *numeric[] = {"sda", "scl", "hz", "expected_revision"};
    for (unsigned key = 0; key < 4; ++key) {
        const double invalid[] = {-1, 0.5, INFINITY, NAN, key < 2 ? 49 : key == 2 ? 200000 : 4294967296.0};
        for (unsigned i = 0; i < 5; ++i) {
            reset();
            cJSON *input = request(configure_request);
            cJSON_GetObjectItemCaseSensitive(input, numeric[key])->valuedouble = invalid[i];
            value = ryz_workbench_i2c_rpc(input, "boot-a");
            envelope(value, false);
            assert(!configurations && !starts && !cancels && !snapshots);
            cJSON_Delete(value);
            cJSON_Delete(input);
        }
        const char *wrong[] = {"\"1\"", "true", "null", "[]", "{}"};
        for (unsigned i = 0; i < 5; ++i) {
            reset();
            cJSON *input = request(configure_request);
            assert(cJSON_ReplaceItemInObjectCaseSensitive(input, numeric[key], request(wrong[i])));
            value = ryz_workbench_i2c_rpc(input, "boot-a");
            envelope(value, false);
            assert(!configurations && !starts && !cancels && !snapshots);
            cJSON_Delete(value);
            cJSON_Delete(input);
        }
        reset();
        cJSON *input = request(configure_request);
        cJSON_DeleteItemFromObjectCaseSensitive(input, numeric[key]);
        value = ryz_workbench_i2c_rpc(input, "boot-a");
        envelope(value, false);
        assert(!configurations);
        cJSON_Delete(value);
        cJSON_Delete(input);
        for (unsigned action = 0; action < 3; ++action) {
            reset();
            input = request(action == 0 ? start_request : action == 1 ? cancel_request : status_request);
            assert(cJSON_AddNumberToObject(input, numeric[key], 1));
            value = ryz_workbench_i2c_rpc(input, "boot-a");
            envelope(value, false);
            assert(!configurations && !starts && !cancels && !snapshots);
            cJSON_Delete(value);
            cJSON_Delete(input);
        }
    }
    const char *duplicate[] = {"id", "op", "action", "boot_id", "sda", "scl", "hz", "expected_revision"};
    for (unsigned i = 0; i < 8; ++i) {
        reset();
        cJSON *input = request(configure_request);
        assert(cJSON_AddNumberToObject(input, duplicate[i], 1));
        value = ryz_workbench_i2c_rpc(input, "boot-a");
        envelope(value, false);
        assert(!configurations && !starts && !cancels && !snapshots);
        cJSON_Delete(value);
        cJSON_Delete(input);
    }
    const char *unknown[] = {"scan_id", "bus", "gamma", "revision", "schema"};
    for (unsigned i = 0; i < 5; ++i) {
        reset();
        cJSON *input = request(configure_request);
        assert(cJSON_AddNumberToObject(input, unknown[i], 1));
        value = ryz_workbench_i2c_rpc(input, "boot-a");
        envelope(value, false);
        assert(!configurations && !starts && !cancels && !snapshots);
        cJSON_Delete(value);
        cJSON_Delete(input);
    }
    for (unsigned kind = 0; kind < 3; ++kind) {
        reset();
        cJSON *input = request(configure_request);
        if (!kind) cJSON_DeleteItemFromObjectCaseSensitive(input, "boot_id");
        else if (kind == 1) assert(cJSON_ReplaceItemInObjectCaseSensitive(input, "boot_id", cJSON_CreateString("old-boot")));
        else cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(input, "expected_revision"), 0);
        value = ryz_workbench_i2c_rpc(input, "boot-a");
        envelope(value, false);
        assert(!configurations && !starts && !cancels && !snapshots);
        cJSON_Delete(value);
        cJSON_Delete(input);
    }
    const esp_err_t errors[] = {ESP_ERR_INVALID_ARG, ESP_ERR_NOT_SUPPORTED, ESP_ERR_INVALID_STATE,
                               ESP_ERR_NOT_FINISHED, ESP_ERR_TIMEOUT, ESP_ERR_NO_MEM, ESP_FAIL};
    for (unsigned i = 0; i < 7; ++i) {
        reset();
        mixed_snapshot();
        const ryz_i2c_scan_snapshot_t before = scan;
        configure_error = errors[i];
        value = invoke(configure_request);
        envelope(value, false);
        assert(number(value, "error_code") == errors[i]);
        assert(configurations == 1 && !starts && !cancels && !snapshots);
        if (errors[i] == ESP_ERR_INVALID_ARG)
            assert(strstr(field(value, "error")->valuestring, "reserved"));
        if (errors[i] == ESP_ERR_INVALID_STATE)
            assert(strstr(field(value, "error")->valuestring, "stale") &&
                   strstr(field(value, "error")->valuestring, "busy"));
        assert(!memcmp(&before, &scan, sizeof(scan)));
        cJSON_Delete(value);
    }
    reset();
    mixed_snapshot();
    next_config_revision = UINT32_MAX;
    cJSON *input = request(configure_request);
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(input, "expected_revision"), UINT32_MAX);
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(input, "sda"), 0);
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(input, "scl"), 48);
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(input, "hz"), 100000);
    value = ryz_workbench_i2c_rpc(input, "boot-a");
    envelope(value, true);
    assert(expected_config_revision == UINT32_MAX);
    assert(number(value, "config_revision") == 4294967295.0);
    config_is(value, "config", 0, 48, 100000); /* Eligibility belongs to the core, not this Adapter. */
    cJSON_Delete(value);
    cJSON_Delete(input);
    reset_calls();
    value = invoke(status_request);
    config_is(value, "config", 0, 48, 100000);
    config_is(value, "scan_config", 41, 40, 100000);
    assert(number(value, "scan_config_revision") == 1 && number(value, "scan_id") == 31);
    cJSON_Delete(value);
    reset();
    scan.phase = RYZ_I2C_SCAN_RELEASING;
    scan.resources_held = true;
    scan.cleanup_error = ESP_FAIL;
    value = invoke(status_request);
    text_is(value, "phase", "releasing");
    bool_is(value, "resources_held", true);
    bool_is(value, "empty", false);
    assert(number(value, "cleanup_error") == ESP_FAIL);
    cJSON_Delete(value);
}

static void configure_oom(void)
{
    for (unsigned error = 0; error < 2; ++error) {
        reset();
        configure_error = error ? ESP_ERR_INVALID_STATE : ESP_OK;
        cJSON *input = request(configure_request);
        const size_t input_live = live_allocations;
        reset_calls();
        cJSON *value = ryz_workbench_i2c_rpc(input, "boot-a");
        assert(value);
        const size_t required = allocations;
        cJSON_Delete(value);
        for (size_t allocation = 1; allocation <= required + 1; ++allocation) {
            reset_calls();
            fail_at = allocation;
            value = ryz_workbench_i2c_rpc(input, "boot-a");
            fail_at = 0;
            if (allocation <= required) {
                assert(!value && !successful_action);
                if (!error) assert(!configurations);
            } else {
                envelope(value, !error);
                assert(configurations == 1);
            }
            assert(!allocations_after_action && !starts && !cancels && !snapshots);
            cJSON_Delete(value);
            assert(live_allocations == input_live);
        }
        cJSON_Delete(input);
        printf("CONFIGURE_OOM %u %zu points\n", error, required);
    }
}

static void gate_case(void)
{
    const char *inputs[] = {start_request,cancel_request,configure_request};
    for (unsigned i = 0; i < 3; ++i) {
        reset(); test_gate_error = ESP_ERR_NOT_FINISHED;
        cJSON *input = request(inputs[i]);
        reset_calls(); const size_t baseline = live_allocations;
        cJSON *value = ryz_workbench_i2c_rpc(input,"boot-a"); envelope(value,false);
        assert(number(value,"error_code") == ESP_ERR_NOT_FINISHED && test_gate_calls == 1);
        size_t count = allocations; cJSON_Delete(value);
        for (size_t point = 1; point <= count; ++point) {
            reset_calls(); fail_at = point;
            value = ryz_workbench_i2c_rpc(input,"boot-a");
            if (value) envelope(value,false);
            assert(!starts && !cancels && !configurations && !test_gate_active && test_gate_calls <= 1);
            cJSON_Delete(value); assert(live_allocations == baseline);
        }
        fail_at = 0; cJSON_Delete(input);
        reset(); start_error = cancel_error = configure_error = ESP_FAIL;
        value = invoke(inputs[i]); envelope(value,false); cJSON_Delete(value);
        assert(test_gate_calls == 1 && !test_gate_active);
        start_error = cancel_error = configure_error = ESP_OK;
        value = invoke(inputs[i]); envelope(value,true); cJSON_Delete(value);
        assert(test_gate_calls == 2 && !test_gate_active);
    }
    reset(); test_gate_error = ESP_ERR_INVALID_STATE;
    cJSON *value = invoke(status_request); envelope(value,true); cJSON_Delete(value);
    assert(!test_gate_calls && snapshots == 1);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    cJSON_Hooks hooks = {.malloc_fn = json_malloc, .free_fn = json_free};
    cJSON_InitHooks(&hooks);
    if (!strcmp(argv[1], "validation")) validation();
    else if (!strcmp(argv[1], "mutations")) mutations();
    else if (!strcmp(argv[1], "status")) status();
    else if (!strcmp(argv[1], "empty")) empty_case();
    else if (!strcmp(argv[1], "mutation_oom")) mutation_oom();
    else if (!strcmp(argv[1], "status_oom")) status_oom();
    else if (!strcmp(argv[1], "configuration")) configuration();
    else if (!strcmp(argv[1], "configure_oom")) configure_oom();
    else if (!strcmp(argv[1], "gate")) gate_case();
    else assert(false);
    assert(live_allocations == 0);
    printf("WORKBENCH_I2C_RPC_PASS %s\n", argv[1]);
    return 0;
}
