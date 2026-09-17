#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "workbench_rgb_rpc.h"
#include "ryz_rgb.h"
#include "tool_gate_test.h"

/* Only the frozen worker dependency is controlled. This links the real JSON
 * Adapter and IDF cJSON, not the RGB driver and never GPIO/RMT hardware. */
static ryz_rgb_snapshot_t snapshot;
static ryz_rgb_color_t submitted;
static esp_err_t submit_error, snapshot_error;
static uint32_t next_id;
static unsigned submits, reads, cleanups;
static uint32_t cleaned_id;
static bool admitted;
static size_t allocations, live, fail_at, post_admission_allocations;
typedef union { max_align_t alignment; size_t magic; } allocation_header_t;

static void *json_malloc(size_t size)
{
    assert(!test_gate_active);
    ++allocations;
    if (admitted) ++post_admission_allocations;
    if (fail_at && allocations == fail_at) return NULL;
    allocation_header_t *header = malloc(sizeof(*header) + size);
    assert(header);
    header->magic = 0x524742U;
    ++live;
    return header + 1;
}

static void json_free(void *pointer)
{
    assert(!test_gate_active);
    if (!pointer) return;
    allocation_header_t *header = (allocation_header_t *)pointer - 1;
    assert(live && header->magic == 0x524742U);
    header->magic = 0;
    --live;
    free(header);
}

esp_err_t ryz_rgb_submit(ryz_rgb_color_t color, uint32_t *out)
{
    assert(test_gate_active);
    assert(out);
    ++submits;
    submitted = color;
    *out = submit_error == ESP_OK ? next_id : 0;
    admitted = submit_error == ESP_OK;
    return submit_error;
}

esp_err_t ryz_rgb_cleanup(uint32_t token)
{
    assert(test_gate_active);
    ++cleanups;
    cleaned_id = token;
    admitted = submit_error == ESP_OK;
    return submit_error;
}

esp_err_t ryz_rgb_get_snapshot(ryz_rgb_snapshot_t *out)
{
    assert(!test_gate_active);
    ++reads;
    *out = snapshot;
    return snapshot_error;
}

static void reset_calls(void)
{
    if (test_gate_error == ESP_OK) assert(test_gate_calls == submits + cleanups);
    test_gate_reset_calls();
    submits = reads = cleanups = 0;
    admitted = false;
    allocations = fail_at = post_admission_allocations = 0;
}

static void reset(void)
{
    reset_calls();
    test_gate_error = ESP_OK;
    memset(&snapshot, 0, sizeof(snapshot));
    memset(&submitted, 0, sizeof(submitted));
    submit_error = snapshot_error = ESP_OK;
    next_id = 31;
}

static cJSON *parse(const char *text)
{
    cJSON *value = cJSON_Parse(text);
    assert(value);
    return value;
}

static const char *set_request = "{\"id\":\"rgb-1\",\"op\":\"rgb\",\"action\":\"set\",\"boot_id\":\"boot-a\",\"red\":255,\"green\":97,\"blue\":0}";
static const char *off_request = "{\"op\":\"rgb\",\"action\":\"off\",\"boot_id\":\"boot-a\"}";
static const char *status_request = "{\"op\":\"rgb\",\"action\":\"status\"}";
static const char *cleanup_request = "{\"op\":\"rgb\",\"action\":\"cleanup\",\"boot_id\":\"boot-a\",\"request_id\":31}";

static cJSON *invoke(const char *text)
{
    cJSON *input = parse(text);
    cJSON *value = ryz_workbench_rgb_rpc(input, "boot-a");
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
    text_is(value, "schema", "ryz-rgb/1");
    text_is(value, "boot_id", "boot-a");
    (void)field(value, "id");
    (void)number(value, "error_code");
}

static void color_is(const cJSON *value, const char *name, int red, int green, int blue)
{
    const cJSON *color = field(value, name);
    assert(cJSON_IsObject(color));
    assert(number(color, "red") == red && number(color, "green") == green && number(color, "blue") == blue);
}

