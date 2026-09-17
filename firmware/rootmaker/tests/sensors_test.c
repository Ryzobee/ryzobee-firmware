#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ryz_sensors.h"
#include "ryz_sensors_platform.h"

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker;
static bool created, stopping;
static unsigned permits, cycles;
static void (*worker_function)(void *);
static _Thread_local unsigned lock_depth;
static int64_t now_ms;
static uint32_t wait_ms;
static unsigned platform_inits, platform_starts, platform_deinits;
static esp_err_t platform_init_error, platform_start_error;
static bool block_start, start_entered, release_start;
static bool block_read, read_entered, release_read;
static unsigned driver_inits, driver_reads, driver_infos;
static esp_err_t init_error, read_error, info_error;
static ryz_imu_info_t driver_info;
static ryz_imu_sample_t driver_sample;

static void timed_wait(void)
{
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 3;
    assert(pthread_cond_timedwait(&changed, &gate, &deadline) == 0);
}

static void take_permit(void)
{
    while (!permits && !stopping) timed_wait();
    if (stopping) {
        assert(pthread_mutex_unlock(&gate) == 0);
        pthread_exit(NULL);
    }
    --permits;
}

static void *worker_entry(void *unused)
{
    (void)unused;
    assert(pthread_mutex_lock(&gate) == 0);
    take_permit();
    assert(pthread_mutex_unlock(&gate) == 0);
    worker_function(NULL);
    abort();
}

esp_err_t ryz_sensors_platform_init(void)
{
    ++platform_inits;
    return platform_init_error;
}
void ryz_sensors_platform_deinit(void) { ++platform_deinits; }
esp_err_t ryz_sensors_platform_start(void (*entry)(void *))
{
    ++platform_starts;
    if (platform_start_error != ESP_OK) return platform_start_error;
    assert(pthread_mutex_lock(&gate) == 0);
    start_entered = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    while (block_start && !release_start) timed_wait();
    assert(pthread_mutex_unlock(&gate) == 0);
    assert(!created);
    worker_function = entry;
    assert(pthread_create(&worker, NULL, worker_entry, NULL) == 0);
    created = true;
    return ESP_OK;
}
void ryz_sensors_platform_lock(void)
{
    assert(lock_depth == 0);
    assert(pthread_mutex_lock(&cache_lock) == 0);
    ++lock_depth;
}
void ryz_sensors_platform_unlock(void)
{
    assert(lock_depth == 1);
    --lock_depth;
    assert(pthread_mutex_unlock(&cache_lock) == 0);
}
int64_t ryz_sensors_platform_now_ms(void) { return now_ms; }
void ryz_sensors_platform_wait_ms(uint32_t milliseconds)
{
    assert(lock_depth == 0);
    assert(milliseconds > 0 && milliseconds <= 5000);
    assert(pthread_mutex_lock(&gate) == 0);
    wait_ms = milliseconds;
    ++cycles;
    assert(pthread_cond_broadcast(&changed) == 0);
    take_permit();
    assert(pthread_mutex_unlock(&gate) == 0);
}

static void driver_call(void)
{
    assert(pthread_equal(pthread_self(), worker));
    assert(lock_depth == 0); /* No snapshot lock spans a physical transaction. */
}
esp_err_t ryz_imu_init(void)
{
    driver_call();
    ++driver_inits;
    return init_error;
}
esp_err_t ryz_imu_read_sample(ryz_imu_sample_t *out)
{
    driver_call();
    ++driver_reads;
    assert(pthread_mutex_lock(&gate) == 0);
    read_entered = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    while (block_read && !release_read) timed_wait();
    assert(pthread_mutex_unlock(&gate) == 0);
    *out = driver_sample; /* Poison on error exercises clearing by the consumer. */
    return read_error;
}
esp_err_t ryz_imu_get_info(ryz_imu_info_t *out)
{
    driver_call();
    ++driver_infos;
    *out = driver_info;
    return info_error;
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
    ryz_sensors_snapshot_t snapshot;
    memset(&snapshot, 0xa5, sizeof(snapshot));
    assert(ryz_sensors_get_snapshot(NULL) == ESP_ERR_INVALID_ARG);
    assert(ryz_sensors_get_snapshot(&snapshot) == ESP_ERR_INVALID_STATE);
    assert(!snapshot.started && !snapshot.sample_valid && !snapshot.ever_sampled);
    platform_init_error = ESP_ERR_NO_MEM;
    assert(ryz_sensors_start() == ESP_ERR_NO_MEM);
    assert(platform_inits == 1 && platform_starts == 0 && platform_deinits == 0);
    platform_init_error = ESP_OK;
    platform_start_error = ESP_ERR_NO_MEM;
    assert(ryz_sensors_start() == ESP_ERR_NO_MEM);
    assert(platform_inits == 2 && platform_starts == 1 && platform_deinits == 1);
    platform_start_error = ESP_OK;
    assert(ryz_sensors_start() == ESP_OK);
    assert(ryz_sensors_start() == ESP_OK);
    assert(platform_inits == 3 && platform_starts == 2 && platform_deinits == 1);
    assert(ryz_sensors_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.started && !snapshot.initialized && !snapshot.ever_sampled);
    assert(driver_inits == 0 && driver_reads == 0 && driver_infos == 0);
}

