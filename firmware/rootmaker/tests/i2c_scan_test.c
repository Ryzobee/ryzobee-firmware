#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ryz_i2c_scan.h"
#include "ryz_i2c_scan_bus.h"
#include "ryz_i2c_scan_platform.h"
#include "ryz_tool_pins.h"

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker;
static void (*entry_function)(void *);
static bool created, released, stopping, event;
static bool idle_blocked;
static bool block_probe, probe_entered, release_probe;
static bool block_start, start_entered, release_start;
static unsigned delay_permits, delays, idle_waits;
static _Thread_local unsigned lock_depth;
static int64_t now_ms;
static esp_err_t platform_init_error, platform_start_error, board_init_error;
static unsigned platform_inits, platform_starts, platform_deinits;
static unsigned board_inits, probes;
static unsigned bus_ends, pair_checks;
static unsigned begin_recoveries;
static bool bus_held;
static esp_err_t pair_error;
static esp_err_t end_error;
static bool hold_on_begin_error, block_end, end_entered, release_end;
static bool race_go;
static unsigned racers_ready;
static ryz_i2c_scan_config_t begun_config;
static esp_err_t probe_results[128][4];
static unsigned attempts[128];

static void timed_wait(void)
{
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 3;
    assert(pthread_cond_timedwait(&changed, &gate, &deadline) == 0);
}
static void check_stop(void)
{
    if (stopping) {
        assert(pthread_mutex_unlock(&gate) == 0);
        pthread_exit(NULL);
    }
}
static void *worker_entry(void *unused)
{
    (void)unused;
    assert(pthread_mutex_lock(&gate) == 0);
    while (!released && !stopping) timed_wait();
    check_stop();
    assert(pthread_mutex_unlock(&gate) == 0);
    entry_function(NULL);
    abort();
}
esp_err_t ryz_i2c_scan_platform_init(void)
{
    ++platform_inits;
    return platform_init_error;
}
void ryz_i2c_scan_platform_deinit(void) { ++platform_deinits; }
esp_err_t ryz_i2c_scan_platform_start(void (*entry)(void *))
{
    ++platform_starts;
    if (platform_start_error != ESP_OK) return platform_start_error;
    assert(pthread_mutex_lock(&gate) == 0);
    start_entered = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    while (block_start && !release_start) timed_wait();
    assert(pthread_mutex_unlock(&gate) == 0);
    assert(!created);
    entry_function = entry;
    assert(pthread_create(&worker, NULL, worker_entry, NULL) == 0);
    created = true;
    return ESP_OK;
}
void ryz_i2c_scan_platform_lock(void)
{
    assert(!lock_depth);
    assert(pthread_mutex_lock(&cache_mutex) == 0);
    ++lock_depth;
}
void ryz_i2c_scan_platform_unlock(void)
{
    assert(lock_depth == 1);
    --lock_depth;
    assert(pthread_mutex_unlock(&cache_mutex) == 0);
}
void ryz_i2c_scan_platform_wake(void)
{
    assert(pthread_mutex_lock(&gate) == 0);
    event = true; /* Binary: repeated wakeups intentionally coalesce. */
    assert(pthread_cond_broadcast(&changed) == 0);
    assert(pthread_mutex_unlock(&gate) == 0);
}
void ryz_i2c_scan_platform_wait(void)
{
    assert(!lock_depth);
    assert(pthread_mutex_lock(&gate) == 0);
    ++idle_waits;
    idle_blocked = !event || !released;
    assert(pthread_cond_broadcast(&changed) == 0);
    while ((!event || !released) && !stopping) timed_wait();
    check_stop();
    idle_blocked = false;
    event = false;
    assert(pthread_mutex_unlock(&gate) == 0);
}
void ryz_i2c_scan_platform_delay_ms(uint32_t milliseconds)
{
    assert(!lock_depth && milliseconds == 5);
    assert(pthread_mutex_lock(&gate) == 0);
    ++delays;
    assert(pthread_cond_broadcast(&changed) == 0);
    while (!delay_permits && !stopping) timed_wait();
    check_stop();
    --delay_permits;
    now_ms += 5;
    assert(pthread_mutex_unlock(&gate) == 0);
}
int64_t ryz_i2c_scan_platform_now_ms(void)
{
    assert(pthread_mutex_lock(&gate) == 0);
    int64_t value = now_ms;
    assert(pthread_mutex_unlock(&gate) == 0);
    return value;
}
static void physical_call(void)
{
    assert(pthread_equal(pthread_self(), worker));
    assert(!lock_depth);
}
/* External bus/pin Adapter: core tests exercise public config transactions;
 * fixed-board eligibility and physical admission have separate real tests. */