static void rejected(cJSON *input)
{
    cJSON *value = ryz_workbench_rgb_rpc(input, "boot-a");
    envelope(value, false);
    assert(!submits && !reads && !cleanups && !test_gate_calls);
    cJSON_Delete(value);
    cJSON_Delete(input);
}

static void validation(void)
{
    assert(ryz_workbench_rgb_rpc_supports("rgb"));
    assert(!ryz_workbench_rgb_rpc_supports(NULL));
    assert(!ryz_workbench_rgb_rpc_supports("RGB"));
    assert(!ryz_workbench_rgb_rpc_supports("i2c_scan"));
    const char *bad[] = {"null", "[]", "42", "{}",
        "{\"op\":\"rgb\"}", "{\"op\":\"rgb\",\"action\":\"configure\"}",
        "{\"op\":\"bad\",\"action\":\"status\"}", "{\"op\":\"rgb\",\"action\":\"off\"}",
        "{\"op\":\"rgb\",\"action\":\"status\",\"boot_id\":\"old-boot\"}",
        "{\"op\":\"rgb\",\"action\":\"off\",\"boot_id\":\"boot-a-extra\"}",
        "{\"op\":\"rgb\",\"action\":\"set\",\"boot_id\":\"boot-a\"}",
        "{\"op\":\"rgb\",\"action\":\"status\",\"boot_id\":null}",
        "{\"op\":\"rgb\",\"action\":true}", "{\"op\":0,\"action\":\"status\"}",
        "{\"op\":\"rgb\",\"action\":\"status\",\"id\":7}"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        reset();
        rejected(parse(bad[i]));
    }
    const char *names[] = {"id", "op", "action", "boot_id", "red", "green", "blue"};
    for (unsigned i = 0; i < 7; ++i) {
        reset();
        cJSON *input = parse(set_request);
        assert(cJSON_AddStringToObject(input, names[i], "duplicate"));
        rejected(input);
    }
    const char *unknown[] = {"pin", "gpio", "brightness", "gamma", "color", "schema", "request_id"};
    for (unsigned i = 0; i < 7; ++i) {
        reset();
        cJSON *input = parse(set_request);
        assert(cJSON_AddNumberToObject(input, unknown[i], 1));
        rejected(input);
    }
    const double numbers[] = {-1, 256, 1.5, INFINITY, NAN};
    const char *types[] = {"\"1\"", "true", "null", "[]", "{}"};
    for (unsigned channel = 4; channel < 7; ++channel) {
        for (unsigned i = 0; i < 5; ++i) {
            reset();
            cJSON *input = parse(set_request);
            /* Avoid the locked cJSON setter's NaN-to-int UB in the fixture. */
            cJSON_GetObjectItemCaseSensitive(input, names[channel])->valuedouble = numbers[i];
            rejected(input);
            reset();
            input = parse(set_request);
            assert(cJSON_ReplaceItemInObjectCaseSensitive(input, names[channel], parse(types[i])));
            rejected(input);
        }
        reset();
        cJSON *input = parse(set_request);
        cJSON_DeleteItemFromObjectCaseSensitive(input, names[channel]);
        rejected(input);
        for (unsigned status = 0; status < 2; ++status) {
            reset();
            input = parse(status ? status_request : off_request);
            assert(cJSON_AddNumberToObject(input, names[channel], 0));
            rejected(input);
        }
    }
    reset();
    rejected(NULL);
    cJSON *input = parse(set_request);
    assert(!ryz_workbench_rgb_rpc(input, NULL) && !ryz_workbench_rgb_rpc(input, ""));
    assert(!submits && !reads);
    cJSON_Delete(input);
}