static void cycle_at(int64_t timestamp)
{
    assert(pthread_mutex_lock(&gate) == 0);
    unsigned target = cycles + 1;
    now_ms = timestamp;
    ++permits;
    assert(pthread_cond_broadcast(&changed) == 0);
    while (cycles < target) timed_wait();
    assert(pthread_mutex_unlock(&gate) == 0);
}

static void valid_fixture(void)
{
    driver_info = (ryz_imu_info_t){.ready = true, .address = 0x19,
        .who_am_i = 0x44, .resolution_bits = 14, .full_scale_g = 2, .odr_hz = 25};
    driver_sample = (ryz_imu_sample_t){.x_mg = 100, .y_mg = -200,
        .z_mg = 980, .timestamp_us = 1234000, .sequence = 7};
}

static void sampling(void)
{
    valid_fixture();
    assert(ryz_sensors_start() == ESP_OK);
    cycle_at(1240);
    ryz_sensors_snapshot_t snapshot;
    assert(ryz_sensors_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.started && snapshot.initialized && snapshot.sample_valid);
    assert(snapshot.ever_sampled && snapshot.sample_count == 1);
    assert(snapshot.imu.address == 0x19 && snapshot.imu.who_am_i == 0x44);
    assert(snapshot.sample.x_mg == 100 && snapshot.sample.y_mg == -200);
    assert(snapshot.sample.z_mg == 980 && snapshot.sample.sequence == 7);
    assert(snapshot.sample.timestamp_us == 1234000);
    assert(snapshot.last_sampled_ms == 1234 && snapshot.sample_age_ms == 6);
    assert(snapshot.last_error == ESP_OK && snapshot.error_count == 0);
    assert(wait_ms == 40 && driver_inits == 1 && driver_reads == 1);
    unsigned reads = driver_reads, infos = driver_infos;
    for (unsigned i = 0; i < 100; ++i) {
        snapshot.sample.x_mg = 5000;
        assert(ryz_sensors_get_snapshot(&snapshot) == ESP_OK);
        assert(snapshot.sample.x_mg == 100);
    }
    assert(driver_reads == reads && driver_infos == infos);
    read_error = ESP_ERR_NOT_FINISHED;
    driver_sample.x_mg = 9000;
    driver_sample.timestamp_us = 1400000;
    cycle_at(1400);
    assert(ryz_sensors_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.initialized && snapshot.sample_valid && snapshot.ever_sampled);
    assert(snapshot.sample.x_mg == 100 && snapshot.sample.timestamp_us == 1234000);
    assert(snapshot.last_sampled_ms == 1234 && snapshot.sample_age_ms == 166);
    assert(snapshot.sample_count == 1 && snapshot.no_data_count == 1);
    assert(snapshot.last_error == ESP_OK && snapshot.error_count == 0);
    assert(snapshot.init_attempts == 1 && snapshot.read_attempts == 2);
}

