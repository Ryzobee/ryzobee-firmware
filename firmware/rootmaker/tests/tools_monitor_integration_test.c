/* Real broker + real Monitor worker/stream. Reuse the existing controlled
 * pthread/OS/UART/logger boundary; never substitute the Monitor public API. */
#define main monitor_fixture_main
#include "monitor_test.c"
#undef main
#include "ryz_tools.h"

/* Other tools are outside this integration: any accidental call is a failure. */
esp_err_t ryz_i2c_scan_validate_config(const ryz_i2c_scan_config_t *value)
{ (void)value; abort(); }
esp_err_t ryz_i2c_scan_configure(const ryz_i2c_scan_config_t *value, uint32_t revision, uint32_t *out)
{ (void)value; (void)revision; (void)out; abort(); }
esp_err_t ryz_i2c_scan_start(uint32_t *out) { (void)out; abort(); }
esp_err_t ryz_i2c_scan_cancel(uint32_t id) { (void)id; abort(); }
esp_err_t ryz_i2c_scan_get_snapshot(ryz_i2c_scan_snapshot_t *out) { (void)out; abort(); }
esp_err_t ryz_rgb_submit(ryz_rgb_color_t color, uint32_t *out) { (void)color; (void)out; abort(); }
esp_err_t ryz_rgb_cleanup(uint32_t id) { (void)id; abort(); }
esp_err_t ryz_rgb_get_snapshot(ryz_rgb_snapshot_t *out) { (void)out; abort(); }
esp_err_t ryz_rgb_lease_open(uint32_t *out) { (void)out; abort(); }
esp_err_t ryz_rgb_lease_submit(uint32_t lease, ryz_rgb_color_t color, uint32_t *out)
{ (void)lease; (void)color; (void)out; abort(); }
esp_err_t ryz_rgb_lease_close(uint32_t lease) { (void)lease; abort(); }
esp_err_t ryz_rgb_lease_retry_close(uint32_t lease) { (void)lease; abort(); }

enum { MONITOR_BIT = 1U << RYZ_TOOL_MONITOR };
static unsigned external_calls;
typedef struct { uint32_t revision, session, operation; } external_t;
static esp_err_t external_start(void *context)
{
    external_t *args = context;
    ++external_calls;
    return ryz_monitor_start(args->revision, &args->session);
}
static esp_err_t external_stop(void *context)
{
    external_t *args = context;
    ++external_calls;
    return ryz_monitor_stop(args->session, &args->operation);
}
static uint32_t open_tools(void)
{
    uint32_t session;
    assert(ryz_tools_open(&session) == ESP_OK && session);
    return session;
}
static ryz_tool_reply_t tool_call(uint32_t session, ryz_tool_request_t request)
{
    ryz_tool_reply_t out;
    assert(ryz_tools_call(session, &request, &out) == ESP_OK);
    return out;
}
static void assert_rejected(uint32_t session, ryz_tool_request_t request, esp_err_t error)
{
    ryz_tool_reply_t out, zero = {0};
    memset(&out, 0xa5, sizeof(out));
    assert(ryz_tools_call(session, &request, &out) == error);
    assert(!memcmp(&out, &zero, sizeof(out)));
}
static ryz_tools_snapshot_t ownership(void)
{
    ryz_tools_snapshot_t out;
    assert(ryz_tools_get_snapshot(&out) == ESP_OK);
    return out;
}
static void close_pending(uint32_t session)
{
    ryz_tools_close_result_t out;
    assert(ryz_tools_close(session, &out) == ESP_ERR_NOT_FINISHED);
    assert(out.session_id == session && out.remaining_mask == MONITOR_BIT);
    assert(out.pending_mask == MONITOR_BIT && !out.failed_mask);
    assert(ownership().tools[RYZ_TOOL_MONITOR].owner_session == session);
}
static void close_done(uint32_t session)
{
    ryz_tools_close_result_t out;
    assert(ryz_tools_close(session, &out) == ESP_OK);
    assert(out.session_id == session && !out.remaining_mask && !out.pending_mask && !out.failed_mask);
}
static uint32_t start_tool_uart(uint32_t owner_session)
{
    ryz_tool_reply_t out = tool_call(owner_session, (ryz_tool_request_t){
        .action = RYZ_TOOL_MONITOR_CONFIGURE, .expected_revision = 1, .config.monitor = uart_a,
    });
    assert(out.operation_id == 1 && !begins && !log_installs);
    drive();
    out = tool_call(owner_session, (ryz_tool_request_t){
        .action = RYZ_TOOL_MONITOR_START, .expected_revision = 2,
    });
    assert(out.operation_id == 2);
    drive();
    ryz_monitor_snapshot_t s = snapshot();
    assert(s.session_id == out.operation_id && s.phase == RYZ_MONITOR_RUNNING);
    assert(s.source_active && s.resources_held && s.stream.accepting && begins == 1);
    assert(ownership().tools[RYZ_TOOL_MONITOR].operation_id == out.operation_id);
    return out.operation_id;
}