static void submit_case(void)
{
    reset();
    cJSON *value = invoke(set_request);
    envelope(value, true);
    text_is(value, "id", "rgb-1");
    text_is(value, "action", "set");
    bool_is(value, "accepted", true);
    color_is(value, "requested", 255, 97, 0);
    assert(number(value, "request_id") == 31);
    assert(submitted.red == 255 && submitted.green == 97 && submitted.blue == 0);
    assert(submits == 1 && !reads && !post_admission_allocations);
    assert(!cJSON_GetObjectItemCaseSensitive(value, "phase"));
    cJSON_Delete(value);
    reset();
    submitted = (ryz_rgb_color_t){255, 255, 255};
    next_id = UINT32_MAX;
    value = invoke(off_request);
    envelope(value, true);
    text_is(value, "action", "off");
    color_is(value, "requested", 0, 0, 0);
    assert(number(value, "request_id") == 4294967295.0);
    assert(submitted.red == 0 && submitted.green == 0 && submitted.blue == 0);
    assert(submits == 1 && !reads && !post_admission_allocations);
    cJSON_Delete(value);
    const esp_err_t errors[] = {ESP_ERR_INVALID_STATE, ESP_ERR_TIMEOUT, ESP_ERR_NO_MEM, ESP_FAIL};
    for (unsigned i = 0; i < 4; ++i) {
        reset();
        submit_error = errors[i];
        value = invoke(i % 2 ? off_request : set_request);
        envelope(value, false);
        assert(number(value, "error_code") == errors[i]);
        assert(submits == 1 && !reads && !admitted);
        cJSON_Delete(value);
    }
    for (unsigned channel = 0; channel < 3; ++channel) {
        reset();
        const char *channels[] = {"red", "green", "blue"};
        cJSON *input = parse(set_request);
        cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(input, channels[channel]), 255);
        value = ryz_workbench_rgb_rpc(input, "boot-a");
        envelope(value, true);
        assert(number(field(value, "requested"), channels[channel]) == 255);
        cJSON_Delete(value);
        cJSON_Delete(input);
    }
}

static void history(void)
{
    snapshot.request_id = 8;
    snapshot.requested = (ryz_rgb_color_t){9, 8, 7};
    snapshot.phase = RYZ_RGB_FAILED;
    snapshot.error = ESP_ERR_TIMEOUT;
    snapshot.resources_held = true;
    snapshot.cleanup_error = ESP_FAIL;
    snapshot.last_completed_id = 7;
    snapshot.last_completed_color = (ryz_rgb_color_t){255, 97, 0};
    snapshot.output_known = false;
    snapshot.queued_ms = 9007199254740993LL;
    snapshot.finished_ms = INT64_MAX;
}