static void watchdog(void)
{
    valid_fixture();
    assert(ryz_sensors_start() == ESP_OK);
    cycle_at(1240);
    read_error = ESP_ERR_NOT_FINISHED;
    cycle_at(2239);
    ryz_sensors_snapshot_t snapshot;
    assert(ryz_sensors_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.sample_valid && snapshot.error_count == 0 && wait_ms == 40);
    cycle_at(2240);
    assert(ryz_sensors_get_snapshot(&snapshot) == ESP_OK);
    assert(!snapshot.initialized && !snapshot.sample_valid && snapshot.ever_sampled);
    assert(!snapshot.imu.ready);
    assert(snapshot.sample.x_mg == 0 && snapshot.sample.y_mg == 0);
    assert(snapshot.sample.z_mg == 0 && snapshot.sample.timestamp_us == 0);
    assert(snapshot.last_sampled_ms == 1234 && snapshot.sample_count == 1);
    assert(snapshot.last_error == ESP_ERR_TIMEOUT && snapshot.error_count == 1);
    assert(snapshot.last_error_ms == 2240 && wait_ms == 5000);
    cycle_at(7240);
    assert(ryz_sensors_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.initialized && !snapshot.sample_valid && snapshot.ever_sampled);
    assert(snapshot.init_attempts == 2 && driver_inits == 2);
    assert(snapshot.last_sampled_ms == 1234 && snapshot.last_error == ESP_OK);
    /* A recovered generation has its own one-second settling/no-data budget. */
    cycle_at(8239);
    assert(ryz_sensors_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.initialized && snapshot.error_count == 1);
    cycle_at(8240);
    assert(ryz_sensors_get_snapshot(&snapshot) == ESP_OK);
    assert(!snapshot.initialized && snapshot.error_count == 2 && wait_ms == 5000);
}

static void *start_entry(void *result)
{
    *(esp_err_t *)result = ryz_sensors_start();
    return NULL;
}

static void concurrent_start(void)
{
    block_start = true;
    pthread_t starter;
    esp_err_t result = ESP_FAIL;
    assert(pthread_create(&starter, NULL, start_entry, &result) == 0);
    assert(pthread_mutex_lock(&gate) == 0);
    while (!start_entered) timed_wait();
    assert(pthread_mutex_unlock(&gate) == 0);
    ryz_sensors_snapshot_t snapshot;
    memset(&snapshot, 0xa5, sizeof(snapshot));
    assert(ryz_sensors_start() == ESP_ERR_INVALID_STATE);
    assert(ryz_sensors_get_snapshot(&snapshot) == ESP_ERR_INVALID_STATE);
    assert(!snapshot.started && !snapshot.initialized && !snapshot.sample_valid);
    assert(pthread_mutex_lock(&gate) == 0);
    release_start = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    assert(pthread_mutex_unlock(&gate) == 0);
    assert(pthread_join(starter, NULL) == 0);
    assert(result == ESP_OK && platform_starts == 1);
    assert(ryz_sensors_start() == ESP_OK);
    assert(ryz_sensors_get_snapshot(&snapshot) == ESP_OK && snapshot.started);
    assert(driver_inits == 0 && driver_reads == 0);
}

static void blocked_read(void)
{
    valid_fixture();
    block_read = true;
    assert(ryz_sensors_start() == ESP_OK);
    assert(pthread_mutex_lock(&gate) == 0);
    now_ms = 1240;
    ++permits;
    assert(pthread_cond_broadcast(&changed) == 0);
    while (!read_entered) timed_wait();
    assert(pthread_mutex_unlock(&gate) == 0);
    /* The real worker is inside the physical dependency, not between cycles. */
    ryz_sensors_snapshot_t snapshot;
    for (unsigned i = 0; i < 100; ++i) {
        assert(ryz_sensors_get_snapshot(&snapshot) == ESP_OK);
        assert(snapshot.started && !snapshot.sample_valid && !snapshot.ever_sampled);
    }
    assert(driver_reads == 1 && driver_infos == 0);
    assert(pthread_mutex_lock(&gate) == 0);
    release_read = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    while (cycles < 1) timed_wait();
    assert(pthread_mutex_unlock(&gate) == 0);
    assert(ryz_sensors_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.sample_valid && snapshot.sample.x_mg == 100);
}

