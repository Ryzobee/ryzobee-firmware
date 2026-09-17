#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "workbench_monitor_rpc.h"
#include "ryz_monitor.h"
#include "tool_gate_test.h"

/* Frozen public Monitor outcomes only; real production JSON Adapter and
 * real locked cJSON. No ring, UART, log sink, or hardware is simulated here. */
static ryz_monitor_snapshot_t snapshot;
static ryz_monitor_page_t page;
static ryz_monitor_config_t submitted;
static uint32_t revision, session, generation, after, result_token;
static bool pause_value, admitted;
static esp_err_t mutation_error, snapshot_error, read_error;
static unsigned mutations, snapshots, reads, last_action;
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
    header->magic = 0x4d4f4eU;
    ++live;
    return header + 1;
}

static void json_free(void *pointer)
{
    assert(!test_gate_active);
    if (!pointer) return;
    allocation_header_t *header = (allocation_header_t *)pointer - 1;
    assert(live && header->magic == 0x4d4f4eU);
    header->magic = 0;
    --live;
    free(header);
}

static esp_err_t mutate(unsigned action, uint32_t *out)
{
    assert(test_gate_active);
    assert(out);
    last_action = action;
    ++mutations;
    admitted = mutation_error == ESP_OK;
    *out = admitted ? result_token : 0;
    return mutation_error;
}

esp_err_t ryz_monitor_start(uint32_t expected, uint32_t *out)
{
    revision = expected;
    return mutate(1, out);
}

esp_err_t ryz_monitor_configure(const ryz_monitor_config_t *config,
                              uint32_t expected, uint32_t expected_session, uint32_t *out)
{
    assert(config);
    submitted = *config;
    revision = expected;
    session = expected_session;
    return mutate(2, out);
}

esp_err_t ryz_monitor_stop(uint32_t current_session, uint32_t *out)
{
    session = current_session;
    return mutate(3, out);
}

esp_err_t ryz_monitor_set_paused(uint32_t current_session, uint32_t expected,
                               bool paused, uint32_t *out)
{
    session = current_session;
    generation = expected;
    pause_value = paused;
    return mutate(4, out);
}

esp_err_t ryz_monitor_clear(uint32_t current_session, uint32_t expected, uint32_t *out)
{
    session = current_session;
    generation = expected;
    return mutate(5, out);
}

esp_err_t ryz_monitor_get_snapshot(ryz_monitor_snapshot_t *out)
{
    assert(!test_gate_active);
    ++snapshots;
    *out = snapshot;
    return snapshot_error;
}

esp_err_t ryz_monitor_read(uint32_t current_session, uint32_t view,
                          uint32_t cursor, ryz_monitor_page_t *out)
{
    assert(!test_gate_active);
    ++reads;
    session = current_session;
    generation = view;
    after = cursor;
    *out = page;
    return read_error;
}

static void reset_calls(void)
{
    if (test_gate_error == ESP_OK) assert(test_gate_calls == mutations);
    test_gate_reset_calls();
    mutations = snapshots = reads = last_action = 0;
    admitted = false;
    allocations = fail_at = post_admission_allocations = 0;
}

static void reset(void)
{
    reset_calls();
    test_gate_error = ESP_OK;
    memset(&snapshot, 0, sizeof(snapshot));
    memset(&page, 0, sizeof(page));
    memset(&submitted, 0, sizeof(submitted));
    snapshot.config = snapshot.capture_config = snapshot.requested_config = RYZ_MONITOR_DEFAULT_CONFIG;
    mutation_error = snapshot_error = read_error = ESP_OK;
    revision = session = generation = after = 0;
    result_token = UINT32_MAX;
    pause_value = false;
}