esp_err_t ryz_i2c_scan_validate_config(const ryz_i2c_scan_config_t *config)
{
    if (!config || (config->hz != 100000 && config->hz != 400000)) return ESP_ERR_INVALID_ARG;
    if (config->sda == 41 && config->scl == 40 && config->hz == 100000) return ESP_OK;
    return config->sda >= 2 && config->sda <= 5 && config->scl >= 2 && config->scl <= 5 &&
        config->sda != config->scl ? ESP_OK : ESP_ERR_INVALID_ARG;
}
esp_err_t ryz_tool_pins_check_pair(int first, int second)
{
    assert(first != second);
    ++pair_checks;
    return pair_error;
}
esp_err_t ryz_i2c_scan_bus_begin(const ryz_i2c_scan_config_t *config)
{
    physical_call();
    ++board_inits;
    if (bus_held) ++begin_recoveries;
    begun_config = *config;
    bus_held = config->sda != 41 && (board_init_error == ESP_OK || hold_on_begin_error);
    return board_init_error;
}
esp_err_t ryz_i2c_scan_bus_end(void)
{
    physical_call();
    assert(pthread_mutex_lock(&gate) == 0);
    ++bus_ends;
    end_entered = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    while (block_end && !release_end && !stopping) timed_wait();
    check_stop();
    if (end_error == ESP_OK) bus_held = false;
    assert(pthread_mutex_unlock(&gate) == 0);
    return end_error;
}
bool ryz_i2c_scan_bus_resources_held(void) { physical_call(); return bus_held; }
esp_err_t ryz_i2c_scan_bus_probe(uint8_t address)
{
    physical_call();
    assert(address >= 0x08 && address <= 0x77);
    assert(attempts[address] < 4);
    ++probes;
    esp_err_t result = probe_results[address][attempts[address]++];
    assert(pthread_mutex_lock(&gate) == 0);
    probe_entered = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    while (block_probe && !release_probe && !stopping) timed_wait();
    check_stop();
    assert(pthread_mutex_unlock(&gate) == 0);
    return result;
}
static void stop_worker(void)
{
    if (!created) return;
    assert(pthread_mutex_lock(&gate) == 0);
    stopping = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    assert(pthread_mutex_unlock(&gate) == 0);
    assert(pthread_join(worker, NULL) == 0);
}
static void startup(void)
{
    ryz_i2c_scan_snapshot_t snapshot;
    memset(&snapshot, 0xa5, sizeof(snapshot));
    assert(ryz_i2c_scan_get_snapshot(NULL) == ESP_ERR_INVALID_ARG);
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_ERR_INVALID_STATE);
    assert(snapshot.phase == RYZ_I2C_SCAN_IDLE && snapshot.scan_id == 0);
    assert(ryz_i2c_scan_cancel(0) == ESP_ERR_INVALID_ARG);
    assert(ryz_i2c_scan_cancel(1) == ESP_ERR_INVALID_STATE);
    assert(ryz_i2c_scan_start(NULL) == ESP_ERR_INVALID_ARG && platform_inits == 0);
    uint32_t id = 900;
    platform_init_error = ESP_ERR_NO_MEM;
    assert(ryz_i2c_scan_start(&id) == ESP_ERR_NO_MEM && id == 0);
    assert(platform_starts == 0 && platform_deinits == 0);
    platform_init_error = ESP_OK;
    platform_start_error = ESP_ERR_NO_MEM;
    assert(ryz_i2c_scan_start(&id) == ESP_ERR_NO_MEM && id == 0);
    assert(platform_starts == 1 && platform_deinits == 1);
    platform_start_error = ESP_OK;
    now_ms = 123;
    assert(ryz_i2c_scan_start(&id) == ESP_OK && id == 1);
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_I2C_SCAN_QUEUED && snapshot.scan_id == id);
    assert(snapshot.queued_ms == 123 && snapshot.started_ms == 0);
    assert(snapshot.completed_addresses == 0 && snapshot.nack_count == 0);
    for (unsigned i = 0; i < 128; ++i) assert(snapshot.results[i] == RYZ_I2C_SCAN_UNSCANNED);
    assert(ryz_i2c_scan_start(&id) == ESP_ERR_INVALID_STATE && id == 0);
    assert(platform_inits == 3 && platform_starts == 2);
    assert(board_inits == 0 && probes == 0);
}

