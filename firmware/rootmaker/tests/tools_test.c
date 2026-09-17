#include "ryz_tools.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static ryz_i2c_scan_snapshot_t i2c;
static ryz_rgb_snapshot_t rgb;
static ryz_monitor_snapshot_t monitor;
static bool initialized[RYZ_TOOL_COUNT];
static unsigned starts[RYZ_TOOL_COUNT], stops[RYZ_TOOL_COUNT], queries, external_calls;
static uint32_t serial;
static esp_err_t query_error, mutation_error;
static uint32_t last_stopped[RYZ_TOOL_COUNT];
static unsigned complete_before_cancel;

esp_err_t ryz_i2c_scan_validate_config(const ryz_i2c_scan_config_t *config)
{ return config && config->sda != config->scl && (config->hz == 100000 || config->hz == 400000) ? ESP_OK : ESP_ERR_INVALID_ARG; }
esp_err_t ryz_monitor_validate_config(const ryz_monitor_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    if (config->source == RYZ_MONITOR_SYSTEM)
        return config->rx == -1 && config->tx == -1 && !config->baud ? ESP_OK : ESP_ERR_INVALID_ARG;
    return config->source == RYZ_MONITOR_UART && config->rx != config->tx && config->baud ? ESP_OK : ESP_ERR_INVALID_ARG;
}
esp_err_t ryz_i2c_scan_get_snapshot(ryz_i2c_scan_snapshot_t *out)
{
    ++queries; memset(out, 0, sizeof(*out));
    if (query_error) return query_error;
    if (!initialized[0]) return ESP_ERR_INVALID_STATE;
    *out = i2c; return ESP_OK;
}
esp_err_t ryz_rgb_get_snapshot(ryz_rgb_snapshot_t *out)
{
    ++queries; memset(out, 0, sizeof(*out));
    if (query_error) return query_error;
    if (!initialized[1]) return ESP_ERR_INVALID_STATE;
    *out = rgb; return ESP_OK;
}
esp_err_t ryz_monitor_get_snapshot(ryz_monitor_snapshot_t *out)
{
    ++queries; memset(out, 0, sizeof(*out));
    if (query_error) return query_error;
    if (!initialized[2]) return ESP_ERR_INVALID_STATE;
    *out = monitor; return ESP_OK;
}
esp_err_t ryz_i2c_scan_configure(const ryz_i2c_scan_config_t *config, uint32_t expected, uint32_t *out)
{
    *out = 0;
    if (mutation_error) return mutation_error;
    if (expected != (initialized[0] ? i2c.config_revision : 1)) return ESP_ERR_INVALID_STATE;
    initialized[0] = true; i2c.config = *config;
    *out = i2c.config_revision = expected + 1;
    return ESP_OK;
}
esp_err_t ryz_i2c_scan_start(uint32_t *out)
{
    *out = 0;
    if (mutation_error) return mutation_error;
    if (!initialized[0]) { i2c.config = RYZ_I2C_SCAN_DEFAULT_CONFIG; i2c.config_revision = 1; }
    initialized[0] = true; ++starts[0]; i2c.phase = RYZ_I2C_SCAN_QUEUED;
    *out = i2c.scan_id = ++serial; return ESP_OK;
}
esp_err_t ryz_i2c_scan_cancel(uint32_t id)
{
    ++stops[0]; last_stopped[0] = id;
    if (mutation_error) return mutation_error;
    assert(id == i2c.scan_id);
    if (complete_before_cancel) {
        i2c.phase = RYZ_I2C_SCAN_COMPLETED;
        i2c.resources_held = false;
        if (complete_before_cancel == 2) i2c.resources_held = true;
        if (complete_before_cancel == 3) ++i2c.scan_id;
        if (complete_before_cancel == 4) query_error = ESP_ERR_NOT_FINISHED;
        if (complete_before_cancel == 5) i2c.phase = (ryz_i2c_scan_phase_t)99;
        return ESP_ERR_INVALID_STATE; /* Real core rejects cancellation after completion. */
    }
    i2c.phase = RYZ_I2C_SCAN_CANCELLING; return ESP_OK;
}
esp_err_t ryz_rgb_submit(ryz_rgb_color_t color, uint32_t *out)
{
    *out = 0;
    if (mutation_error) return mutation_error;
    initialized[1] = true; ++starts[1]; rgb.requested = color;
    rgb.phase = RYZ_RGB_QUEUED; *out = rgb.request_id = ++serial; return ESP_OK;
}
esp_err_t ryz_rgb_cleanup(uint32_t id)
{
    ++stops[1]; last_stopped[1] = id;
    if (mutation_error) return mutation_error;
    assert(id == rgb.request_id);
    rgb.phase = RYZ_RGB_CLEANING; return ESP_OK;
}
#include "rgb_lease_fixture.h"
esp_err_t ryz_monitor_configure(const ryz_monitor_config_t *config, uint32_t expected,
                              uint32_t session, uint32_t *out)
{
    *out = 0;
    if (mutation_error) return mutation_error;
    assert(session == monitor.session_id);
    assert(expected == (initialized[2] ? monitor.config_revision : 1));
    initialized[2] = true; monitor.requested_config = *config;
    monitor.operation_pending = true; monitor.phase = RYZ_MONITOR_RECONFIGURING;
    monitor.operation = RYZ_MONITOR_OP_CONFIGURE;
    *out = monitor.operation_id = ++serial; return ESP_OK;
}
esp_err_t ryz_monitor_start(uint32_t expected, uint32_t *out)
{
    *out = 0;
    if (mutation_error) return mutation_error;
    if (!initialized[2]) { monitor.config = RYZ_MONITOR_DEFAULT_CONFIG; monitor.config_revision = 1; }
    assert(expected == monitor.config_revision);
    initialized[2] = true; ++starts[2]; monitor.operation_pending = true;
    monitor.phase = RYZ_MONITOR_STARTING; monitor.operation = RYZ_MONITOR_OP_START;
    *out = monitor.operation_id = monitor.session_id = ++serial;
    return ESP_OK;
}
esp_err_t ryz_monitor_stop(uint32_t id, uint32_t *out)
{
    *out = 0; ++stops[2]; last_stopped[2] = id;
    if (mutation_error) return mutation_error;
    assert(id == monitor.session_id);
    monitor.operation_pending = true; monitor.phase = RYZ_MONITOR_STOPPING;
    monitor.operation = RYZ_MONITOR_OP_STOP;
    *out = monitor.operation_id = ++serial; return ESP_OK;
}
esp_err_t ryz_monitor_set_paused(uint32_t id, uint32_t gen, bool paused, uint32_t *out)
{ assert(id == monitor.session_id); monitor.stream.paused = paused; *out = gen + 1; return ESP_OK; }
esp_err_t ryz_monitor_clear(uint32_t id, uint32_t gen, uint32_t *out)
{ assert(id == monitor.session_id); *out = gen + 1; return ESP_OK; }
esp_err_t ryz_monitor_read(uint32_t id, uint32_t gen, uint32_t after, ryz_monitor_page_t *out)
{
    assert(id == monitor.session_id); memset(out, 0, sizeof(*out));
    out->stream.session_id = id; out->stream.view_generation = gen; out->next_sequence = after;
    return ESP_OK;
}
static uint32_t open_session(void)
{ uint32_t id; assert(ryz_tools_open(&id) == ESP_OK && id); return id; }
static ryz_tools_snapshot_t snapshot(void)
{ ryz_tools_snapshot_t out; assert(ryz_tools_get_snapshot(&out) == ESP_OK); return out; }
static ryz_tool_reply_t call(uint32_t session, ryz_tool_request_t request)
{ ryz_tool_reply_t out; assert(ryz_tools_call(session, &request, &out) == ESP_OK); return out; }
static esp_err_t external(void *context)
{ (void)context; ++external_calls; return mutation_error; }
static void released(void)
{
    i2c.phase = RYZ_I2C_SCAN_CANCELLED; i2c.resources_held = false; i2c.cleanup_error = 0;
    rgb.phase = RYZ_RGB_CLEANED; rgb.resources_held = false; rgb.cleanup_error = 0;
    monitor.phase = RYZ_MONITOR_STOPPED; monitor.operation_pending = false;
    monitor.source_active = monitor.resources_held = false; monitor.cleanup_error = 0;
}
static void test_lazy(void)
{
    uint32_t session = open_session(), extra = 9;
    assert(ryz_tools_open(&extra) == ESP_ERR_INVALID_STATE && !extra && !queries);
    ryz_tool_reply_t out;
    ryz_tool_request_t invalid = {.action = (ryz_tool_action_t)999};
    memset(&out, 0xa5, sizeof(out));
    assert(ryz_tools_call(session, &invalid, &out) == ESP_ERR_INVALID_ARG && !out.operation_id && !queries);
    assert(ryz_tools_call(session, NULL, &out) == ESP_ERR_INVALID_ARG);
    assert(ryz_tools_external((ryz_tool_t)99, external, NULL) == ESP_ERR_INVALID_ARG && !external_calls);
    out = call(session, (ryz_tool_request_t){.action = RYZ_TOOL_I2C_STATUS});
    assert(!out.started && out.state.i2c.config_revision == 1 && out.state.i2c.config.sda == 41);
    assert(!starts[0] && snapshot().tools[0].owner_session == session);
    ryz_tools_close_result_t close;
    assert(ryz_tools_close(session, &close) == ESP_OK && !close.remaining_mask && !stops[0]);
}
static void test_external_active(void)
{
    uint32_t native; assert(ryz_i2c_scan_start(&native) == ESP_OK);
    uint32_t session = open_session();
    ryz_tool_reply_t out;
    ryz_tool_request_t request = {.action = RYZ_TOOL_I2C_CANCEL, .operation_id = native};
    assert(ryz_tools_call(session, &request, &out) == ESP_ERR_NOT_FINISHED && !stops[0]);
    assert(!snapshot().tools[0].owner_session);
    ryz_tools_close_result_t close;
    assert(ryz_tools_close(session, &close) == ESP_OK && !stops[0]);
}
static void test_lease(void)
{
    uint32_t session = open_session();
    ryz_tool_reply_t out = call(session, (ryz_tool_request_t){.action = RYZ_TOOL_I2C_CONFIGURE,
        .expected_revision = 1, .config.i2c = {15, 16, 400000}});
    assert(out.revision == 2 && !starts[0]);
    assert(ryz_tools_external(RYZ_TOOL_I2C, external, NULL) == ESP_ERR_NOT_FINISHED && !external_calls);
    assert(ryz_tools_external(RYZ_TOOL_RGB, external, NULL) == ESP_OK && external_calls == 1);
    ryz_tool_request_t request = {.action = RYZ_TOOL_I2C_START, .expected_revision = 1};
    assert(ryz_tools_call(session, &request, &out) == ESP_ERR_INVALID_STATE && !starts[0]);
    request.expected_revision = 2; out = call(session, request);
    assert(out.operation_id == i2c.scan_id && snapshot().tools[0].operation_id == out.operation_id);
}
static void test_tokens(void)
{
    uint32_t session = open_session();
    ryz_tool_reply_t out = call(session, (ryz_tool_request_t){.action = RYZ_TOOL_I2C_START, .expected_revision = 1});
    uint32_t old = out.operation_id;
    released();
    out = call(session, (ryz_tool_request_t){.action = RYZ_TOOL_I2C_START, .expected_revision = 1});
    assert(out.operation_id != old);
    ryz_tool_request_t cancel = {.action = RYZ_TOOL_I2C_CANCEL, .operation_id = old};
    assert(ryz_tools_call(session, &cancel, &out) == ESP_ERR_INVALID_STATE && !stops[0]);
    cancel.operation_id = i2c.scan_id; call(session, cancel);
    assert(last_stopped[0] == i2c.scan_id);
}
static void test_close(void)
{
    uint32_t session = open_session();
    call(session, (ryz_tool_request_t){.action = RYZ_TOOL_I2C_START, .expected_revision = 1});
    call(session, (ryz_tool_request_t){.action = RYZ_TOOL_RGB_SET});
    call(session, (ryz_tool_request_t){.action = RYZ_TOOL_MONITOR_START, .expected_revision = 1});
    ryz_tools_close_result_t close;
    assert(ryz_tools_close(session, &close) == ESP_ERR_NOT_FINISHED && close.pending_mask == 7);
    ryz_tool_reply_t out;
    ryz_tool_request_t request = {.action = RYZ_TOOL_RGB_SET};
    assert(ryz_tools_call(session, &request, &out) == ESP_ERR_INVALID_STATE);
    assert(ryz_tools_close(session, &close) == ESP_ERR_NOT_FINISHED);
    assert(stops[0] == 1 && stops[1] == 1 && stops[2] == 1);
    released();
    assert(ryz_tools_close(session, &close) == ESP_OK && !close.remaining_mask);
}
static void test_failed(void)
{
    uint32_t session = open_session();
    call(session, (ryz_tool_request_t){.action = RYZ_TOOL_I2C_START, .expected_revision = 1});
    ryz_tools_close_result_t close;
    assert(ryz_tools_close(session, &close) == ESP_ERR_NOT_FINISHED);
    i2c.phase = RYZ_I2C_SCAN_FAILED; i2c.resources_held = true; i2c.cleanup_error = ESP_FAIL;
    assert(ryz_tools_close(session, &close) == ESP_FAIL && close.failed_mask == 1);
    assert(ryz_tools_close(session, &close) == ESP_FAIL && stops[0] == 1);
    uint32_t next = open_session();
    ryz_tool_reply_t out; ryz_tool_request_t read = {.action = RYZ_TOOL_I2C_STATUS};
    assert(ryz_tools_call(next, &read, &out) == ESP_ERR_NOT_FINISHED);
    call(next, (ryz_tool_request_t){.action = RYZ_TOOL_RGB_STATUS});
    assert(ryz_tools_retry_close(session) == ESP_OK);
    assert(ryz_tools_close(session, &close) == ESP_ERR_NOT_FINISHED && stops[0] == 2);
    released();
    assert(ryz_tools_close(session, &close) == ESP_OK && snapshot().current_session == next);
}
static void test_completion_race(unsigned mode)
{
    uint32_t session = open_session();
    call(session, (ryz_tool_request_t){.action = RYZ_TOOL_I2C_START, .expected_revision = 1});
    i2c.phase = RYZ_I2C_SCAN_RUNNING;
    i2c.resources_held = true;
    complete_before_cancel = mode;
    ryz_tools_close_result_t close;
    esp_err_t error = ryz_tools_close(session, &close);
    if (mode == 4) {
        assert(error == ESP_ERR_NOT_FINISHED && close.pending_mask == 1 && !close.failed_mask);
        query_error = ESP_OK;
        error = ryz_tools_close(session, &close);
    } else if (mode != 1) {
        assert(error == ESP_FAIL && close.failed_mask == 1);
        assert(snapshot().tools[0].owner_session == session);
        assert(ryz_tools_close(session, &close) == ESP_FAIL && stops[0] == 1);
        return;
    }
    assert(error == ESP_OK && !close.remaining_mask && !close.failed_mask);
    assert(stops[0] == 1 && !snapshot().tools[0].owner_session);
    assert(ryz_tools_close(session, &close) == ESP_OK && stops[0] == 1);
}
static void test_stale(void)
{
    uint32_t old = open_session();
    call(old, (ryz_tool_request_t){.action = RYZ_TOOL_RGB_STATUS});
    ryz_tools_close_result_t close; assert(ryz_tools_close(old, &close) == ESP_OK);
    uint32_t next = open_session();
    call(next, (ryz_tool_request_t){.action = RYZ_TOOL_RGB_SET});
    assert(ryz_tools_close(old, &close) == ESP_OK && !stops[1]);
    assert(snapshot().tools[1].owner_session == next && snapshot().current_session == next);
}
static void test_monitor_config(void)
{
    initialized[2] = true; monitor.session_id = 66; monitor.config_revision = 4;
    monitor.config = RYZ_MONITOR_DEFAULT_CONFIG; monitor.phase = RYZ_MONITOR_STOPPED;
    uint32_t session = open_session();
    call(session, (ryz_tool_request_t){.action = RYZ_TOOL_MONITOR_CONFIGURE, .expected_revision = 4,
        .config.monitor = {RYZ_MONITOR_UART, 15, 16, 115200}});
    assert(!snapshot().tools[2].operation_id);
    ryz_tools_close_result_t close;
    assert(ryz_tools_close(session, &close) == ESP_ERR_NOT_FINISHED && !stops[2]);
    monitor.operation_pending = false; monitor.phase = RYZ_MONITOR_STOPPED;
    assert(ryz_tools_close(session, &close) == ESP_OK && !stops[2]);
}
static void test_monitor_tokens(void)
{
    uint32_t session = open_session();
    ryz_tool_reply_t out = call(session, (ryz_tool_request_t){.action = RYZ_TOOL_MONITOR_START, .expected_revision = 1});
    uint32_t receive = out.operation_id;
    monitor.operation_pending = false; monitor.source_active = true; monitor.phase = RYZ_MONITOR_RUNNING;
    out = call(session, (ryz_tool_request_t){.action = RYZ_TOOL_MONITOR_CONFIGURE, .expected_revision = 1,
        .config.monitor = {RYZ_MONITOR_UART, 15, 16, 115200}});
    uint32_t configuration = out.operation_id;
    assert(configuration != receive && snapshot().tools[2].operation_id == receive);
    ryz_tool_request_t stop = {.action = RYZ_TOOL_MONITOR_STOP, .operation_id = configuration};
    assert(ryz_tools_call(session, &stop, &out) == ESP_ERR_INVALID_STATE && !stops[2]);
    stop.operation_id = receive; call(session, stop); assert(last_stopped[2] == receive);
}
static void test_rgb_parked(void)
{
    initialized[1] = true; rgb.request_id = 900; rgb.phase = RYZ_RGB_COMPLETED;
    rgb.resources_held = rgb.output_known = true;
    uint32_t session = open_session();
    call(session, (ryz_tool_request_t){.action = RYZ_TOOL_RGB_STATUS});
    ryz_tools_close_result_t close;
    assert(ryz_tools_close(session, &close) == ESP_OK && !stops[1] && rgb.resources_held);
    session = open_session();
    ryz_tool_reply_t out = call(session, (ryz_tool_request_t){.action = RYZ_TOOL_RGB_SET});
    assert(out.operation_id != 900);
    assert(ryz_tools_close(session, &close) == ESP_ERR_NOT_FINISHED && last_stopped[1] == out.operation_id);
}
static void test_identity_fault(void)
{
    uint32_t session = open_session();
    call(session, (ryz_tool_request_t){.action = RYZ_TOOL_I2C_START, .expected_revision = 1});
    ++i2c.scan_id; /* Synthetic unsupported raw-writer fault: never adopt it. */
    ryz_tools_close_result_t close;
    assert(ryz_tools_close(session, &close) == ESP_FAIL && close.failed_mask == 1 && !stops[0]);
}
static void test_errors(void)
{
    uint32_t session = open_session();
    query_error = ESP_ERR_NOT_FINISHED;
    ryz_tool_reply_t out; ryz_tool_request_t request = {.action = RYZ_TOOL_RGB_SET};
    assert(ryz_tools_call(session, &request, &out) == ESP_ERR_NOT_FINISHED && !out.operation_id);
    assert(!snapshot().tools[1].owner_session);
    query_error = ESP_OK; mutation_error = ESP_ERR_NO_MEM;
    assert(ryz_tools_call(session, &request, &out) == ESP_ERR_NO_MEM && !out.operation_id && !starts[1]);
    ryz_tools_close_result_t close; assert(ryz_tools_close(session, &close) == ESP_OK && !stops[1]);
    assert(ryz_tools_external(RYZ_TOOL_RGB, external, NULL) == ESP_ERR_NO_MEM);
    mutation_error = ESP_OK;
    assert(ryz_tools_external(RYZ_TOOL_RGB, external, NULL) == ESP_OK && external_calls == 2);
}
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool entered, released_gate;
static esp_err_t delayed_admit(void *unused)
{
    (void)unused;
    pthread_mutex_lock(&mutex); entered = true; pthread_cond_broadcast(&changed);
    while (!released_gate) pthread_cond_wait(&changed, &mutex);
    pthread_mutex_unlock(&mutex);
    uint32_t native; return ryz_i2c_scan_start(&native);
}
static void *external_thread(void *unused)
{
    (void)unused;
    assert(ryz_tools_external(RYZ_TOOL_I2C, delayed_admit, NULL) == ESP_OK);
    return NULL;
}
static void test_concurrency(void)
{
    uint32_t session = open_session(); pthread_t thread;
    assert(!pthread_create(&thread, NULL, external_thread, NULL));
    pthread_mutex_lock(&mutex);
    while (!entered) pthread_cond_wait(&changed, &mutex);
    pthread_mutex_unlock(&mutex);
    ryz_tool_reply_t out; ryz_tool_request_t request = {.action = RYZ_TOOL_I2C_STATUS};
    for (unsigned i = 0; i < 100; ++i)
        assert(ryz_tools_call(session, &request, &out) == ESP_ERR_NOT_FINISHED && !out.started);
    pthread_mutex_lock(&mutex); released_gate = true; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&mutex);
    assert(!pthread_join(thread, NULL));
    assert(ryz_tools_call(session, &request, &out) == ESP_ERR_NOT_FINISHED && !snapshot().tools[0].owner_session);
    assert(starts[0] == 1 && !stops[0]);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "lazy")) test_lazy();
    else if (!strcmp(argv[1], "external_active")) test_external_active();
    else if (!strcmp(argv[1], "lease")) test_lease();
    else if (!strcmp(argv[1], "tokens")) test_tokens();
    else if (!strcmp(argv[1], "close")) test_close();
    else if (!strcmp(argv[1], "failed")) test_failed();
    else if (!strcmp(argv[1], "completion_race")) test_completion_race(1);
    else if (!strcmp(argv[1], "race_held")) test_completion_race(2);
    else if (!strcmp(argv[1], "race_replaced")) test_completion_race(3);
    else if (!strcmp(argv[1], "race_busy")) test_completion_race(4);
    else if (!strcmp(argv[1], "race_unknown")) test_completion_race(5);
    else if (!strcmp(argv[1], "stale")) test_stale();
    else if (!strcmp(argv[1], "monitor_config")) test_monitor_config();
    else if (!strcmp(argv[1], "monitor_tokens")) test_monitor_tokens();
    else if (!strcmp(argv[1], "rgb_parked")) test_rgb_parked();
    else if (!strcmp(argv[1], "identity_fault")) test_identity_fault();
    else if (!strcmp(argv[1], "errors")) test_errors();
    else if (!strcmp(argv[1], "concurrency")) test_concurrency();
    else assert(!"unknown case");
    puts("TOOLS_PASS"); return 0;
}
