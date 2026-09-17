#include "workbench_file_rpc.h"
#include "ryz_script_store.h"
#include "script_store_host.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>

#ifdef NDEBUG
#error "RPC tests require assertions"
#endif

static const char *const BOOT = "test-boot";
static const char *const ABC_SHA =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

typedef struct {
    atomic_bool held, allowed;
    atomic_uint begins, ends;
} write_state_t;
static write_state_t writer = {.allowed = ATOMIC_VAR_INIT(true)};
static bool begin_write(void *context)
{
    write_state_t *state = context;
    atomic_fetch_add(&state->begins, 1);
    bool expected = false;
    return atomic_load(&state->allowed) &&
        atomic_compare_exchange_strong(&state->held, &expected, true);
}
static void end_write(void *context)
{
    write_state_t *state = context;
    assert(atomic_exchange(&state->held, false));
    atomic_fetch_add(&state->ends, 1);
}
static const ryz_workbench_write_guard_t guard = {&writer, begin_write, end_write};

static cJSON *field(const cJSON *value, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(value, key);
}

static const char *text(const cJSON *value, const char *key)
{
    cJSON *item = field(value, key);
    assert(cJSON_IsString(item));
    return item->valuestring;
}

static void expect_ok(const cJSON *value, bool expected)
{
    assert(value);
    assert(cJSON_IsBool(field(value, "ok")));
    assert(cJSON_IsTrue(field(value, "ok")) == expected);
}

static cJSON *request(const char *operation, const char *name)
{
    cJSON *value = cJSON_CreateObject();
    assert(value && cJSON_AddStringToObject(value, "id", "rpc-test"));
    assert(cJSON_AddStringToObject(value, "op", operation));
    if (name) assert(cJSON_AddStringToObject(value, "name", name));
    return value;
}

static cJSON *versioned(const char *action, const char *name)
{
    cJSON *value = request("scripts", name);
    assert(cJSON_AddStringToObject(value, "schema", "ryz-script-store/1"));
    assert(cJSON_AddStringToObject(value, "boot_id", BOOT));
    assert(cJSON_AddStringToObject(value, "action", action));
    return value;
}

static cJSON *call(cJSON *value)
{
    const unsigned begins = atomic_load(&writer.begins), ends = atomic_load(&writer.ends);
    cJSON *result = ryz_workbench_file_rpc(value, BOOT, false, NULL, NULL, &guard);
    assert(!atomic_load(&writer.held));
    assert(atomic_load(&writer.begins) - begins <= 1);
    assert(atomic_load(&writer.begins) - begins == atomic_load(&writer.ends) - ends);
    cJSON_Delete(value);
    assert(result);
    return result;
}

static void put(const char *name, const char *source)
{
    cJSON *value = request("put", name);
    assert(cJSON_AddStringToObject(value, "source", source));
    cJSON *result = call(value);
    expect_ok(result, true);
    cJSON_Delete(result);
}

static void source_is(const char *name, const char *expected)
{
    ryz_script_store_snapshot_t snapshot;
    assert(ryz_script_store_get(name, &snapshot) == ESP_OK);
    assert(snapshot.entry.bytes == strlen(expected));
    assert(strcmp(snapshot.source, expected) == 0);
    ryz_script_store_snapshot_free(&snapshot);
}

static void compatibility(void)
{
    assert(ryz_workbench_file_rpc_supports("list"));
    assert(ryz_workbench_file_rpc_supports("scripts"));
    assert(!ryz_workbench_file_rpc_supports(NULL));
    assert(!ryz_workbench_file_rpc_supports("erase-all"));
    cJSON *value = request("put", "compat.lua");
    assert(cJSON_AddStringToObject(value, "source", "abc"));
    assert(cJSON_AddStringToObject(value, "sha256", ABC_SHA));
    cJSON *result = call(value);
    expect_ok(result, true);
    assert(cJSON_GetArraySize(result) == 4); /* id, ok, bytes, sha256 */
    assert(!strcmp(text(result, "sha256"), ABC_SHA));
    cJSON_Delete(result);
    result = call(request("get", "compat.lua"));
    expect_ok(result, true);
    assert(cJSON_GetArraySize(result) == 6);
    assert(!strcmp(text(result, "source"), "abc"));
    assert(!strcmp(text(result, "sha256"), ABC_SHA));
    cJSON_Delete(result);
    result = call(request("list", NULL));
    expect_ok(result, true);
    assert(cJSON_GetArraySize(result) == 3);
    cJSON *files = field(result, "files");
    assert(cJSON_GetArraySize(files) == 1);
    cJSON *entry = cJSON_GetArrayItem(files, 0);
    assert(cJSON_GetArraySize(entry) == 3);
    assert(!strcmp(text(entry, "name"), "compat.lua"));
    assert(cJSON_IsFalse(field(entry, "protected")));
    cJSON_Delete(result);
    result = call(request("remove", "compat.lua"));
    expect_ok(result, true);
    assert(cJSON_GetArraySize(result) == 2);
    cJSON_Delete(result);
}

