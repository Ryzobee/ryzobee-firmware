#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "workbench_network_rpc.h"
#include "ryz_system_services.h"

/* Real production RPC and IDF cJSON. Only the typed service snapshot/admission
 * dependency is controlled; this does not run Wi-Fi, NVS or the service worker.
 * Embedded JSON NUL rejection belongs to the serial dispatcher, not this API. */
static ryz_system_services_snapshot_t snapshot;
static esp_err_t read_error, submit_error;
static unsigned reads, submits;
static uint32_t next_id;
static ryz_system_network_action_t submitted;
static bool admitted;
static size_t allocations, live, fail_at, post_admission_allocations;
typedef union { max_align_t alignment; size_t magic; } allocation_header_t;

static void *json_malloc(size_t size)
{
    ++allocations;
    if (admitted) ++post_admission_allocations;
    if (fail_at && allocations == fail_at) return NULL;
    assert(size <= SIZE_MAX - sizeof(allocation_header_t));
    allocation_header_t *header = malloc(sizeof(*header) + size);
    assert(header);
    header->magic = 0x4e4554U;
    ++live;
    return header + 1;
}

static void json_free(void *pointer)
{
    if (!pointer) return;
    allocation_header_t *header = (allocation_header_t *)pointer - 1;
    assert(live && header->magic == 0x4e4554U);
    header->magic = 0;
    --live;
    free(header);
}

esp_err_t ryz_system_services_get_snapshot(ryz_system_services_snapshot_t *out)
{
    assert(out);
    ++reads;
    *out = snapshot; /* Even erroneous dependencies must not leak a stale view. */
    return read_error;
}

esp_err_t ryz_system_services_request_network(ryz_system_network_action_t action,
                                             uint32_t *out_id)
{
    assert(out_id && action >= RYZ_SYSTEM_NETWORK_ON && action <= RYZ_SYSTEM_NETWORK_OPEN_AP);
    ++submits;
    submitted = action;
    *out_id = submit_error == ESP_OK ? next_id : 0;
    admitted = submit_error == ESP_OK;
    return submit_error;
}

static void reset_calls(void)
{
    reads = submits = 0;
    submitted = RYZ_SYSTEM_NETWORK_NONE;
    admitted = false;
    allocations = fail_at = post_admission_allocations = 0;
}

static void reset(void)
{
    reset_calls();
    memset(&snapshot, 0, sizeof(snapshot));
    read_error = submit_error = ESP_OK;
    next_id = 31;
}

static const char *requests[] = {
    "{\"id\":\"net-1\",\"op\":\"network\",\"action\":\"on\",\"boot_id\":\"boot-a\"}",
    "{\"id\":\"net-1\",\"op\":\"network\",\"action\":\"off\",\"boot_id\":\"boot-a\"}",
    "{\"id\":\"net-1\",\"op\":\"network\",\"action\":\"reprovision\",\"boot_id\":\"boot-a\",\"confirm\":true}",
    "{\"id\":\"net-1\",\"op\":\"network\",\"action\":\"status\"}",
    "{\"id\":\"net-1\",\"op\":\"network\",\"action\":\"setup\",\"boot_id\":\"boot-a\"}",
};

static cJSON *parse(const char *text)
{
    cJSON *value = cJSON_Parse(text);
    assert(value);
    return value;
}

