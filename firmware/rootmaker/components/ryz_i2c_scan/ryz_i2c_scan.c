#include "ryz_i2c_scan.h"
#include "ryz_i2c_scan_bus.h"
#include "ryz_i2c_scan_platform.h"
#include "ryz_tool_pins.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>

enum { STOPPED, STARTING, READY };
static atomic_int s_lifecycle = ATOMIC_VAR_INIT(STOPPED);
static ryz_i2c_scan_snapshot_t s_snapshot;
static uint32_t s_last_id;
static bool s_cancel_requested;

static bool active(ryz_i2c_scan_phase_t phase)
{
    return phase == RYZ_I2C_SCAN_QUEUED || phase == RYZ_I2C_SCAN_RUNNING ||
           phase == RYZ_I2C_SCAN_CANCELLING || phase == RYZ_I2C_SCAN_RELEASING;
}

/* All helpers ending in _locked run with only the snapshot mutex held. */
static void finish_locked(ryz_i2c_scan_phase_t phase, esp_err_t error)
{
    s_snapshot.phase = phase;
    s_snapshot.error = error;
    s_snapshot.current_address = 0;
    s_snapshot.finished_ms = ryz_i2c_scan_platform_now_ms();
}

static bool running_locked(uint32_t id)
{
    return s_snapshot.scan_id == id && s_snapshot.phase == RYZ_I2C_SCAN_RUNNING &&
           !s_cancel_requested;
}

static void release_scan(uint32_t id, esp_err_t error)
{
    ryz_i2c_scan_platform_lock();
    s_snapshot.phase = RYZ_I2C_SCAN_RELEASING;
    s_snapshot.error = error;
    s_snapshot.current_address = 0;
    ryz_i2c_scan_platform_unlock();

    /* Even queued cancellation and failed begin must acknowledge teardown.
     * No new scan/config can enter until the real end result is published. */
    esp_err_t cleanup = ryz_i2c_scan_bus_end();
    bool held = ryz_i2c_scan_bus_resources_held();
    if (cleanup == ESP_OK && held) cleanup = ESP_ERR_INVALID_STATE;
    ryz_i2c_scan_platform_lock();
    if (s_snapshot.scan_id == id) {
        s_snapshot.cleanup_error = cleanup;
        s_snapshot.resources_held = held;
        if (error != ESP_OK || cleanup != ESP_OK) {
            finish_locked(RYZ_I2C_SCAN_FAILED, error != ESP_OK ? error : cleanup);
        } else {
            finish_locked(s_cancel_requested ? RYZ_I2C_SCAN_CANCELLED :
                          RYZ_I2C_SCAN_COMPLETED, ESP_OK);
        }
    }
    ryz_i2c_scan_platform_unlock();
}

static void record_locked(uint8_t address, esp_err_t error)
{
    ryz_i2c_scan_result_t result;
    if (error == ESP_OK) {
        result = RYZ_I2C_SCAN_ACK;
        ++s_snapshot.ack_count;
    } else if (error == ESP_ERR_NOT_FOUND) {
        result = RYZ_I2C_SCAN_NACK;
        ++s_snapshot.nack_count;
    } else if (error == ESP_ERR_NOT_FINISHED) {
        result = RYZ_I2C_SCAN_BUSY;
        ++s_snapshot.busy_count;
    } else if (error == ESP_ERR_TIMEOUT) {
        result = RYZ_I2C_SCAN_TIMEOUT;
        ++s_snapshot.timeout_count;
    } else {
        result = RYZ_I2C_SCAN_ERROR;
        ++s_snapshot.error_count;
    }
    s_snapshot.results[address] = (uint8_t)result;
    s_snapshot.errors[address] = error;
    ++s_snapshot.completed_addresses;
}