static void claim_start_stream(void)
{
    uint32_t session = open_tools();
    assert(!created && !begins && !log_installs);
    ryz_tool_reply_t out = tool_call(session, (ryz_tool_request_t){.action = RYZ_TOOL_MONITOR_STATUS});
    assert(!out.started && out.state.monitor.config_revision == 1);
    assert(out.state.monitor.config.source == RYZ_MONITOR_SYSTEM && !created);
    external_t external = {.revision = 1};
    assert(ryz_tools_external(RYZ_TOOL_MONITOR, external_start, &external) == ESP_ERR_NOT_FINISHED);
    assert(!external_calls && ownership().tools[RYZ_TOOL_MONITOR].owner_session == session);
    out = tool_call(session, (ryz_tool_request_t){.action = RYZ_TOOL_MONITOR_START, .expected_revision = 1});
    uint32_t capture_session = out.operation_id;
    assert(capture_session == 1 && !log_installs);
    drive();
    out = tool_call(session, (ryz_tool_request_t){.action = RYZ_TOOL_MONITOR_STATUS});
    assert(out.started && out.state.monitor.phase == RYZ_MONITOR_RUNNING);
    assert(out.state.monitor.session_id == capture_session && out.state.monitor.stream.accepting);
    const uint8_t bytes[] = {'r', 0, 'g', 0xff};
    emit_log(bytes, sizeof(bytes));
    out = tool_call(session, (ryz_tool_request_t){.action = RYZ_TOOL_MONITOR_PAUSE,
        .operation_id = capture_session, .view_generation = 1, .paused = true});
    assert(out.view_generation == 2);
    emit_log("live", 4);
    out = tool_call(session, (ryz_tool_request_t){.action = RYZ_TOOL_MONITOR_READ,
        .operation_id = capture_session, .view_generation = 2});
    assert(out.state.page.count == 1 && out.state.page.records[0].length == sizeof(bytes));
    assert(!memcmp(out.state.page.records[0].bytes, bytes, sizeof(bytes)));
    assert(out.state.page.stream.paused && out.state.page.stream.chunks == 2);
    ryz_monitor_capture_token_t old;
    assert(ryz_monitor_stream_token(&old) == ESP_OK);
    out = tool_call(session, (ryz_tool_request_t){.action = RYZ_TOOL_MONITOR_CLEAR,
        .operation_id = capture_session, .view_generation = 2});
    assert(out.view_generation == 3);
    assert(ryz_monitor_stream_append(old, "late", 4, 4, 160) == ESP_ERR_INVALID_STATE);
    out = tool_call(session, (ryz_tool_request_t){.action = RYZ_TOOL_MONITOR_READ,
        .operation_id = capture_session, .view_generation = 3});
    assert(!out.state.page.count && out.state.page.stream.paused);
    close_pending(session);
    assert_rejected(session, (ryz_tool_request_t){.action = RYZ_TOOL_MONITOR_STATUS}, ESP_ERR_INVALID_STATE);
    assert(ryz_tools_external(RYZ_TOOL_MONITOR, external_start, &external) == ESP_ERR_NOT_FINISHED);
    drive();
    assert(snapshot().phase == RYZ_MONITOR_STOPPED && !capture);
    close_done(session);
    assert(ryz_tools_external(RYZ_TOOL_MONITOR, external_start, &external) == ESP_OK && external_calls == 1);
    drive();
    assert(snapshot().phase == RYZ_MONITOR_RUNNING && external.session > capture_session);
    close_done(session); /* Old application cannot stop the external capture. */
    assert(snapshot().session_id == external.session && snapshot().source_active && capture);
}

