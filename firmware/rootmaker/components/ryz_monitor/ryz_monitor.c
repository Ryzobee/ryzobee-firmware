#include "ryz_monitor.h"
#include "ryz_monitor_platform.h"
#include "ryz_monitor_source.h"
#include "ryz_monitor_stream.h"
#include <stdatomic.h>
#include <string.h>

enum { STOPPED, CREATING, READY };
static atomic_int s_lifecycle = ATOMIC_VAR_INIT(STOPPED);
static ryz_monitor_snapshot_t s_snapshot;
static uint32_t s_last_id;
static ryz_monitor_phase_t s_previous_phase;

static bool inactive(ryz_monitor_phase_t phase)
{ return phase == RYZ_MONITOR_IDLE || phase == RYZ_MONITOR_STOPPED || phase == RYZ_MONITOR_FAILED; }
static bool current_locked(uint32_t operation)
{ return s_snapshot.operation_id == operation; }
static bool current(uint32_t operation)
{
    ryz_monitor_platform_lock();
    bool result = current_locked(operation);
    ryz_monitor_platform_unlock();
    return result;
}
static void finish_locked(ryz_monitor_phase_t phase, esp_err_t error,
                          esp_err_t cleanup, esp_err_t restore)
{
    s_snapshot.phase = phase;
    s_snapshot.operation_pending = false;
    s_snapshot.operation_error = error;
    s_snapshot.cleanup_error = cleanup;
    s_snapshot.restore_error = restore;
    s_snapshot.operation_finished_ms = ryz_monitor_platform_now_ms();
}
static void finish(uint32_t operation, ryz_monitor_phase_t phase, esp_err_t error,
                   esp_err_t cleanup, esp_err_t restore)
{
    ryz_monitor_platform_lock();
    if (current_locked(operation)) finish_locked(phase, error, cleanup, restore);
    ryz_monitor_platform_unlock();
}
static void observe_resources(void)
{
    bool held = ryz_monitor_uart_resources_held();
    ryz_monitor_platform_lock();
    s_snapshot.resources_held = held;
    ryz_monitor_platform_unlock();
}

/* Stream calls try their own atomic lock once. Never spin under the core
 * mutex; stop superseding this operation is checked after every yield. */