static void version_and_cas(void)
{
    cJSON *value = versioned("put", "cas.lua");
    assert(cJSON_AddStringToObject(value, "source", "abc"));
    assert(cJSON_AddStringToObject(value, "sha256", ABC_SHA));
    assert(cJSON_AddStringToObject(value, "previous_sha256", ""));
    cJSON *result = call(value);
    expect_ok(result, true);
    assert(!strcmp(text(result, "schema"), "ryz-script-store/1"));
    assert(!strcmp(text(result, "store_commit"), "committed"));
    cJSON_Delete(result);

    value = versioned("remove", "cas.lua");
    assert(cJSON_AddStringToObject(value, "previous_sha256", ""));
    result = call(value);
    expect_ok(result, false);
    cJSON_Delete(result);
    source_is("cas.lua", "abc");

    value = versioned("remove", "cas.lua");
    assert(cJSON_AddStringToObject(value, "previous_sha256", ABC_SHA));
    cJSON_SetValuestring(field(value, "boot_id"), "previous-boot");
    result = call(value);
    expect_ok(result, false);
    cJSON_Delete(result);
    source_is("cas.lua", "abc");

    result = call(versioned("remove", "cas.lua")); /* no CAS must not delete */
    expect_ok(result, false);
    cJSON_Delete(result);
    source_is("cas.lua", "abc");

    value = versioned("status", NULL);
    cJSON_SetValuestring(field(value, "schema"), "ryz-script-store/2");
    result = call(value);
    expect_ok(result, false);
    cJSON_Delete(result);

    put("cas.lua", "new content");
    value = versioned("remove", "cas.lua");
    assert(cJSON_AddStringToObject(value, "previous_sha256", ABC_SHA));
    result = call(value);
    expect_ok(result, false);
    cJSON_Delete(result);
    source_is("cas.lua", "new content");
}

static void paging(void)
{
    for (int i = 21; i >= 0; --i) {
        char name[32];
        snprintf(name, sizeof(name), "page_%02d.lua", i);
        put(name, "abc");
    }
    cJSON *value = versioned("catalog", NULL);
    assert(cJSON_AddNumberToObject(value, "limit", 4));
    cJSON *result = call(value);
    expect_ok(result, true);
    assert(field(result, "total")->valueint == 22);
    assert(field(result, "count")->valueint == 4);
    assert(!strcmp(text(cJSON_GetArrayItem(field(result, "files"), 0), "name"), "page_00.lua"));
    uint32_t revision = (uint32_t)field(result, "revision")->valuedouble;
    cJSON_Delete(result);
    value = versioned("catalog", NULL);
    assert(cJSON_AddNumberToObject(value, "offset", 4));
    assert(cJSON_AddNumberToObject(value, "revision", revision));
    result = call(value);
    expect_ok(result, true);
    assert(field(result, "count")->valueint == 16);
    assert(!strcmp(text(cJSON_GetArrayItem(field(result, "files"), 0), "name"), "page_04.lua"));
    cJSON_Delete(result);
    result = call(request("list", NULL));
    expect_ok(result, true);
    assert(cJSON_GetArraySize(field(result, "files")) == 22); /* no 16-item truncation */
    cJSON_Delete(result);
    put("page_00.lua", "changed");
    value = versioned("catalog", NULL);
    assert(cJSON_AddNumberToObject(value, "offset", 4));
    assert(cJSON_AddNumberToObject(value, "revision", revision));
    result = call(value);
    expect_ok(result, false);
    cJSON_Delete(result);
    const double invalid[] = {-1, 0, 17, 1.5};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        value = versioned("catalog", NULL);
        assert(cJSON_AddNumberToObject(value, "limit", invalid[i]));
        result = call(value);
        expect_ok(result, false);
        cJSON_Delete(result);
    }
    value = versioned("catalog", NULL);
    assert(cJSON_AddNumberToObject(value, "offset", 4));
    result = call(value); /* non-first page without revision is unsafe */
    expect_ok(result, false);
    cJSON_Delete(result);
}