static const char *const requests[] = {
    "{\"id\":\"monitor-1\",\"op\":\"monitor\",\"action\":\"start\",\"boot_id\":\"boot-a\",\"expected_revision\":7}",
    "{\"op\":\"monitor\",\"action\":\"configure\",\"boot_id\":\"boot-a\",\"expected_revision\":7,\"session_id\":0,\"source\":\"system\"}",
    "{\"op\":\"monitor\",\"action\":\"stop\",\"boot_id\":\"boot-a\",\"session_id\":31}",
    "{\"op\":\"monitor\",\"action\":\"pause\",\"boot_id\":\"boot-a\",\"session_id\":31,\"expected_generation\":9,\"paused\":true}",
    "{\"op\":\"monitor\",\"action\":\"clear\",\"boot_id\":\"boot-a\",\"session_id\":31,\"expected_generation\":9}",
    "{\"op\":\"monitor\",\"action\":\"status\"}",
    "{\"op\":\"monitor\",\"action\":\"read\",\"boot_id\":\"boot-a\",\"session_id\":31,\"view_generation\":9,\"after_sequence\":0}",
    "{\"op\":\"monitor\",\"action\":\"configure\",\"boot_id\":\"boot-a\",\"expected_revision\":7,\"session_id\":31,\"source\":\"uart\",\"rx\":4,\"tx\":5,\"baud\":115200}",
};

static cJSON *parse(const char *text)
{
    cJSON *value = cJSON_Parse(text);
    assert(value);
    return value;
}

static cJSON *invoke(unsigned index)
{
    cJSON *input = parse(requests[index]);
    cJSON *value = ryz_workbench_monitor_rpc(input, "boot-a");
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
    text_is(value, "schema", "ryz-monitor/1");
    text_is(value, "boot_id", "boot-a");
    (void)field(value, "id");
    (void)number(value, "error_code");
}

static void rejected(cJSON *input)
{
    cJSON *value = ryz_workbench_monitor_rpc(input, "boot-a");
    envelope(value, false);
    assert(!mutations && !snapshots && !reads && !test_gate_calls);
    cJSON_Delete(value);
    cJSON_Delete(input);
}

