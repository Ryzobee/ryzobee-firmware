/* Real broker + real scan worker; reuse the existing controlled OS/bus
 * Adapter. Public scan commands, cancellation and status are never mocked. */
#define main i2c_scan_fixture_main
#include "i2c_scan_test.c"
#undef main
#include "ryz_tools.h"

/* This test-only public-call scheduling gate does not synthesize a native
 * result. The scanner object is compiled with only its cancel symbol renamed;
 * all state transitions and the cancellation result remain the real core's. */
esp_err_t ryz_i2c_scan_cancel_real(uint32_t id);
static bool finish_before_cancel;
static unsigned natural_completion_windows;
static esp_err_t observed_cancel_result;
esp_err_t ryz_i2c_scan_cancel(uint32_t id)
{
    if (finish_before_cancel) {
        finish_before_cancel = false;
        ryz_i2c_scan_snapshot_t s;
        assert(ryz_i2c_scan_get_snapshot(&s) == ESP_OK);
        assert(s.scan_id == id && s.phase == RYZ_I2C_SCAN_RUNNING && s.completed_addresses == 1);
        run_to_idle();
        assert(ryz_i2c_scan_get_snapshot(&s) == ESP_OK);
        assert(s.scan_id == id && s.phase == RYZ_I2C_SCAN_COMPLETED && !s.resources_held);
        assert(s.completed_addresses == 112 && s.nack_count == 112 && probes == 112 && bus_ends == 1);
        ++natural_completion_windows;
    }
    observed_cancel_result = ryz_i2c_scan_cancel_real(id);
    return observed_cancel_result;
}

/* Other tool cores are outside this integration; accidental calls fail. */
esp_err_t ryz_rgb_submit(ryz_rgb_color_t color, uint32_t *out) { (void)color; (void)out; abort(); }
esp_err_t ryz_rgb_cleanup(uint32_t id) { (void)id; abort(); }
esp_err_t ryz_rgb_get_snapshot(ryz_rgb_snapshot_t *out) { (void)out; abort(); }
esp_err_t ryz_rgb_lease_open(uint32_t *out) { (void)out; abort(); }
esp_err_t ryz_rgb_lease_submit(uint32_t lease, ryz_rgb_color_t color, uint32_t *out)
{ (void)lease; (void)color; (void)out; abort(); }
esp_err_t ryz_rgb_lease_close(uint32_t lease) { (void)lease; abort(); }
esp_err_t ryz_rgb_lease_retry_close(uint32_t lease) { (void)lease; abort(); }
esp_err_t ryz_monitor_validate_config(const ryz_monitor_config_t *value) { (void)value; abort(); }
esp_err_t ryz_monitor_start(uint32_t revision, uint32_t *out) { (void)revision; (void)out; abort(); }
esp_err_t ryz_monitor_configure(const ryz_monitor_config_t *value, uint32_t revision,
                              uint32_t session, uint32_t *out)
{ (void)value; (void)revision; (void)session; (void)out; abort(); }
esp_err_t ryz_monitor_stop(uint32_t session, uint32_t *out) { (void)session; (void)out; abort(); }
esp_err_t ryz_monitor_set_paused(uint32_t session, uint32_t generation, bool paused, uint32_t *out)
{ (void)session; (void)generation; (void)paused; (void)out; abort(); }
esp_err_t ryz_monitor_clear(uint32_t session, uint32_t generation, uint32_t *out)
{ (void)session; (void)generation; (void)out; abort(); }
esp_err_t ryz_monitor_get_snapshot(ryz_monitor_snapshot_t *out) { (void)out; abort(); }
esp_err_t ryz_monitor_read(uint32_t session, uint32_t generation, uint32_t after, ryz_monitor_page_t *out)
{ (void)session; (void)generation; (void)after; (void)out; abort(); }