static void scan(uint32_t id, const ryz_i2c_scan_config_t *config)
{
    ryz_i2c_scan_platform_lock();
    bool running = running_locked(id);
    ryz_i2c_scan_platform_unlock();
    if (!running) {
        release_scan(id, ESP_OK);
        return;
    }
    esp_err_t error = ryz_i2c_scan_bus_begin(config);
    bool held = ryz_i2c_scan_bus_resources_held();
    ryz_i2c_scan_platform_lock();
    s_snapshot.resources_held = held;
    if (error == ESP_OK) s_snapshot.cleanup_error = ESP_OK;
    running = running_locked(id);
    ryz_i2c_scan_platform_unlock();
    if (!running || error != ESP_OK) {
        release_scan(id, error);
        return;
    }

    for (uint8_t address = RYZ_I2C_SCAN_FIRST; address <= RYZ_I2C_SCAN_LAST; ++address) {
        for (unsigned attempt = 0; attempt < RYZ_I2C_SCAN_ATTEMPTS; ++attempt) {
            ryz_i2c_scan_platform_lock();
            running = running_locked(id);
            if (running) {
                s_snapshot.current_address = address;
                /* Admission of this call is the in-flight linearization point.
                 * Cancellation after this point waits for its return. */
                ++s_snapshot.probe_calls;
            }
            ryz_i2c_scan_platform_unlock();
            if (!running) {
                release_scan(id, ESP_OK);
                return;
            }

            error = ryz_i2c_scan_bus_probe(address);
            ryz_i2c_scan_platform_lock();
            running = running_locked(id);
            bool retry = false;
            if (running) {
                if (error == ESP_ERR_NOT_FINISHED) ++s_snapshot.busy_responses;
                retry = error == ESP_ERR_NOT_FINISHED &&
                        attempt + 1 < RYZ_I2C_SCAN_ATTEMPTS;
                if (!retry) {
                    record_locked(address, error);
                }
            }
            ryz_i2c_scan_platform_unlock();
            /* No result or response counter from a cancelled call is published;
             * probe_calls still includes that admitted/in-flight call. */
            if (!running) {
                release_scan(id, ESP_OK);
                return;
            }
            if (!retry) break;
            ryz_i2c_scan_platform_delay_ms(5);
        }
        if (address != RYZ_I2C_SCAN_LAST) ryz_i2c_scan_platform_delay_ms(5);
    }
    release_scan(id, ESP_OK);
}

static void scan_task(void *unused)
{
    (void)unused;
    for (;;) {
        uint32_t id = 0;
        ryz_i2c_scan_config_t config = {0};
        ryz_i2c_scan_platform_lock();
        if (s_snapshot.phase == RYZ_I2C_SCAN_CANCELLING ||
            s_snapshot.phase == RYZ_I2C_SCAN_QUEUED) {
            id = s_snapshot.scan_id;
            config = s_snapshot.scan_config;
            if (!s_cancel_requested) {
                s_snapshot.phase = RYZ_I2C_SCAN_RUNNING;
                s_snapshot.started_ms = ryz_i2c_scan_platform_now_ms();
            }
        }
        ryz_i2c_scan_platform_unlock();
        if (id) scan(id, &config);
        else ryz_i2c_scan_platform_wait();
    }
}

/* Leave first creation STARTING until the initiating operation has committed
 * or rejected. A concurrent request cannot steal that first admission. */
static esp_err_t ensure_worker(bool *creating)
{
    int expected = STOPPED;
    *creating = atomic_compare_exchange_strong(&s_lifecycle, &expected, STARTING);
    if (!*creating && expected != READY) return ESP_ERR_INVALID_STATE;
    if (*creating) {
        esp_err_t error = ryz_i2c_scan_platform_init();
        if (error == ESP_OK) {
            memset(&s_snapshot, 0, sizeof(s_snapshot));
            s_snapshot.config = RYZ_I2C_SCAN_DEFAULT_CONFIG;
            s_snapshot.config_revision = 1;
            error = ryz_i2c_scan_platform_start(scan_task);
            if (error != ESP_OK) ryz_i2c_scan_platform_deinit();
        }
        if (error != ESP_OK) {
            atomic_store_explicit(&s_lifecycle, STOPPED, memory_order_release);
            return error;
        }
    }
    return ESP_OK;
}