static void close_reconfigure(unsigned blocked_begin)
{
    uint32_t session = open_tools();
    uint32_t receive = start_tool_uart(session);
    unsigned previous_polls = polls;
    block_begin = blocked_begin;
    if (blocked_begin == 3) begin_errors[1] = ESP_ERR_TIMEOUT;
    ryz_tool_reply_t out = tool_call(session, (ryz_tool_request_t){
        .action = RYZ_TOOL_MONITOR_CONFIGURE, .expected_revision = 2, .config.monitor = uart_b,
    });
    assert(out.operation_id > receive);
    kick();
    assert(!pthread_mutex_lock(&gate));
    while (begin_entered != blocked_begin) wait_changed();
    assert(!pthread_mutex_unlock(&gate));
    close_pending(session);
    ryz_monitor_snapshot_t s = snapshot();
    assert(s.phase == RYZ_MONITOR_STOPPING && s.operation_id > out.operation_id);
    assert(s.session_id == receive && s.config_revision == 2 && !s.stream.accepting);
    external_t external = {.revision = 2};
    for (unsigned i = 0; i < 50; ++i) {
        close_pending(session);
        assert(ryz_tools_external(RYZ_TOOL_MONITOR, external_start, &external) == ESP_ERR_NOT_FINISHED);
    }
    assert(!external_calls && !snapshot().source_active);
    assert(!pthread_mutex_lock(&gate)); release_begin = true;
    assert(!pthread_cond_broadcast(&changed)); assert(!pthread_mutex_unlock(&gate));
    drive(); s = snapshot();
    assert(s.phase == RYZ_MONITOR_STOPPED && !s.operation_pending && !s.resources_held && !held);
    assert(!s.source_active && !s.stream.accepting && s.config_revision == 2 && s.config.rx == 2);
    assert(begins == blocked_begin && ends == blocked_begin && polls == previous_polls);
    close_done(session);
    assert(!ownership().tools[RYZ_TOOL_MONITOR].owner_session);
}

static void config_only_history(void)
{
    external_t external = {.revision = 1};
    assert(ryz_tools_external(RYZ_TOOL_MONITOR, external_start, &external) == ESP_OK);
    drive(); emit_log("historical", 10);
    assert(ryz_tools_external(RYZ_TOOL_MONITOR, external_stop, &external) == ESP_OK);
    drive();
    uint32_t session = open_tools();
    /* An idle worker may consume the configure notification immediately.
     * Hold only the test adapter's wake delivery so close_pending actually
     * observes the intended in-flight state; drive() releases this gate. */
    assert(!pthread_mutex_lock(&gate)); defer_wake = true;
    assert(!pthread_mutex_unlock(&gate));
    ryz_tool_reply_t out = tool_call(session, (ryz_tool_request_t){
        .action = RYZ_TOOL_MONITOR_CONFIGURE, .expected_revision = 1, .config.monitor = uart_a,
    });
    uint32_t configure_id = out.operation_id;
    assert(configure_id > external.operation && !ownership().tools[RYZ_TOOL_MONITOR].operation_id);
    close_pending(session);
    ryz_monitor_snapshot_t s = snapshot();
    assert(s.operation == RYZ_MONITOR_OP_CONFIGURE && s.operation_id == configure_id);
    assert(s.session_id == external.session && !s.resources_held && !s.source_active);
    assert(!ownership().tools[RYZ_TOOL_MONITOR].cleanup_requested);
    drive(); s = snapshot();
    assert(s.phase == RYZ_MONITOR_STOPPED && !s.operation_pending && s.operation_id == configure_id);
    assert(s.config_revision == 2 && s.config.rx == 2 && s.session_id == external.session);
    assert(s.capture_config.source == RYZ_MONITOR_SYSTEM && s.stream.chunks == 1);
    assert(!begins && !ends && !polls && log_installs == 1 && external_calls == 2);
    close_done(session);
    assert(!ownership().tools[RYZ_TOOL_MONITOR].owner_session);
}