static void reliable_times(void)
{
    static const char *const queries[] = {"describe", "inspect"};
    script_store_host_utc(INT64_C(1709164799)); /* 2024-02-28T23:59:59Z */
    put("clock.lua", "abc");
    script_store_host_utc(INT64_C(1709164800));
    put("clock.lua", "return 2");
    for (unsigned i = 0; i < 2; ++i) {
        cJSON *result = call(versioned(queries[i], "clock.lua"));
        expect_ok(result, true);
        assert(!strcmp(text(result, "created_at"), "2024-02-28T23:59:59Z"));
        assert(!strcmp(text(result, "modified_at"), "2024-02-29T00:00:00Z"));
        cJSON_Delete(result);
    }
    script_store_host_utc(0);
    put("clock.lua", "return 3");
    cJSON *result = call(versioned("inspect", "clock.lua"));
    expect_ok(result, true);
    assert(!strcmp(text(result, "created_at"), "2024-02-28T23:59:59Z"));
    assert(cJSON_IsNull(field(result, "modified_at")));
    cJSON_Delete(result);

    script_store_host_utc(INT64_C(253402300799));
    put("max.lua", "abc");
    result = call(versioned("describe", "max.lua"));
    assert(!strcmp(text(result, "created_at"), "9999-12-31T23:59:59Z"));
    cJSON_Delete(result);
    script_store_host_utc(INT64_MAX);
    put("max.lua", "return 4");
    result = call(versioned("describe", "max.lua"));
    assert(!strcmp(text(result, "created_at"), "9999-12-31T23:59:59Z"));
    assert(cJSON_IsNull(field(result, "modified_at")));
    cJSON_Delete(result);

    script_store_host_fault(SCRIPT_HOST_WRITE_PARTIAL, ".ryz-time-new", NULL, 0, 1);
    cJSON *value = versioned("put", "clock.lua");
    assert(cJSON_AddStringToObject(value, "source", "return 5"));
    ryz_script_store_description_t selected;
    char digest[65];
    assert(ryz_script_store_describe("clock.lua", &selected) == ESP_OK);
    assert(ryz_script_store_source_sha256("return 5", 8, digest) == ESP_OK);
    assert(cJSON_AddStringToObject(value, "previous_sha256", selected.sha256));
    assert(cJSON_AddStringToObject(value, "sha256", digest));
    result = call(value);
    expect_ok(result, true); /* Existing wire contract means SOURCE committed. */
    assert(!strcmp(text(result, "store_commit"), "committed"));
    assert(cJSON_IsTrue(field(result, "store_recovery_required")));
    assert(field(result, "store_cleanup_error")->valueint != ESP_OK);
    cJSON_Delete(result);
    result = call(versioned("inspect", "clock.lua"));
    assert(cJSON_IsNull(field(result, "created_at")) && cJSON_IsNull(field(result, "modified_at")));
    cJSON_Delete(result);
    ryz_script_store_mutation_t recovery;
    assert(ryz_script_store_recover(&recovery) == ESP_OK);
    result = call(versioned("describe", "clock.lua"));
    assert(!strcmp(text(result, "created_at"), "2024-02-28T23:59:59Z"));
    assert(cJSON_IsNull(field(result, "modified_at")));
    cJSON_Delete(result);
}

