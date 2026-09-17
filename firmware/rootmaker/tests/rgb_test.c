#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ryz_rgb.h"
#include "ryz_rgb_driver.h"
#include "ryz_rgb_platform.h"

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker;
static void (*entry_function)(void *);
static bool created, released, stopping, event, idle_blocked;
static bool block_driver, driver_entered, release_driver;
static bool block_start, start_entered, release_start, race_go;
static unsigned racers_ready;
static _Thread_local unsigned lock_depth;
static int64_t now_ms;
static esp_err_t init_error, start_error, driver_error;
static unsigned inits, starts, deinits, writes, idle_waits;
static ryz_rgb_color_t sent[128];
static unsigned cleanups;
static bool driver_resources, block_cleanup, cleanup_entered, release_cleanup;
static bool cleanup_keeps_resources;
static esp_err_t cleanup_error, driver_cleanup_error;

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
esp_err_t ryz_rgb_platform_init(void) { ++inits; return init_error; }
void ryz_rgb_platform_deinit(void) { ++deinits; }
esp_err_t ryz_rgb_platform_start(void (*entry)(void *))
{
    ++starts;
    if (start_error != ESP_OK) return start_error;
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
void ryz_rgb_platform_lock(void)
{
    assert(!lock_depth);
    assert(pthread_mutex_lock(&cache_mutex) == 0);
    ++lock_depth;
}
void ryz_rgb_platform_unlock(void)
{
    assert(lock_depth == 1);
    --lock_depth;
    assert(pthread_mutex_unlock(&cache_mutex) == 0);
}
void ryz_rgb_platform_wake(void)
{
    assert(pthread_mutex_lock(&gate) == 0);
    event = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    assert(pthread_mutex_unlock(&gate) == 0);
}
void ryz_rgb_platform_wait(void)
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
int64_t ryz_rgb_platform_now_ms(void)
{
    assert(pthread_mutex_lock(&gate) == 0);
    int64_t value = now_ms;
    assert(pthread_mutex_unlock(&gate) == 0);
    return value;
}
esp_err_t ryz_rgb_driver_write(ryz_rgb_color_t color)
{
    assert(pthread_equal(pthread_self(), worker) && !lock_depth);
    ryz_rgb_snapshot_t snapshot;
    assert(ryz_rgb_get_snapshot(&snapshot) == ESP_OK);
    assert((snapshot.phase == RYZ_RGB_SENDING || snapshot.phase == RYZ_RGB_CLEANING) && !snapshot.output_known);
    assert(writes < 128);
    sent[writes++] = color;
    driver_resources = true;
    assert(pthread_mutex_lock(&gate) == 0);
    driver_entered = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    while (block_driver && !release_driver && !stopping) timed_wait();
    check_stop();
    assert(pthread_mutex_unlock(&gate) == 0);
    return driver_error;
}
esp_err_t ryz_rgb_driver_cleanup(void)
{
    assert(pthread_equal(pthread_self(), worker) && !lock_depth);
    ++cleanups;
    assert(pthread_mutex_lock(&gate) == 0);
    cleanup_entered = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    while (block_cleanup && !release_cleanup && !stopping) timed_wait();
    check_stop();
    assert(pthread_mutex_unlock(&gate) == 0);
    driver_cleanup_error = cleanup_error;
    if (cleanup_error == ESP_OK && !cleanup_keeps_resources) driver_resources = false;
    return cleanup_error;
}
ryz_rgb_driver_status_t ryz_rgb_driver_get_status(void)
{
    assert(pthread_equal(pthread_self(), worker) && !lock_depth);
    return (ryz_rgb_driver_status_t){driver_resources, driver_cleanup_error};
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
static void assert_color(ryz_rgb_color_t value, uint8_t red, uint8_t green, uint8_t blue)
{
    assert(value.red == red && value.green == green && value.blue == blue);
}
static void startup(void)
{
    const ryz_rgb_color_t color = {255, 17, 83};
    ryz_rgb_snapshot_t snapshot;
    memset(&snapshot, 0xa5, sizeof(snapshot));
    assert(ryz_rgb_get_snapshot(NULL) == ESP_ERR_INVALID_ARG);
    assert(ryz_rgb_get_snapshot(&snapshot) == ESP_ERR_INVALID_STATE);
    assert(snapshot.phase == RYZ_RGB_IDLE && snapshot.request_id == 0);
    assert(!snapshot.output_known && snapshot.last_completed_id == 0);
    assert(ryz_rgb_submit(color, NULL) == ESP_ERR_INVALID_ARG && inits == 0);
    uint32_t id = 999;
    init_error = ESP_ERR_NO_MEM;
    assert(ryz_rgb_submit(color, &id) == ESP_ERR_NO_MEM && id == 0);
    assert(inits == 1 && starts == 0 && deinits == 0);
    init_error = ESP_OK;
    start_error = ESP_ERR_NO_MEM;
    assert(ryz_rgb_submit(color, &id) == ESP_ERR_NO_MEM && id == 0);
    assert(inits == 2 && starts == 1 && deinits == 1);
    start_error = ESP_OK;
    now_ms = 123;
    assert(ryz_rgb_submit(color, &id) == ESP_OK && id == 1);
    assert(ryz_rgb_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_RGB_QUEUED && snapshot.request_id == id);
    assert(snapshot.queued_ms == 123 && snapshot.finished_ms == 0);
    assert(!snapshot.output_known && snapshot.last_completed_id == 0);
    assert_color(snapshot.requested, 255, 17, 83);
    const ryz_rgb_color_t replacement = {1, 2, 3};
    assert(ryz_rgb_submit(replacement, &id) == ESP_ERR_INVALID_STATE && id == 0);
    assert(ryz_rgb_get_snapshot(&snapshot) == ESP_OK);
    assert_color(snapshot.requested, 255, 17, 83);
    assert(inits == 3 && starts == 2 && writes == 0);
}

static void run_to_idle_at(int64_t timestamp)
{
    assert(pthread_mutex_lock(&gate) == 0);
    now_ms = timestamp;
    released = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    while (!idle_blocked || event) timed_wait();
    assert(pthread_mutex_unlock(&gate) == 0);
}
static void pause_worker(void)
{
    assert(pthread_mutex_lock(&gate) == 0);
    assert(idle_blocked && !event);
    released = false;
    assert(pthread_mutex_unlock(&gate) == 0);
}
static void success_off(void)
{
    uint32_t id;
    const ryz_rgb_color_t color = {255, 17, 83};
    now_ms = 123;
    assert(ryz_rgb_submit(color, &id) == ESP_OK && id == 1);
    run_to_idle_at(200);
    ryz_rgb_snapshot_t snapshot;
    assert(ryz_rgb_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_RGB_COMPLETED && snapshot.output_known);
    assert(snapshot.request_id == id && snapshot.last_completed_id == id);
    assert(snapshot.error == ESP_OK && snapshot.queued_ms == 123 && snapshot.finished_ms == 200);
    assert_color(snapshot.last_completed_color, 255, 17, 83);
    assert(writes == 1);
    assert_color(sent[0], 255, 17, 83); /* No gamma, clipping or brightness transform. */
    pause_worker();
    const ryz_rgb_color_t black = {0, 0, 0};
    uint32_t next;
    assert(ryz_rgb_submit(black, &next) == ESP_OK && next == 2);
    assert(ryz_rgb_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_RGB_QUEUED && snapshot.output_known);
    assert(snapshot.last_completed_id == id && snapshot.finished_ms == 0);
    assert_color(snapshot.requested, 0, 0, 0);
    assert_color(snapshot.last_completed_color, 255, 17, 83);
    run_to_idle_at(300);
    assert(ryz_rgb_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_RGB_COMPLETED && snapshot.output_known);
    assert(snapshot.last_completed_id == next && snapshot.finished_ms == 300);
    assert_color(snapshot.last_completed_color, 0, 0, 0);
    assert(writes == 2 && inits == 1 && starts == 1);
    assert_color(sent[1], 0, 0, 0); /* Black must not be optimized into a no-op. */
}

static void sending_recovery(void)
{
    uint32_t first;
    const ryz_rgb_color_t color = {255, 17, 83}, black = {0, 0, 0};
    assert(ryz_rgb_submit(color, &first) == ESP_OK);
    run_to_idle_at(100);
    pause_worker();
    block_driver = true;
    driver_entered = false;
    driver_error = ESP_ERR_TIMEOUT;
    uint32_t second;
    assert(ryz_rgb_submit(black, &second) == ESP_OK && second == first + 1);
    ryz_rgb_snapshot_t snapshot;
    assert(ryz_rgb_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_RGB_QUEUED && snapshot.output_known);
    assert(pthread_mutex_lock(&gate) == 0);
    released = true;
    now_ms = 200;
    assert(pthread_cond_broadcast(&changed) == 0);
    while (!driver_entered) timed_wait();
    assert(pthread_mutex_unlock(&gate) == 0);
    for (unsigned i = 0; i < 100; ++i) {
        assert(ryz_rgb_get_snapshot(&snapshot) == ESP_OK);
        assert(snapshot.phase == RYZ_RGB_SENDING && !snapshot.output_known);
        assert(snapshot.request_id == second && snapshot.last_completed_id == first);
        assert(snapshot.finished_ms == 0 && snapshot.error == ESP_OK);
        assert_color(snapshot.requested, 0, 0, 0);
        assert_color(snapshot.last_completed_color, 255, 17, 83);
    }
    uint32_t rejected = 999;
    assert(ryz_rgb_submit(color, &rejected) == ESP_ERR_INVALID_STATE && rejected == 0);
    assert(writes == 2);
    assert(pthread_mutex_lock(&gate) == 0);
    release_driver = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    assert(pthread_mutex_unlock(&gate) == 0);
    run_to_idle_at(300);
    assert(ryz_rgb_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_RGB_FAILED && !snapshot.output_known);
    assert(snapshot.request_id == second && snapshot.last_completed_id == first);
    assert(snapshot.error == ESP_ERR_TIMEOUT);
    assert_color(snapshot.last_completed_color, 255, 17, 83);
    assert_color(sent[1], 0, 0, 0);
    pause_worker();
    driver_error = ESP_OK;
    block_driver = false;
    uint32_t third;
    assert(ryz_rgb_submit(black, &third) == ESP_OK && third == second + 1);
    assert(ryz_rgb_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_RGB_QUEUED && !snapshot.output_known);
    assert(snapshot.last_completed_id == first && snapshot.error == ESP_OK);
    run_to_idle_at(400);
    assert(ryz_rgb_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_RGB_COMPLETED && snapshot.output_known);
    assert(snapshot.last_completed_id == third && snapshot.finished_ms == 400);
    assert_color(snapshot.last_completed_color, 0, 0, 0);
    assert(writes == 3 && starts == 1 && inits == 1);
}

static void initial_failure(void)
{
    driver_error = ESP_ERR_NO_MEM;
    const ryz_rgb_color_t white = {255, 255, 255};
    uint32_t id;
    assert(ryz_rgb_submit(white, &id) == ESP_OK && id == 1);
    run_to_idle_at(50);
    ryz_rgb_snapshot_t snapshot;
    for (unsigned i = 0; i < 100; ++i) {
        assert(ryz_rgb_get_snapshot(&snapshot) == ESP_OK);
        assert(snapshot.phase == RYZ_RGB_FAILED && !snapshot.output_known);
        assert(snapshot.error == ESP_ERR_NO_MEM && snapshot.last_completed_id == 0);
        assert(snapshot.request_id == id && snapshot.finished_ms == 50);
    }
    assert(writes == 1); /* Idle worker does not re-send or assume an off frame. */
    pause_worker();
    driver_error = ESP_OK;
    uint32_t next;
    assert(ryz_rgb_submit(white, &next) == ESP_OK && next == id + 1);
    run_to_idle_at(75);
    assert(ryz_rgb_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_RGB_COMPLETED && snapshot.output_known);
    assert(snapshot.last_completed_id == next && snapshot.error == ESP_OK);
    assert_color(snapshot.last_completed_color, 255, 255, 255);
    assert(writes == 2 && starts == 1);
}

typedef struct {
    ryz_rgb_color_t color;
    uint32_t id;
    esp_err_t error;
    bool race;
} submitter_t;
static void *submit_entry(void *argument)
{
    submitter_t *submitter = argument;
    if (submitter->race) {
        assert(pthread_mutex_lock(&gate) == 0);
        ++racers_ready;
        assert(pthread_cond_broadcast(&changed) == 0);
        while (!race_go) timed_wait();
        assert(pthread_mutex_unlock(&gate) == 0);
    }
    submitter->error = ryz_rgb_submit(submitter->color, &submitter->id);
    return NULL;
}
static void concurrent_start(void)
{
    block_start = true;
    submitter_t first = {.color = {41, 42, 43}, .id = 999, .error = ESP_FAIL};
    pthread_t starter;
    assert(pthread_create(&starter, NULL, submit_entry, &first) == 0);
    assert(pthread_mutex_lock(&gate) == 0);
    while (!start_entered) timed_wait();
    assert(pthread_mutex_unlock(&gate) == 0);
    uint32_t id = 555;
    const ryz_rgb_color_t other = {1, 2, 3};
    assert(ryz_rgb_submit(other, &id) == ESP_ERR_INVALID_STATE && id == 0);
    ryz_rgb_snapshot_t snapshot;
    memset(&snapshot, 0xa5, sizeof(snapshot));
    assert(ryz_rgb_get_snapshot(&snapshot) == ESP_ERR_INVALID_STATE);
    assert(snapshot.phase == RYZ_RGB_IDLE && !snapshot.output_known && !snapshot.request_id);
    assert(pthread_mutex_lock(&gate) == 0);
    release_start = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    assert(pthread_mutex_unlock(&gate) == 0);
    assert(pthread_join(starter, NULL) == 0);
    assert(first.error == ESP_OK && first.id == 1 && starts == 1 && writes == 0);
    assert(ryz_rgb_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_RGB_QUEUED && snapshot.request_id == 1);
    assert_color(snapshot.requested, 41, 42, 43);
}

static void submit_race(void)
{
    submitter_t callers[2] = {
        {.color = {11, 12, 13}, .race = true, .id = 999},
        {.color = {21, 22, 23}, .race = true, .id = 999},
    };
    pthread_t threads[2];
    for (unsigned i = 0; i < 2; ++i) assert(pthread_create(&threads[i], NULL, submit_entry, &callers[i]) == 0);
    assert(pthread_mutex_lock(&gate) == 0);
    while (racers_ready != 2) timed_wait();
    race_go = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    assert(pthread_mutex_unlock(&gate) == 0);
    for (unsigned i = 0; i < 2; ++i) assert(pthread_join(threads[i], NULL) == 0);
    unsigned winner = callers[0].error == ESP_OK ? 0 : 1;
    unsigned loser = 1 - winner;
    assert(callers[winner].error == ESP_OK && callers[winner].id == 1);
    assert(callers[loser].error == ESP_ERR_INVALID_STATE && callers[loser].id == 0);
    ryz_rgb_snapshot_t snapshot;
    assert(ryz_rgb_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_RGB_QUEUED && snapshot.request_id == 1);
    assert_color(snapshot.requested, callers[winner].color.red,
                 callers[winner].color.green, callers[winner].color.blue);
    assert(inits == 1 && starts == 1 && writes == 0);
    run_to_idle_at(100);
    assert(writes == 1);
    assert_color(sent[0], callers[winner].color.red,
                 callers[winner].color.green, callers[winner].color.blue);
}

static void generations(void)
{
    const ryz_rgb_color_t color = {39, 73, 201};
    for (uint32_t i = 0; i < 64; ++i) {
        if (i) pause_worker();
        uint32_t id;
        assert(ryz_rgb_submit(color, &id) == ESP_OK && id == i + 1);
        run_to_idle_at((int64_t)i * 10 + 1);
        ryz_rgb_snapshot_t snapshot;
        assert(ryz_rgb_get_snapshot(&snapshot) == ESP_OK);
        assert(snapshot.phase == RYZ_RGB_COMPLETED && snapshot.output_known);
        assert(snapshot.request_id == id && snapshot.last_completed_id == id);
        assert(writes == i + 1 && snapshot.error == ESP_OK);
        assert_color(sent[i], 39, 73, 201);
    }
    assert(inits == 1 && starts == 1);
}
static void cleanup_queued(void)
{
    uint32_t id;
    assert(ryz_rgb_submit((ryz_rgb_color_t){15, 35, 75}, &id) == ESP_OK);
    assert(ryz_rgb_cleanup(id) == ESP_OK);
    ryz_rgb_snapshot_t snapshot;
    assert(ryz_rgb_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_RGB_CLEANING && snapshot.request_id == id);
    assert(ryz_rgb_cleanup(id) == ESP_OK && cleanups == 0 && writes == 0);
    run_to_idle_at(100);
    assert(ryz_rgb_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_RGB_CLEANED && !snapshot.resources_held);
    assert(snapshot.cleanup_error == ESP_OK && !snapshot.output_known && !snapshot.last_completed_id);
    assert(snapshot.finished_ms == 100 && cleanups == 1 && writes == 0);
    assert(ryz_rgb_cleanup(id) == ESP_OK && cleanups == 1);
}
static void cleanup_history(void)
{
    assert(ryz_rgb_cleanup(0) == ESP_ERR_INVALID_ARG);
    assert(ryz_rgb_cleanup(1) == ESP_ERR_INVALID_STATE && inits == 0);
    uint32_t id;
    assert(ryz_rgb_submit((ryz_rgb_color_t){15, 35, 75}, &id) == ESP_OK);
    run_to_idle_at(100);
    pause_worker();
    assert(ryz_rgb_cleanup(id + 1) == ESP_ERR_INVALID_STATE);
    assert(ryz_rgb_cleanup(id) == ESP_OK);
    ryz_rgb_snapshot_t snapshot;
    assert(ryz_rgb_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_RGB_CLEANING && snapshot.resources_held && snapshot.output_known);
    assert(snapshot.finished_ms == 0 && writes == 1 && cleanups == 0);
    run_to_idle_at(200);
    assert(ryz_rgb_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_RGB_CLEANED && !snapshot.resources_held && snapshot.output_known);
    assert(snapshot.last_completed_id == id && snapshot.error == ESP_OK);
    assert_color(snapshot.last_completed_color, 15, 35, 75);
    assert(writes == 1 && cleanups == 1);
    pause_worker();
    uint32_t next;
    assert(ryz_rgb_submit((ryz_rgb_color_t){95, 115, 135}, &next) == ESP_OK && next == id + 1);
    assert(ryz_rgb_cleanup(id) == ESP_ERR_INVALID_STATE);
    run_to_idle_at(300);
    assert(ryz_rgb_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_RGB_COMPLETED && snapshot.last_completed_id == next);
    assert_color(snapshot.last_completed_color, 95, 115, 135);
    assert(writes == 2 && cleanups == 1);
}
static void cleanup_inflight(bool write_fails)
{
    uint32_t id;
    block_driver = block_cleanup = true;
    driver_error = write_fails ? ESP_ERR_TIMEOUT : ESP_OK;
    driver_cleanup_error = write_fails ? ESP_ERR_INVALID_STATE : ESP_OK;
    assert(ryz_rgb_submit((ryz_rgb_color_t){15, 35, 75}, &id) == ESP_OK);
    assert(pthread_mutex_lock(&gate) == 0);
    released = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    while (!driver_entered) timed_wait();
    assert(pthread_mutex_unlock(&gate) == 0);
    assert(ryz_rgb_cleanup(id) == ESP_OK && ryz_rgb_cleanup(id) == ESP_OK);
    ryz_rgb_snapshot_t snapshot;
    for (unsigned i = 0; i < 100; ++i) {
        assert(ryz_rgb_get_snapshot(&snapshot) == ESP_OK);
        assert(snapshot.phase == RYZ_RGB_CLEANING && !snapshot.output_known);
        assert(!snapshot.finished_ms && writes == 1 && cleanups == 0);
    }
    uint32_t rejected = 999;
    assert(ryz_rgb_submit((ryz_rgb_color_t){0}, &rejected) == ESP_ERR_INVALID_STATE && rejected == 0);
    assert(pthread_mutex_lock(&gate) == 0);
    release_driver = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    while (!cleanup_entered) timed_wait();
    assert(pthread_mutex_unlock(&gate) == 0);
    assert(ryz_rgb_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_RGB_CLEANING && snapshot.resources_held && !snapshot.finished_ms);
    assert(snapshot.error == driver_error && snapshot.cleanup_error == driver_cleanup_error);
    assert(snapshot.output_known == !write_fails);
    assert(snapshot.last_completed_id == (write_fails ? 0 : id));
    assert(ryz_rgb_cleanup(id) == ESP_OK && cleanups == 1 && writes == 1);
    assert(pthread_mutex_lock(&gate) == 0);
    release_cleanup = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    assert(pthread_mutex_unlock(&gate) == 0);
    run_to_idle_at(300);
    assert(ryz_rgb_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_RGB_CLEANED && !snapshot.resources_held);
    assert(snapshot.cleanup_error == ESP_OK && snapshot.error == driver_error);
    assert(snapshot.output_known == !write_fails && cleanups == 1 && writes == 1);
}
static void cleanup_failure_retry(bool inconsistent_success)
{
    uint32_t id;
    assert(ryz_rgb_submit((ryz_rgb_color_t){25, 65, 95}, &id) == ESP_OK);
    run_to_idle_at(100);
    pause_worker();
    cleanup_error = inconsistent_success ? ESP_OK : ESP_FAIL;
    cleanup_keeps_resources = inconsistent_success;
    assert(ryz_rgb_cleanup(id) == ESP_OK);
    run_to_idle_at(200);
    ryz_rgb_snapshot_t snapshot;
    for (unsigned i = 0; i < 100; ++i) {
        assert(ryz_rgb_get_snapshot(&snapshot) == ESP_OK);
        assert(snapshot.phase == RYZ_RGB_FAILED && snapshot.resources_held && snapshot.output_known);
        assert(snapshot.cleanup_error == (inconsistent_success ? ESP_ERR_INVALID_STATE : ESP_FAIL));
        assert(snapshot.error == ESP_OK && snapshot.last_completed_id == id);
    }
    assert(cleanups == 1 && writes == 1);
    pause_worker();
    cleanup_error = ESP_OK;
    cleanup_keeps_resources = false;
    assert(ryz_rgb_cleanup(id) == ESP_OK);
    run_to_idle_at(300);
    assert(ryz_rgb_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_RGB_CLEANED && !snapshot.resources_held && snapshot.cleanup_error == ESP_OK);
    assert(snapshot.last_completed_id == id && snapshot.output_known);
    assert_color(snapshot.last_completed_color, 25, 65, 95);
    assert(cleanups == 2 && writes == 1);
}

static void *cleanup_entry(void *argument)
{
    submitter_t *caller = argument;
    assert(pthread_mutex_lock(&gate) == 0);
    ++racers_ready;
    assert(pthread_cond_broadcast(&changed) == 0);
    while (!race_go) timed_wait();
    assert(pthread_mutex_unlock(&gate) == 0);
    caller->error = ryz_rgb_cleanup(caller->id);
    return NULL;
}
static void cleanup_submit_race(void)
{
    uint32_t first;
    assert(ryz_rgb_submit((ryz_rgb_color_t){15, 35, 75}, &first) == ESP_OK);
    run_to_idle_at(100);
    pause_worker();
    submitter_t cleaner = {.id = first}, sender = {.color = {25, 45, 85}, .race = true};
    pthread_t threads[2];
    assert(pthread_create(&threads[0], NULL, cleanup_entry, &cleaner) == 0);
    assert(pthread_create(&threads[1], NULL, submit_entry, &sender) == 0);
    assert(pthread_mutex_lock(&gate) == 0);
    while (racers_ready != 2) timed_wait();
    race_go = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    assert(pthread_mutex_unlock(&gate) == 0);
    for (unsigned i = 0; i < 2; ++i) assert(pthread_join(threads[i], NULL) == 0);
    assert((cleaner.error == ESP_OK) != (sender.error == ESP_OK));
    ryz_rgb_snapshot_t snapshot;
    assert(ryz_rgb_get_snapshot(&snapshot) == ESP_OK);
    if (cleaner.error == ESP_OK) {
        assert(sender.error == ESP_ERR_INVALID_STATE && !sender.id);
        assert(snapshot.phase == RYZ_RGB_CLEANING && snapshot.request_id == first);
    } else {
        assert(cleaner.error == ESP_ERR_INVALID_STATE && sender.id == first + 1);
        assert(snapshot.phase == RYZ_RGB_QUEUED && snapshot.request_id == sender.id);
    }
    run_to_idle_at(200);
    assert(ryz_rgb_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == (cleaner.error == ESP_OK ? RYZ_RGB_CLEANED : RYZ_RGB_COMPLETED));
    assert(writes == (cleaner.error == ESP_OK ? 1U : 2U));
    assert(cleanups == (cleaner.error == ESP_OK ? 1U : 0U));
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "startup")) startup();
    else if (!strcmp(argv[1], "success_off")) success_off();
    else if (!strcmp(argv[1], "sending_recovery")) sending_recovery();
    else if (!strcmp(argv[1], "initial_failure")) initial_failure();
    else if (!strcmp(argv[1], "concurrent_start")) concurrent_start();
    else if (!strcmp(argv[1], "submit_race")) submit_race();
    else if (!strcmp(argv[1], "generations")) generations();
    else if (!strcmp(argv[1], "cleanup_queued")) cleanup_queued();
    else if (!strcmp(argv[1], "cleanup_history")) cleanup_history();
    else if (!strcmp(argv[1], "cleanup_inflight")) cleanup_inflight(false);
    else if (!strcmp(argv[1], "cleanup_inflight_failed")) cleanup_inflight(true);
    else if (!strcmp(argv[1], "cleanup_failure_retry")) cleanup_failure_retry(false);
    else if (!strcmp(argv[1], "cleanup_inconsistent")) cleanup_failure_retry(true);
    else if (!strcmp(argv[1], "cleanup_submit_race")) cleanup_submit_race();
    else abort();
    stop_worker();
    printf("RGB_PASS %s\n", argv[1]);
    return 0;
}