static void status_case(void)
{
    reset();
    history();
    const ryz_rgb_snapshot_t before = snapshot;
    cJSON *value = invoke(status_request);
    envelope(value, true);
    text_is(value, "phase", "failed");
    bool_is(value, "output_known", false);
    bool_is(value, "worker_running", true);
    bool_is(value, "resources_held", true);
    assert(number(value,"cleanup_error") == ESP_FAIL);
    assert(number(value, "request_id") == 8 && number(value, "last_completed_id") == 7);
    assert(number(value, "error_code") == ESP_ERR_TIMEOUT);
    color_is(value, "requested", 9, 8, 7);
    color_is(value, "color", 255, 97, 0);
    text_is(value, "queued_ms", "9007199254740993");
    text_is(value, "finished_ms", "9223372036854775807");
    assert(reads == 1 && !submits && !memcmp(&before, &snapshot, sizeof(snapshot)));
    cJSON_Delete(value);
    const char *phases[] = {"idle", "queued", "sending", "completed", "failed", "cleaning", "cleaned"};
    for (unsigned i = 0; i < 7; ++i) {
        reset();
        snapshot.phase = (ryz_rgb_phase_t)i;
        snapshot.output_known = i == RYZ_RGB_COMPLETED;
        if (snapshot.output_known) snapshot.last_completed_id = snapshot.request_id = 31;
        value = invoke("{\"op\":\"rgb\",\"action\":\"status\",\"boot_id\":\"boot-a\"}");
        envelope(value, true);
        text_is(value, "phase", phases[i]);
        bool_is(value, "output_known", i == RYZ_RGB_COMPLETED);
        if (i == RYZ_RGB_COMPLETED) color_is(value, "color", 0, 0, 0);
        else assert(cJSON_IsNull(field(value, "color")));
        assert(reads == 1 && !submits);
        cJSON_Delete(value);
    }
    reset();
    history();
    snapshot.output_known = true; /* Even a dirty dependency output must clear on unstarted. */
    snapshot_error = ESP_ERR_INVALID_STATE;
    value = invoke(status_request);
    envelope(value, true);
    text_is(value, "phase", "unstarted");
    bool_is(value, "worker_running", false);
    bool_is(value, "output_known", false);
    bool_is(value, "resources_held", false);
    assert(number(value,"cleanup_error") == ESP_OK);
    assert(number(value, "request_id") == 0 && number(value, "last_completed_id") == 0);
    assert(cJSON_IsNull(field(value, "requested")) && cJSON_IsNull(field(value, "color")));
    assert(number(value, "error_code") == ESP_ERR_INVALID_STATE);
    cJSON_Delete(value);
    const esp_err_t errors[] = {ESP_ERR_TIMEOUT, ESP_FAIL, ESP_ERR_NO_MEM};
    for (unsigned i = 0; i < 3; ++i) {
        reset();
        snapshot_error = errors[i];
        value = invoke(status_request);
        envelope(value, false);
        assert(number(value, "error_code") == errors[i]);
        assert(!cJSON_GetObjectItemCaseSensitive(value, "color"));
        assert(reads == 1 && !submits);
        cJSON_Delete(value);
    }
    reset();
    snapshot.phase = (ryz_rgb_phase_t)99;
    value = invoke(status_request);
    envelope(value, false);
    cJSON_Delete(value);
}

static void oom(void)
{
    for (unsigned mode = 0; mode < 9; ++mode) {
        reset();
        cJSON *input = parse(mode == 0 || mode == 5 ? set_request :
                             mode == 1 ? off_request : mode == 6 ? "{}" : mode >= 7 ? cleanup_request : status_request);
        if (mode == 2) history();
        else if (mode == 3) snapshot_error = ESP_ERR_INVALID_STATE;
        else if (mode == 4) snapshot_error = ESP_ERR_TIMEOUT;
        else if (mode == 5 || mode == 8) submit_error = ESP_ERR_TIMEOUT;
        const size_t input_live = live;
        reset_calls();
        cJSON *value = ryz_workbench_rgb_rpc(input, "boot-a");
        assert(value && !post_admission_allocations);
        const size_t required = allocations;
        cJSON_Delete(value);
        for (size_t allocation = 1; allocation <= required + 1; ++allocation) {
            reset_calls();
            fail_at = allocation;
            value = ryz_workbench_rgb_rpc(input, "boot-a");
            fail_at = 0;
            if (allocation <= required) {
                assert(!value && !admitted);
                if (mode <= 1 || mode == 7) assert(!submits && !cleanups && !test_gate_calls);
            } else {
                envelope(value, mode < 4 || mode == 7);
                if (mode <= 1 || mode == 7) {
                    assert(admitted && submits + cleanups == 1 && !reads);
                    bool_is(value, "accepted", true);
                    assert(number(value, "request_id") == 31);
                }
            }
            assert(!post_admission_allocations && submits + cleanups <= 1 && reads <= 1 && !test_gate_active);
            cJSON_Delete(value);
            assert(live == input_live);
        }
        cJSON_Delete(input);
        printf("RGB_OOM %u %zu points\n", mode, required);
    }
}