static void validation(void)
{
    assert(ryz_workbench_monitor_rpc_supports("monitor"));
    assert(!ryz_workbench_monitor_rpc_supports(NULL));
    assert(!ryz_workbench_monitor_rpc_supports("Monitor"));
    assert(!ryz_workbench_monitor_rpc_supports("rgb"));
    const char *bad[] = {"null", "[]", "42", "{}", "{\"op\":\"monitor\"}",
        "{\"op\":\"monitor\",\"action\":\"watch\"}",
        "{\"op\":\"bad\",\"action\":\"status\"}",
        "{\"op\":\"monitor\",\"action\":\"start\",\"expected_revision\":1}",
        "{\"op\":\"monitor\",\"action\":\"status\",\"boot_id\":\"old-boot\"}",
        "{\"op\":\"monitor\",\"action\":\"status\",\"boot_id\":null}",
        "{\"op\":\"monitor\",\"action\":true}",
        "{\"op\":0,\"action\":\"status\"}",
        "{\"op\":\"monitor\",\"action\":\"status\",\"id\":7}"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        reset(); rejected(parse(bad[i]));
    }
    const char *names[] = {"id", "op", "action", "boot_id", "source", "rx", "tx", "baud", "expected_revision", "session_id"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        reset();
        cJSON *input = parse(requests[7]);
        if (!strcmp(names[i], "id")) assert(cJSON_AddStringToObject(input, "id", "first"));
        assert(cJSON_AddStringToObject(input, names[i], "duplicate"));
        rejected(input);
    }
    const char *unknown[] = {"sda", "scl", "hz", "uart", "limit", "schema", "sequence", "generation", "filter"};
    for (unsigned request = 0; request < 8; ++request) {
        for (unsigned i = 0; i < sizeof(unknown) / sizeof(unknown[0]); ++i) {
            reset(); cJSON *input = parse(requests[request]);
            assert(cJSON_AddNumberToObject(input, unknown[i], 1)); rejected(input);
        }
    }
    struct { unsigned request; const char *name; uint32_t minimum, maximum; } numeric[] = {
        {0,"expected_revision",1,UINT32_MAX}, {1,"session_id",0,UINT32_MAX},
        {2,"session_id",1,UINT32_MAX}, {3,"expected_generation",1,UINT32_MAX},
        {6,"view_generation",1,UINT32_MAX}, {6,"after_sequence",0,UINT32_MAX},
        {7,"rx",0,48}, {7,"tx",0,48}, {7,"baud",1200,921600},
    };
    const char *types[] = {"\"1\"", "true", "null", "[]", "{}"};
    for (unsigned n = 0; n < sizeof(numeric) / sizeof(numeric[0]); ++n) {
        const double invalid[] = {(double)numeric[n].minimum - 1, (double)numeric[n].maximum + 1, 1.5, INFINITY, NAN};
        for (unsigned i = 0; i < 5; ++i) {
            reset(); cJSON *input = parse(requests[numeric[n].request]);
            /* Direct DOM assignment avoids third-party NaN-to-int setter UB. */
            cJSON_GetObjectItemCaseSensitive(input, numeric[n].name)->valuedouble = invalid[i];
            rejected(input);
            reset(); input = parse(requests[numeric[n].request]);
            assert(cJSON_ReplaceItemInObjectCaseSensitive(input, numeric[n].name, parse(types[i])));
            rejected(input);
        }
        reset(); cJSON *input = parse(requests[numeric[n].request]);
        cJSON_DeleteItemFromObjectCaseSensitive(input, numeric[n].name); rejected(input);
    }
    for (unsigned i = 0; i < 5; ++i) {
        reset(); cJSON *input = parse(requests[3]);
        const char *invalid[] = {"1", "\"true\"", "null", "[]", "{}"};
        assert(cJSON_ReplaceItemInObjectCaseSensitive(input,"paused",parse(invalid[i]))); rejected(input);
    }
    const char *extra[] = {"rx", "tx", "baud", "expected_revision", "session_id", "paused", "view_generation", "expected_generation"};
    for (unsigned i = 0; i < sizeof(extra) / sizeof(extra[0]); ++i) {
        reset(); cJSON *input = parse(requests[5]);
        assert(cJSON_AddNumberToObject(input, extra[i], 1)); rejected(input);
    }
    for (unsigned i = 0; i < 3; ++i) {
        reset(); cJSON *input = parse(requests[1]);
        assert(cJSON_AddNumberToObject(input, extra[i], 0)); rejected(input);
    }
    reset(); cJSON *input = parse(requests[7]);
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(input, "baud"), 12345); rejected(input);
    reset(); input = parse(requests[1]);
    assert(cJSON_ReplaceItemInObjectCaseSensitive(input,"source",parse("\"stdout\""))); rejected(input);
    reset(); rejected(NULL);
    input = parse(requests[0]);
    assert(!ryz_workbench_monitor_rpc(input, NULL) && !ryz_workbench_monitor_rpc(input, ""));
    cJSON_Delete(input);
    assert(!mutations && !snapshots && !reads);
}

static void mutations_case(void)
{
    for (unsigned i = 0; i < 5; ++i) {
        reset();
        cJSON *input = parse(requests[i]);
        allocations = 0;
        cJSON *value = ryz_workbench_monitor_rpc(input, "boot-a");
        envelope(value, true);
        bool_is(value, "accepted", true);
        assert(number(value, i < 3 ? "operation_id" : "view_generation") == UINT32_MAX);
        if (!i) assert(number(value, "session_id") == UINT32_MAX && revision == 7);
        if (i == 1) assert(revision == 7 && !session && submitted.source == RYZ_MONITOR_SYSTEM &&
                           submitted.rx == -1 && submitted.tx == -1 && !submitted.baud);
        if (i >= 2) assert(session == 31);
        if (i >= 3) assert(generation == 9);
        if (i == 3) assert(pause_value);
        assert(mutations == 1 && last_action == i + 1 && !reads && !snapshots && !post_admission_allocations);
        cJSON_Delete(value); cJSON_Delete(input);
    }
    reset(); cJSON *value = invoke(7);
    envelope(value, true);
    assert(submitted.source == RYZ_MONITOR_UART && submitted.rx == 4 && submitted.tx == 5 &&
           submitted.baud == 115200 && session == 31 && revision == 7 && last_action == 2);
    cJSON_Delete(value);
    reset(); cJSON *input = parse(requests[3]);
    assert(cJSON_ReplaceItemInObjectCaseSensitive(input,"paused",cJSON_CreateFalse()));
    value = ryz_workbench_monitor_rpc(input,"boot-a"); envelope(value,true);
    assert(!pause_value); cJSON_Delete(value); cJSON_Delete(input);
    const esp_err_t errors[] = {ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE, ESP_ERR_TIMEOUT,
        ESP_ERR_NOT_FINISHED, ESP_ERR_NO_MEM, ESP_FAIL};
    for (unsigned i = 0; i < 5; ++i) for (unsigned e = 0; e < sizeof(errors) / sizeof(errors[0]); ++e) {
        reset(); mutation_error = errors[e]; value = invoke(i); envelope(value, false);
        assert(number(value,"error_code") == errors[e] && mutations == 1 && !admitted);
        cJSON_Delete(value);
    }
}

