#include "workbench_tools.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

static bool rgb_started;
static ryz_rgb_snapshot_t rgb;
static unsigned queries, submits, cleanups;
static bool i2c_started, monitor_started;
static ryz_i2c_scan_snapshot_t i2c;
static ryz_monitor_snapshot_t monitor;
static unsigned i2c_cancels, monitor_stops;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool gate_entered, gate_release, hold_query;
static void wait_at_gate(void)
{
    pthread_mutex_lock(&mutex); gate_entered = true; pthread_cond_broadcast(&changed);
    while (!gate_release) pthread_cond_wait(&changed, &mutex);
    pthread_mutex_unlock(&mutex);
}
esp_err_t ryz_i2c_scan_get_snapshot(ryz_i2c_scan_snapshot_t *out)
{ *out = i2c; return i2c_started ? ESP_OK : ESP_ERR_INVALID_STATE; }
esp_err_t ryz_i2c_scan_validate_config(const ryz_i2c_scan_config_t *config)
{ (void)config; return ESP_OK; }
esp_err_t ryz_i2c_scan_configure(const ryz_i2c_scan_config_t *config, uint32_t rev, uint32_t *out)
{ (void)config; (void)rev; *out = 2; return ESP_OK; }
esp_err_t ryz_i2c_scan_start(uint32_t *out)
{ i2c_started = true; i2c.config_revision = 1; i2c.phase = RYZ_I2C_SCAN_RUNNING; i2c.resources_held = true; *out = ++i2c.scan_id; return ESP_OK; }
esp_err_t ryz_i2c_scan_cancel(uint32_t id)
{ assert(id == i2c.scan_id); ++i2c_cancels; i2c.phase = RYZ_I2C_SCAN_CANCELLING; return ESP_OK; }
esp_err_t ryz_monitor_get_snapshot(ryz_monitor_snapshot_t *out)
{ *out = monitor; return monitor_started ? ESP_OK : ESP_ERR_INVALID_STATE; }
esp_err_t ryz_monitor_validate_config(const ryz_monitor_config_t *config)
{ (void)config; return ESP_OK; }
esp_err_t ryz_monitor_configure(const ryz_monitor_config_t *config, uint32_t rev, uint32_t session, uint32_t *out)
{ (void)config; (void)rev; (void)session; *out = 1; return ESP_OK; }
esp_err_t ryz_monitor_start(uint32_t rev, uint32_t *out)
{ (void)rev; monitor_started = true; monitor.phase = RYZ_MONITOR_RUNNING; monitor.source_active = true;
  monitor.resources_held = true; monitor.config_revision = 1; *out = ++monitor.session_id; return ESP_OK; }
esp_err_t ryz_monitor_stop(uint32_t id, uint32_t *out)
{ assert(id == monitor.session_id); ++monitor_stops; monitor.phase = RYZ_MONITOR_STOPPING;
  monitor.operation_pending = true; monitor.source_active = false; *out = 2; return ESP_OK; }
