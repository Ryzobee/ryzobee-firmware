#include "fake_platform.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>

#include "ryz_time_platform.h"

static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static ryz_time_platform_sync_sink_t s_sink;
static esp_err_t s_start_result;
static unsigned s_init_count;
static unsigned s_start_count;
static _Atomic int64_t s_now_us;
static bool s_start_sync;
static int64_t s_start_sync_unix;

void fake_time_platform_reset(void)
{
    s_sink = NULL;
    s_start_result = ESP_OK;
    s_init_count = 0;
    s_start_count = 0;
    s_now_us = 0;
    s_start_sync = false;
    s_start_sync_unix = 0;
}

void fake_time_platform_set_start_sync(bool enabled, int64_t unix_seconds)
{
    s_start_sync = enabled;
    s_start_sync_unix = unix_seconds;
}

void fake_time_platform_set_now_us(int64_t now_us)
{
    s_now_us = now_us;
}

int64_t ryz_time_platform_now_us(void)
{
    return s_now_us;
}

void fake_time_platform_set_start_result(esp_err_t result)
{
    s_start_result = result;
}

void fake_time_platform_emit_sync(int64_t unix_seconds)
{
    fake_time_platform_emit_precise_sync(unix_seconds, 0, atomic_load(&s_now_us));
}

void fake_time_platform_emit_precise_sync(int64_t unix_seconds, uint32_t subsecond_us,
                                         int64_t observed_monotonic_us)
{
    if (s_sink != NULL) s_sink(unix_seconds, subsecond_us, observed_monotonic_us);
}

void fake_time_platform_hold_snapshot(void)
{
    assert(pthread_mutex_lock(&s_mutex) == 0);
}

void fake_time_platform_release_snapshot(void)
{
    assert(pthread_mutex_unlock(&s_mutex) == 0);
}

unsigned fake_time_platform_init_count(void)
{
    return s_init_count;
}

unsigned fake_time_platform_start_count(void)
{
    return s_start_count;
}

void ryz_time_platform_snapshot_lock(void)
{
    pthread_mutex_lock(&s_mutex);
}

bool ryz_time_platform_snapshot_try_lock(void)
{
    return pthread_mutex_trylock(&s_mutex) == 0;
}

void ryz_time_platform_snapshot_unlock(void)
{
    pthread_mutex_unlock(&s_mutex);
}

esp_err_t ryz_time_platform_init(ryz_time_platform_sync_sink_t sink)
{
    ++s_init_count;
    if (s_sink != NULL) return ESP_ERR_INVALID_STATE;
    s_sink = sink;
    return ESP_OK;
}

esp_err_t ryz_time_platform_start(void)
{
    /* Real SDK enters TCP/IP synchronously; its callback needs this mutex. */
    assert(pthread_mutex_trylock(&s_mutex) == 0);
    assert(pthread_mutex_unlock(&s_mutex) == 0);
    ++s_start_count;
    if (s_start_sync) {
        s_start_sync = false;
        s_sink(s_start_sync_unix, 0, atomic_load(&s_now_us));
    }
    return s_start_result;
}