static void populate(void)
{
    snapshot.config = (ryz_monitor_config_t){RYZ_MONITOR_UART,4,5,115200};
    snapshot.capture_config = RYZ_MONITOR_DEFAULT_CONFIG;
    snapshot.requested_config = (ryz_monitor_config_t){RYZ_MONITOR_UART,6,7,921600};
    snapshot.config_revision = 7; snapshot.session_id = 31; snapshot.operation_id = 36;
    snapshot.operation = RYZ_MONITOR_OP_CONFIGURE; snapshot.phase = RYZ_MONITOR_RECONFIGURING;
    snapshot.operation_pending = true; snapshot.source_active = false; snapshot.resources_held = true;
    snapshot.operation_error = ESP_FAIL; snapshot.cleanup_error = ESP_ERR_TIMEOUT;
    snapshot.restore_error = ESP_ERR_INVALID_STATE;
    snapshot.started_ms = INT64_MAX; snapshot.operation_finished_ms = 9007199254740993LL;
    snapshot.stream = (ryz_monitor_stream_info_t){.session_id=31,.config_revision=6,.view_generation=9,
        .source=RYZ_MONITOR_SYSTEM,.accepting=false,.paused=true,.capture_gap_since_boot=true,
        .sequence_exhausted=true,.first_sequence=10,.last_sequence=41,.retained_records=32,
        .chunks=UINT64_MAX,.bytes=9007199254740993ULL,.overwritten_chunks=15,.overwritten_bytes=18,
        .truncated_bytes=29,.filtered_logs=100};
    snapshot.uart_events = (ryz_monitor_uart_events_t){UINT64_MAX,2,3,4,5,true};
    page.stream = snapshot.stream; page.next_sequence = 17; page.gap = true; page.more = true;
    page.count = RYZ_MONITOR_PAGE_RECORDS;
    const uint16_t lengths[] = {0,1,2,3,4,95,96,96};
    for (unsigned i = 0; i < page.count; ++i) {
        page.records[i].sequence = i + 10; page.records[i].captured_ms = INT64_MAX;
        page.records[i].length = lengths[i]; page.records[i].truncated = i == 7;
        for (unsigned j = 0; j < lengths[i]; ++j) page.records[i].bytes[j] = (uint8_t)j;
    }
}