static void nack_fixture(void)
{
    for (unsigned address = 0; address < 128; ++address) {
        for (unsigned i = 0; i < 4; ++i) probe_results[address][i] = ESP_ERR_NOT_FOUND;
    }
}
static void run_to_idle(void)
{
    assert(pthread_mutex_lock(&gate) == 0);
    released = true;
    delay_permits = 1000;
    assert(pthread_cond_broadcast(&changed) == 0);
    while (!idle_blocked || event) timed_wait();
    assert(pthread_mutex_unlock(&gate) == 0);
}
static void complete(void)
{
    nack_fixture();
    probe_results[0x15][0] = ESP_OK;
    uint32_t id;
    now_ms = 100;
    assert(ryz_i2c_scan_start(&id) == ESP_OK && id == 1);
    run_to_idle();
    ryz_i2c_scan_snapshot_t snapshot;
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_I2C_SCAN_COMPLETED && snapshot.current_address == 0);
    assert(snapshot.error == ESP_OK && snapshot.completed_addresses == 112);
    assert(snapshot.probe_calls == 112 && snapshot.busy_responses == 0);
    assert(snapshot.ack_count == 1 && snapshot.nack_count == 111);
    assert(!snapshot.busy_count && !snapshot.timeout_count && !snapshot.error_count);
    assert(snapshot.started_ms == 100 && snapshot.finished_ms == 655);
    assert(probes == 112 && board_inits == 1 && delays == 111);
    for (unsigned address = 0; address < 128; ++address) {
        if (address < 8 || address > 0x77) {
            assert(snapshot.results[address] == RYZ_I2C_SCAN_UNSCANNED);
            assert(attempts[address] == 0);
        } else if (address == 0x15) {
            assert(snapshot.results[address] == RYZ_I2C_SCAN_ACK);
            assert(snapshot.errors[address] == ESP_OK);
        } else {
            assert(snapshot.results[address] == RYZ_I2C_SCAN_NACK);
            assert(snapshot.errors[address] == ESP_ERR_NOT_FOUND);
        }
    }
    assert(ryz_i2c_scan_cancel(id) == ESP_ERR_INVALID_STATE);
}

static void consistent(const ryz_i2c_scan_snapshot_t *snapshot)
{
    unsigned counts[6] = {0};
    for (unsigned address = 8; address <= 0x77; ++address) {
        assert(snapshot->results[address] <= RYZ_I2C_SCAN_ERROR);
        ++counts[snapshot->results[address]];
    }
    assert(snapshot->ack_count == counts[RYZ_I2C_SCAN_ACK]);
    assert(snapshot->nack_count == counts[RYZ_I2C_SCAN_NACK]);
    assert(snapshot->busy_count == counts[RYZ_I2C_SCAN_BUSY]);
    assert(snapshot->timeout_count == counts[RYZ_I2C_SCAN_TIMEOUT]);
    assert(snapshot->error_count == counts[RYZ_I2C_SCAN_ERROR]);
    assert(snapshot->completed_addresses == 112 - counts[RYZ_I2C_SCAN_UNSCANNED]);
    assert(snapshot->probe_calls <= 448);
}

static void busy(void)
{
    for (unsigned address = 0; address < 128; ++address) {
        for (unsigned i = 0; i < 4; ++i) probe_results[address][i] = ESP_ERR_NOT_FINISHED;
    }
    uint32_t id;
    assert(ryz_i2c_scan_start(&id) == ESP_OK);
    run_to_idle();
    ryz_i2c_scan_snapshot_t snapshot;
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    consistent(&snapshot);
    assert(snapshot.phase == RYZ_I2C_SCAN_COMPLETED && snapshot.error == ESP_OK);
    assert(snapshot.probe_calls == 448 && snapshot.busy_responses == 448);
    assert(snapshot.completed_addresses == 112 && snapshot.busy_count == 112);
    assert(snapshot.nack_count == 0 && snapshot.ack_count == 0);
    assert(delays == 447 && snapshot.current_address == 0);
    for (unsigned address = 8; address <= 0x77; ++address) {
        assert(attempts[address] == 4);
        assert(snapshot.errors[address] == ESP_ERR_NOT_FINISHED);
    }
}

static void mixed(void)
{
    nack_fixture();
    for (unsigned i = 0; i < 4; ++i) {
        probe_results[8][i] = i == 3 ? ESP_OK : ESP_ERR_NOT_FINISHED;
        probe_results[9][i] = ESP_ERR_NOT_FINISHED;
    }
    probe_results[10][0] = ESP_ERR_TIMEOUT;
    probe_results[10][1] = ESP_OK; /* Must not be tried after real TIMEOUT. */
    probe_results[11][0] = ESP_FAIL;
    probe_results[12][0] = ESP_ERR_INVALID_STATE;
    uint32_t id;
    assert(ryz_i2c_scan_start(&id) == ESP_OK);
    run_to_idle();
    ryz_i2c_scan_snapshot_t snapshot;
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    consistent(&snapshot);
    assert(snapshot.phase == RYZ_I2C_SCAN_COMPLETED && snapshot.error == ESP_OK);
    assert(snapshot.probe_calls == 118 && snapshot.busy_responses == 7);
    assert(snapshot.ack_count == 1 && snapshot.nack_count == 107);
    assert(snapshot.busy_count == 1 && snapshot.timeout_count == 1 && snapshot.error_count == 2);
    assert(snapshot.results[8] == RYZ_I2C_SCAN_ACK && snapshot.errors[8] == ESP_OK);
    assert(snapshot.results[9] == RYZ_I2C_SCAN_BUSY && snapshot.errors[9] == ESP_ERR_NOT_FINISHED);
    assert(snapshot.results[10] == RYZ_I2C_SCAN_TIMEOUT && snapshot.errors[10] == ESP_ERR_TIMEOUT);
    assert(snapshot.results[11] == RYZ_I2C_SCAN_ERROR && snapshot.errors[11] == ESP_FAIL);
    assert(snapshot.results[12] == RYZ_I2C_SCAN_ERROR && snapshot.errors[12] == ESP_ERR_INVALID_STATE);
    assert(attempts[10] == 1 && attempts[11] == 1 && attempts[12] == 1 && delays == 117);
}

