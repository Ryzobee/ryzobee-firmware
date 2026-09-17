/* Actual Lua facade -> actual job controller -> broker -> I2C worker.
 * Only clock/allocator/OS/bus seams are controlled, never native snapshots. */
#define main scan_fixture_main
#include "i2c_scan_test.c"
#undef main
#include "app_runtime.h"
#include "workbench_tools.h"

/* Unused tools fail loudly; this is not a simulated RGB/Monitor backend. */
esp_err_t ryz_rgb_submit(ryz_rgb_color_t c, uint32_t *out) { (void)c; (void)out; abort(); }
esp_err_t ryz_rgb_cleanup(uint32_t id) { (void)id; abort(); }
esp_err_t ryz_rgb_get_snapshot(ryz_rgb_snapshot_t *out) { (void)out; abort(); }
esp_err_t ryz_rgb_lease_open(uint32_t *out) { (void)out; abort(); }
esp_err_t ryz_rgb_lease_submit(uint32_t lease, ryz_rgb_color_t color, uint32_t *out)
{ (void)lease; (void)color; (void)out; abort(); }
esp_err_t ryz_rgb_lease_close(uint32_t lease) { (void)lease; abort(); }
esp_err_t ryz_rgb_lease_retry_close(uint32_t lease) { (void)lease; abort(); }
esp_err_t ryz_monitor_validate_config(const ryz_monitor_config_t *c) { (void)c; abort(); }
esp_err_t ryz_monitor_start(uint32_t rev, uint32_t *out) { (void)rev; (void)out; abort(); }
esp_err_t ryz_monitor_configure(const ryz_monitor_config_t *c, uint32_t rev, uint32_t id, uint32_t *out)
{ (void)c; (void)rev; (void)id; (void)out; abort(); }
esp_err_t ryz_monitor_stop(uint32_t id, uint32_t *out) { (void)id; (void)out; abort(); }
esp_err_t ryz_monitor_set_paused(uint32_t id, uint32_t view, bool pause, uint32_t *out)
{ (void)id; (void)view; (void)pause; (void)out; abort(); }
esp_err_t ryz_monitor_clear(uint32_t id, uint32_t view, uint32_t *out)
{ (void)id; (void)view; (void)out; abort(); }
esp_err_t ryz_monitor_get_snapshot(ryz_monitor_snapshot_t *out) { (void)out; abort(); }
esp_err_t ryz_monitor_read(uint32_t id, uint32_t view, uint32_t after, ryz_monitor_page_t *out)
{ (void)id; (void)view; (void)after; (void)out; abort(); }

typedef union { max_align_t align; size_t size; } allocation_t;
static size_t lua_bytes;
static bool fail_lua_allocation, app_cancelled;
static uint64_t app_clock;
static unsigned accepted;
static const char *mode;

static void *app_resize(void *ptr, size_t bytes)
{
    allocation_t *previous = ptr ? (allocation_t *)ptr - 1 : NULL;
    size_t old = previous ? previous->size : 0;
    if (!bytes) { lua_bytes -= old; free(previous); return NULL; }
    if (fail_lua_allocation) return NULL;
    allocation_t *next = realloc(previous, sizeof(*next) + bytes);
    assert(next);
    next->size = bytes;
    lua_bytes = lua_bytes - old + bytes;
    return next + 1;
}
static uint64_t app_now(void *context) { (void)context; return app_clock; }
static bool app_cancel(void *context) { (void)context; return app_cancelled; }
static void app_pause(void *context, unsigned ms)
{ (void)context; app_clock += ms ? ms : 1; }
static ryz_tool_call_result_t admit_tools(void *context, const ryz_tool_request_t *request,
                                         ryz_tool_reply_t *out)
{
    ryz_workbench_tools_job_t *job = context;
    int result = ryz_workbench_tools_call(job, request, out);
    if (result == RYZ_TOOL_CALL_OK && request->action == RYZ_TOOL_I2C_START) {
        ++accepted;
        ryz_tools_snapshot_t owner;
        assert(ryz_tools_get_snapshot(&owner) == ESP_OK);
        assert(owner.tools[RYZ_TOOL_I2C].owner_session == job->session_id);
        assert(owner.tools[RYZ_TOOL_I2C].operation_id == out->operation_id && out->operation_id);
        /* Place a real probe in flight before allowing Lua to see its ACK. */
        assert(pthread_mutex_lock(&gate) == 0);
        released = true;
        assert(pthread_cond_broadcast(&changed) == 0);
        while (!probe_entered) timed_wait();
        assert(pthread_mutex_unlock(&gate) == 0);
        if (!strcmp(mode, "memory")) fail_lua_allocation = true;
        if (!strcmp(mode, "stopped")) app_cancelled = true;
    }
    return (ryz_tool_call_result_t)result;
}

static ryz_workbench_tools_report_t report(const char *id, const ryz_workbench_tools_report_t *final)
{
    ryz_workbench_tools_report_t out;
    assert(ryz_workbench_tools_report(id, final, &out) == ESP_OK);
    return out;
}

