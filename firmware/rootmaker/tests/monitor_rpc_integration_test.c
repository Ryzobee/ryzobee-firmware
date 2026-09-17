/* Reuse only the existing external OS/source adapters and deterministic gates.
 * Production core, stream, and RPC are compiled separately and unchanged. */
#define main monitor_core_test_main
#include "monitor_test.c"
#undef main
#include "workbench_monitor_rpc.h"

static cJSON *rpc(const char *action, const char *fields)
{
    char request[512];
    int length = snprintf(request, sizeof(request),
        "{\"id\":\"integration\",\"op\":\"monitor\",\"action\":\"%s\",\"boot_id\":\"boot-a\"%s}", action, fields);
    assert(length > 0 && (size_t)length < sizeof(request));
    cJSON *input = cJSON_Parse(request); assert(input);
    cJSON *response = ryz_workbench_monitor_rpc(input, "boot-a");
    assert(response); cJSON_Delete(input);
    return response;
}
static const cJSON *item(const cJSON *value, const char *name)
{
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(value, name);
    assert(result); return result;
}
static uint32_t integer(const cJSON *value, const char *name)
{
    const cJSON *result = item(value, name);
    assert(cJSON_IsNumber(result) && result->valuedouble >= 0 && result->valuedouble <= UINT32_MAX);
    return (uint32_t)result->valuedouble;
}
static void text_is(const cJSON *value, const char *name, const char *expected)
{
    const cJSON *result = item(value, name);
    assert(cJSON_IsString(result) && !strcmp(result->valuestring, expected));
}
static void ok(const cJSON *value)
{
    assert(cJSON_IsTrue(item(value,"ok")));
    text_is(value,"schema","ryz-monitor/1");
    text_is(value,"boot_id","boot-a");
}
static void error_is(const cJSON *value, esp_err_t error)
{
    assert(cJSON_IsFalse(item(value,"ok")));
    assert(item(value,"error_code")->valuedouble == error);
}
static void initial_configure(void)
{
    cJSON *value = rpc("status", ""); ok(value);
    text_is(value,"phase","unstarted");
    assert(cJSON_IsNull(item(value,"stream")) && !created); cJSON_Delete(value);
    value = rpc("configure", ",\"source\":\"uart\",\"rx\":2,\"tx\":3,\"baud\":115200,\"expected_revision\":1,\"session_id\":0");
    ok(value); assert(integer(value,"operation_id") == 1); cJSON_Delete(value);
    value = rpc("status", ""); ok(value); text_is(value,"phase","reconfiguring");
    assert(!integer(value,"session_id") && integer(value,"config_revision") == 1);
    assert(cJSON_IsNull(item(value,"capture_config")) && cJSON_IsNull(item(value,"stream")));
    text_is(item(value,"config"),"source","system");
    text_is(item(value,"requested_config"),"source","uart"); cJSON_Delete(value);
    assert(!begins && !log_installs); drive();
    value = rpc("status", ""); ok(value); text_is(value,"phase","idle");
    assert(integer(value,"config_revision") == 2 && !integer(value,"session_id"));
    assert(cJSON_IsNull(item(value,"stream")) && cJSON_IsNull(item(value,"capture_config")));
    cJSON_Delete(value);
    assert(!begins && !log_installs && !polls);
    value = rpc("read", ",\"session_id\":1,\"view_generation\":1,\"after_sequence\":0");
    error_is(value,ESP_ERR_INVALID_STATE); cJSON_Delete(value);
}
static void pending_start_stop(void)
{
    /* The existing Host adapter keeps a newly created worker behind a
     * condition-variable gate until drive(), not a timing-dependent sleep. */
    cJSON *value = rpc("start", ",\"expected_revision\":1"); ok(value);
    assert(integer(value,"session_id") == 1); cJSON_Delete(value);
    value = rpc("status", ""); ok(value); text_is(value,"phase","starting");
    assert(cJSON_IsNull(item(value,"stream"))); /* Exact pre-reset regression. */
    cJSON_Delete(value);
    value = rpc("read", ",\"session_id\":1,\"view_generation\":1,\"after_sequence\":0");
    error_is(value,ESP_ERR_INVALID_STATE); cJSON_Delete(value);
    value = rpc("stop", ",\"session_id\":1"); ok(value);
    assert(integer(value,"operation_id") == 2); cJSON_Delete(value);
    value = rpc("status", ""); ok(value); text_is(value,"phase","stopping");
    assert(cJSON_IsNull(item(value,"stream")) && cJSON_IsTrue(item(value,"operation_pending")));
    cJSON_Delete(value); drive();
    value = rpc("status", ""); ok(value); text_is(value,"phase","stopped");
    assert(cJSON_IsNull(item(value,"stream")) && cJSON_IsFalse(item(value,"source_active")));
    cJSON_Delete(value); assert(!begins && !log_installs && !polls);
}
static void history(void)
{
    cJSON *value = rpc("start", ",\"expected_revision\":1"); ok(value); cJSON_Delete(value); drive();
    const uint8_t binary[] = {'a',0,'b'}; emit_log(binary,sizeof(binary));
    value = rpc("pause", ",\"session_id\":1,\"expected_generation\":1,\"paused\":true");
    ok(value); assert(integer(value,"view_generation") == 2); cJSON_Delete(value);
    for (unsigned i = 0; i < 40; ++i) emit_log("x",1);
    value = rpc("read", ",\"session_id\":1,\"view_generation\":2,\"after_sequence\":0"); ok(value);
    assert(integer(value,"count") == 1);
    text_is(cJSON_GetArrayItem(item(value,"records"),0),"data_b64","YQBi");
    text_is(item(value,"stream"),"chunks","41"); text_is(item(value,"stream"),"overwritten_chunks","9");
    cJSON_Delete(value);
    value = rpc("pause", ",\"session_id\":1,\"expected_generation\":2,\"paused\":false");
    ok(value); assert(integer(value,"view_generation") == 3); cJSON_Delete(value);
    value = rpc("read", ",\"session_id\":1,\"view_generation\":2,\"after_sequence\":0");
    error_is(value,ESP_ERR_INVALID_STATE); cJSON_Delete(value);
    value = rpc("read", ",\"session_id\":1,\"view_generation\":3,\"after_sequence\":1"); ok(value);
    assert(integer(value,"count") == 8 && cJSON_IsTrue(item(value,"gap")) && cJSON_IsTrue(item(value,"more")));
    cJSON_Delete(value);
    value = rpc("clear", ",\"session_id\":1,\"expected_generation\":3");
    ok(value); assert(integer(value,"view_generation") == 4); cJSON_Delete(value);
    value = rpc("read", ",\"session_id\":1,\"view_generation\":4,\"after_sequence\":0"); ok(value);
    assert(!integer(value,"count")); text_is(item(value,"stream"),"chunks","41"); cJSON_Delete(value);
    emit_log("kept",4);
    value = rpc("stop", ",\"session_id\":1"); ok(value); cJSON_Delete(value); drive();
    value = rpc("configure", ",\"source\":\"uart\",\"rx\":2,\"tx\":3,\"baud\":115200,\"expected_revision\":1,\"session_id\":1");
    ok(value); cJSON_Delete(value); drive();
    value = rpc("status", ""); ok(value); text_is(value,"phase","stopped");
    text_is(item(value,"config"),"source","uart"); text_is(item(value,"capture_config"),"source","system");
    text_is(item(value,"stream"),"source","system");
    assert(integer(value,"config_revision") == 2 && integer(item(value,"stream"),"config_revision") == 1);
    assert(cJSON_IsFalse(item(value,"source_active")) && !begins); cJSON_Delete(value);
    value = rpc("read", ",\"session_id\":1,\"view_generation\":4,\"after_sequence\":0"); ok(value);
    assert(integer(value,"count") == 1); text_is(cJSON_GetArrayItem(item(value,"records"),0),"data_b64","a2VwdA==");
    cJSON_Delete(value);
    /* A boot-lifetime loss latch survives the no-history interval at restart. */
    ryz_monitor_stream_gap();
    value = rpc("start", ",\"expected_revision\":2"); ok(value);
    uint32_t newer = integer(value,"session_id"); assert(newer == 4); cJSON_Delete(value);
    value = rpc("status", ""); ok(value);
    assert(cJSON_IsTrue(item(value,"capture_gap_since_boot")));
    const cJSON *view = item(value,"stream");
    assert(cJSON_IsNull(view) || integer(view,"session_id") == newer); cJSON_Delete(value);
    value = rpc("stop", ",\"session_id\":1"); error_is(value,ESP_ERR_INVALID_STATE); cJSON_Delete(value);
    value = rpc("read", ",\"session_id\":1,\"view_generation\":4,\"after_sequence\":0");
    error_is(value,ESP_ERR_INVALID_STATE); cJSON_Delete(value);
    drive();
    value = rpc("status", ""); ok(value);
    assert(cJSON_IsTrue(item(value,"capture_gap_since_boot")));
    text_is(item(value,"stream"),"source","uart"); cJSON_Delete(value);
}
static void switch_source(void)
{
    cJSON *value = rpc("start", ",\"expected_revision\":1"); ok(value); cJSON_Delete(value); drive();
    emit_log("kept",4);
    begin_errors[0] = ESP_ERR_TIMEOUT;
    const char *candidate = ",\"source\":\"uart\",\"rx\":2,\"tx\":3,\"baud\":115200,\"expected_revision\":1,\"session_id\":1";
    value = rpc("configure",candidate); ok(value); cJSON_Delete(value);
    /* Running worker is parked in delay_ms until drive grants a permit. */
    value = rpc("status", ""); ok(value); text_is(value,"phase","reconfiguring");
    text_is(item(value,"config"),"source","system"); text_is(item(value,"capture_config"),"source","system");
    text_is(item(value,"requested_config"),"source","uart");
    assert(cJSON_IsTrue(item(value,"operation_pending"))); cJSON_Delete(value);
    value = rpc("pause", ",\"session_id\":1,\"expected_generation\":1,\"paused\":true");
    error_is(value,ESP_ERR_INVALID_STATE); cJSON_Delete(value);
    value = rpc("read", ",\"session_id\":1,\"view_generation\":1,\"after_sequence\":0"); ok(value);
    assert(integer(value,"count") == 1); cJSON_Delete(value); drive();
    value = rpc("status", ""); ok(value); text_is(value,"phase","running");
    assert(integer(value,"operation_error") == ESP_ERR_TIMEOUT && !integer(value,"restore_error") &&
           integer(value,"config_revision") == 1 && cJSON_IsFalse(item(value,"operation_pending")));
    text_is(item(value,"requested_config"),"source","uart"); text_is(item(value,"capture_config"),"source","system");
    assert(integer(item(value,"stream"),"view_generation") == 1 && cJSON_IsTrue(item(value,"source_active")));
    cJSON_Delete(value);
    value = rpc("read", ",\"session_id\":1,\"view_generation\":1,\"after_sequence\":0"); ok(value);
    text_is(cJSON_GetArrayItem(item(value,"records"),0),"data_b64","a2VwdA=="); cJSON_Delete(value);
    value = rpc("configure",candidate); ok(value); cJSON_Delete(value); drive();
    value = rpc("status", ""); ok(value); text_is(value,"phase","running");
    assert(integer(value,"config_revision") == 2 && !integer(value,"operation_error") && integer(value,"session_id") == 1);
    text_is(item(value,"config"),"source","uart"); text_is(item(value,"capture_config"),"source","uart");
    text_is(item(value,"stream"),"source","uart"); assert(integer(item(value,"stream"),"view_generation") == 2);
    assert(cJSON_IsTrue(item(value,"resources_held")) && cJSON_IsTrue(item(item(value,"uart_events"),"events_may_be_lost")));
    cJSON_Delete(value);
    value = rpc("read", ",\"session_id\":1,\"view_generation\":1,\"after_sequence\":0");
    error_is(value,ESP_ERR_INVALID_STATE); cJSON_Delete(value);
    value = rpc("read", ",\"session_id\":1,\"view_generation\":2,\"after_sequence\":0"); ok(value);
    assert(!integer(value,"count")); cJSON_Delete(value);
    value = rpc("stop", ",\"session_id\":1"); ok(value); cJSON_Delete(value); drive();
    value = rpc("status", ""); ok(value); text_is(value,"phase","stopped");
    assert(cJSON_IsFalse(item(value,"resources_held"))); cJSON_Delete(value);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1],"initial_configure")) initial_configure();
    else if (!strcmp(argv[1],"pending_start_stop")) pending_start_stop();
    else if (!strcmp(argv[1],"history")) history();
    else { assert(!strcmp(argv[1],"switch_source")); switch_source(); }
    stop_worker();
    printf("MONITOR_RPC_INTEGRATION_PASS %s\n",argv[1]);
    return 0;
}