static void detail_and_validation(void)
{
    put("details.lua", "abc");
    cJSON *result = call(versioned("describe", "details.lua"));
    expect_ok(result, true);
    assert(cJSON_IsNull(field(result, "created_at")));
    assert(cJSON_IsNull(field(result, "modified_at")));
    assert(!field(result, "source"));
    assert(!strcmp(text(result, "sha256"), ABC_SHA));
    cJSON_Delete(result);
    cJSON *value = request("put", "details.lua");
    assert(cJSON_AddStringToObject(value, "source", "different"));
    assert(cJSON_AddStringToObject(value, "sha256", ABC_SHA));
    result = call(value);
    expect_ok(result, false);
    cJSON_Delete(result);
    source_is("details.lua", "abc");
    result = call(request("get", "../details.lua"));
    expect_ok(result, false);
    cJSON_Delete(result);
    value = request("remove", "details.lua");
    result = ryz_workbench_file_rpc(value, BOOT, true, NULL, NULL, &guard);
    expect_ok(result, false);
    cJSON_Delete(result);
    cJSON_Delete(value);
    source_is("details.lua", "abc");
    value = request("put", "boot.lua");
    assert(cJSON_AddStringToObject(value, "source", "abc"));
    result = call(value);
    expect_ok(result, true);
    cJSON_Delete(result);
    source_is("boot.lua", "abc");
    value = versioned("remove", "boot.lua");
    assert(cJSON_AddStringToObject(value, "previous_sha256", "0000000000000000000000000000000000000000000000000000000000000000"));
    result = call(value);
    expect_ok(result, false);
    cJSON_Delete(result);
    source_is("boot.lua", "abc");
    value = versioned("remove", "boot.lua");
    assert(cJSON_AddStringToObject(value, "previous_sha256", ABC_SHA));
    result = call(value);
    expect_ok(result, true);
    cJSON_Delete(result);
    result = call(request("get", "boot.lua"));
    expect_ok(result, false);
    cJSON_Delete(result);
    result = call(versioned("status", NULL));
    expect_ok(result, true);
    assert(cJSON_IsTrue(field(result, "ready")));
    assert(cJSON_IsFalse(field(result, "recovery_required")));
    assert(cJSON_IsTrue(field(result, "capacity_valid")));
    assert(cJSON_IsNumber(field(result, "total_bytes")));
    assert(cJSON_IsNumber(field(result, "used_bytes")));
    cJSON_Delete(result);
}

static void cleanup_outcome(void)
{
    put("cleanup.lua", "old content");
    script_store_host_fault(SCRIPT_HOST_REMOVE_BEFORE, ".ryz-old", NULL, 0, 1);
    cJSON *value = request("put", "cleanup.lua");
    assert(cJSON_AddStringToObject(value, "source", "abc"));
    cJSON *result = call(value);
    expect_ok(result, true); /* commit happened even though cleanup failed */
    assert(script_store_host_fault_hits() == 1);
    assert(!strcmp(text(result, "store_commit"), "committed"));
    assert(cJSON_IsTrue(field(result, "store_recovery_required")));
    assert(!strcmp(text(result, "sha256"), ABC_SHA));
    cJSON_Delete(result);
    source_is("cleanup.lua", "abc");
    result = call(versioned("status", NULL));
    expect_ok(result, true);
    assert(cJSON_IsTrue(field(result, "recovery_required")));
    cJSON_Delete(result);
    script_store_host_clear_fault();
    ryz_script_store_mutation_t recovered;
    assert(ryz_script_store_recover(&recovered) == ESP_OK);
    source_is("cleanup.lua", "abc");
}

static void raw_description(const char *root)
{
    const size_t sizes[] = {0, 3, RYZ_SCRIPT_STORE_SOURCE_MAX + 1024};
    for (size_t index = 0; index < sizeof(sizes) / sizeof(sizes[0]); ++index) {
        char name[32], path[1024];
        snprintf(name, sizeof(name), "raw_%zu.lua", index);
        int written = snprintf(path, sizeof(path), "%s/%s", root, name);
        assert(written > 0 && (size_t)written < sizeof(path));
        FILE *file = fopen(path, "wb");
        assert(file);
        for (size_t byte = 0; byte < sizes[index]; ++byte)
            assert(fputc(index == 1 && byte == 1 ? 0 : 'x', file) != EOF);
        assert(fclose(file) == 0);

        /* A non-executable old file must still be identifiable and removable.
         * This crosses the actual RPC Adapter, not only the Store seam. */
        cJSON *result = call(request("get", name));
        expect_ok(result, false);
        cJSON_Delete(result);
        result = call(versioned("describe", name));
        expect_ok(result, true);
        assert(field(result, "bytes")->valuedouble == (double)sizes[index]);
        assert(!field(result, "source"));
        assert(strlen(text(result, "sha256")) == 64);
        cJSON *value = versioned("remove", name);
        assert(cJSON_AddStringToObject(value, "previous_sha256", text(result, "sha256")));
        cJSON_Delete(result);
        result = call(value);
        expect_ok(result, true);
        assert(!strcmp(text(result, "store_commit"), "committed"));
        cJSON_Delete(result);
        result = call(versioned("describe", name));
        expect_ok(result, false);
        cJSON_Delete(result);
    }
}