int main(int argc, char **argv)
{
    assert(argc == 2); mode = argv[1];
    nack_fixture(); block_probe = true;
    ryz_workbench_tools_job_t job;
    ryz_workbench_tools_begin(&job, "lua-old");
    ryz_app_platform_t platform = {.context = &job, .resize = app_resize, .now_ms = app_now,
        .pause_ms = app_pause, .cancelled = app_cancel, .tool_call = admit_tools};
    char source[1024];
    const char *ending = !strcmp(mode, "runtime") ? "error('requested fault')" :
        !strcmp(mode, "timeout") ? "while true do end" : "assert(r.ok and r.operation_id=='1')";
    snprintf(source, sizeof(source),
        "-- ryz-app/1\nlocal t=require('tools'); "
        "assert(t.i2c.configure({sda=2,scl=3,hz=400000,expected_revision='1'}).ok); "
        "local r=t.i2c.start('2'); %s", ending);
    if (!strcmp(mode, "unused")) strcpy(source, "-- ryz-app/1\nassert(1+1==2)");
    ryz_lua_result_t result;
    ryz_app_execute(source, strlen(source), "=tools-integration", 50, &platform, &result);
    fail_lua_allocation = false;
    assert(lua_bytes == 0);
    const char *expected = !strcmp(mode, "unused") || !strcmp(mode, "done") ||
        !strcmp(mode, "failed_cleanup") ? "done" : mode;
    if (strcmp(result.phase, expected)) {
        fprintf(stderr, "expected %s got %s: %s\n", expected, result.phase, result.error);
        abort();
    }
    assert(result.ok == !strcmp(expected, "done"));
    assert(ryz_workbench_tools_finish(&job, 1000));
    ryz_workbench_tools_report_t final = job.final;
    ryz_workbench_tools_report_t state = report("lua-old", &final);
    if (!strcmp(mode, "unused")) {
        assert(!accepted && !created && !job.session_id && ryz_workbench_tools_clean(&state));
        puts("APP_TOOLS_I2C_PASS"); return 0;
    }
    assert(accepted == 1 && !ryz_workbench_tools_clean(&state));
    assert(state.phase == RYZ_WB_TOOLS_PENDING && state.remaining_mask == 1);
    assert(board_inits == 1 && probes == 1 && !bus_ends);
    uint32_t session = job.session_id;
    /* A freed Job is no longer the cleanup owner: only numeric retired data
     * survives, and tick keeps progressing after its observation deadline. */
    memset(&job, 0xa5, sizeof(job));
    ryz_workbench_tools_tick(1000 + RYZ_WORKBENCH_TOOLS_OBSERVATION_US + 1);
    state = report("lua-old", &final);
    assert(state.timed_out && !ryz_workbench_tools_clean(&state));
    assert(state.session_id == session && state.pending_mask == 1);
    if (!strcmp(mode, "failed_cleanup")) end_error = ESP_FAIL;
    assert(pthread_mutex_lock(&gate) == 0);
    release_probe = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    assert(pthread_mutex_unlock(&gate) == 0);
    run_to_idle();
    ryz_workbench_tools_tick(700000);
    state = report("lua-old", &final);
    if (!strcmp(mode, "failed_cleanup")) {
        assert(state.phase == RYZ_WB_TOOLS_FAILED && state.failed_mask == 1);
        unsigned old_ends = bus_ends;
        for (unsigned i = 0; i < 10; ++i) ryz_workbench_tools_tick(700001 + i);
        assert(bus_ends == old_ends); /* Explicit retry only. */
        char command[160];
        snprintf(command, sizeof(command),
            "{\"id\":\"retry-a\",\"op\":\"tools\",\"action\":\"retry_close\",\"boot_id\":\"boot-a\",\"session_id\":%u}", session);
        cJSON *request = cJSON_Parse(command);
        cJSON *reply = ryz_workbench_tools_rpc(request, "boot-a");
        assert(reply && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply, "ok")));
        cJSON_Delete(reply); cJSON_Delete(request);
        end_error = ESP_OK;
        ryz_workbench_tools_tick(800000); run_to_idle(); ryz_workbench_tools_tick(900000);
        state = report("lua-old", &final);
    }
    assert(ryz_workbench_tools_clean(&state) && !state.remaining_mask);
    assert(!bus_held && board_inits == 1 && probes == 1);
    ryz_tools_snapshot_t owner;
    assert(ryz_tools_get_snapshot(&owner) == ESP_OK && !owner.current_session);
    assert(!owner.tools[RYZ_TOOL_I2C].owner_session);
    assert(pthread_mutex_lock(&gate) == 0);
    stopping = true; assert(pthread_cond_broadcast(&changed) == 0);
    assert(pthread_mutex_unlock(&gate) == 0); assert(pthread_join(worker, NULL) == 0);
    puts("APP_TOOLS_I2C_PASS"); return 0;
}