static cJSON *invoke(const char *text)
{
    cJSON *input = parse(text);
    reset_calls();
    cJSON *value = ryz_workbench_network_rpc(input, "boot-a");
    cJSON_Delete(input);
    return value;
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

static void text_is(const cJSON *object, const char *name, const char *text)
{
    const cJSON *value = field(object, name);
    assert(cJSON_IsString(value) && !strcmp(value->valuestring, text));
}

static void bool_is(const cJSON *object, const char *name, bool expected)
{
    const cJSON *value = field(object, name);
    assert(cJSON_IsBool(value) && (cJSON_IsTrue(value) != 0) == expected);
}

static void envelope(const cJSON *value, esp_err_t error)
{
    assert(cJSON_IsObject(value));
    text_is(value, "schema", "ryz-network/1");
    text_is(value, "boot_id", "boot-a");
    bool_is(value, "ok", error == ESP_OK);
    assert(number(value, "error_code") == error);
}

static void rejected(cJSON *input, esp_err_t error)
{
    reset_calls();
    cJSON *value = ryz_workbench_network_rpc(input, "boot-a");
    envelope(value, error);
    assert(!reads && !submits && !admitted);
    assert(!cJSON_HasObjectItem(value, "accepted") && !cJSON_HasObjectItem(value, "network"));
    cJSON_Delete(value);
    cJSON_Delete(input);
}

static void validation(void)
{
    reset();
    const char *invalid[] = {"null", "[]", "true", "42", "\"network\"", "{}"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i)
        rejected(parse(invalid[i]), ESP_ERR_INVALID_ARG);
    rejected(NULL, ESP_ERR_INVALID_ARG);
    const char *required[] = {"id", "op", "action"};
    const char *wrong_types[] = {"null", "true", "0", "[]", "{}"};
    for (unsigned mode = 0; mode < 5; ++mode) {
        for (unsigned key = 0; key < 3; ++key) {
            cJSON *input = parse(requests[mode]);
            cJSON_DeleteItemFromObjectCaseSensitive(input, required[key]);
            rejected(input, ESP_ERR_INVALID_ARG);
            for (unsigned type = 0; type < 5; ++type) {
                input = parse(requests[mode]);
                assert(cJSON_ReplaceItemInObjectCaseSensitive(input, required[key], parse(wrong_types[type])));
                rejected(input, ESP_ERR_INVALID_ARG);
            }
        }
        const char *extras[] = {"password", "ssid", "enabled", "operation_id", "confirm", "BOOT_ID"};
        for (unsigned key = 0; key < 6; ++key) {
            cJSON *input = parse(requests[mode]);
            assert(cJSON_AddBoolToObject(input, extras[key], true));
            /* reprovision's existing confirm is deliberately duplicated. */
            rejected(input, ESP_ERR_INVALID_ARG);
        }
        const char *duplicates[] = {"id", "op", "action", "boot_id"};
        for (unsigned key = 0; key < 4; ++key) {
            cJSON *input = parse(requests[mode]);
            if (mode == 3 && key == 3) assert(cJSON_AddStringToObject(input, "boot_id", "boot-a"));
            assert(cJSON_AddStringToObject(input, duplicates[key], "duplicate"));
            rejected(input, ESP_ERR_INVALID_ARG);
        }
        const char *bad_boots[] = {"\"\"", "\"old-boot\"", "null", "true", "1", "[]", "{}"};
        for (unsigned type = 0; type < 7; ++type) {
            cJSON *input = parse(requests[mode]);
            cJSON_DeleteItemFromObjectCaseSensitive(input, "boot_id");
            assert(cJSON_AddItemToObject(input, "boot_id", parse(bad_boots[type])));
            rejected(input, ESP_ERR_INVALID_STATE);
        }
        if (mode != 3) {
            cJSON *input = parse(requests[mode]);
            cJSON_DeleteItemFromObjectCaseSensitive(input, "boot_id");
            rejected(input, ESP_ERR_INVALID_STATE);
        }
    }
    const char *bad_confirms[] = {"false", "1", "\"true\"", "null", "[]", "{}"};
    for (unsigned i = 0; i < 6; ++i) {
        cJSON *input = parse(requests[2]);
        assert(cJSON_ReplaceItemInObjectCaseSensitive(input, "confirm", parse(bad_confirms[i])));
        rejected(input, ESP_ERR_INVALID_ARG);
    }
    cJSON *input = parse(requests[2]);
    cJSON_DeleteItemFromObjectCaseSensitive(input, "confirm");
    rejected(input, ESP_ERR_INVALID_ARG);
    const char *bad_actions[] = {"ON", "cancel", "forget", "", "status "};
    for (unsigned i = 0; i < 5; ++i) {
        input = parse(requests[0]);
        assert(cJSON_ReplaceItemInObjectCaseSensitive(input, "action", cJSON_CreateString(bad_actions[i])));
        rejected(input, ESP_ERR_INVALID_ARG);
    }
    input = parse(requests[0]);
    assert(cJSON_ReplaceItemInObjectCaseSensitive(input, "op", cJSON_CreateString("NETWORK")));
    rejected(input, ESP_ERR_INVALID_ARG);
    char id[66]; memset(id, 'x', sizeof(id)); id[65] = '\0';
    input = parse(requests[0]);
    assert(cJSON_ReplaceItemInObjectCaseSensitive(input, "id", cJSON_CreateString(id)));
    rejected(input, ESP_ERR_INVALID_ARG);
    id[64] = '\0'; input = parse(requests[3]);
    assert(cJSON_ReplaceItemInObjectCaseSensitive(input, "id", cJSON_CreateString(id)));
    assert(cJSON_AddStringToObject(input, "boot_id", "boot-a"));
    reset_calls(); cJSON *value = ryz_workbench_network_rpc(input, "boot-a");
    envelope(value, ESP_OK); text_is(value, "id", id); assert(reads == 1 && !submits);
    cJSON_Delete(value);
    reset_calls();
    assert(!ryz_workbench_network_rpc(input, NULL) && !ryz_workbench_network_rpc(input, ""));
    assert(!reads && !submits && !allocations);
    cJSON_Delete(input);
}

static void mutation(void)
{
    const char *names[] = {"on", "off", "reprovision", "setup"};
    const unsigned indices[] = {0, 1, 2, 4};
    const ryz_system_network_action_t actions[] = {
        RYZ_SYSTEM_NETWORK_ON, RYZ_SYSTEM_NETWORK_OFF, RYZ_SYSTEM_NETWORK_REPROVISION,
        RYZ_SYSTEM_NETWORK_OPEN_AP};
    for (unsigned mode = 0; mode < 4; ++mode) {
        reset(); next_id = UINT32_MAX;
        cJSON *value = invoke(requests[indices[mode]]);
        envelope(value, ESP_OK); text_is(value, "id", "net-1"); text_is(value, "action", names[mode]);
        bool_is(value, "accepted", true);
        assert(number(value, "operation_id") == UINT32_MAX);
        assert(submits == 1 && !reads && admitted && submitted == actions[mode]);
        assert(!post_admission_allocations && cJSON_GetArraySize(value) == 8);
        assert(!cJSON_HasObjectItem(value, "network") && !cJSON_HasObjectItem(value, "completed"));
        cJSON_Delete(value);
        const esp_err_t errors[] = {ESP_ERR_INVALID_STATE, ESP_ERR_TIMEOUT, ESP_ERR_NO_MEM, ESP_FAIL};
        for (unsigned i = 0; i < 4; ++i) {
            reset(); submit_error = errors[i];
            value = invoke(requests[indices[mode]]); envelope(value, errors[i]);
            assert(submits == 1 && !reads && !admitted && submitted == actions[mode]);
            assert(!cJSON_HasObjectItem(value, "accepted") && !cJSON_HasObjectItem(value, "operation_id"));
            cJSON_Delete(value);
        }
    }
}

static void history(void)
{
    snapshot.network_valid = true;
    snapshot.network = (ryz_provisioning_snapshot_t){
        .enabled = false, .stop_confirmed = false, .state = RYZ_PROVISIONING_FAILED,
        .revision = UINT32_MAX, .last_error = ESP_ERR_TIMEOUT, .detail = -17,
        .credentials_stored = true, .portal_active = true,
        .ap_ssid = "LOCAL-QR-SSID-SECRET", .ap_password = "QR-PASSWORD-NEVER-SERIALIZE",
        .portal_ip = "192.168.4.1", .sta_ssid = "saved-network", .ipv4 = "10.0.0.2"};
    snapshot.network_operation = (ryz_system_network_operation_t){
        .requested = UINT32_MAX, .completed = UINT32_MAX - 1,
        .action = RYZ_SYSTEM_NETWORK_OFF, .completed_action = RYZ_SYSTEM_NETWORK_REPROVISION,
        .phase = RYZ_SYSTEM_NETWORK_WAITING_OTA, .result = ESP_FAIL};
}

static void status_case(void)
{
    reset(); history();
    cJSON *value = invoke(requests[3]); envelope(value, ESP_OK);
    assert(reads == 1 && !submits && !admitted && cJSON_GetArraySize(value) == 8);
    bool_is(value, "network_valid", true);
    const cJSON *network = field(value, "network"), *operation = field(value, "operation");
    assert(cJSON_GetArraySize(network) == 11 && cJSON_GetArraySize(operation) == 6);
    bool_is(network, "enabled", false); bool_is(network, "stop_confirmed", false);
    bool_is(network, "credentials_stored", true); bool_is(network, "portal_active", true);
    text_is(network, "state", "failed"); text_is(network, "portal_ip", "192.168.4.1");
    text_is(network, "sta_ssid", "saved-network"); text_is(network, "ipv4", "10.0.0.2");
    assert(number(network, "revision") == UINT32_MAX && number(network, "last_error") == ESP_ERR_TIMEOUT);
    assert(number(network, "detail") == -17);
    assert(number(operation, "requested") == UINT32_MAX && number(operation, "completed") == UINT32_MAX - 1);
    text_is(operation, "action", "off"); text_is(operation, "completed_action", "reprovision");
    text_is(operation, "phase", "waiting_ota"); assert(number(operation, "result") == ESP_FAIL);
    assert(!cJSON_HasObjectItem(value, "accepted"));
    char *wire = cJSON_PrintUnformatted(value); assert(wire);
    assert(!strstr(wire, "password") && !strstr(wire, "QR-PASSWORD") && !strstr(wire, "LOCAL-QR-SSID"));
    cJSON_free(wire); cJSON_Delete(value);
    const char *states[] = {"unconfigured", "ap_starting", "ap_ready", "credentials_received",
                           "connecting", "online", "failed", "stopping", "off"};
    for (unsigned i = 0; i < 9; ++i) {
        reset(); snapshot.network.state = (ryz_provisioning_state_t)i;
        snapshot.network.stop_confirmed = i == RYZ_PROVISIONING_OFF;
        value = invoke(requests[3]); envelope(value, ESP_OK); bool_is(value, "network_valid", false);
        network = field(value, "network"); text_is(network, "state", states[i]);
        bool_is(network, "stop_confirmed", i == RYZ_PROVISIONING_OFF);
        text_is(field(value, "operation"), "phase", "idle");
        cJSON_Delete(value);
    }
    const char *actions[] = {"none", "on", "off", "reprovision", "setup"};
    const char *phases[] = {"idle", "queued", "waiting_ota", "applying", "done", "failed"};
    for (unsigned i = 0; i < 6; ++i) {
        reset(); snapshot.network_operation.phase = (ryz_system_network_phase_t)i;
        snapshot.network_operation.action = (ryz_system_network_action_t)(i % 5);
        snapshot.network_operation.completed_action = (ryz_system_network_action_t)(i % 5);
        value = invoke(requests[3]); envelope(value, ESP_OK); operation = field(value, "operation");
        text_is(operation, "phase", phases[i]); text_is(operation, "action", actions[i % 5]);
        text_is(operation, "completed_action", actions[i % 5]); cJSON_Delete(value);
    }
}

static void errors_case(void)
{
    const esp_err_t errors[] = {ESP_ERR_INVALID_STATE, ESP_ERR_TIMEOUT, ESP_ERR_NO_MEM, ESP_FAIL};
    for (unsigned mode = 0; mode < 8; ++mode) {
        reset(); history();
        esp_err_t expected = ESP_ERR_INVALID_STATE;
        if (mode < 4) read_error = expected = errors[mode];
        else if (mode == 4) snapshot.network.state = (ryz_provisioning_state_t)99;
        else if (mode == 5) snapshot.network_operation.action = (ryz_system_network_action_t)99;
        else if (mode == 6) snapshot.network_operation.completed_action = (ryz_system_network_action_t)-1;
        else snapshot.network_operation.phase = (ryz_system_network_phase_t)99;
        cJSON *value = invoke(requests[3]); envelope(value, expected);
        assert(reads == 1 && !submits && cJSON_GetArraySize(value) == 5);
        assert(!cJSON_HasObjectItem(value, "network") && !cJSON_HasObjectItem(value, "operation"));
        cJSON_Delete(value);
    }
}

static void oom(void)
{
    /* Each baseline enumerates actual allocations in production/cJSON. Every
     * point is then failed once, including failure replies after rejected native
     * calls. Parsing and transport serialization are deliberately outside scope. */
    for (unsigned mode = 0; mode < 11; ++mode) {
        reset(); history();
        cJSON *input = parse(mode < 3 ? requests[mode] : mode < 6 ? requests[3] :
                             mode == 6 ? "{}" : mode < 9 ? requests[mode == 7 ? 0 : 2] : requests[4]);
        const bool success_mutation = mode < 3 || mode == 9;
        if (mode == 4) read_error = ESP_ERR_TIMEOUT;
        else if (mode == 5) snapshot.network.state = (ryz_provisioning_state_t)99;
        else if (mode == 7 || mode == 8 || mode == 10) submit_error = ESP_ERR_INVALID_STATE;
        const size_t baseline_live = live;
        reset_calls();
        cJSON *value = ryz_workbench_network_rpc(input, "boot-a");
        assert(value && !post_admission_allocations);
        const size_t required = allocations;
        cJSON_Delete(value);
        for (size_t point = 1; point <= required + 1; ++point) {
            reset_calls(); fail_at = point;
            value = ryz_workbench_network_rpc(input, "boot-a");
            fail_at = 0;
            if (point <= required) {
                assert(!value && !admitted);
                if (success_mutation) assert(!submits);
            } else {
                assert(value);
                if (success_mutation) {
                    envelope(value, ESP_OK); bool_is(value, "accepted", true);
                    assert(submits == 1 && admitted && number(value, "operation_id") == 31);
                } else if (mode == 3) envelope(value, ESP_OK);
                else envelope(value, mode == 4 ? ESP_ERR_TIMEOUT :
                              mode == 6 ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE);
            }
            assert(!post_admission_allocations && submits <= 1 && reads <= 1);
            cJSON_Delete(value); assert(live == baseline_live);
        }
        cJSON_Delete(input);
        printf("NETWORK_OOM %u %zu points\n", mode, required);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    cJSON_Hooks hooks = {.malloc_fn = json_malloc, .free_fn = json_free};
    cJSON_InitHooks(&hooks);
    if (!strcmp(argv[1], "validation")) validation();
    else if (!strcmp(argv[1], "mutation")) mutation();
    else if (!strcmp(argv[1], "status")) status_case();
    else if (!strcmp(argv[1], "errors")) errors_case();
    else if (!strcmp(argv[1], "oom")) oom();
    else assert(false);
    assert(live == 0);
    printf("WORKBENCH_NETWORK_RPC_PASS %s\n", argv[1]);
    return 0;
}