enum { I2C_BIT = 1U << RYZ_TOOL_I2C };
static const ryz_i2c_scan_config_t custom_pair = {2, 3, 400000};
static unsigned external_calls;
static esp_err_t external_start(void *context)
{ ++external_calls; return ryz_i2c_scan_start(context); }
static esp_err_t external_cancel(void *context)
{ ++external_calls; return ryz_i2c_scan_cancel(*(uint32_t *)context); }
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
static ryz_i2c_scan_snapshot_t scan_snapshot(void)
{
    ryz_i2c_scan_snapshot_t out;
    assert(ryz_i2c_scan_get_snapshot(&out) == ESP_OK);
    return out;
}
static void close_pending(uint32_t session)
{
    ryz_tools_close_result_t out;
    assert(ryz_tools_close(session, &out) == ESP_ERR_NOT_FINISHED);
    assert(out.session_id == session && out.remaining_mask == I2C_BIT);
    assert(out.pending_mask == I2C_BIT && !out.failed_mask);
    assert(ownership().tools[RYZ_TOOL_I2C].owner_session == session);
}
static void close_done(uint32_t session)
{
    ryz_tools_close_result_t out;
    assert(ryz_tools_close(session, &out) == ESP_OK);
    assert(out.session_id == session && !out.remaining_mask && !out.pending_mask && !out.failed_mask);
}
static uint32_t configure_start(uint32_t session)
{
    ryz_tool_reply_t out = tool_call(session, (ryz_tool_request_t){
        .action = RYZ_TOOL_I2C_CONFIGURE, .expected_revision = 1, .config.i2c = custom_pair,
    });
    assert(out.revision == 2 && !board_inits && !probes && !bus_ends);
    assert_rejected(session, (ryz_tool_request_t){.action = RYZ_TOOL_I2C_START,
        .expected_revision = 1}, ESP_ERR_INVALID_STATE);
    out = tool_call(session, (ryz_tool_request_t){.action = RYZ_TOOL_I2C_START, .expected_revision = 2});
    assert(out.operation_id == 1);
    ryz_i2c_scan_snapshot_t s = scan_snapshot();
    assert(s.phase == RYZ_I2C_SCAN_QUEUED && s.scan_id == out.operation_id);
    assert(s.scan_config.sda == 2 && s.scan_config.scl == 3 && s.scan_config.hz == 400000);
    assert(s.config_revision == 2 && s.scan_config_revision == 2);
    assert(ownership().tools[RYZ_TOOL_I2C].operation_id == out.operation_id);
    return out.operation_id;
}
static void await_probe(void)
{
    assert(pthread_mutex_lock(&gate) == 0);
    released = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    while (!probe_entered) timed_wait();
    assert(pthread_mutex_unlock(&gate) == 0);
}
static void unblock_probe(void)
{
    assert(pthread_mutex_lock(&gate) == 0);
    release_probe = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    assert(pthread_mutex_unlock(&gate) == 0);
}

static void claim_scan_release(void)
{
    nack_fixture(); block_probe = block_end = true;
    uint32_t session = open_tools();
    ryz_tool_reply_t out = tool_call(session, (ryz_tool_request_t){.action = RYZ_TOOL_I2C_STATUS});
    assert(!out.started && out.state.i2c.config_revision == 1 && !created);
    assert(out.state.i2c.config.sda == 41 && out.state.i2c.config.scl == 40);
    uint32_t external_id = 0;
    assert(ryz_tools_external(RYZ_TOOL_I2C, external_start, &external_id) == ESP_ERR_NOT_FINISHED);
    assert(!external_calls && !created);
    uint32_t scan = configure_start(session);
    assert_rejected(session, (ryz_tool_request_t){.action = RYZ_TOOL_I2C_CANCEL,
        .operation_id = scan + 1}, ESP_ERR_INVALID_STATE);
    await_probe();
    out = tool_call(session, (ryz_tool_request_t){.action = RYZ_TOOL_I2C_STATUS});
    assert(out.state.i2c.phase == RYZ_I2C_SCAN_RUNNING && out.state.i2c.resources_held);
    assert(out.state.i2c.probe_calls == 1 && !out.state.i2c.completed_addresses);
    close_pending(session);
    assert(scan_snapshot().phase == RYZ_I2C_SCAN_CANCELLING);
    assert_rejected(session, (ryz_tool_request_t){.action = RYZ_TOOL_I2C_STATUS}, ESP_ERR_INVALID_STATE);
    for (unsigned i = 0; i < 50; ++i) close_pending(session);
    assert(probes == 1 && !bus_ends && bus_held);
    unblock_probe();
    assert(pthread_mutex_lock(&gate) == 0);
    while (!end_entered) timed_wait();
    assert(pthread_mutex_unlock(&gate) == 0);
    for (unsigned i = 0; i < 50; ++i) {
        ryz_i2c_scan_snapshot_t s = scan_snapshot();
        assert(s.phase == RYZ_I2C_SCAN_RELEASING && s.scan_id == scan && s.resources_held);
        assert(!s.current_address && !s.finished_ms && !s.completed_addresses);
        close_pending(session);
        assert(ryz_tools_external(RYZ_TOOL_I2C, external_start, &external_id) == ESP_ERR_NOT_FINISHED);
    }
    assert(!external_calls && probes == 1 && board_inits == 1 && bus_ends == 1);
    assert(pthread_mutex_lock(&gate) == 0); release_end = true;
    assert(pthread_cond_broadcast(&changed) == 0); assert(pthread_mutex_unlock(&gate) == 0);
    run_to_idle();
    ryz_i2c_scan_snapshot_t s = scan_snapshot();
    assert(s.phase == RYZ_I2C_SCAN_CANCELLED && !s.resources_held && !bus_held);
    assert(s.cleanup_error == ESP_OK && s.probe_calls == 1 && !s.completed_addresses);
    assert(!s.nack_count && s.results[8] == RYZ_I2C_SCAN_UNSCANNED);
    close_done(session);
    assert(!ownership().tools[RYZ_TOOL_I2C].owner_session);
}