static void cancel_queued(void)
{
    uint32_t id, rejected = 99;
    assert(ryz_i2c_scan_start(&id) == ESP_OK);
    assert(ryz_i2c_scan_cancel(id) == ESP_OK);
    assert(ryz_i2c_scan_cancel(id) == ESP_OK);
    assert(ryz_i2c_scan_start(&rejected) == ESP_ERR_INVALID_STATE && rejected == 0);
    ryz_i2c_scan_snapshot_t snapshot;
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_I2C_SCAN_CANCELLING && snapshot.current_address == 0);
    assert(snapshot.probe_calls == 0 && board_inits == 0);
    run_to_idle();
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    consistent(&snapshot);
    assert(snapshot.phase == RYZ_I2C_SCAN_CANCELLED && snapshot.started_ms == 0);
    assert(snapshot.probe_calls == 0 && snapshot.completed_addresses == 0);
    assert(snapshot.nack_count == 0 && probes == 0 && board_inits == 0);
    assert(ryz_i2c_scan_cancel(id) == ESP_OK);
}

static void pause_worker(void)
{
    assert(pthread_mutex_lock(&gate) == 0);
    assert(idle_blocked && !event);
    released = false;
    delay_permits = 0;
    assert(pthread_mutex_unlock(&gate) == 0);
}

static void cancel_inflight(void)
{
    nack_fixture();
    probe_results[8][0] = ESP_ERR_NOT_FINISHED;
    block_probe = true;
    uint32_t id;
    assert(ryz_i2c_scan_start(&id) == ESP_OK);
    assert(pthread_mutex_lock(&gate) == 0);
    released = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    while (!probe_entered) timed_wait();
    assert(pthread_mutex_unlock(&gate) == 0);
    ryz_i2c_scan_snapshot_t snapshot;
    for (unsigned i = 0; i < 100; ++i) {
        assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
        assert(snapshot.phase == RYZ_I2C_SCAN_RUNNING && snapshot.current_address == 8);
        assert(snapshot.probe_calls == 1 && snapshot.completed_addresses == 0);
    }
    assert(ryz_i2c_scan_cancel(id) == ESP_OK);
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_I2C_SCAN_CANCELLING && snapshot.probe_calls == 1);
    assert(snapshot.results[8] == RYZ_I2C_SCAN_UNSCANNED && probes == 1);
    assert(pthread_mutex_lock(&gate) == 0);
    release_probe = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    assert(pthread_mutex_unlock(&gate) == 0);
    run_to_idle();
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    consistent(&snapshot);
    assert(snapshot.phase == RYZ_I2C_SCAN_CANCELLED && snapshot.current_address == 0);
    assert(snapshot.probe_calls == 1 && snapshot.busy_responses == 0);
    assert(snapshot.completed_addresses == 0 && probes == 1);
    assert(ryz_i2c_scan_cancel(id) == ESP_OK);
    pause_worker();
    block_probe = false;
    nack_fixture();
    memset(attempts, 0, sizeof(attempts));
    uint32_t next;
    assert(ryz_i2c_scan_start(&next) == ESP_OK && next == id + 1);
    assert(ryz_i2c_scan_cancel(id) == ESP_ERR_INVALID_STATE);
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_I2C_SCAN_QUEUED && snapshot.scan_id == next);
    assert(snapshot.probe_calls == 0 && snapshot.completed_addresses == 0);
    run_to_idle();
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    consistent(&snapshot);
    assert(snapshot.phase == RYZ_I2C_SCAN_COMPLETED && snapshot.scan_id == next);
    assert(snapshot.completed_addresses == 112 && snapshot.nack_count == 112);
    assert(snapshot.probe_calls == 112 && snapshot.busy_responses == 0);
    assert(probes == 113 && board_inits == 2);
}

static void cancel_wait(bool retry)
{
    nack_fixture();
    if (retry) probe_results[8][0] = ESP_ERR_NOT_FINISHED;
    uint32_t id;
    assert(ryz_i2c_scan_start(&id) == ESP_OK);
    assert(pthread_mutex_lock(&gate) == 0);
    released = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    while (delays < 1) timed_wait();
    assert(pthread_mutex_unlock(&gate) == 0);
    assert(ryz_i2c_scan_cancel(id) == ESP_OK);
    ryz_i2c_scan_snapshot_t snapshot;
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_I2C_SCAN_CANCELLING && snapshot.current_address == 8);
    assert(snapshot.probe_calls == 1 && probes == 1);
    assert(snapshot.completed_addresses == (retry ? 0 : 1));
    assert(snapshot.busy_responses == (retry ? 1 : 0));
    run_to_idle();
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    consistent(&snapshot);
    assert(snapshot.phase == RYZ_I2C_SCAN_CANCELLED && snapshot.current_address == 0);
    assert(snapshot.probe_calls == 1 && probes == 1);
    assert(snapshot.results[9] == RYZ_I2C_SCAN_UNSCANNED);
    assert(snapshot.results[8] == (retry ? RYZ_I2C_SCAN_UNSCANNED : RYZ_I2C_SCAN_NACK));
}