static void status_case(void)
{
    reset(); snapshot_error = ESP_ERR_INVALID_STATE;
    cJSON *value = invoke(5); envelope(value, true);
    text_is(value,"phase","unstarted"); bool_is(value,"worker_running",false);
    assert(number(value,"config_revision") == 1 && !number(value,"session_id"));
    text_is(field(value,"config"),"source","system");
    assert(number(field(value,"config"),"rx") == -1 && !number(field(value,"config"),"baud"));
    assert(cJSON_IsNull(field(value,"capture_config")) && cJSON_IsNull(field(value,"requested_config")) &&
           cJSON_IsNull(field(value,"stream")));
    bool_is(field(value,"uart_events"),"events_may_be_lost",false);
    bool_is(value,"capture_gap_since_boot",false);
    assert(!mutations && snapshots == 1 && !reads); cJSON_Delete(value);
    reset(); populate(); value = invoke(5); envelope(value,true);
    text_is(value,"phase","reconfiguring"); text_is(value,"operation","configure");
    bool_is(value,"operation_pending",true); bool_is(value,"resources_held",true);
    bool_is(value,"source_active",false); bool_is(value,"worker_running",true);
    assert(number(field(value,"config"),"rx") == 4 && number(field(value,"requested_config"),"rx") == 6);
    text_is(field(value,"capture_config"),"source","system");
    assert(number(value,"cleanup_error") == ESP_ERR_TIMEOUT && number(value,"restore_error") == ESP_ERR_INVALID_STATE);
    text_is(value,"started_ms","9223372036854775807");
    text_is(value,"operation_finished_ms","9007199254740993");
    text_is(field(value,"stream"),"chunks","18446744073709551615");
    text_is(field(value,"stream"),"bytes","9007199254740993");
    text_is(field(value,"stream"),"source","system");
    assert(number(field(value,"stream"),"config_revision") == 6 && number(value,"config_revision") == 7);
    bool_is(field(value,"stream"),"capture_gap_since_boot",true);
    bool_is(value,"capture_gap_since_boot",true);
    bool_is(field(value,"stream"),"paused",true);
    bool_is(field(value,"uart_events"),"events_may_be_lost",true);
    text_is(field(value,"uart_events"),"fifo_overflow","18446744073709551615");
    assert(snapshots == 1 && !mutations && !reads); cJSON_Delete(value);
    const char *phases[] = {"idle","starting","running","reconfiguring","stopping","stopped","failed"};
    for (unsigned i = 0; i < sizeof(phases) / sizeof(phases[0]); ++i) {
        reset(); snapshot.phase = (ryz_monitor_phase_t)i;
        value = invoke(5); envelope(value,true); text_is(value,"phase",phases[i]); cJSON_Delete(value);
    }
    reset(); snapshot_error = ESP_ERR_TIMEOUT; value = invoke(5); envelope(value,false);
    assert(number(value,"error_code") == ESP_ERR_TIMEOUT); cJSON_Delete(value);
    reset(); snapshot.phase = (ryz_monitor_phase_t)99; value=invoke(5); envelope(value,false);
    assert(number(value,"error_code") == ESP_ERR_INVALID_RESPONSE); cJSON_Delete(value);
}

static void read_case(void)
{
    reset(); populate();
    cJSON *value = invoke(6); envelope(value,true);
    assert(session == 31 && generation == 9 && after == 0 && reads == 1 && !snapshots && !mutations);
    assert(number(value,"count") == 8 && number(value,"next_sequence") == 17);
    bool_is(value,"gap",true); bool_is(value,"more",true);
    const cJSON *records = field(value,"records");
    assert(cJSON_GetArraySize(records) == 8);
    const char *encoded[] = {"","AA==","AAE=","AAEC","AAECAw=="};
    for (unsigned i = 0; i < 5; ++i) {
        const cJSON *record = cJSON_GetArrayItem(records,(int)i);
        text_is(record,"data_b64",encoded[i]); assert(number(record,"length") == i);
        text_is(record,"captured_ms","9223372036854775807");
    }
    const char *full = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIjJCUmJygpKissLS4vMDEyMzQ1Njc4OTo7PD0+P0BBQkNERUZHSElKS0xNTk9QUVJTVFVWV1hZWltcXV5f";
    text_is(cJSON_GetArrayItem(records,6),"data_b64",full);
    text_is(cJSON_GetArrayItem(records,7),"data_b64",full);
    bool_is(cJSON_GetArrayItem(records,7),"truncated",true);
    cJSON_Delete(value);
    reset_calls(); value=invoke(6); envelope(value,true);
    assert(number(value,"next_sequence") == 17 && reads == 1); cJSON_Delete(value);
    reset(); read_error=ESP_ERR_INVALID_STATE; value=invoke(6); envelope(value,false);
    assert(reads == 1 && !mutations && number(value,"error_code") == ESP_ERR_INVALID_STATE); cJSON_Delete(value);
    reset(); page.count=9; value=invoke(6); envelope(value,false);
    assert(number(value,"error_code") == ESP_ERR_INVALID_RESPONSE); cJSON_Delete(value);
    reset(); page.count=1; page.records[0].length=97; value=invoke(6); envelope(value,false);
    assert(number(value,"error_code") == ESP_ERR_INVALID_RESPONSE); cJSON_Delete(value);
    reset(); value=invoke(6); envelope(value,true);
    assert(!number(value,"count") && cJSON_GetArraySize(field(value,"records")) == 0); cJSON_Delete(value);
}