static void inspect_metadata(void)
{
    const char *source = "-- @author: RyzoBee\n-- @version: 1.2\n"
                         "-- @description: os.execute('not code')\nprint('app')\n"
                         "-- @author: not a header\n";
    put("metadata.lua", source);
    cJSON *result = call(versioned("inspect", "metadata.lua"));
    expect_ok(result, true);
    cJSON *fields = field(result, "metadata");
    assert(cJSON_IsObject(fields));
    assert(!strcmp(text(field(fields, "author"), "value"), "RyzoBee"));
    assert(!strcmp(text(field(fields, "version"), "value"), "1.2"));
    assert(!strcmp(text(field(fields, "description"), "value"), "os.execute('not code')"));
    assert(cJSON_IsFalse(field(field(fields, "author"), "invalid")));
    assert(!field(result, "source"));
    assert(cJSON_IsNull(field(result, "created_at")) && cJSON_IsNull(field(result, "modified_at")));
    ryz_script_store_snapshot_t snapshot;
    assert(ryz_script_store_get("metadata.lua", &snapshot) == ESP_OK);
    assert(!strcmp(text(result, "sha256"), snapshot.sha256));
    assert(field(result, "bytes")->valuedouble == (double)snapshot.entry.bytes);
    ryz_script_store_snapshot_free(&snapshot);
    cJSON_Delete(result);

    put("metadata.lua", "print('changed')");
    result = call(versioned("inspect", "metadata.lua"));
    expect_ok(result, true);
    fields = field(result, "metadata");
    assert(cJSON_IsFalse(field(field(fields, "author"), "present")));
    assert(cJSON_IsNull(field(field(fields, "author"), "value")));
    cJSON_Delete(result);
}

static int allocation_budget;
static void *reply_alloc(size_t size)
{
    assert(!atomic_load(&writer.held)); /* No JSON allocation while reserving writes. */
    if (allocation_budget-- <= 0) return NULL;
    return malloc(size);
}

static void allocation_outcomes(void)
{
    unsigned committed_without_reply = 0, successful_replies = 0;
    for (int budget = 0; budget < 30; ++budget) {
        char name[32];
        snprintf(name, sizeof(name), "allocation_%02d.lua", budget);
        cJSON *value = request("put", name);
        assert(cJSON_AddStringToObject(value, "source", "abc"));
        allocation_budget = budget;
        cJSON_Hooks hooks = {.malloc_fn = reply_alloc, .free_fn = free};
        const unsigned begins = atomic_load(&writer.begins), ends = atomic_load(&writer.ends);
        cJSON_InitHooks(&hooks);
        cJSON *result = ryz_workbench_file_rpc(value, BOOT, false, NULL, NULL, &guard);
        cJSON_InitHooks(NULL);
        assert(!atomic_load(&writer.held));
        assert(atomic_load(&writer.begins) - begins == atomic_load(&writer.ends) - ends);
        assert(atomic_load(&writer.begins) - begins <= 1);
        cJSON_Delete(value);
        ryz_script_store_snapshot_t snapshot;
        esp_err_t error = ryz_script_store_get(name, &snapshot);
        if (result) {
            expect_ok(result, true);
            assert(error == ESP_OK && !strcmp(snapshot.source, "abc"));
            assert(!strcmp(text(result, "sha256"), ABC_SHA));
            ++successful_replies;
        } else if (error == ESP_OK) {
            assert(!strcmp(snapshot.source, "abc"));
            ++committed_without_reply;
        } else {
            assert(error == ESP_ERR_NOT_FOUND);
            assert(atomic_load(&writer.begins) == begins); /* ACK OOM before admission. */
        }
        if (error == ESP_OK) ryz_script_store_snapshot_free(&snapshot);
        cJSON_Delete(result);
    }
    assert(committed_without_reply && successful_replies);
}