static void initial_failure(void)
{
    valid_fixture();
    init_error = ESP_ERR_NOT_FOUND;
    assert(ryz_sensors_start() == ESP_OK);
    cycle_at(100);
    ryz_sensors_snapshot_t snapshot;
    assert(ryz_sensors_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.started && !snapshot.initialized && !snapshot.imu.ready);
    assert(!snapshot.sample_valid && !snapshot.ever_sampled);
    assert(snapshot.last_error == ESP_ERR_NOT_FOUND && snapshot.error_count == 1);
    assert(snapshot.last_error_ms == 100 && wait_ms == 5000 && driver_reads == 0);
    init_error = ESP_ERR_TIMEOUT; /* Timeout is not absence or NO_DATA. */
    cycle_at(5100);
    assert(ryz_sensors_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.last_error == ESP_ERR_TIMEOUT && snapshot.error_count == 2);
    assert(driver_reads == 0 && snapshot.init_attempts == 2 && wait_ms == 5000);
    init_error = ESP_OK;
    read_error = ESP_ERR_NOT_FINISHED;
    cycle_at(10100);
    assert(ryz_sensors_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.initialized && !snapshot.sample_valid && !snapshot.ever_sampled);
    assert(snapshot.last_sampled_ms == 0 && snapshot.sample.timestamp_us == 0);
    assert(snapshot.last_error == ESP_OK && snapshot.error_count == 2);
    assert(snapshot.no_data_count == 1 && wait_ms == 40);
    cycle_at(11099);
    assert(ryz_sensors_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.initialized && !snapshot.ever_sampled && snapshot.error_count == 2);
    cycle_at(11100);
    assert(ryz_sensors_get_snapshot(&snapshot) == ESP_OK);
    assert(!snapshot.initialized && !snapshot.ever_sampled && !snapshot.imu.ready);
    assert(snapshot.error_count == 3 && snapshot.last_error == ESP_ERR_TIMEOUT);
    assert(snapshot.last_sampled_ms == 0 && snapshot.sample_count == 0);
    assert(wait_ms == 5000 && driver_inits == 3);
}

static void transfer_failure(void)
{
    valid_fixture();
    assert(ryz_sensors_start() == ESP_OK);
    cycle_at(1240);
    read_error = ESP_FAIL;
    driver_sample.x_mg = 9000;
    driver_sample.timestamp_us = 1280000;
    cycle_at(1280);
    ryz_sensors_snapshot_t snapshot;
    assert(ryz_sensors_get_snapshot(&snapshot) == ESP_OK);
    assert(!snapshot.initialized && !snapshot.sample_valid && !snapshot.imu.ready);
    assert(snapshot.ever_sampled && snapshot.last_sampled_ms == 1234);
    assert(snapshot.sample.x_mg == 0 && snapshot.sample.timestamp_us == 0);
    assert(snapshot.sample_count == 1 && snapshot.last_error == ESP_FAIL);
    assert(snapshot.error_count == 1 && wait_ms == 5000);
    read_error = ESP_ERR_NOT_FINISHED;
    cycle_at(6280);
    assert(ryz_sensors_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.initialized && !snapshot.sample_valid && snapshot.ever_sampled);
    assert(snapshot.last_sampled_ms == 1234 && snapshot.error_count == 1);
    read_error = ESP_OK;
    driver_sample = (ryz_imu_sample_t){.x_mg = -600, .y_mg = 500,
        .z_mg = 700, .timestamp_us = 6320000, .sequence = 9};
    cycle_at(6320);
    assert(ryz_sensors_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.sample_valid && snapshot.sample.x_mg == -600);
    assert(snapshot.last_sampled_ms == 6320 && snapshot.sample_count == 2);
    assert(snapshot.sample.sequence == 9 && snapshot.sample_age_ms == 0);
    assert(snapshot.init_attempts == 2 && snapshot.last_error == ESP_OK);
    read_error = ESP_ERR_NOT_FINISHED;
    info_error = ESP_ERR_INVALID_STATE;
    cycle_at(6360);
    assert(ryz_sensors_get_snapshot(&snapshot) == ESP_OK);
    assert(!snapshot.initialized && !snapshot.sample_valid && !snapshot.imu.ready);
    assert(snapshot.imu.address == 0 && snapshot.imu.who_am_i == 0);
    assert(snapshot.last_error == ESP_ERR_INVALID_STATE && snapshot.error_count == 2);
    assert(snapshot.last_sampled_ms == 6320 && wait_ms == 5000);
    /* Preserve the primary physical failure if diagnostic copying also fails. */
    init_error = ESP_ERR_NOT_FOUND;
    cycle_at(11360);
    assert(ryz_sensors_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.last_error == ESP_ERR_NOT_FOUND && snapshot.error_count == 3);
    assert(snapshot.sample_count == 2 && snapshot.ever_sampled);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "startup")) startup();
    else if (!strcmp(argv[1], "sampling")) sampling();
    else if (!strcmp(argv[1], "watchdog")) watchdog();
    else if (!strcmp(argv[1], "concurrent_start")) concurrent_start();
    else if (!strcmp(argv[1], "blocked_read")) blocked_read();
    else if (!strcmp(argv[1], "initial_failure")) initial_failure();
    else if (!strcmp(argv[1], "transfer_failure")) transfer_failure();
    else abort();
    stop_worker();
    printf("SENSORS_PASS %s\n", argv[1]);
    return 0;
}