static void init_failure(void)
{
    board_init_error = ESP_ERR_TIMEOUT;
    uint32_t id;
    assert(ryz_i2c_scan_start(&id) == ESP_OK);
    run_to_idle();
    ryz_i2c_scan_snapshot_t snapshot;
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    consistent(&snapshot);
    assert(snapshot.phase == RYZ_I2C_SCAN_FAILED && snapshot.error == ESP_ERR_TIMEOUT);
    assert(snapshot.probe_calls == 0 && snapshot.nack_count == 0 && snapshot.current_address == 0);
    assert(probes == 0 && board_inits == 1);
    pause_worker();
    board_init_error = ESP_OK;
    nack_fixture();
    uint32_t next;
    assert(ryz_i2c_scan_start(&next) == ESP_OK && next > id);
    run_to_idle();
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_I2C_SCAN_COMPLETED && snapshot.error == ESP_OK);
    assert(snapshot.nack_count == 112 && snapshot.probe_calls == 112);
    assert(board_inits == 2 && platform_starts == 1);
}

static void *start_entry(void *result)
{
    uint32_t id;
    *(esp_err_t *)result = ryz_i2c_scan_start(&id);
    assert(id == 1);
    return NULL;
}
static void concurrent_start(void)
{
    block_start = true;
    pthread_t starter;
    esp_err_t error = ESP_FAIL;
    assert(pthread_create(&starter, NULL, start_entry, &error) == 0);
    assert(pthread_mutex_lock(&gate) == 0);
    while (!start_entered) timed_wait();
    assert(pthread_mutex_unlock(&gate) == 0);
    uint32_t id = 555;
    assert(ryz_i2c_scan_start(&id) == ESP_ERR_INVALID_STATE && id == 0);
    ryz_i2c_scan_snapshot_t snapshot;
    memset(&snapshot, 0xa5, sizeof(snapshot));
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_ERR_INVALID_STATE);
    assert(snapshot.phase == RYZ_I2C_SCAN_IDLE && snapshot.scan_id == 0);
    assert(ryz_i2c_scan_cancel(1) == ESP_ERR_INVALID_STATE);
    assert(pthread_mutex_lock(&gate) == 0);
    release_start = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    assert(pthread_mutex_unlock(&gate) == 0);
    assert(pthread_join(starter, NULL) == 0);
    assert(error == ESP_OK && platform_starts == 1 && board_inits == 0);
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.scan_id == 1 && snapshot.phase == RYZ_I2C_SCAN_QUEUED);
}

static void configure(void)
{
    ryz_i2c_scan_config_t custom = {2, 3, 400000};
    uint32_t revision = 999;
    assert(ryz_i2c_scan_configure(NULL, 1, &revision) == ESP_ERR_INVALID_ARG && !revision);
    assert(ryz_i2c_scan_configure(&custom, 0, &revision) == ESP_ERR_INVALID_ARG && !revision);
    assert(ryz_i2c_scan_configure(&custom, 1, NULL) == ESP_ERR_INVALID_ARG);
    ryz_i2c_scan_config_t invalid = {41, 40, 400000};
    assert(ryz_i2c_scan_configure(&invalid, 1, &revision) == ESP_ERR_INVALID_ARG && !revision);
    assert(platform_inits == 0 && board_inits == 0 && pair_checks == 0);
    assert(ryz_i2c_scan_configure(&custom, 1, &revision) == ESP_OK && revision == 2);
    ryz_i2c_scan_snapshot_t snapshot;
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_I2C_SCAN_IDLE && !snapshot.scan_id);
    assert(snapshot.config_revision == 2 && snapshot.scan_config_revision == 0);
    assert(snapshot.config.sda == 2 && snapshot.config.scl == 3 && snapshot.config.hz == 400000);
    assert(board_inits == 0 && probes == 0 && bus_ends == 0 && pair_checks == 1);
    custom = (ryz_i2c_scan_config_t){4, 5, 100000};
    assert(ryz_i2c_scan_configure(&custom, 1, &revision) == ESP_ERR_INVALID_STATE && !revision);
    pair_error = ESP_ERR_NOT_FINISHED;
    assert(ryz_i2c_scan_configure(&custom, 2, &revision) == ESP_ERR_NOT_FINISHED && !revision);
    pair_error = ESP_ERR_INVALID_STATE;
    assert(ryz_i2c_scan_configure(&custom, 2, &revision) == ESP_ERR_INVALID_STATE && !revision);
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.config_revision == 2 && snapshot.config.sda == 2);
    ryz_i2c_scan_config_t defaults = RYZ_I2C_SCAN_DEFAULT_CONFIG;
    unsigned checks = pair_checks;
    assert(ryz_i2c_scan_configure(&defaults, 2, &revision) == ESP_OK && revision == 3);
    assert(pair_checks == checks && board_inits == 0 && probes == 0 && bus_ends == 0);
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.config.sda == 41 && snapshot.config.scl == 40 && snapshot.config.hz == 100000);
    assert(snapshot.config_revision == 3 && !snapshot.scan_config_revision);
}