static void missing_guard(void)
{
    cJSON *value = request("put", "guard.lua");
    assert(cJSON_AddStringToObject(value, "source", "abc"));
    const size_t before = script_store_host_platform_calls();
    cJSON *result = ryz_workbench_file_rpc(value, BOOT, false, NULL, NULL, NULL);
    expect_ok(result, false);
    assert(script_store_host_platform_calls() == before);
    assert(atomic_load(&writer.begins) == 0 && atomic_load(&writer.ends) == 0);
    cJSON_Delete(result); cJSON_Delete(value);
}

static cJSON *mutation_request(bool version, bool putting, const char *name)
{
    cJSON *value = version ? versioned(putting ? "put" : "remove", name)
                           : request(putting ? "put" : "remove", name);
    if (putting) assert(cJSON_AddStringToObject(value, "source", "abc"));
    if (version) {
        assert(cJSON_AddStringToObject(value, "previous_sha256", ABC_SHA));
        if (putting) assert(cJSON_AddStringToObject(value, "sha256", ABC_SHA));
    }
    return value;
}

static cJSON *changed_put(const char *name)
{
    cJSON *value = mutation_request(true, true, name);
    char sha[65];
    script_store_host_fixture_sha256("changed", 7, sha);
    assert(cJSON_SetValuestring(field(value, "source"), "changed"));
    assert(cJSON_SetValuestring(field(value, "sha256"), sha));
    return value;
}

static void guard_matrix(void)
{
    put("guard.lua", "abc");
    const ryz_workbench_write_guard_t no_begin = {&writer, NULL, end_write};
    const ryz_workbench_write_guard_t no_end = {&writer, begin_write, NULL};
    const ryz_workbench_write_guard_t *guards[] = {NULL, &no_begin, &no_end, &guard};
    atomic_store(&writer.allowed, false);
    for (size_t g = 0; g < sizeof(guards) / sizeof(guards[0]); ++g) {
        for (unsigned mode = 0; mode < 4; ++mode) {
            const bool version = (mode & 2) != 0, putting = (mode & 1) != 0;
            cJSON *value = mutation_request(version, putting, "guard.lua");
            const unsigned begins = atomic_load(&writer.begins), ends = atomic_load(&writer.ends);
            const size_t calls = script_store_host_platform_calls();
            cJSON *result = ryz_workbench_file_rpc(value, BOOT, false, NULL, NULL, guards[g]);
            expect_ok(result, false);
            /* Only versioned put's immutable input checksum precedes admission;
             * no platform file/capacity/transaction operation is allowed. */
            assert(script_store_host_platform_calls() == calls + (version && putting ? 1U : 0U));
            assert(atomic_load(&writer.begins) == begins + (g == 3 ? 1U : 0U));
            assert(atomic_load(&writer.ends) == ends && !atomic_load(&writer.held));
            source_is("guard.lua", "abc");
            cJSON_Delete(result); cJSON_Delete(value);
        }
    }
    atomic_store(&writer.allowed, true);
    cJSON *result = call(mutation_request(true, false, "guard.lua"));
    expect_ok(result, true); /* Rejection did not leave the lease occupied. */
    cJSON_Delete(result);
}

static void validation_before_guard(void)
{
    cJSON *values[] = {
        request("put", "bad.lua"), request("remove", "../bad.lua"),
        versioned("remove", "bad.lua"), mutation_request(true, true, "bad.lua"),
        mutation_request(false, true, "busy.lua"),
    };
    cJSON_SetValuestring(field(values[3], "sha256"),
        "0000000000000000000000000000000000000000000000000000000000000000");
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        cJSON *result = ryz_workbench_file_rpc(values[i], BOOT, i == 4, NULL, NULL, &guard);
        expect_ok(result, false);
        assert(atomic_load(&writer.begins) == 0 && atomic_load(&writer.ends) == 0);
        cJSON_Delete(result); cJSON_Delete(values[i]);
    }
}

static cJSON *prepared_run(void *context, const char *id,
                          const ryz_script_store_snapshot_t *snapshot)
{
    unsigned *calls = context;
    ++*calls;
    assert(!strcmp(snapshot->source, "abc") && !strcmp(snapshot->sha256, ABC_SHA));
    assert(id && !strcmp(id, "rpc-test"));
    cJSON *value = cJSON_CreateObject();
    assert(value && cJSON_AddBoolToObject(value, "ok", true));
    return value; /* Trusted snapshot callback, not a simulated job execution. */
}