static esp_err_t quiesce(uint32_t operation)
{
    ryz_monitor_log_capture(false);
    for (;;) {
        ryz_monitor_platform_lock();
        if (!current_locked(operation)) {
            ryz_monitor_platform_unlock(); return ESP_ERR_INVALID_STATE;
        }
        ryz_monitor_stream_info_t info;
        esp_err_t error = ryz_monitor_stream_info(&info);
        if (error == ESP_ERR_INVALID_STATE) error = ESP_OK;
        else if (error == ESP_OK) error = ryz_monitor_stream_accept(info.session_id, false);
        if (error == ESP_OK) s_snapshot.source_active = false;
        ryz_monitor_platform_unlock();
        if (error != ESP_ERR_NOT_FINISHED) return error;
        ryz_monitor_platform_delay_ms(1);
    }
}
static esp_err_t close_source(uint32_t operation)
{
    esp_err_t error = quiesce(operation);
    if (error != ESP_OK || !current(operation)) return error;
    if (ryz_monitor_uart_resources_held()) error = ryz_monitor_uart_end();
    bool held = ryz_monitor_uart_resources_held();
    ryz_monitor_platform_lock();
    s_snapshot.resources_held = held;
    ryz_monitor_platform_unlock();
    return error == ESP_OK && held ? ESP_ERR_INVALID_STATE : error;
}
static esp_err_t open_source(uint32_t operation, const ryz_monitor_config_t *config)
{
    if (!current(operation)) return ESP_ERR_INVALID_STATE;
    esp_err_t error = config->source == RYZ_MONITOR_SYSTEM ?
        ryz_monitor_log_install() : ryz_monitor_uart_begin(config);
    observe_resources();
    return error;
}
static esp_err_t reset_stream(uint32_t operation, const ryz_monitor_config_t *config,
                             uint32_t revision)
{
    for (;;) {
        ryz_monitor_platform_lock();
        if (!current_locked(operation)) {
            ryz_monitor_platform_unlock(); return ESP_ERR_INVALID_STATE;
        }
        esp_err_t error = ryz_monitor_stream_reset(s_snapshot.session_id, revision, config->source);
        ryz_monitor_platform_unlock();
        if (error != ESP_ERR_NOT_FINISHED) return error;
        ryz_monitor_platform_delay_ms(1);
    }
}
static esp_err_t activate(uint32_t operation, const ryz_monitor_config_t *config,
                         uint32_t revision, bool commit, esp_err_t operation_error)
{
    for (;;) {
        ryz_monitor_platform_lock();
        if (!current_locked(operation)) {
            ryz_monitor_platform_unlock(); return ESP_ERR_INVALID_STATE;
        }
        /* Reconfigure atomically changes stream identity AND opens capture;
         * a failed try leaves the previous view/config wholly untouched. */
        esp_err_t error = commit ? ryz_monitor_stream_reconfigure(s_snapshot.session_id, revision, config->source) :
            ryz_monitor_stream_accept(s_snapshot.session_id, true);
        if (error == ESP_OK) {
            /* This is only an atomic flag, never a logger/OS/stream callback. */
            ryz_monitor_log_capture(config->source == RYZ_MONITOR_SYSTEM);
            if (commit) {
                s_snapshot.config = *config;
                s_snapshot.config_revision = revision;
            }
            s_snapshot.capture_config = *config;
            s_snapshot.source_active = true;
            if (config->source == RYZ_MONITOR_UART) s_snapshot.uart_events.events_may_be_lost = true;
            if (s_snapshot.operation == RYZ_MONITOR_OP_START)
                s_snapshot.started_ms = ryz_monitor_platform_now_ms();
            finish_locked(RYZ_MONITOR_RUNNING, operation_error, ESP_OK, ESP_OK);
        }
        ryz_monitor_platform_unlock();
        if (error != ESP_ERR_NOT_FINISHED) return error;
        ryz_monitor_platform_delay_ms(1);
    }
}
static void fail_and_close(uint32_t operation, esp_err_t error, esp_err_t restore)
{
    if (!current(operation)) return;
    ryz_monitor_platform_lock();
    if (current_locked(operation)) s_snapshot.phase = RYZ_MONITOR_STOPPING;
    ryz_monitor_platform_unlock();
    esp_err_t cleanup = close_source(operation);
    if (current(operation)) finish(operation, RYZ_MONITOR_FAILED, error, cleanup, restore);
}
static void start_operation(uint32_t operation, ryz_monitor_config_t config, uint32_t revision)
{
    esp_err_t error = quiesce(operation);
    if (!current(operation)) return;
    if (error == ESP_OK) error = reset_stream(operation, &config, revision);
    if (!current(operation)) return;
    if (error == ESP_OK) error = open_source(operation, &config);
    if (!current(operation)) return;
    if (error == ESP_OK) error = activate(operation, &config, revision, false, ESP_OK);
    if (error != ESP_OK) fail_and_close(operation, error, ESP_OK);
}
static void configure_operation(uint32_t operation, ryz_monitor_config_t candidate,
                                ryz_monitor_config_t previous, uint32_t revision,
                                ryz_monitor_phase_t previous_phase)
{
    if (inactive(previous_phase)) {
        ryz_monitor_platform_lock();
        if (current_locked(operation)) {
            s_snapshot.config = candidate;
            s_snapshot.config_revision = revision + 1;
            finish_locked(previous_phase, ESP_OK, ESP_OK, ESP_OK);
        }
        ryz_monitor_platform_unlock();
        return;
    }
    esp_err_t error = close_source(operation);
    if (!current(operation)) return;
    if (error != ESP_OK) {
        finish(operation, RYZ_MONITOR_FAILED, error, error, ESP_ERR_INVALID_STATE);
        return;
    }
    error = open_source(operation, &candidate);
    if (!current(operation)) return;
    if (error == ESP_OK) error = activate(operation, &candidate, revision + 1, true, ESP_OK);
    if (!current(operation) || error == ESP_OK) return;
    esp_err_t cleanup = close_source(operation);
    if (!current(operation)) return;
    if (cleanup != ESP_OK) {
        finish(operation, RYZ_MONITOR_FAILED, error, cleanup, ESP_ERR_INVALID_STATE);
        return;
    }
    esp_err_t restore = open_source(operation, &previous);
    if (!current(operation)) return;
    if (restore == ESP_OK) restore = activate(operation, &previous, revision, false, error);
    if (!current(operation)) return;
    if (restore != ESP_OK) fail_and_close(operation, error, restore);
}
static void stop_operation(uint32_t operation)
{
    esp_err_t cleanup = close_source(operation);
    if (current(operation)) finish(operation, cleanup == ESP_OK ? RYZ_MONITOR_STOPPED :
                                  RYZ_MONITOR_FAILED, cleanup, cleanup, ESP_OK);
}
static void add(uint64_t *value, uint32_t amount)
{ *value = amount > UINT64_MAX - *value ? UINT64_MAX : *value + amount; }
static void poll_cycle(uint32_t operation)
{
    for (unsigned i = 0; i < 8; ++i) {
        ryz_monitor_platform_lock();
        bool running = current_locked(operation) && s_snapshot.phase == RYZ_MONITOR_RUNNING &&
            !s_snapshot.operation_pending;
        ryz_monitor_capture_token_t token;
        esp_err_t error = running ? ryz_monitor_stream_token(&token) : ESP_ERR_INVALID_STATE;
        ryz_monitor_platform_unlock();
        if (!running || !current(operation)) return;
        if (error == ESP_ERR_NOT_FINISHED) break;
        if (error != ESP_OK) {
            ryz_monitor_stream_info_t info;
            if (ryz_monitor_stream_info(&info) == ESP_OK && info.sequence_exhausted)
                fail_and_close(operation, ESP_ERR_INVALID_STATE, ESP_OK);
            break;
        }
        if (token.source != RYZ_MONITOR_UART) break;
        ryz_monitor_sample_t sample;
        error = ryz_monitor_uart_poll(&sample);
        if (!current(operation)) return;
        if (error != ESP_OK || sample.length > RYZ_MONITOR_RECORD_BYTES) {
            fail_and_close(operation, error != ESP_OK ? error : ESP_ERR_INVALID_SIZE, ESP_OK);
            return;
        }
        ryz_monitor_platform_lock();
        if (current_locked(operation)) {
            add(&s_snapshot.uart_events.fifo_overflow, sample.fifo_overflow);
            add(&s_snapshot.uart_events.buffer_full, sample.buffer_full);
            add(&s_snapshot.uart_events.frame_error, sample.frame_error);
            add(&s_snapshot.uart_events.parity_error, sample.parity_error);
            add(&s_snapshot.uart_events.break_events, sample.break_events);
            if (sample.length) {
                (void)ryz_monitor_stream_append(token, sample.bytes, sample.length, sample.length,
                                               ryz_monitor_platform_now_ms());
            }
        }
        ryz_monitor_platform_unlock();
        if (!sample.length) break;
    }
    ryz_monitor_platform_delay_ms(1);
}
static void monitor_task(void *unused)
{
    (void)unused;
    for (;;) {
        ryz_monitor_platform_lock();
        uint32_t operation = s_snapshot.operation_id;
        ryz_monitor_operation_t type = s_snapshot.operation_pending ? s_snapshot.operation : RYZ_MONITOR_OP_NONE;
        bool running = s_snapshot.phase == RYZ_MONITOR_RUNNING;
        ryz_monitor_config_t config = s_snapshot.config, candidate = s_snapshot.requested_config;
        uint32_t revision = s_snapshot.config_revision;
        ryz_monitor_phase_t previous = s_previous_phase;
        ryz_monitor_platform_unlock();
        if (type == RYZ_MONITOR_OP_START) start_operation(operation, config, revision);
        else if (type == RYZ_MONITOR_OP_CONFIGURE) configure_operation(operation, candidate, config, revision, previous);
        else if (type == RYZ_MONITOR_OP_STOP) stop_operation(operation);
        else if (running) poll_cycle(operation);
        else ryz_monitor_platform_wait();
    }
}
static esp_err_t ensure_worker(bool *creating)
{
    int expected = STOPPED;
    *creating = atomic_compare_exchange_strong(&s_lifecycle, &expected, CREATING);
    if (!*creating && expected != READY) return ESP_ERR_INVALID_STATE;
    if (*creating) {
        esp_err_t error = ryz_monitor_platform_init();
        if (error == ESP_OK) {
            memset(&s_snapshot, 0, sizeof(s_snapshot));
            s_snapshot.config = s_snapshot.requested_config = RYZ_MONITOR_DEFAULT_CONFIG;
            s_snapshot.config_revision = 1;
            error = ryz_monitor_platform_start(monitor_task);
            if (error != ESP_OK) ryz_monitor_platform_deinit();
        }
        if (error != ESP_OK) {
            atomic_store_explicit(&s_lifecycle, STOPPED, memory_order_release);
            return error;
        }
    }
    return ESP_OK;
}
static void request_locked(ryz_monitor_operation_t operation, ryz_monitor_phase_t phase, uint32_t id)
{
    s_previous_phase = s_snapshot.phase;
    s_snapshot.operation = operation;
    s_snapshot.operation_id = id;
    s_snapshot.operation_pending = true;
    s_snapshot.phase = phase;
    s_snapshot.operation_error = ESP_OK;
    s_snapshot.restore_error = ESP_OK;
    s_snapshot.operation_finished_ms = 0;
}
esp_err_t ryz_monitor_start(uint32_t expected_revision, uint32_t *out_operation)
{
    if (!out_operation) return ESP_ERR_INVALID_ARG;
    *out_operation = 0;
    if (!expected_revision) return ESP_ERR_INVALID_ARG;
    bool creating;
    esp_err_t error = ensure_worker(&creating);
    if (error != ESP_OK) return error;
    ryz_monitor_platform_lock();
    error = ESP_ERR_INVALID_STATE;
    if (!s_snapshot.operation_pending && inactive(s_snapshot.phase) && !s_snapshot.resources_held &&
        !s_snapshot.source_active && expected_revision == s_snapshot.config_revision && s_last_id < UINT32_MAX - 1) {
        uint32_t id = ++s_last_id;
        request_locked(RYZ_MONITOR_OP_START, RYZ_MONITOR_STARTING, id);
        s_snapshot.session_id = id;
        s_snapshot.capture_config = s_snapshot.config;
        s_snapshot.requested_config = s_snapshot.config;
        s_snapshot.started_ms = 0;
        s_snapshot.cleanup_error = ESP_OK;
        memset(&s_snapshot.uart_events, 0, sizeof(s_snapshot.uart_events));
        *out_operation = id;
        error = ESP_OK;
    }
    ryz_monitor_platform_unlock();
    if (creating) atomic_store_explicit(&s_lifecycle, READY, memory_order_release);
    if (error == ESP_OK) ryz_monitor_platform_wake();
    return error;
}
esp_err_t ryz_monitor_configure(const ryz_monitor_config_t *config, uint32_t expected_revision,
                              uint32_t expected_session, uint32_t *out_operation)
{
    if (!out_operation) return ESP_ERR_INVALID_ARG;
    ryz_monitor_config_t value = config ? *config : (ryz_monitor_config_t){0};
    *out_operation = 0;
    if (!config || !expected_revision) return ESP_ERR_INVALID_ARG;
    esp_err_t error = ryz_monitor_validate_config(&value);
    if (error != ESP_OK) return error;
    bool creating;
    error = ensure_worker(&creating);
    if (error != ESP_OK) return error;
    ryz_monitor_platform_lock();
    error = ESP_ERR_INVALID_STATE;
    bool allowed = s_snapshot.phase == RYZ_MONITOR_RUNNING ||
        (inactive(s_snapshot.phase) && !s_snapshot.resources_held && !s_snapshot.source_active);
    if (!s_snapshot.operation_pending && allowed && expected_revision == s_snapshot.config_revision &&
        expected_revision != UINT32_MAX && expected_session == s_snapshot.session_id && s_last_id < UINT32_MAX - 1) {
        uint32_t id = ++s_last_id;
        request_locked(RYZ_MONITOR_OP_CONFIGURE, RYZ_MONITOR_RECONFIGURING, id);
        s_snapshot.requested_config = value;
        *out_operation = id;
        error = ESP_OK;
    }
    ryz_monitor_platform_unlock();
    if (creating) atomic_store_explicit(&s_lifecycle, READY, memory_order_release);
    if (error == ESP_OK) ryz_monitor_platform_wake();
    return error;
}
esp_err_t ryz_monitor_stop(uint32_t session_id, uint32_t *out_operation)
{
    if (!out_operation) return ESP_ERR_INVALID_ARG;
    *out_operation = 0;
    if (!session_id) return ESP_ERR_INVALID_ARG;
    if (atomic_load_explicit(&s_lifecycle, memory_order_acquire) != READY) return ESP_ERR_INVALID_STATE;
    ryz_monitor_platform_lock();
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (session_id == s_snapshot.session_id) {
        if (s_snapshot.operation_pending && s_snapshot.operation == RYZ_MONITOR_OP_STOP) {
            *out_operation = s_snapshot.operation_id;
        } else {
            /* UINT32_MAX is reserved for stop. Once used no later start or
             * configure can enter, so cleanup retries may keep this token. */
            uint32_t id = s_last_id == UINT32_MAX ? UINT32_MAX : ++s_last_id;
            request_locked(RYZ_MONITOR_OP_STOP, RYZ_MONITOR_STOPPING, id);
            *out_operation = id;
        }
        error = ESP_OK;
    }
    ryz_monitor_platform_unlock();
    if (error == ESP_OK) ryz_monitor_platform_wake();
    return error;
}
static esp_err_t cache_action(uint32_t session, uint32_t generation, bool clear,
                              bool paused, uint32_t *out_generation)
{
    if (!out_generation) return ESP_ERR_INVALID_ARG;
    *out_generation = 0;
    if (!session || !generation) return ESP_ERR_INVALID_ARG;
    if (atomic_load_explicit(&s_lifecycle, memory_order_acquire) != READY) return ESP_ERR_INVALID_STATE;
    ryz_monitor_platform_lock();
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (session == s_snapshot.session_id && !s_snapshot.operation_pending &&
        (s_snapshot.phase == RYZ_MONITOR_RUNNING || inactive(s_snapshot.phase))) {
        error = clear ? ryz_monitor_stream_clear(session, generation, out_generation) :
            ryz_monitor_stream_pause(session, generation, paused, out_generation);
    }
    ryz_monitor_platform_unlock();
    return error;
}
esp_err_t ryz_monitor_set_paused(uint32_t session, uint32_t generation, bool paused, uint32_t *out_generation)
{ return cache_action(session, generation, false, paused, out_generation); }
esp_err_t ryz_monitor_clear(uint32_t session, uint32_t generation, uint32_t *out_generation)
{ return cache_action(session, generation, true, false, out_generation); }
esp_err_t ryz_monitor_get_snapshot(ryz_monitor_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (atomic_load_explicit(&s_lifecycle, memory_order_acquire) != READY) return ESP_ERR_INVALID_STATE;
    ryz_monitor_platform_lock();
    *out = s_snapshot;
    esp_err_t error = ryz_monitor_stream_info(&out->stream);
    if (error == ESP_ERR_INVALID_STATE) error = ESP_OK;
    /* Reset belongs to the worker. Until then, never attach the preceding
     * session's history to a newly admitted start (or its queued stop). */
    if (error == ESP_OK && out->stream.session_id != out->session_id) {
        bool gap = out->stream.capture_gap_since_boot;
        memset(&out->stream, 0, sizeof(out->stream));
        out->stream.capture_gap_since_boot = gap;
    }
    ryz_monitor_platform_unlock();
    if (error != ESP_OK) memset(out, 0, sizeof(*out));
    return error;
}
esp_err_t ryz_monitor_read(uint32_t session, uint32_t generation, uint32_t after, ryz_monitor_page_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (atomic_load_explicit(&s_lifecycle, memory_order_acquire) != READY) return ESP_ERR_INVALID_STATE;
    ryz_monitor_platform_lock();
    esp_err_t error = session == s_snapshot.session_id ?
        ryz_monitor_stream_page(session, generation, after, out) : ESP_ERR_INVALID_STATE;
    ryz_monitor_platform_unlock();
    return error;
}