static void cleanup_retry(void)
{
    ryz_i2c_scan_config_t custom = {2, 3, 400000};
    uint32_t revision, id;
    assert(ryz_i2c_scan_configure(&custom, 1, &revision) == ESP_OK && revision == 2);
    board_init_error = ESP_ERR_TIMEOUT;
    hold_on_begin_error = true;
    end_error = ESP_FAIL;
    assert(ryz_i2c_scan_start(&id) == ESP_OK && id == 1);
    run_to_idle();
    ryz_i2c_scan_snapshot_t snapshot;
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_I2C_SCAN_FAILED && snapshot.error == ESP_ERR_TIMEOUT);
    assert(snapshot.cleanup_error == ESP_FAIL && snapshot.resources_held);
    assert(snapshot.completed_addresses == 0 && snapshot.probe_calls == 0);
    assert(board_inits == 1 && bus_ends == 1 && probes == 0);
    unsigned checks = pair_checks;
    custom = (ryz_i2c_scan_config_t){4, 5, 100000};
    assert(ryz_i2c_scan_configure(&custom, 2, &revision) == ESP_ERR_INVALID_STATE && !revision);
    assert(pair_checks == checks);
    pause_worker();
    assert(ryz_i2c_scan_cancel(id) == ESP_OK);
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_I2C_SCAN_CANCELLING && snapshot.scan_id == id);
    assert(snapshot.resources_held && snapshot.config_revision == 2);
    run_to_idle();
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_I2C_SCAN_FAILED && snapshot.error == ESP_FAIL);
    assert(snapshot.cleanup_error == ESP_FAIL && snapshot.resources_held);
    assert(board_inits == 1 && bus_ends == 2 && probes == 0);
    pause_worker();
    end_error = ESP_OK;
    assert(ryz_i2c_scan_cancel(id) == ESP_OK);
    run_to_idle();
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_I2C_SCAN_CANCELLED && snapshot.scan_id == id);
    assert(snapshot.error == ESP_OK && snapshot.cleanup_error == ESP_OK && !snapshot.resources_held);
    assert(board_inits == 1 && bus_ends == 3 && probes == 0);
    assert(ryz_i2c_scan_cancel(id) == ESP_OK);
    assert(ryz_i2c_scan_configure(&custom, 2, &revision) == ESP_OK && revision == 3);
    pause_worker();
    uint32_t next;
    assert(ryz_i2c_scan_start(&next) == ESP_OK && next == id + 1);
    assert(ryz_i2c_scan_cancel(id) == ESP_ERR_INVALID_STATE);
    assert(board_inits == 1 && bus_ends == 3 && probes == 0);
}

static void configure_history(void)
{
    nack_fixture();
    ryz_i2c_scan_config_t custom = {2, 3, 400000};
    uint32_t revision, id;
    assert(ryz_i2c_scan_configure(&custom, 1, &revision) == ESP_OK && revision == 2);
    assert(ryz_i2c_scan_start(&id) == ESP_OK && id == 1);
    unsigned checks = pair_checks;
    custom = (ryz_i2c_scan_config_t){4, 5, 100000};
    assert(ryz_i2c_scan_configure(&custom, 2, &revision) == ESP_ERR_INVALID_STATE && !revision);
    assert(pair_checks == checks && !board_inits);
    run_to_idle();
    assert(begun_config.sda == 2 && begun_config.scl == 3 && begun_config.hz == 400000);
    assert(bus_ends == 1 && !bus_held);
    assert(ryz_i2c_scan_configure(&custom, 2, &revision) == ESP_OK && revision == 3);
    ryz_i2c_scan_snapshot_t snapshot;
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.config_revision == 3 && snapshot.config.sda == 4 && snapshot.config.scl == 5);
    assert(snapshot.scan_config_revision == 2 && snapshot.scan_config.sda == 2);
    assert(snapshot.scan_config.scl == 3 && snapshot.scan_config.hz == 400000);
    assert(snapshot.scan_id == 1 && snapshot.phase == RYZ_I2C_SCAN_COMPLETED);
    assert(snapshot.nack_count == 112 && snapshot.probe_calls == 112);
    pause_worker();
    memset(attempts, 0, sizeof(attempts));
    assert(ryz_i2c_scan_start(&id) == ESP_OK && id == 2);
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.config_revision == 3 && snapshot.scan_config_revision == 3);
    assert(snapshot.scan_config.sda == 4 && snapshot.scan_config.scl == 5);
    assert(snapshot.scan_config.hz == 100000 && !snapshot.completed_addresses);
    assert(board_inits == 1 && probes == 112);
    run_to_idle();
    assert(begun_config.sda == 4 && begun_config.scl == 5 && begun_config.hz == 100000);
    assert(board_inits == 2 && bus_ends == 2 && probes == 224);
}