static void reads_without_guard(void)
{
    put("read.lua", "abc");
    const unsigned begins = atomic_load(&writer.begins), ends = atomic_load(&writer.ends);
    cJSON *values[] = {request("list", NULL), request("get", "read.lua"),
        versioned("status", NULL), versioned("catalog", NULL),
        versioned("describe", "read.lua"), versioned("inspect", "read.lua"),
        versioned("run", "read.lua")};
    assert(cJSON_AddStringToObject(values[6], "sha256", ABC_SHA));
    unsigned runs = 0;
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        cJSON *result = ryz_workbench_file_rpc(values[i], BOOT, false, prepared_run, &runs, NULL);
        expect_ok(result, true);
        assert(atomic_load(&writer.begins) == begins && atomic_load(&writer.ends) == ends);
        cJSON_Delete(result); cJSON_Delete(values[i]);
    }
    assert(runs == 1);
}

static void store_failure_release(void)
{
    put("failure.lua", "abc");
    script_store_host_fault(SCRIPT_HOST_WRITE_PARTIAL, ".ryz-new", NULL, 0, 1);
    cJSON *result = call(changed_put("failure.lua"));
    expect_ok(result, false);
    assert(!strcmp(text(result, "store_commit"), "not_committed"));
    assert(script_store_host_fault_hits() == 1);
    assert(atomic_load(&writer.begins) == 2 && atomic_load(&writer.ends) == 2);
    cJSON_Delete(result);
    source_is("failure.lua", "abc");
    script_store_host_clear_fault();
    result = call(mutation_request(true, false, "failure.lua"));
    expect_ok(result, true);
    cJSON_Delete(result);
    result = call(mutation_request(false, false, "failure.lua"));
    expect_ok(result, false); /* Missing file still pairs admission and release. */
    assert(atomic_load(&writer.begins) == 4 && atomic_load(&writer.ends) == 4);
    cJSON_Delete(result);
}

static void unknown_release(void)
{
    put("unknown.lua", "abc");
    script_store_host_fault(SCRIPT_HOST_WRITE_PARTIAL, ".ryz-txn", NULL, 0, 1);
    cJSON *result = call(changed_put("unknown.lua"));
    expect_ok(result, false);
    assert(!strcmp(text(result, "store_commit"), "unknown"));
    assert(cJSON_IsTrue(field(result, "store_recovery_required")));
    assert(script_store_host_fault_hits() == 1);
    assert(atomic_load(&writer.begins) == 2 && atomic_load(&writer.ends) == 2);
    cJSON_Delete(result);
    script_store_host_clear_fault();
    result = call(mutation_request(true, false, "unknown.lua"));
    expect_ok(result, false); /* The Store's recovery gate, not a leaked lease. */
    assert(cJSON_IsTrue(field(result, "store_recovery_required")));
    assert(atomic_load(&writer.begins) == 3 && atomic_load(&writer.ends) == 3);
    cJSON_Delete(result);
    source_is("unknown.lua", "abc");
}

typedef struct {cJSON *request, *result;} concurrent_call_t;
static void *blocking_write(void *context)
{
    concurrent_call_t *call = context;
    call->result = ryz_workbench_file_rpc(call->request, BOOT, false, NULL, NULL, &guard);
    return NULL;
}

static void concurrent_guard(void)
{
    concurrent_call_t first = {.request = mutation_request(false, true, "concurrent.lua")};
    cJSON *second = mutation_request(false, false, "concurrent.lua");
    script_store_host_sha_gate_arm();
    pthread_t thread;
    assert(pthread_create(&thread, NULL, blocking_write, &first) == 0);
    script_store_host_sha_gate_wait(); /* Actual Store paused after write admission. */
    assert(atomic_load(&writer.held));
    const size_t calls = script_store_host_platform_calls();
    cJSON *result = ryz_workbench_file_rpc(second, BOOT, false, NULL, NULL, &guard);
    expect_ok(result, false);
    assert(script_store_host_platform_calls() == calls);
    assert(atomic_load(&writer.held) && atomic_load(&writer.ends) == 0);
    cJSON_Delete(result);
    script_store_host_sha_gate_release();
    assert(pthread_join(thread, NULL) == 0);
    expect_ok(first.result, true);
    assert(!atomic_load(&writer.held) && atomic_load(&writer.ends) == 1);
    source_is("concurrent.lua", "abc");
    result = call(second); /* Explicit retry after the earlier writer completed. */
    expect_ok(result, true);
    assert(atomic_load(&writer.begins) == 3 && atomic_load(&writer.ends) == 2);
    cJSON_Delete(result); cJSON_Delete(first.result); cJSON_Delete(first.request);
}