static void cleanup_failure_isolation(void)
{
    uint32_t old = open_tools(), receive = start_tool_uart(old);
    end_error = ESP_FAIL;
    close_pending(old);
    drive();
    ryz_monitor_snapshot_t s = snapshot();
    assert(s.phase == RYZ_MONITOR_FAILED && s.cleanup_error == ESP_FAIL && s.resources_held);
    assert(!s.source_active && !s.stream.accepting && held && ends == 1);
    ryz_tools_close_result_t close;
    assert(ryz_tools_close(old, &close) == ESP_FAIL);
    assert(close.session_id == old && close.remaining_mask == MONITOR_BIT && close.failed_mask == MONITOR_BIT);
    assert(!close.pending_mask && close.errors[RYZ_TOOL_MONITOR] == ESP_FAIL);
    uint32_t next = open_tools();
    assert(next > old && ownership().current_session == next);
    assert_rejected(next, (ryz_tool_request_t){.action = RYZ_TOOL_MONITOR_STATUS}, ESP_ERR_NOT_FINISHED);
    assert_rejected(next, (ryz_tool_request_t){.action = RYZ_TOOL_MONITOR_START, .expected_revision = 2}, ESP_ERR_NOT_FINISHED);
    external_t external = {.revision = 2};
    assert(ryz_tools_external(RYZ_TOOL_MONITOR, external_start, &external) == ESP_ERR_NOT_FINISHED);
    for (unsigned i = 0; i < 50; ++i) assert(ryz_tools_close(old, &close) == ESP_FAIL);
    assert(ends == 1 && begins == 1 && !external_calls);
    unsigned previous_polls = polls;
    end_error = ESP_OK;
    assert(ryz_tools_retry_close(old) == ESP_OK && ends == 1);
    close_pending(old);
    assert(snapshot().session_id == receive && snapshot().phase == RYZ_MONITOR_STOPPING);
    drive(); close_done(old);
    assert(ends == 2 && begins == 1 && polls == previous_polls && !held);
    assert(ownership().current_session == next && !ownership().tools[RYZ_TOOL_MONITOR].owner_session);
    ryz_tool_reply_t out = tool_call(next, (ryz_tool_request_t){
        .action = RYZ_TOOL_MONITOR_START, .expected_revision = 2,
    });
    uint32_t new_receive = out.operation_id;
    assert(new_receive > receive);
    drive();
    close_done(old);
    assert(ryz_tools_retry_close(old) == ESP_OK);
    s = snapshot();
    assert(s.session_id == new_receive && s.phase == RYZ_MONITOR_RUNNING && s.source_active);
    assert(begins == 2 && ends == 2 && ownership().tools[RYZ_TOOL_MONITOR].owner_session == next);
    close_pending(next); drive(); close_done(next);
    assert(ends == 3 && !held);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "claim_start_stream")) claim_start_stream();
    else if (!strcmp(argv[1], "close_candidate")) close_reconfigure(2);
    else if (!strcmp(argv[1], "close_restore")) close_reconfigure(3);
    else if (!strcmp(argv[1], "config_only_history")) config_only_history();
    else if (!strcmp(argv[1], "cleanup_failure_isolation")) cleanup_failure_isolation();
    else abort();
    stop_worker();
    printf("TOOLS_MONITOR_PASS %s\n", argv[1]);
    return 0;
}