static void release_gate(bool queued_cancel)
{
    nack_fixture();
    uint32_t revision, id, rejected;
    ryz_i2c_scan_config_t custom = {2, 3, 400000};
    assert(ryz_i2c_scan_configure(&custom, 1, &revision) == ESP_OK && revision == 2);
    block_end = true;
    assert(ryz_i2c_scan_start(&id) == ESP_OK && id == 1);
    if (queued_cancel) assert(ryz_i2c_scan_cancel(id) == ESP_OK);
    assert(pthread_mutex_lock(&gate) == 0);
    released = true;
    delay_permits = 1000;
    assert(pthread_cond_broadcast(&changed) == 0);
    while (!end_entered) timed_wait();
    assert(pthread_mutex_unlock(&gate) == 0);
    ryz_i2c_scan_snapshot_t snapshot;
    for (unsigned i = 0; i < 100; ++i) {
        assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
        assert(snapshot.phase == RYZ_I2C_SCAN_RELEASING && !snapshot.current_address);
        assert(snapshot.finished_ms == 0 && snapshot.resources_held == !queued_cancel);
        assert(snapshot.completed_addresses == (queued_cancel ? 0 : 112));
        assert(snapshot.probe_calls == (queued_cancel ? 0 : 112));
    }
    assert(bus_ends == 1 && board_inits == (queued_cancel ? 0 : 1));
    assert(ryz_i2c_scan_start(&rejected) == ESP_ERR_INVALID_STATE && !rejected);
    unsigned checks = pair_checks;
    assert(ryz_i2c_scan_configure(&custom, 2, &revision) == ESP_ERR_INVALID_STATE && !revision);
    assert(pair_checks == checks);
    assert(ryz_i2c_scan_cancel(id + 1) == ESP_ERR_INVALID_STATE);
    assert(ryz_i2c_scan_cancel(id) == ESP_OK);
    assert(ryz_i2c_scan_cancel(id) == ESP_OK);
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_I2C_SCAN_RELEASING && !snapshot.finished_ms);
    assert(pthread_mutex_lock(&gate) == 0);
    release_end = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    assert(pthread_mutex_unlock(&gate) == 0);
    run_to_idle();
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_I2C_SCAN_CANCELLED && !snapshot.resources_held);
    assert(snapshot.cleanup_error == ESP_OK && snapshot.error == ESP_OK);
    assert(bus_ends == 1 && probes == (queued_cancel ? 0 : 112));
}

static void held_restart(void)
{
    nack_fixture();
    uint32_t id, revision;
    ryz_i2c_scan_config_t custom = {2, 3, 400000};
    assert(ryz_i2c_scan_configure(&custom, 1, &revision) == ESP_OK);
    end_error = ESP_ERR_TIMEOUT;
    assert(ryz_i2c_scan_start(&id) == ESP_OK && id == 1);
    run_to_idle();
    ryz_i2c_scan_snapshot_t snapshot;
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_I2C_SCAN_FAILED && snapshot.error == ESP_ERR_TIMEOUT);
    assert(snapshot.cleanup_error == ESP_ERR_TIMEOUT && snapshot.resources_held);
    assert(snapshot.nack_count == 112 && snapshot.probe_calls == 112);
    pause_worker();
    end_error = ESP_OK;
    memset(attempts, 0, sizeof(attempts));
    assert(ryz_i2c_scan_start(&id) == ESP_OK && id == 2);
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_I2C_SCAN_QUEUED && snapshot.resources_held);
    assert(snapshot.cleanup_error == ESP_ERR_TIMEOUT && snapshot.scan_config_revision == 2);
    assert(board_inits == 1 && bus_ends == 1 && probes == 112 && begin_recoveries == 0);
    run_to_idle();
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_I2C_SCAN_COMPLETED && snapshot.scan_id == 2);
    assert(snapshot.error == ESP_OK && snapshot.cleanup_error == ESP_OK && !snapshot.resources_held);
    assert(snapshot.nack_count == 112 && board_inits == 2 && bus_ends == 2);
    assert(probes == 224 && begin_recoveries == 1 && platform_starts == 1);
}

static void configure_startup(void)
{
    ryz_i2c_scan_config_t custom = {2, 3, 400000};
    uint32_t revision;
    platform_init_error = ESP_ERR_NO_MEM;
    assert(ryz_i2c_scan_configure(&custom, 1, &revision) == ESP_ERR_NO_MEM && !revision);
    assert(platform_starts == 0 && platform_deinits == 0);
    platform_init_error = ESP_OK;
    platform_start_error = ESP_ERR_NO_MEM;
    assert(ryz_i2c_scan_configure(&custom, 1, &revision) == ESP_ERR_NO_MEM && !revision);
    assert(platform_starts == 1 && platform_deinits == 1);
    ryz_i2c_scan_snapshot_t snapshot;
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_ERR_INVALID_STATE);
    assert(!snapshot.config_revision && !snapshot.scan_id);
    platform_start_error = ESP_OK;
    assert(ryz_i2c_scan_configure(&custom, 1, &revision) == ESP_OK && revision == 2);
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.config_revision == 2 && snapshot.phase == RYZ_I2C_SCAN_IDLE);
    assert(snapshot.config.sda == 2 && !snapshot.scan_config_revision && !snapshot.scan_id);
    assert(board_inits == 0 && bus_ends == 0 && probes == 0);
}