esp_err_t ryz_i2c_scan_configure(const ryz_i2c_scan_config_t *config,
                              uint32_t expected_revision, uint32_t *out_revision)
{
    if (!out_revision) return ESP_ERR_INVALID_ARG;
    ryz_i2c_scan_config_t value = config ? *config : (ryz_i2c_scan_config_t){0};
    *out_revision = 0;
    if (!config || !expected_revision) return ESP_ERR_INVALID_ARG;
    esp_err_t error = ryz_i2c_scan_validate_config(&value);
    if (error != ESP_OK) return error;
    bool creating;
    error = ensure_worker(&creating);
    if (error != ESP_OK) return error;
    ryz_i2c_scan_platform_lock();
    error = ESP_ERR_INVALID_STATE;
    if (!active(s_snapshot.phase) && !s_snapshot.resources_held &&
        expected_revision == s_snapshot.config_revision && expected_revision != UINT32_MAX) {
        bool defaults = value.sda == 41 && value.scl == 40 && value.hz == 100000;
        error = defaults ? ESP_OK : ryz_tool_pins_check_pair(value.sda, value.scl);
        if (error == ESP_OK) {
            s_snapshot.config = value;
            *out_revision = ++s_snapshot.config_revision;
        }
    }
    ryz_i2c_scan_platform_unlock();
    if (creating) atomic_store_explicit(&s_lifecycle, READY, memory_order_release);
    return error;
}

esp_err_t ryz_i2c_scan_start(uint32_t *out_id)
{
    if (!out_id) return ESP_ERR_INVALID_ARG;
    *out_id = 0;
    bool creating;
    esp_err_t error = ensure_worker(&creating);
    if (error != ESP_OK) return error;
    ryz_i2c_scan_platform_lock();
    error = ESP_ERR_INVALID_STATE;
    if (!active(s_snapshot.phase) && s_last_id != UINT32_MAX) {
        ryz_i2c_scan_config_t config = s_snapshot.config;
        uint32_t revision = s_snapshot.config_revision;
        bool held = s_snapshot.resources_held;
        esp_err_t cleanup = s_snapshot.cleanup_error;
        memset(&s_snapshot, 0, sizeof(s_snapshot));
        s_snapshot.config = s_snapshot.scan_config = config;
        s_snapshot.config_revision = s_snapshot.scan_config_revision = revision;
        s_snapshot.resources_held = held;
        s_snapshot.cleanup_error = cleanup;
        s_cancel_requested = false;
        s_snapshot.scan_id = ++s_last_id;
        s_snapshot.phase = RYZ_I2C_SCAN_QUEUED;
        s_snapshot.queued_ms = ryz_i2c_scan_platform_now_ms();
        *out_id = s_snapshot.scan_id;
        error = ESP_OK;
    }
    ryz_i2c_scan_platform_unlock();
    if (creating) atomic_store_explicit(&s_lifecycle, READY, memory_order_release);
    if (error == ESP_OK) ryz_i2c_scan_platform_wake();
    return error;
}
esp_err_t ryz_i2c_scan_cancel(uint32_t scan_id)
{
    if (!scan_id) return ESP_ERR_INVALID_ARG;
    if (atomic_load_explicit(&s_lifecycle, memory_order_acquire) != READY) {
        return ESP_ERR_INVALID_STATE;
    }
    bool wake = false;
    esp_err_t error = ESP_ERR_INVALID_STATE;
    ryz_i2c_scan_platform_lock();
    if (scan_id == s_snapshot.scan_id) {
        if (active(s_snapshot.phase) ||
            (s_snapshot.phase == RYZ_I2C_SCAN_FAILED && s_snapshot.resources_held)) {
            s_cancel_requested = true;
            if (s_snapshot.phase != RYZ_I2C_SCAN_RELEASING) {
                s_snapshot.phase = RYZ_I2C_SCAN_CANCELLING;
            }
            error = ESP_OK;
            wake = true;
        } else if (s_snapshot.phase == RYZ_I2C_SCAN_CANCELLED) error = ESP_OK;
    }
    ryz_i2c_scan_platform_unlock();
    if (wake) ryz_i2c_scan_platform_wake();
    return error;
}
esp_err_t ryz_i2c_scan_get_snapshot(ryz_i2c_scan_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (atomic_load_explicit(&s_lifecycle, memory_order_acquire) != READY) {
        return ESP_ERR_INVALID_STATE;
    }
    ryz_i2c_scan_platform_lock();
    *out = s_snapshot;
    ryz_i2c_scan_platform_unlock();
    return ESP_OK;
}