esp_err_t ryz_monitor_set_paused(uint32_t id, uint32_t generation, bool paused, uint32_t *out)
{ (void)id; (void)paused; *out = generation; return ESP_OK; }
esp_err_t ryz_monitor_clear(uint32_t id, uint32_t generation, uint32_t *out)
{ (void)id; *out = generation; return ESP_OK; }
esp_err_t ryz_monitor_read(uint32_t id, uint32_t generation, uint32_t after, ryz_monitor_page_t *out)
{ (void)id; (void)generation; (void)after; memset(out, 0, sizeof(*out)); return ESP_OK; }
esp_err_t ryz_rgb_get_snapshot(ryz_rgb_snapshot_t *out)
{ ++queries; if (hold_query) wait_at_gate(); *out = rgb; return rgb_started ? ESP_OK : ESP_ERR_INVALID_STATE; }
esp_err_t ryz_rgb_submit(ryz_rgb_color_t color, uint32_t *out)
{
    rgb_started = true; ++submits;
    rgb.request_id = submits; rgb.requested = color;
    rgb.phase = RYZ_RGB_QUEUED; rgb.resources_held = true;
    *out = rgb.request_id; return ESP_OK;
}
esp_err_t ryz_rgb_cleanup(uint32_t id)
{ assert(id == rgb.request_id); ++cleanups; rgb.phase = RYZ_RGB_CLEANING; return ESP_OK; }
#include "rgb_lease_fixture.h"
static void native_clean(void)
{ rgb.phase = RYZ_RGB_CLEANED; rgb.resources_held = false; rgb.cleanup_error = ESP_OK; }
static void unused(void)
{
    ryz_workbench_tools_job_t job;
    ryz_workbench_tools_begin(&job, "boot-1");
    assert(ryz_workbench_tools_finish(&job, 100));
    assert(job.finished && !job.session_id && job.final.phase == RYZ_WB_TOOLS_UNUSED);
    assert(!queries && !submits && !cleanups);
}
static ryz_workbench_tools_job_t running(void)
{
    ryz_workbench_tools_job_t job;
    ryz_workbench_tools_begin(&job, "boot-1");
    assert(!job.session_id && !queries);
    ryz_tool_request_t request = {.action = RYZ_TOOL_RGB_SET};
    ryz_tool_reply_t reply;
    assert(ryz_workbench_tools_call(&job, &request, &reply) == RYZ_TOOL_CALL_OK);
    assert(job.session_id && reply.operation_id == 1 && reply.error_code == ESP_OK);
    return job;
}
static ryz_workbench_tools_report_t report(const ryz_workbench_tools_job_t *job)
{
    ryz_workbench_tools_report_t out;
    assert(ryz_workbench_tools_report(job->job_id, job->finished ? &job->final : NULL, &out) == ESP_OK);
    return out;
}
static void retire(void)
{
    ryz_workbench_tools_job_t job = running();
    assert(ryz_workbench_tools_finish(&job, 100));
    assert(job.finished && cleanups == 1 && report(&job).phase == RYZ_WB_TOOLS_PENDING);
    ryz_workbench_tools_job_t next;
    ryz_workbench_tools_begin(&next, "boot-2");
    ryz_workbench_tools_tick(600100);
    assert(report(&job).timed_out && cleanups == 1);
    native_clean();
    ryz_workbench_tools_tick(700100);
    ryz_workbench_tools_report_t value = report(&job);
    assert(ryz_workbench_tools_clean(&value));
    assert(ryz_workbench_tools_finish(&next, 800100));
}
static cJSON *rpc(const char *text)
{
    cJSON *request = cJSON_Parse(text); assert(request);
    cJSON *reply = ryz_workbench_tools_rpc(request, "boot"); cJSON_Delete(request); return reply;
}
static bool ok(cJSON *reply)
{ return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply, "ok")); }
static void recovery(void)
{
    ryz_workbench_tools_job_t job = running();
    assert(ryz_workbench_tools_finish(&job, 0));
    rgb.phase = RYZ_RGB_FAILED; rgb.cleanup_error = ESP_FAIL;
    ryz_workbench_tools_tick(100);
    assert(report(&job).phase == RYZ_WB_TOOLS_FAILED);
    ryz_workbench_tools_tick(1000000); assert(cleanups == 1);
    cJSON *reply = rpc("{\"id\":\"r\",\"op\":\"tools\",\"action\":\"retry_close\",\"boot_id\":\"boot\",\"session_id\":1}");
    assert(ok(reply)); cJSON_Delete(reply);
    ryz_workbench_tools_tick(1100000); assert(cleanups == 2);
    native_clean(); ryz_workbench_tools_tick(1200000);
    ryz_workbench_tools_report_t value = report(&job); assert(ryz_workbench_tools_clean(&value));
}
static long budget;
static void *limited(size_t size) { if (budget-- == 0) return NULL; return malloc(size); }
static void oom(void)
{
    ryz_workbench_tools_job_t job = running(); assert(ryz_workbench_tools_finish(&job, 0));
    rgb.phase = RYZ_RGB_FAILED; rgb.cleanup_error = ESP_FAIL; ryz_workbench_tools_tick(1);
    cJSON *request = cJSON_Parse("{\"id\":\"r\",\"op\":\"tools\",\"action\":\"retry_close\",\"boot_id\":\"boot\",\"session_id\":1}");
    assert(request); bool success = false;
    for (long n = 0; n < 80; ++n) {
        budget = n; cJSON_Hooks hooks = {.malloc_fn = limited, .free_fn = free}; cJSON_InitHooks(&hooks);
        cJSON *reply = ryz_workbench_tools_rpc(request, "boot"); cJSON_InitHooks(NULL);
        if (reply && ok(reply)) { cJSON_Delete(reply); success = true; break; }
        cJSON_Delete(reply);
        ryz_workbench_tools_tick(100 + n);
        assert(cleanups == 1 && report(&job).phase == RYZ_WB_TOOLS_FAILED);
    }
    assert(success); cJSON_Delete(request);
    ryz_workbench_tools_tick(1000); assert(cleanups == 2);
}
static void validation(void)
{
    const char *bad[] = {
        "{\"id\":\"r\",\"op\":\"tools\",\"action\":\"status\",\"surprise\":1}",
        "{\"id\":\"r\",\"op\":\"tools\",\"action\":\"status\",\"action\":\"status\"}",
        "{\"id\":\"r\",\"op\":\"tools\",\"action\":\"retry_close\",\"session_id\":1}",
        "{\"id\":\"r\",\"op\":\"tools\",\"action\":\"retry_close\",\"boot_id\":\"old\",\"session_id\":1}",
        "{\"id\":\"r\",\"op\":\"tools\",\"action\":\"retry_close\",\"boot_id\":\"boot\",\"session_id\":1.5}",
        "{\"id\":\"r\",\"op\":\"tools\",\"action\":\"retry_close\",\"boot_id\":\"boot\",\"session_id\":4294967296}"
    };
    for (unsigned i = 0; i < sizeof(bad)/sizeof(*bad); ++i) { cJSON *reply = rpc(bad[i]); assert(!ok(reply)); cJSON_Delete(reply); }
    cJSON *reply = rpc("{\"id\":\"r\",\"op\":\"tools\",\"action\":\"status\"}");
    assert(ok(reply)); cJSON_Delete(reply); assert(!queries);
}
static esp_err_t delayed_admit(void *unused) { (void)unused; wait_at_gate(); return ESP_OK; }
static void *external_thread(void *unused)
{ (void)unused; assert(ryz_tools_external(RYZ_TOOL_I2C, delayed_admit, NULL) == ESP_OK); return NULL; }
static void *other_external_thread(void *unused)
{ (void)unused; assert(ryz_tools_external(RYZ_TOOL_MONITOR, delayed_admit, NULL) == ESP_OK); return NULL; }
static void await_gate(void)
{
    pthread_mutex_lock(&mutex);
    while (!gate_entered) pthread_cond_wait(&changed, &mutex);
    pthread_mutex_unlock(&mutex);
}
static void release_gate(pthread_t thread)
{
    pthread_mutex_lock(&mutex); gate_release = true; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&mutex);
    assert(!pthread_join(thread, NULL));
}
static void broker_busy(void)
{
    ryz_workbench_tools_job_t job = running();
    pthread_t thread; assert(!pthread_create(&thread, NULL, external_thread, NULL)); await_gate();
    assert(ryz_workbench_tools_finish(&job, 100));
    assert(job.final.phase == RYZ_WB_TOOLS_PENDING && !job.final.observation_valid &&
           job.final.session_id && !job.final.remaining_mask && !cleanups);
    release_gate(thread);
    ryz_workbench_tools_tick(200); assert(cleanups == 1);
    native_clean(); ryz_workbench_tools_tick(300);
    ryz_workbench_tools_report_t value = report(&job); assert(ryz_workbench_tools_clean(&value));
}
static void *query_thread(void *context)
{
    ryz_tool_request_t request = {.action = RYZ_TOOL_RGB_STATUS}; ryz_tool_reply_t reply;
    assert(ryz_workbench_tools_call(context, &request, &reply) == RYZ_TOOL_CALL_OK); return NULL;
}
static void helper_busy(void)
{
    ryz_workbench_tools_job_t job = running();
    hold_query = true; pthread_t thread;
    assert(!pthread_create(&thread, NULL, query_thread, &job)); await_gate();
    ryz_workbench_tools_job_t unused_job; ryz_workbench_tools_begin(&unused_job, "boot-2");
    assert(ryz_workbench_tools_finish(&unused_job, 1));
    ryz_workbench_tools_report_t value;
    /* An unrelated tool gate must not turn an unused legacy job into failure. */
    assert(ryz_workbench_tools_report(unused_job.job_id, &unused_job.final, &value) == ESP_OK);
    assert(ryz_workbench_tools_clean(&value));
    assert(!ryz_workbench_tools_finish(&job, 2));
    release_gate(thread); hold_query = false;
    assert(ryz_workbench_tools_finish(&job, 3));
}
static void mixed(void)
{
    ryz_workbench_tools_job_t job = running();
    ryz_tool_request_t request = {.action = RYZ_TOOL_I2C_START, .expected_revision = 1}; ryz_tool_reply_t reply;
    assert(ryz_workbench_tools_call(&job, &request, &reply) == RYZ_TOOL_CALL_OK);
    assert(ryz_workbench_tools_finish(&job, 0));
    rgb.phase = RYZ_RGB_FAILED; rgb.cleanup_error = ESP_FAIL;
    ryz_workbench_tools_tick(100);
    ryz_workbench_tools_report_t value = report(&job);
    assert(value.phase == RYZ_WB_TOOLS_FAILED && value.pending_mask == 1 && value.failed_mask == 2);
    pthread_t thread; assert(!pthread_create(&thread, NULL, other_external_thread, NULL)); await_gate();
    ryz_workbench_tools_tick(150);
    value = report(&job);
    assert(value.phase == RYZ_WB_TOOLS_FAILED && value.error == ESP_FAIL && value.pending_mask == 1);
    release_gate(thread);
    i2c.phase = RYZ_I2C_SCAN_CANCELLED; i2c.resources_held = false;
    ryz_workbench_tools_tick(200);
    value = report(&job);
    assert(value.phase == RYZ_WB_TOOLS_FAILED && !value.pending_mask && value.remaining_mask == 2);
    assert(i2c_cancels == 1 && cleanups == 1);
}
static void capacity(void)
{
    ryz_workbench_tools_job_t first = running();
    assert(ryz_workbench_tools_finish(&first, 0));
    rgb.phase = RYZ_RGB_FAILED; rgb.cleanup_error = ESP_FAIL; ryz_workbench_tools_tick(1);
    ryz_workbench_tools_job_t second; ryz_workbench_tools_begin(&second, "boot-2");
    ryz_tool_request_t request = {.action = RYZ_TOOL_I2C_START, .expected_revision = 1}; ryz_tool_reply_t reply;
    assert(ryz_workbench_tools_call(&second, &request, &reply) == RYZ_TOOL_CALL_OK);
    assert(ryz_workbench_tools_finish(&second, 2));
    i2c.phase = RYZ_I2C_SCAN_FAILED; i2c.cleanup_error = ESP_FAIL; ryz_workbench_tools_tick(3);
    ryz_workbench_tools_job_t third; ryz_workbench_tools_begin(&third, "boot-3");
    request.action = RYZ_TOOL_MONITOR_START;
    assert(ryz_workbench_tools_call(&third, &request, &reply) == RYZ_TOOL_CALL_OK);
    assert(ryz_workbench_tools_finish(&third, 4));
    monitor.phase = RYZ_MONITOR_FAILED; monitor.operation_pending = false; monitor.cleanup_error = ESP_FAIL;
    ryz_workbench_tools_tick(5);
    ryz_workbench_tools_job_t fourth; ryz_workbench_tools_begin(&fourth, "boot-4");
    request.action = RYZ_TOOL_RGB_STATUS;
    assert(ryz_workbench_tools_call(&fourth, &request, &reply) == RYZ_TOOL_CALL_BUSY && fourth.session_id);
    cJSON *state = rpc("{\"id\":\"s\",\"op\":\"tools\",\"action\":\"status\"}");
    assert(ok(state) && cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(state, "sessions")) == 4);
    cJSON_Delete(state); assert(ryz_workbench_tools_finish(&fourth, 6));
    assert(ryz_workbench_tools_clean(&fourth.final));
    state = rpc("{\"id\":\"s\",\"op\":\"tools\",\"action\":\"status\"}");
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(state, "sessions")) == 3); cJSON_Delete(state);
    assert(cleanups == 1 && i2c_cancels == 1 && monitor_stops == 1);
}
static void open_failure(void)
{
    uint32_t external_session; assert(ryz_tools_open(&external_session) == ESP_OK);
    ryz_workbench_tools_job_t job; ryz_workbench_tools_begin(&job, "boot-1");
    ryz_tool_request_t request = {.action = RYZ_TOOL_RGB_SET}; ryz_tool_reply_t reply;
    for (unsigned i = 0; i < 8; ++i) {
        assert(ryz_workbench_tools_call(&job, &request, &reply) == RYZ_TOOL_CALL_STATE && !job.session_id);
        assert(reply.error_code == ESP_ERR_INVALID_STATE && !reply.operation_id);
    }
    cJSON *state = rpc("{\"id\":\"s\",\"op\":\"tools\",\"action\":\"status\"}");
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(state, "sessions")) == 0); cJSON_Delete(state);
    ryz_tools_close_result_t close; assert(ryz_tools_close(external_session, &close) == ESP_OK);
    assert(ryz_workbench_tools_call(&job, &request, &reply) == RYZ_TOOL_CALL_OK);
}
static void destroyed_job(void)
{
    ryz_workbench_tools_job_t *job = malloc(sizeof(*job)); assert(job);
    *job = running(); assert(ryz_workbench_tools_finish(job, 0));
    const uint32_t session = job->session_id; free(job);
    ryz_workbench_tools_tick(600000);
    cJSON *state = rpc("{\"id\":\"s\",\"op\":\"tools\",\"action\":\"status\"}");
    cJSON *entry = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(state, "sessions"), 0);
    assert(entry && cJSON_GetObjectItemCaseSensitive(entry, "session_id")->valueint == (int)session);
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(entry, "job_id")->valuestring, "boot-1"));
    cJSON_Delete(state);
    native_clean(); ryz_workbench_tools_tick(700000);
}
static void missing_record(void)
{
    ryz_workbench_tools_job_t job; ryz_workbench_tools_begin(&job, "broken-owner");
    /* Unsupported C caller/invariant fault must stay observable, not become
     * successful merely because there is no corresponding retention record. */
    job.session_id = 999;
    assert(ryz_workbench_tools_finish(&job, 0));
    assert(job.final.phase == RYZ_WB_TOOLS_FAILED);
    ryz_workbench_tools_report_t value = report(&job);
    assert(value.phase == RYZ_WB_TOOLS_FAILED && value.error == ESP_ERR_INVALID_STATE);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "unused")) unused();
    else if (!strcmp(argv[1], "retire")) retire();
    else if (!strcmp(argv[1], "recovery")) recovery();
    else if (!strcmp(argv[1], "oom")) oom();
    else if (!strcmp(argv[1], "validation")) validation();
    else if (!strcmp(argv[1], "broker_busy")) broker_busy();
    else if (!strcmp(argv[1], "helper_busy")) helper_busy();
    else if (!strcmp(argv[1], "mixed")) mixed();
    else if (!strcmp(argv[1], "capacity")) capacity();
    else if (!strcmp(argv[1], "open_failure")) open_failure();
    else if (!strcmp(argv[1], "destroyed_job")) destroyed_job();
    else if (!strcmp(argv[1], "missing_record")) missing_record();
    else assert(false);
    printf("WORKBENCH_TOOLS_PASS %s\n", argv[1]);
}