typedef struct {
    ryz_i2c_scan_config_t config;
    uint32_t expected, revision;
    esp_err_t error;
    bool race;
} configure_call_t;
static void *configure_entry(void *context)
{
    configure_call_t *call = context;
    if (call->race) {
        assert(pthread_mutex_lock(&gate) == 0);
        ++racers_ready;
        assert(pthread_cond_broadcast(&changed) == 0);
        while (!race_go) timed_wait();
        assert(pthread_mutex_unlock(&gate) == 0);
    }
    call->error = ryz_i2c_scan_configure(&call->config, call->expected, &call->revision);
    return NULL;
}
static void configure_concurrent(void)
{
    block_start = true;
    configure_call_t first = {.config = {2, 3, 400000}, .expected = 1};
    pthread_t creator;
    assert(pthread_create(&creator, NULL, configure_entry, &first) == 0);
    assert(pthread_mutex_lock(&gate) == 0);
    while (!start_entered) timed_wait();
    assert(pthread_mutex_unlock(&gate) == 0);
    uint32_t id = 999, revision = 999;
    assert(ryz_i2c_scan_start(&id) == ESP_ERR_INVALID_STATE && !id);
    assert(ryz_i2c_scan_configure(&first.config, 1, &revision) == ESP_ERR_INVALID_STATE && !revision);
    ryz_i2c_scan_snapshot_t snapshot;
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_ERR_INVALID_STATE && !snapshot.config_revision);
    assert(pthread_mutex_lock(&gate) == 0);
    release_start = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    assert(pthread_mutex_unlock(&gate) == 0);
    assert(pthread_join(creator, NULL) == 0);
    assert(first.error == ESP_OK && first.revision == 2);
    configure_call_t calls[2] = {
        {.config = {4, 5, 100000}, .expected = 2, .race = true},
        {.config = {5, 4, 400000}, .expected = 2, .race = true},
    };
    pthread_t racers[2];
    for (unsigned i = 0; i < 2; ++i)
        assert(pthread_create(&racers[i], NULL, configure_entry, &calls[i]) == 0);
    assert(pthread_mutex_lock(&gate) == 0);
    while (racers_ready != 2) timed_wait();
    race_go = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    assert(pthread_mutex_unlock(&gate) == 0);
    for (unsigned i = 0; i < 2; ++i) assert(pthread_join(racers[i], NULL) == 0);
    unsigned winner = calls[0].error == ESP_OK ? 0 : 1;
    unsigned loser = 1 - winner;
    assert(calls[winner].error == ESP_OK && calls[winner].revision == 3);
    assert(calls[loser].error == ESP_ERR_INVALID_STATE && !calls[loser].revision);
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.config_revision == 3 && snapshot.config.sda == calls[winner].config.sda);
    assert(snapshot.config.scl == calls[winner].config.scl && snapshot.config.hz == calls[winner].config.hz);
    assert(snapshot.phase == RYZ_I2C_SCAN_IDLE && !snapshot.scan_config_revision);
    assert(pair_checks == 2 && platform_starts == 1 && board_inits == 0 && probes == 0 && bus_ends == 0);
    assert(ryz_i2c_scan_start(&id) == ESP_OK && id == 1);
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.scan_config_revision == 3 && snapshot.scan_config.sda == calls[winner].config.sda);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "startup")) startup();
    else if (!strcmp(argv[1], "complete")) complete();
    else if (!strcmp(argv[1], "busy")) busy();
    else if (!strcmp(argv[1], "mixed")) mixed();
    else if (!strcmp(argv[1], "cancel_queued")) cancel_queued();
    else if (!strcmp(argv[1], "cancel_inflight")) cancel_inflight();
    else if (!strcmp(argv[1], "cancel_gap")) cancel_wait(false);
    else if (!strcmp(argv[1], "cancel_retry")) cancel_wait(true);
    else if (!strcmp(argv[1], "init_failure")) init_failure();
    else if (!strcmp(argv[1], "concurrent_start")) concurrent_start();
    else if (!strcmp(argv[1], "configure")) configure();
    else if (!strcmp(argv[1], "cleanup_retry")) cleanup_retry();
    else if (!strcmp(argv[1], "configure_history")) configure_history();
    else if (!strcmp(argv[1], "release_gate")) release_gate(false);
    else if (!strcmp(argv[1], "queued_release_gate")) release_gate(true);
    else if (!strcmp(argv[1], "held_restart")) held_restart();
    else if (!strcmp(argv[1], "configure_startup")) configure_startup();
    else if (!strcmp(argv[1], "configure_concurrent")) configure_concurrent();
    else abort();
    stop_worker();
    printf("I2C_SCAN_PASS %s\n", argv[1]);
    return 0;
}