static void cleanup_failed_retry(void)
{
    nack_fixture(); block_probe = true; end_error = ESP_FAIL;
    uint32_t old = open_tools(), scan = configure_start(old);
    await_probe(); close_pending(old); unblock_probe(); run_to_idle();
    ryz_i2c_scan_snapshot_t s = scan_snapshot();
    assert(s.phase == RYZ_I2C_SCAN_FAILED && s.cleanup_error == ESP_FAIL && s.resources_held);
    assert(s.scan_id == scan && bus_held && probes == 1 && board_inits == 1 && bus_ends == 1);
    ryz_tools_close_result_t close;
    assert(ryz_tools_close(old, &close) == ESP_FAIL);
    assert(close.session_id == old && close.remaining_mask == I2C_BIT && close.failed_mask == I2C_BIT);
    assert(!close.pending_mask && close.errors[RYZ_TOOL_I2C] == ESP_FAIL);
    uint32_t next = open_tools();
    assert(next > old);
    assert_rejected(next, (ryz_tool_request_t){.action = RYZ_TOOL_I2C_STATUS}, ESP_ERR_NOT_FINISHED);
    assert_rejected(next, (ryz_tool_request_t){.action = RYZ_TOOL_I2C_START,
        .expected_revision = 2}, ESP_ERR_NOT_FINISHED);
    uint32_t external_id = 0;
    assert(ryz_tools_external(RYZ_TOOL_I2C, external_start, &external_id) == ESP_ERR_NOT_FINISHED);
    for (unsigned i = 0; i < 50; ++i) assert(ryz_tools_close(old, &close) == ESP_FAIL);
    assert(bus_ends == 1 && !external_calls);
    end_error = ESP_OK;
    assert(ryz_tools_retry_close(old) == ESP_OK && bus_ends == 1);
    close_pending(old); run_to_idle(); close_done(old);
    s = scan_snapshot();
    assert(s.phase == RYZ_I2C_SCAN_CANCELLED && s.scan_id == scan && !s.resources_held && !bus_held);
    assert(bus_ends == 2 && board_inits == 1 && probes == 1); /* cleanup-only, no re-scan */
    pause_worker(); block_probe = false;
    ryz_tool_reply_t out = tool_call(next, (ryz_tool_request_t){.action = RYZ_TOOL_I2C_START,
        .expected_revision = 2});
    uint32_t next_scan = out.operation_id;
    assert(next_scan == scan + 1);
    close_done(old);
    assert(ryz_tools_retry_close(old) == ESP_OK);
    assert(scan_snapshot().phase == RYZ_I2C_SCAN_QUEUED && scan_snapshot().scan_id == next_scan);
    assert(ownership().current_session == next && ownership().tools[RYZ_TOOL_I2C].owner_session == next);
    run_to_idle();
    s = scan_snapshot();
    assert(s.phase == RYZ_I2C_SCAN_COMPLETED && s.completed_addresses == 112 && s.nack_count == 112);
    assert(s.scan_id == next_scan && probes == 113 && board_inits == 2 && bus_ends == 3);
    close_done(next);
}