static void cleanup_case(void)
{
    reset(); cJSON *value = invoke(cleanup_request); envelope(value,true);
    assert(cleanups == 1 && !submits && cleaned_id == 31 && !post_admission_allocations);
    assert(number(value,"request_id") == 31 && !cJSON_GetObjectItemCaseSensitive(value,"requested"));
    text_is(value,"action","cleanup"); cJSON_Delete(value);
    const double invalid[] = {0,-1,4294967296.0,1.5,INFINITY,NAN};
    for (unsigned i = 0; i < 6; ++i) {
        reset(); cJSON *input = parse(cleanup_request);
        cJSON_GetObjectItemCaseSensitive(input,"request_id")->valuedouble = invalid[i]; rejected(input);
    }
    const char *types[] = {"\"31\"","true","null","[]","{}"};
    for (unsigned i = 0; i < 5; ++i) {
        reset(); cJSON *input = parse(cleanup_request);
        assert(cJSON_ReplaceItemInObjectCaseSensitive(input,"request_id",parse(types[i]))); rejected(input);
    }
    reset(); cJSON *input = parse(cleanup_request);
    cJSON_DeleteItemFromObjectCaseSensitive(input,"request_id"); rejected(input);
    const char *extra[] = {"request_id","boot_id","red","green","blue"};
    for (unsigned i = 0; i < 5; ++i) {
        reset(); input = parse(cleanup_request);
        assert(cJSON_AddNumberToObject(input,extra[i],31)); rejected(input);
    }
    reset(); input = parse(cleanup_request);
    assert(cJSON_ReplaceItemInObjectCaseSensitive(input,"boot_id",parse("\"old-boot\""))); rejected(input);
    reset(); input = parse(cleanup_request);
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(input,"request_id"),UINT32_MAX);
    value = ryz_workbench_rgb_rpc(input,"boot-a"); envelope(value,true);
    assert(cleaned_id == UINT32_MAX && number(value,"request_id") == UINT32_MAX);
    cJSON_Delete(value); cJSON_Delete(input);
}

static void gate_case(void)
{
    const char *inputs[] = {set_request,off_request,cleanup_request};
    for (unsigned i = 0; i < 3; ++i) {
        reset(); test_gate_error = ESP_ERR_NOT_FINISHED;
        cJSON *input = parse(inputs[i]); reset_calls(); const size_t baseline = live;
        cJSON *value = ryz_workbench_rgb_rpc(input,"boot-a"); envelope(value,false);
        assert(number(value,"error_code") == ESP_ERR_NOT_FINISHED && test_gate_calls == 1);
        size_t count = allocations; cJSON_Delete(value);
        for (size_t point = 1; point <= count; ++point) {
            reset_calls(); fail_at = point;
            value = ryz_workbench_rgb_rpc(input,"boot-a");
            if (value) envelope(value,false);
            assert(!submits && !cleanups && !test_gate_active && test_gate_calls <= 1);
            cJSON_Delete(value); assert(live == baseline);
        }
        fail_at=0; cJSON_Delete(input);
        reset(); submit_error = ESP_FAIL;
        value=invoke(inputs[i]); envelope(value,false); cJSON_Delete(value);
        assert(test_gate_calls == 1 && !test_gate_active);
        submit_error = ESP_OK;
        value=invoke(inputs[i]); envelope(value,true); cJSON_Delete(value);
        assert(test_gate_calls == 2 && !test_gate_active);
    }
    reset(); test_gate_error=ESP_ERR_INVALID_STATE;
    cJSON *value=invoke(status_request); envelope(value,true); cJSON_Delete(value);
    assert(!test_gate_calls && reads == 1);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    cJSON_Hooks hooks = {.malloc_fn = json_malloc, .free_fn = json_free};
    cJSON_InitHooks(&hooks);
    if (!strcmp(argv[1], "validation")) validation();
    else if (!strcmp(argv[1], "submit")) submit_case();
    else if (!strcmp(argv[1], "status")) status_case();
    else if (!strcmp(argv[1], "oom")) oom();
    else if (!strcmp(argv[1], "cleanup")) cleanup_case();
    else if (!strcmp(argv[1], "gate")) gate_case();
    else assert(false);
    assert(live == 0);
    printf("WORKBENCH_RGB_RPC_PASS %s\n", argv[1]);
    return 0;
}