static void remove_allocation_outcomes(void)
{
    for (unsigned version = 0; version < 2; ++version) {
        unsigned before_admission = 0, committed_without_reply = 0, successful_replies = 0;
        for (int budget = 0; budget < 40; ++budget) {
            char name[32];
            snprintf(name, sizeof(name), "remove_oom_%u_%02d.lua", version, budget);
            put(name, "abc");
            cJSON *value = mutation_request(version != 0, false, name);
            const unsigned begins = atomic_load(&writer.begins), ends = atomic_load(&writer.ends);
            allocation_budget = budget;
            cJSON_Hooks hooks = {.malloc_fn = reply_alloc, .free_fn = free};
            cJSON_InitHooks(&hooks);
            cJSON *result = ryz_workbench_file_rpc(value, BOOT, false, NULL, NULL, &guard);
            cJSON_InitHooks(NULL);
            cJSON_Delete(value);
            assert(!atomic_load(&writer.held));
            assert(atomic_load(&writer.begins) - begins == atomic_load(&writer.ends) - ends);
            assert(atomic_load(&writer.begins) - begins <= 1);
            ryz_script_store_snapshot_t snapshot;
            esp_err_t error = ryz_script_store_get(name, &snapshot);
            if (result) {
                expect_ok(result, true);
                assert(error == ESP_ERR_NOT_FOUND);
                ++successful_replies;
            } else if (error == ESP_ERR_NOT_FOUND) {
                assert(atomic_load(&writer.ends) == ends + 1);
                ++committed_without_reply;
            } else {
                assert(error == ESP_OK && !strcmp(snapshot.source, "abc"));
                assert(atomic_load(&writer.begins) == begins);
                ryz_script_store_snapshot_free(&snapshot);
                ++before_admission;
            }
            cJSON_Delete(result);
        }
        assert(before_admission && successful_replies);
        /* Legacy remove adds no fields after commit, while the versioned ACK
         * has post-commit allocations whose failure is genuinely unknown. */
        assert(version ? committed_without_reply != 0 : committed_without_reply == 0);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    script_store_host_configure(argv[2]);
    ryz_script_store_mutation_t recovery;
    assert(ryz_script_store_init(argv[2], &recovery) == ESP_OK);
    if (!strcmp(argv[1], "compatibility")) compatibility();
    else if (!strcmp(argv[1], "version_cas")) version_and_cas();
    else if (!strcmp(argv[1], "paging")) paging();
    else if (!strcmp(argv[1], "detail_validation")) detail_and_validation();
    else if (!strcmp(argv[1], "reliable_times")) reliable_times();
    else if (!strcmp(argv[1], "cleanup_outcome")) cleanup_outcome();
    else if (!strcmp(argv[1], "raw_description")) raw_description(argv[2]);
    else if (!strcmp(argv[1], "inspect_metadata")) inspect_metadata();
    else if (!strcmp(argv[1], "allocation_outcomes")) allocation_outcomes();
    else if (!strcmp(argv[1], "missing_guard")) missing_guard();
    else if (!strcmp(argv[1], "guard_matrix")) guard_matrix();
    else if (!strcmp(argv[1], "validation_before_guard")) validation_before_guard();
    else if (!strcmp(argv[1], "reads_without_guard")) reads_without_guard();
    else if (!strcmp(argv[1], "store_failure_release")) store_failure_release();
    else if (!strcmp(argv[1], "unknown_release")) unknown_release();
    else if (!strcmp(argv[1], "concurrent_guard")) concurrent_guard();
    else if (!strcmp(argv[1], "remove_allocation_outcomes")) remove_allocation_outcomes();
    else assert(!"unknown test case");
    printf("WORKBENCH_FILE_RPC_PASS %s\n", argv[1]);
    return 0;
}