static void config_only_history(void)
{
    nack_fixture(); probe_results[0x15][0] = ESP_OK;
    uint32_t external_id;
    assert(ryz_tools_external(RYZ_TOOL_I2C, external_start, &external_id) == ESP_OK);
    run_to_idle();
    ryz_i2c_scan_snapshot_t before = scan_snapshot();
    assert(before.phase == RYZ_I2C_SCAN_COMPLETED && before.ack_count == 1 && before.nack_count == 111);
    assert(probes == 112 && board_inits == 1 && bus_ends == 1);
    uint32_t session = open_tools();
    ryz_tool_reply_t out = tool_call(session, (ryz_tool_request_t){.action = RYZ_TOOL_I2C_CONFIGURE,
        .expected_revision = 1, .config.i2c = custom_pair});
    assert(out.revision == 2 && !ownership().tools[RYZ_TOOL_I2C].operation_id);
    ryz_i2c_scan_snapshot_t after = scan_snapshot();
    assert(after.config.sda == 2 && after.config_revision == 2 && after.scan_id == external_id);
    assert(after.scan_config.sda == 41 && after.scan_config_revision == 1);
    assert(!memcmp(after.results, before.results, sizeof(after.results)));
    close_done(session);
    assert(scan_snapshot().phase == RYZ_I2C_SCAN_COMPLETED && probes == 112 && bus_ends == 1);
    assert(!ownership().tools[RYZ_TOOL_I2C].owner_session);
    pause_worker(); block_probe = true; probe_entered = release_probe = false;
    assert(ryz_tools_external(RYZ_TOOL_I2C, external_start, &external_id) == ESP_OK);
    await_probe();
    assert(scan_snapshot().scan_id == 2 && scan_snapshot().phase == RYZ_I2C_SCAN_RUNNING);
    close_done(session);
    assert(scan_snapshot().phase == RYZ_I2C_SCAN_RUNNING && bus_ends == 1);
    assert(ryz_tools_external(RYZ_TOOL_I2C, external_cancel, &external_id) == ESP_OK);
    unblock_probe(); run_to_idle();
    assert(scan_snapshot().phase == RYZ_I2C_SCAN_CANCELLED && bus_ends == 2);
    assert(external_calls == 3);
}

static void queued_close(void)
{
    uint32_t session = open_tools(), scan = configure_start(session);
    close_pending(session);
    assert(scan_snapshot().phase == RYZ_I2C_SCAN_CANCELLING && !probes && !board_inits);
    run_to_idle();
    ryz_i2c_scan_snapshot_t s = scan_snapshot();
    assert(s.scan_id == scan && s.phase == RYZ_I2C_SCAN_CANCELLED && !s.resources_held);
    assert(!s.probe_calls && !s.completed_addresses && !s.nack_count && !board_inits && bus_ends == 1);
    close_done(session);
}

static void active_external_cannot_be_adopted(void)
{
    block_probe = true;
    uint32_t external_id;
    assert(ryz_tools_external(RYZ_TOOL_I2C, external_start, &external_id) == ESP_OK);
    await_probe();
    uint32_t session = open_tools();
    assert_rejected(session, (ryz_tool_request_t){.action = RYZ_TOOL_I2C_STATUS}, ESP_ERR_NOT_FINISHED);
    assert_rejected(session, (ryz_tool_request_t){.action = RYZ_TOOL_I2C_CANCEL,
        .operation_id = external_id}, ESP_ERR_NOT_FINISHED);
    assert(!ownership().tools[RYZ_TOOL_I2C].owner_session);
    close_done(session);
    assert(scan_snapshot().phase == RYZ_I2C_SCAN_RUNNING && !bus_ends);
    assert(ryz_tools_external(RYZ_TOOL_I2C, external_cancel, &external_id) == ESP_OK);
    unblock_probe(); run_to_idle();
    assert(scan_snapshot().phase == RYZ_I2C_SCAN_CANCELLED && probes == 1 && bus_ends == 1);
}

static void completes_between_snapshot_and_cancel(void)
{
    nack_fixture();
    uint32_t session = open_tools(), scan = configure_start(session);
    assert(pthread_mutex_lock(&gate) == 0);
    released = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    while (delays < 1) timed_wait();
    assert(pthread_mutex_unlock(&gate) == 0);
    assert(scan_snapshot().phase == RYZ_I2C_SCAN_RUNNING && probes == 1);
    finish_before_cancel = true;
    /* Broker's first cache read sees RUNNING; the native call gate lets the
     * real worker complete naturally before executing the real cancellation.
     * INVALID_STATE must be reconciled against the same completed identity. */
    close_done(session);
    assert(natural_completion_windows == 1 && observed_cancel_result == ESP_ERR_INVALID_STATE);
    assert(!ownership().tools[RYZ_TOOL_I2C].owner_session);
    ryz_i2c_scan_snapshot_t s = scan_snapshot();
    assert(s.scan_id == scan && s.phase == RYZ_I2C_SCAN_COMPLETED && !s.resources_held);
    assert(s.probe_calls == 112 && s.nack_count == 112 && !s.cleanup_error);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "claim_scan_release")) claim_scan_release();
    else if (!strcmp(argv[1], "cleanup_failed_retry")) cleanup_failed_retry();
    else if (!strcmp(argv[1], "config_only_history")) config_only_history();
    else if (!strcmp(argv[1], "queued_close")) queued_close();
    else if (!strcmp(argv[1], "external_active")) active_external_cannot_be_adopted();
    else if (!strcmp(argv[1], "completion_race")) completes_between_snapshot_and_cancel();
    else abort();
    stop_worker();
    printf("TOOLS_I2C_PASS %s\n", argv[1]);
    return 0;
}