static void oom(void)
{
    size_t points = 0;
    for (unsigned mode = 0; mode < 19; ++mode) {
        /* 0..7 successful full replies, 8..15 dependency failures,
         * 16 unstarted status, 17 invalid request, 18 malformed service page. */
        unsigned index = mode < 16 ? mode % 8 : mode == 16 ? 5 : mode == 17 ? 0 : 6;
        reset(); populate();
        if (mode >= 8 && mode < 16) mutation_error = snapshot_error = read_error = ESP_ERR_TIMEOUT;
        if (mode == 16) snapshot_error = ESP_ERR_INVALID_STATE;
        if (mode == 18) page.count = 9;
        cJSON *input = parse(requests[index]);
        if (mode == 17) cJSON_DeleteItemFromObjectCaseSensitive(input,"boot_id");
        reset_calls();
        const size_t baseline = live;
        cJSON *value = ryz_workbench_monitor_rpc(input,"boot-a");
        assert(value);
        const size_t count = allocations;
        assert(!post_admission_allocations);
        cJSON_Delete(value); assert(live == baseline);
        for (size_t point = 1; point <= count; ++point) {
            reset_calls(); fail_at = point;
            value = ryz_workbench_monitor_rpc(input,"boot-a");
            assert(allocations >= point);
            if (admitted) { assert(value); envelope(value,true); }
            assert(!post_admission_allocations && mutations <= 1 && snapshots <= 1 && reads <= 1);
            /* Every admitted mutation has a full ACK; failed dependency may
             * return a later allocated error, never an accepted operation. */
            if (value && mode < 8 && index != 5 && index != 6) assert(admitted);
            cJSON_Delete(value); assert(live == baseline);
            ++points;
        }
        fail_at = 0; admitted = false;
        cJSON_Delete(input); assert(!live);
    }
    printf("MONITOR_OOM_POINTS %zu\n", points);
}

static void gate_case(void)
{
    for (unsigned i = 0; i < 5; ++i) {
        reset(); test_gate_error=ESP_ERR_NOT_FINISHED;
        cJSON *input=parse(requests[i]); reset_calls(); const size_t baseline=live;
        cJSON *value=ryz_workbench_monitor_rpc(input,"boot-a"); envelope(value,false);
        assert(number(value,"error_code") == ESP_ERR_NOT_FINISHED && test_gate_calls == 1);
        size_t count=allocations; cJSON_Delete(value);
        for (size_t point=1; point<=count; ++point) {
            reset_calls(); fail_at=point;
            value=ryz_workbench_monitor_rpc(input,"boot-a");
            if (value) envelope(value,false);
            assert(!mutations && !test_gate_active && test_gate_calls<=1);
            cJSON_Delete(value); assert(live==baseline);
        }
        fail_at=0; cJSON_Delete(input);
        reset(); mutation_error=ESP_FAIL;
        value=invoke(i); envelope(value,false); cJSON_Delete(value);
        assert(test_gate_calls==1 && !test_gate_active);
        mutation_error=ESP_OK;
        value=invoke(i); envelope(value,true); cJSON_Delete(value);
        assert(test_gate_calls==2 && !test_gate_active);
    }
    reset(); test_gate_error=ESP_ERR_INVALID_STATE;
    cJSON *value=invoke(5); envelope(value,true); cJSON_Delete(value);
    value=invoke(6); envelope(value,true); cJSON_Delete(value);
    assert(!test_gate_calls && snapshots==1 && reads==1);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    cJSON_Hooks hooks = {json_malloc,json_free}; cJSON_InitHooks(&hooks);
    reset();
    if (!strcmp(argv[1],"validation")) validation();
    else if (!strcmp(argv[1],"mutations")) mutations_case();
    else if (!strcmp(argv[1],"status")) status_case();
    else if (!strcmp(argv[1],"read")) read_case();
    else if (!strcmp(argv[1],"gate")) gate_case();
    else { assert(!strcmp(argv[1],"oom")); oom(); }
    assert(!live);
    printf("WORKBENCH_MONITOR_RPC_PASS %s\n",argv[1]);
    return 0;
}
