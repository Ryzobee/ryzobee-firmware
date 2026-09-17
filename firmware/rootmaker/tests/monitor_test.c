#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ryz_monitor.h"
#include "ryz_monitor_platform.h"
#include "ryz_monitor_source.h"
#include "ryz_monitor_stream.h"

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER, cache = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static pthread_t worker;
static void (*entry_function)(void *);
static bool created, released, stopping, event, idle, delay_blocked, defer_wake;
static unsigned permits, park_epoch;
static _Thread_local unsigned cache_depth;
static int64_t now_ms = 100;
static esp_err_t init_error, start_error, log_error, end_error, poll_error;
static unsigned inits, starts, deinits, log_installs, begins, ends, polls;
static bool capture, held;
static esp_err_t begin_errors[16];
static ryz_monitor_config_t opened[16];
static bool continuous, block_poll, poll_entered, release_poll;
static unsigned block_begin, begin_entered;
static bool release_begin, block_end, end_entered, release_end;
static unsigned polls_since_yield;

static void wait_changed(void)
{
    struct timespec deadline;
    assert(!clock_gettime(CLOCK_REALTIME, &deadline));
    deadline.tv_sec += 4;
    assert(!pthread_cond_timedwait(&changed, &gate, &deadline));
}
static void check_stop(void)
{
    if (stopping) { assert(!pthread_mutex_unlock(&gate)); pthread_exit(NULL); }
}
static void *worker_entry(void *unused)
{
    (void)unused;
    assert(!pthread_mutex_lock(&gate));
    while (!released && !stopping) wait_changed();
    check_stop();
    assert(!pthread_mutex_unlock(&gate));
    entry_function(NULL);
    abort();
}
esp_err_t ryz_monitor_platform_init(void) { ++inits; return init_error; }
void ryz_monitor_platform_deinit(void) { ++deinits; }
esp_err_t ryz_monitor_platform_start(void (*entry)(void *))
{
    ++starts;
    if (start_error != ESP_OK) return start_error;
    assert(!created);
    entry_function = entry;
    assert(!pthread_create(&worker, NULL, worker_entry, NULL));
    created = true;
    return ESP_OK;
}
void ryz_monitor_platform_lock(void)
{ assert(!cache_depth); assert(!pthread_mutex_lock(&cache)); ++cache_depth; }
void ryz_monitor_platform_unlock(void)
{ assert(cache_depth == 1); --cache_depth; assert(!pthread_mutex_unlock(&cache)); }
void ryz_monitor_platform_wake(void)
{
    assert(!pthread_mutex_lock(&gate)); event = true;
    assert(!pthread_cond_broadcast(&changed)); assert(!pthread_mutex_unlock(&gate));
}
void ryz_monitor_platform_wait(void)
{
    assert(!cache_depth); assert(!pthread_mutex_lock(&gate));
    while ((!event || defer_wake) && !stopping) {
        idle = true; ++park_epoch;
        assert(!pthread_cond_broadcast(&changed)); wait_changed();
    }
    check_stop(); idle = false; event = false;
    assert(!pthread_mutex_unlock(&gate));
}
void ryz_monitor_platform_delay_ms(uint32_t ms)
{
    assert(!cache_depth && ms >= 1 && ms <= 20);
    assert(!pthread_mutex_lock(&gate));
    assert(polls_since_yield <= 8); polls_since_yield = 0;
    while (!permits && !stopping) {
        delay_blocked = true; ++park_epoch;
        assert(!pthread_cond_broadcast(&changed)); wait_changed();
    }
    check_stop(); delay_blocked = false; --permits; now_ms += ms;
    assert(!pthread_mutex_unlock(&gate));
}
int64_t ryz_monitor_platform_now_ms(void)
{ assert(!pthread_mutex_lock(&gate)); int64_t value = now_ms; assert(!pthread_mutex_unlock(&gate)); return value; }
static void owner(void) { assert(!cache_depth && pthread_equal(pthread_self(), worker)); }
esp_err_t ryz_monitor_validate_config(const ryz_monitor_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    if (config->source == RYZ_MONITOR_SYSTEM)
        return config->rx == -1 && config->tx == -1 && !config->baud ? ESP_OK : ESP_ERR_INVALID_ARG;
    return config->source == RYZ_MONITOR_UART && config->rx >= 2 && config->rx <= 5 &&
        config->tx >= 2 && config->tx <= 5 && config->rx != config->tx &&
        (config->baud == 115200 || config->baud == 9600) ? ESP_OK : ESP_ERR_INVALID_ARG;
}
esp_err_t ryz_monitor_log_install(void) { owner(); ++log_installs; return log_error; }
void ryz_monitor_log_capture(bool enabled) { assert(pthread_equal(pthread_self(), worker)); capture = enabled; }
esp_err_t ryz_monitor_uart_begin(const ryz_monitor_config_t *config)
{
    owner(); assert(!pthread_mutex_lock(&gate));
    assert(begins < 16); opened[begins] = *config;
    esp_err_t error = begin_errors[begins++];
    held = true;
    begin_entered = begins;
    assert(!pthread_cond_broadcast(&changed));
    while (block_begin == begins && !release_begin && !stopping) wait_changed();
    check_stop(); assert(!pthread_mutex_unlock(&gate));
    return error;
}
esp_err_t ryz_monitor_uart_end(void)
{
    owner(); assert(!pthread_mutex_lock(&gate)); ++ends; end_entered = true;
    assert(!pthread_cond_broadcast(&changed));
    while (block_end && !release_end && !stopping) wait_changed();
    check_stop(); if (!end_error) held = false;
    assert(!pthread_mutex_unlock(&gate)); return end_error;
}
bool ryz_monitor_uart_resources_held(void) { owner(); return held; }
esp_err_t ryz_monitor_uart_poll(ryz_monitor_sample_t *out)
{
    owner(); assert(!pthread_mutex_lock(&gate)); ++polls; ++polls_since_yield;
    memset(out, 0, sizeof(*out));
    if (poll_error == ESP_OK && continuous) {
        out->length = 3;
        out->bytes[0] = (uint8_t)polls; out->bytes[1] = 0; out->bytes[2] = 0xff;
        out->fifo_overflow = 1; out->buffer_full = 2; out->frame_error = 3;
        out->parity_error = 4; out->break_events = 5;
    }
    poll_entered = true; assert(!pthread_cond_broadcast(&changed));
    while (block_poll && !release_poll && !stopping) wait_changed();
    check_stop(); assert(!pthread_mutex_unlock(&gate)); return poll_error;
}
static void stop_worker(void)
{
    if (!created) return;
    assert(!pthread_mutex_lock(&gate)); stopping = true;
    assert(!pthread_cond_broadcast(&changed)); assert(!pthread_mutex_unlock(&gate));
    assert(!pthread_join(worker, NULL));
}
static void startup(void)
{
    ryz_monitor_snapshot_t snapshot;
    memset(&snapshot, 0xa5, sizeof(snapshot));
    assert(ryz_monitor_get_snapshot(&snapshot) == ESP_ERR_INVALID_STATE && !snapshot.session_id);
    uint32_t op;
    assert(ryz_monitor_start(1, &op) == ESP_OK && op == 1);
    assert(ryz_monitor_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_MONITOR_STARTING && snapshot.operation_pending);
    assert(snapshot.operation_id == op && snapshot.session_id == op && snapshot.config_revision == 1);
    assert(!snapshot.source_active && !snapshot.resources_held);
    assert(inits == 1 && starts == 1 && !begins && !log_installs && !polls);
    uint32_t rejected = 999;
    assert(ryz_monitor_start(1, &rejected) == ESP_ERR_INVALID_STATE && !rejected);
}
static void drive(void)
{
    assert(!pthread_mutex_lock(&gate));
    unsigned previous = park_epoch;
    released = true; permits = 1; defer_wake = false;
    assert(!pthread_cond_broadcast(&changed));
    while (park_epoch <= previous || !((idle && !event) || (delay_blocked && !permits))) wait_changed();
    assert(!pthread_mutex_unlock(&gate));
}
static ryz_monitor_snapshot_t snapshot(void)
{
    ryz_monitor_snapshot_t out;
    assert(ryz_monitor_get_snapshot(&out) == ESP_OK);
    return out;
}
static void emit_log(const void *data, size_t length)
{
    assert(capture);
    ryz_monitor_capture_token_t token;
    assert(ryz_monitor_stream_token(&token) == ESP_OK);
    assert(ryz_monitor_stream_append(token, data, length, length, 150) == ESP_OK);
}
static void system_lifecycle(void)
{
    uint32_t session, op, generation;
    assert(ryz_monitor_start(1, &session) == ESP_OK);
    drive();
    ryz_monitor_snapshot_t s = snapshot();
    assert(s.phase == RYZ_MONITOR_RUNNING && !s.operation_pending && s.source_active);
    assert(!s.resources_held && s.stream.accepting && capture && log_installs == 1 && !polls);
    const uint8_t binary[] = {'a', 0, 'b'};
    emit_log(binary, sizeof(binary));
    assert(ryz_monitor_set_paused(session, 1, true, &generation) == ESP_OK && generation == 2);
    for (unsigned i = 0; i < 40; ++i) emit_log("x", 1);
    ryz_monitor_page_t page;
    assert(ryz_monitor_read(session, generation, 0, &page) == ESP_OK && page.count == 1);
    assert(page.records[0].length == 3 && !memcmp(page.records[0].bytes, binary, 3));
    assert(page.stream.paused && page.stream.chunks == 41 && page.stream.overwritten_chunks == 9);
    ryz_monitor_capture_token_t old;
    assert(ryz_monitor_stream_token(&old) == ESP_OK);
    assert(ryz_monitor_clear(session, generation, &generation) == ESP_OK && generation == 3);
    assert(ryz_monitor_stream_append(old, "late", 4, 4, 151) == ESP_ERR_INVALID_STATE);
    assert(ryz_monitor_read(session, generation, 0, &page) == ESP_OK && !page.count && page.stream.paused);
    emit_log("new", 3);
    assert(ryz_monitor_set_paused(session, generation, false, &generation) == ESP_OK && generation == 4);
    assert(ryz_monitor_read(session, generation, 1, &page) == ESP_OK && page.count == 1 && page.gap);
    assert(page.records[0].sequence == 42 && page.stream.chunks == 42);
    assert(ryz_monitor_stop(session, &op) == ESP_OK && op > session);
    drive(); s = snapshot();
    assert(s.phase == RYZ_MONITOR_STOPPED && !s.source_active && !s.resources_held && !capture);
    assert(!s.stream.accepting && s.stream.chunks == 42 && !begins && !ends && !polls);
    ryz_monitor_stream_gap();
    uint32_t next;
    assert(ryz_monitor_start(1, &next) == ESP_OK && next > op);
    s = snapshot();
    assert(!s.stream.session_id); /* Pending new session cannot expose old history. */
    assert(s.stream.capture_gap_since_boot); /* Boot-lifetime loss never becomes false again. */
    drive(); s = snapshot();
    assert(s.session_id == next && s.stream.session_id == next && !s.stream.chunks);
    assert(ryz_monitor_stop(session, &op) == ESP_ERR_INVALID_STATE && !op);
    assert(ryz_monitor_read(session, generation, 0, &page) == ESP_ERR_INVALID_STATE && !page.count);
}
static const ryz_monitor_config_t uart_a = {RYZ_MONITOR_UART, 2, 3, 115200};
static const ryz_monitor_config_t uart_b = {RYZ_MONITOR_UART, 4, 5, 9600};
static uint32_t configure_uart(void)
{
    uint32_t op;
    assert(ryz_monitor_configure(&uart_a, 1, 0, &op) == ESP_OK && op == 1);
    ryz_monitor_snapshot_t s = snapshot();
    assert(s.operation_pending && s.config_revision == 1 && s.config.source == RYZ_MONITOR_SYSTEM);
    assert(s.requested_config.rx == 2 && !begins && !log_installs);
    drive(); s = snapshot();
    assert(s.phase == RYZ_MONITOR_IDLE && !s.operation_pending && s.config_revision == 2);
    assert(s.config.rx == 2 && s.config.tx == 3 && !s.session_id && !begins && !log_installs);
    return op;
}
static uint32_t start_uart(void)
{
    (void)configure_uart();
    uint32_t session;
    assert(ryz_monitor_start(2, &session) == ESP_OK && session == 2);
    drive();
    ryz_monitor_snapshot_t s = snapshot();
    assert(s.phase == RYZ_MONITOR_RUNNING && s.source_active && s.resources_held);
    assert(s.stream.accepting && s.stream.source == RYZ_MONITOR_UART && begins == 1);
    return session;
}
static void uart_capture(void)
{
    continuous = true;
    uint32_t session = start_uart();
    ryz_monitor_snapshot_t s = snapshot();
    assert(s.stream.chunks == 16 && polls == 16 && s.stream.bytes == 48);
    assert(s.uart_events.events_may_be_lost && s.uart_events.fifo_overflow == 16);
    assert(s.uart_events.buffer_full == 32 && s.uart_events.frame_error == 48);
    assert(s.uart_events.parity_error == 64 && s.uart_events.break_events == 80);
    ryz_monitor_page_t page;
    assert(ryz_monitor_read(session, 1, 0, &page) == ESP_OK && page.count == 8 && page.more);
    assert(page.records[0].length == 3 && page.records[0].bytes[1] == 0 && page.records[0].bytes[2] == 0xff);
    assert(ryz_monitor_read(session, 1, page.next_sequence, &page) == ESP_OK && page.count == 8 && !page.more);
    uint32_t op;
    assert(ryz_monitor_configure(&uart_b, 1, session, &op) == ESP_ERR_INVALID_STATE && !op);
    assert(ryz_monitor_configure(&uart_b, 2, session + 1, &op) == ESP_ERR_INVALID_STATE && !op);
    ryz_monitor_config_t defaults = RYZ_MONITOR_DEFAULT_CONFIG;
    assert(ryz_monitor_configure(&defaults, 2, session, &op) == ESP_OK);
    s = snapshot();
    assert(s.config_revision == 2 && s.config.source == RYZ_MONITOR_UART && s.operation_pending);
    drive(); s = snapshot();
    assert(s.phase == RYZ_MONITOR_RUNNING && s.config_revision == 3 && s.session_id == session);
    assert(s.config.source == RYZ_MONITOR_SYSTEM && s.capture_config.source == RYZ_MONITOR_SYSTEM);
    assert(s.stream.source == RYZ_MONITOR_SYSTEM && s.stream.config_revision == 3);
    assert(s.stream.chunks == 16 && s.stream.retained_records == 0 && s.stream.view_generation == 2);
    assert(capture && !s.resources_held && begins == 1 && ends == 1 && polls == 16);
    assert(ryz_monitor_read(session, 1, 0, &page) == ESP_ERR_INVALID_STATE && !page.count);
}
static void restore(bool fail_restore)
{
    uint32_t session = start_uart(), op;
    begin_errors[1] = ESP_ERR_TIMEOUT;
    if (fail_restore) begin_errors[2] = ESP_ERR_NO_MEM;
    assert(ryz_monitor_configure(&uart_b, 2, session, &op) == ESP_OK);
    drive();
    ryz_monitor_snapshot_t s = snapshot();
    assert(!s.operation_pending && s.operation_error == ESP_ERR_TIMEOUT);
    assert(s.config_revision == 2 && s.config.rx == 2 && s.capture_config.rx == 2);
    assert(s.requested_config.rx == 4 && s.session_id == session && s.stream.config_revision == 2);
    assert(begins == 3 && opened[1].rx == 4 && opened[2].rx == 2);
    if (fail_restore) {
        assert(s.phase == RYZ_MONITOR_FAILED && s.restore_error == ESP_ERR_NO_MEM);
        assert(!s.source_active && !s.resources_held && !s.stream.accepting && ends == 3);
    } else {
        assert(s.phase == RYZ_MONITOR_RUNNING && s.restore_error == ESP_OK);
        assert(s.source_active && s.resources_held && s.stream.accepting && ends == 2);
        assert(s.stream.view_generation == 1);
    }
}
static void cleanup_retry(void)
{
    uint32_t session = start_uart(), op, rejected;
    unsigned prior_polls = polls;
    end_error = ESP_FAIL;
    assert(ryz_monitor_stop(session, &op) == ESP_OK);
    drive();
    ryz_monitor_snapshot_t s = snapshot();
    assert(s.phase == RYZ_MONITOR_FAILED && s.operation_error == ESP_FAIL && s.cleanup_error == ESP_FAIL);
    assert(s.resources_held && !s.source_active && !s.stream.accepting);
    assert(ryz_monitor_start(2, &rejected) == ESP_ERR_INVALID_STATE && !rejected);
    assert(ryz_monitor_configure(&uart_b, 2, session, &rejected) == ESP_ERR_INVALID_STATE && !rejected);
    end_error = ESP_OK;
    assert(ryz_monitor_stop(session, &op) == ESP_OK);
    drive(); s = snapshot();
    assert(s.phase == RYZ_MONITOR_STOPPED && !s.resources_held && !s.source_active);
    assert(s.cleanup_error == ESP_OK && begins == 1 && ends == 2 && polls == prior_polls);
}
static void queued_stop(void)
{
    uint32_t session, op, same;
    assert(ryz_monitor_start(1, &session) == ESP_OK);
    assert(ryz_monitor_stop(session, &op) == ESP_OK && op != session);
    assert(ryz_monitor_stop(session, &same) == ESP_OK && same == op);
    drive();
    ryz_monitor_snapshot_t s = snapshot();
    assert(s.phase == RYZ_MONITOR_STOPPED && !s.started_ms && !s.operation_pending);
    assert(!capture && !log_installs && !begins && !polls && !ends);
}
static void kick(void)
{
    assert(!pthread_mutex_lock(&gate)); released = true; permits = 1;
    assert(!pthread_cond_broadcast(&changed)); assert(!pthread_mutex_unlock(&gate));
}
static void cancel_begin(unsigned which)
{
    uint32_t session, op;
    if (which == 1) {
        (void)configure_uart(); block_begin = 1;
        assert(ryz_monitor_start(2, &session) == ESP_OK);
    } else {
        session = start_uart(); block_begin = which;
        if (which == 3) begin_errors[1] = ESP_ERR_TIMEOUT;
        assert(ryz_monitor_configure(&uart_b, 2, session, &op) == ESP_OK);
    }
    kick();
    assert(!pthread_mutex_lock(&gate));
    while (begin_entered != which) wait_changed();
    assert(!pthread_mutex_unlock(&gate));
    ryz_monitor_snapshot_t s = snapshot();
    assert(s.operation_pending && s.config_revision == 2);
    uint32_t unused;
    assert(ryz_monitor_clear(session, 1, &unused) == ESP_ERR_INVALID_STATE && !unused);
    assert(ryz_monitor_stop(session, &op) == ESP_OK);
    assert(!pthread_mutex_lock(&gate)); release_begin = true;
    assert(!pthread_cond_broadcast(&changed)); assert(!pthread_mutex_unlock(&gate));
    drive(); s = snapshot();
    assert(s.phase == RYZ_MONITOR_STOPPED && s.operation_id == op && !s.operation_pending);
    assert(!s.resources_held && !s.source_active && !s.stream.accepting && begins == which);
    assert(s.config_revision == 2 && s.config.rx == 2 && ends == which);
    assert(polls == (which == 1 ? 0 : 2));
}
static void clear_inflight(bool stop)
{
    (void)configure_uart();
    block_poll = true; continuous = true;
    uint32_t session, op;
    assert(ryz_monitor_start(2, &session) == ESP_OK);
    kick();
    assert(!pthread_mutex_lock(&gate)); while (!poll_entered) wait_changed();
    assert(!pthread_mutex_unlock(&gate));
    ryz_monitor_snapshot_t s = snapshot();
    assert(s.phase == RYZ_MONITOR_RUNNING && !s.stream.chunks && s.source_active);
    if (stop) assert(ryz_monitor_stop(session, &op) == ESP_OK);
    else assert(ryz_monitor_clear(session, s.stream.view_generation, &op) == ESP_OK);
    assert(!pthread_mutex_lock(&gate)); release_poll = true; continuous = false;
    assert(!pthread_cond_broadcast(&changed)); assert(!pthread_mutex_unlock(&gate));
    drive(); s = snapshot();
    assert(!s.stream.chunks); /* The sample was captured with the old epoch. */
    if (stop) {
        assert(s.phase == RYZ_MONITOR_STOPPED && polls == 1 && ends == 1);
    } else assert(s.phase == RYZ_MONITOR_RUNNING && s.stream.view_generation == op);
}
static void allocation_retry(void)
{
    uint32_t op;
    init_error = ESP_ERR_NO_MEM;
    assert(ryz_monitor_start(1, &op) == ESP_ERR_NO_MEM && !op && !starts && !deinits);
    init_error = ESP_OK; start_error = ESP_ERR_NO_MEM;
    assert(ryz_monitor_configure(&uart_a, 1, 0, &op) == ESP_ERR_NO_MEM && !op);
    assert(starts == 1 && deinits == 1);
    ryz_monitor_snapshot_t s;
    assert(ryz_monitor_get_snapshot(&s) == ESP_ERR_INVALID_STATE && !s.config_revision);
    start_error = ESP_OK;
    assert(ryz_monitor_start(1, &op) == ESP_OK && op == 1);
    assert(inits == 3 && starts == 2 && !log_installs && !begins);
}
static void stop_old_release(void)
{
    uint32_t session = start_uart(), op, stopped;
    block_end = true;
    assert(ryz_monitor_configure(&uart_b, 2, session, &op) == ESP_OK);
    kick();
    assert(!pthread_mutex_lock(&gate)); while (!end_entered) wait_changed();
    assert(!pthread_mutex_unlock(&gate));
    for (unsigned i = 0; i < 100; ++i) {
        ryz_monitor_snapshot_t s = snapshot();
        assert(s.phase == RYZ_MONITOR_RECONFIGURING && s.operation_pending && !s.source_active);
        assert(s.resources_held && !s.stream.accepting && s.config_revision == 2);
    }
    assert(ryz_monitor_stop(session, &stopped) == ESP_OK && stopped > op);
    assert(!pthread_mutex_lock(&gate)); release_end = true;
    assert(!pthread_cond_broadcast(&changed)); assert(!pthread_mutex_unlock(&gate));
    drive();
    ryz_monitor_snapshot_t s = snapshot();
    assert(s.phase == RYZ_MONITOR_STOPPED && s.operation_id == stopped);
    assert(!s.resources_held && !s.source_active && s.config_revision == 2);
    assert(begins == 1 && ends == 1 && polls == 2);
}
static void old_release_failure(void)
{
    uint32_t session = start_uart(), op;
    end_error = ESP_ERR_TIMEOUT;
    assert(ryz_monitor_configure(&uart_b, 2, session, &op) == ESP_OK);
    drive();
    ryz_monitor_snapshot_t s = snapshot();
    assert(s.phase == RYZ_MONITOR_FAILED && s.operation_error == ESP_ERR_TIMEOUT);
    assert(s.cleanup_error == ESP_ERR_TIMEOUT && s.restore_error == ESP_ERR_INVALID_STATE);
    assert(s.config_revision == 2 && s.config.rx == 2 && s.resources_held && !s.source_active);
    assert(!s.stream.accepting && begins == 1 && ends == 1);
}
static void native_failure(bool logger)
{
    uint32_t session;
    if (logger) {
        log_error = ESP_ERR_NO_MEM;
        assert(ryz_monitor_start(1, &session) == ESP_OK);
    } else {
        (void)configure_uart();
        poll_error = ESP_ERR_TIMEOUT;
        assert(ryz_monitor_start(2, &session) == ESP_OK);
    }
    drive();
    ryz_monitor_snapshot_t s = snapshot();
    assert(s.phase == RYZ_MONITOR_FAILED && !s.operation_pending);
    assert(s.operation_error == (logger ? ESP_ERR_NO_MEM : ESP_ERR_TIMEOUT));
    assert(!s.source_active && !s.resources_held && !s.stream.accepting && !capture);
    assert(s.cleanup_error == ESP_OK && !s.stream.chunks);
    assert(logger ? (!begins && !ends && !polls && log_installs == 1) : (begins == 1 && ends == 1 && polls == 1));
    log_error = ESP_OK; poll_error = ESP_OK;
    uint32_t next;
    assert(ryz_monitor_start(logger ? 1 : 2, &next) == ESP_OK && next > session);
    drive(); s = snapshot();
    assert(s.phase == RYZ_MONITOR_RUNNING && s.session_id == next && s.source_active);
    assert(logger ? (log_installs == 2 && !begins) : (begins == 2 && ends == 1));
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "startup")) startup();
    else if (!strcmp(argv[1], "system_lifecycle")) system_lifecycle();
    else if (!strcmp(argv[1], "uart_capture")) uart_capture();
    else if (!strcmp(argv[1], "restore")) restore(false);
    else if (!strcmp(argv[1], "restore_fail")) restore(true);
    else if (!strcmp(argv[1], "cleanup_retry")) cleanup_retry();
    else if (!strcmp(argv[1], "queued_stop")) queued_stop();
    else if (!strcmp(argv[1], "cancel_begin")) cancel_begin(1);
    else if (!strcmp(argv[1], "cancel_candidate")) cancel_begin(2);
    else if (!strcmp(argv[1], "cancel_restore")) cancel_begin(3);
    else if (!strcmp(argv[1], "clear_inflight")) clear_inflight(false);
    else if (!strcmp(argv[1], "stop_inflight")) clear_inflight(true);
    else if (!strcmp(argv[1], "allocation_retry")) allocation_retry();
    else if (!strcmp(argv[1], "stop_old_release")) stop_old_release();
    else if (!strcmp(argv[1], "old_release_failure")) old_release_failure();
    else if (!strcmp(argv[1], "poll_failure")) native_failure(false);
    else if (!strcmp(argv[1], "logger_failure")) native_failure(true);
    else abort();
    stop_worker();
    printf("MONITOR_PASS %s\n", argv[1]);
    return 0;
}
