#include "ryz_sensors.h"
#include "ryz_sensors_platform.h"

#include <stdatomic.h>
#include <string.h>

enum { STOPPED, STARTING, RUNNING };
static atomic_int s_lifecycle = ATOMIC_VAR_INIT(STOPPED);
static ryz_sensors_snapshot_t s_snapshot;

static void increment(uint32_t *counter)
{
    if (*counter != UINT32_MAX) ++*counter;
}

static void invalidate(ryz_sensors_snapshot_t *snapshot, esp_err_t error)
{
    snapshot->initialized = false;
    snapshot->imu.ready = false;
    snapshot->sample_valid = false;
    memset(&snapshot->sample, 0, sizeof(snapshot->sample));
    snapshot->last_error = error;
    snapshot->last_error_ms = (uint64_t)ryz_sensors_platform_now_ms();
    increment(&snapshot->error_count);
}

static void sensor_task(void *unused)
{
    (void)unused;
    ryz_sensors_snapshot_t state = {.started = true};
    int64_t last_progress_ms = 0;
    for (;;) {
        esp_err_t error = ESP_OK;
        if (!state.initialized) {
            increment(&state.init_attempts);
            error = ryz_imu_init();
            state.initialized = error == ESP_OK;
            if (state.initialized) last_progress_ms = ryz_sensors_platform_now_ms();
        }
        if (error == ESP_OK) {
            ryz_imu_sample_t sample = {0};
            increment(&state.read_attempts);
            error = ryz_imu_read_sample(&sample);
            if (error == ESP_OK) {
                state.sample = sample;
                state.sample_valid = state.ever_sampled = true;
                state.last_sampled_ms = sample.timestamp_us / 1000;
                increment(&state.sample_count);
                last_progress_ms = ryz_sensors_platform_now_ms();
            } else if (error == ESP_ERR_NOT_FINISHED) {
                increment(&state.no_data_count);
                int64_t now = ryz_sensors_platform_now_ms();
                error = now >= last_progress_ms && now - last_progress_ms >= 1000 ?
                    ESP_ERR_TIMEOUT : ESP_OK;
            }
        }
        esp_err_t info_error = ryz_imu_get_info(&state.imu);
        if (info_error != ESP_OK) {
            memset(&state.imu, 0, sizeof(state.imu));
            if (error == ESP_OK) error = info_error;
        }
        if (error != ESP_OK) invalidate(&state, error);
        else state.last_error = ESP_OK;
        ryz_sensors_platform_lock();
        s_snapshot = state;
        ryz_sensors_platform_unlock();
        ryz_sensors_platform_wait_ms(error == ESP_OK ? 40 : 5000);
    }
}

esp_err_t ryz_sensors_start(void)
{
    int expected = STOPPED;
    if (!atomic_compare_exchange_strong(&s_lifecycle, &expected, STARTING)) {
        return expected == RUNNING ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = ryz_sensors_platform_init();
    if (error == ESP_OK) {
        memset(&s_snapshot, 0, sizeof(s_snapshot));
        s_snapshot.started = true;
        s_snapshot.last_error = ESP_ERR_INVALID_STATE;
        error = ryz_sensors_platform_start(sensor_task);
        if (error != ESP_OK) ryz_sensors_platform_deinit();
    }
    atomic_store_explicit(&s_lifecycle, error == ESP_OK ? RUNNING : STOPPED,
                          memory_order_release);
    return error;
}

esp_err_t ryz_sensors_get_snapshot(ryz_sensors_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (atomic_load_explicit(&s_lifecycle, memory_order_acquire) != RUNNING) {
        return ESP_ERR_INVALID_STATE;
    }
    ryz_sensors_platform_lock();
    *out = s_snapshot;
    ryz_sensors_platform_unlock();
    if (out->ever_sampled) {
        int64_t now = ryz_sensors_platform_now_ms();
        out->sample_age_ms = now > 0 && (uint64_t)now > out->last_sampled_ms ?
            (uint64_t)now - out->last_sampled_ms : 0;
    }
    return ESP_OK;
}
