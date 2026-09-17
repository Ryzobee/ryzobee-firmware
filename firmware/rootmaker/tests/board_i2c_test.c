#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board_i2c.h"
#include "touch.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* The three production C files are separately compiled. Only ESP-IDF's
 * driver/RTOS boundary is adapted here; no public board or TP logic is copied.
 * Each case runs in a fresh process because the bus is boot-lifetime state. */
struct host_i2c_bus { unsigned identity; };
struct host_i2c_device { uint16_t address; };
struct host_i2c_mutex {
    pthread_mutex_t guard;
    pthread_cond_t changed;
    bool held;
    pthread_t owner;
};
static struct host_i2c_mutex bus_mutex;
static bool mutex_live, fail_mutex_create;
static unsigned mutex_creates, mutex_deletes, mutex_waiters;
static TickType_t last_lock_timeout;
static _Thread_local bool owns_bus_mutex;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static struct host_i2c_bus bus = {0x52595a49};
static struct host_i2c_device devices[3];
static unsigned bus_creates, bus_deletes, registrations[3], device_deletes;
static i2c_master_bus_config_t saved_bus;
static i2c_device_config_t saved_devices[3];
static esp_err_t new_bus_error, add_device_error, transfer_error, probe_error;
static bool block_new_bus, new_bus_entered, release_new_bus;
static bool block_transfer, transfer_entered, release_transfer;
static unsigned active_transfers, maximum_transfers;
static unsigned transfer_calls, probe_calls;
static uint16_t probed_address;
static int last_driver_timeout, last_probe_timeout;
static uint8_t identity[4] = {0xb6, 0x12, 0x34, 0x56};
static uint8_t sample_data[6];
static uint8_t auto_sleep = 1;
static int failed_read_register = -1;
static unsigned failed_read_count;
static esp_err_t failed_read_error;
static bool fail_awake_write;
static unsigned gpio_configs, gpio_levels, delays;
static unsigned fail_gpio_config_at, fail_gpio_level_at;
static int64_t now_us = INT64_C(123456000);

typedef struct {
    unsigned kind;
    int first;
    int second;
    size_t length;
    uint8_t data[RYZ_BOARD_I2C_WRITE_MAX];
} trace_t;
enum { TRACE_GPIO_CONFIG, TRACE_GPIO_LEVEL, TRACE_DELAY, TRACE_READ, TRACE_WRITE };
static trace_t trace[128];
static size_t trace_count;
static gpio_config_t gpio_records[8];

static struct timespec deadline_ms(unsigned milliseconds)
{
    struct timespec value;
    assert(clock_gettime(CLOCK_REALTIME, &value) == 0);
    value.tv_sec += milliseconds / 1000U;
    value.tv_nsec += (long)(milliseconds % 1000U) * 1000000L;
    if (value.tv_nsec >= 1000000000L) { ++value.tv_sec; value.tv_nsec -= 1000000000L; }
    return value;
}

static void wait_changed(void)
{
    const struct timespec limit = deadline_ms(5000);
    assert(pthread_cond_timedwait(&changed, &gate, &limit) == 0);
}

static void record(unsigned kind, int first, int second,
                    const uint8_t *data, size_t length)
{
    assert(trace_count < sizeof(trace) / sizeof(trace[0]));
    assert(length <= RYZ_BOARD_I2C_WRITE_MAX);
    trace_t *entry = &trace[trace_count++];
    *entry = (trace_t){.kind = kind, .first = first, .second = second, .length = length};
    if (data && length) memcpy(entry->data, data, length);
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    ++mutex_creates;
    if (fail_mutex_create) { fail_mutex_create = false; return NULL; }
    assert(!mutex_live);
    memset(&bus_mutex, 0, sizeof(bus_mutex));
    assert(pthread_mutex_init(&bus_mutex.guard, NULL) == 0);
    assert(pthread_cond_init(&bus_mutex.changed, NULL) == 0);
    mutex_live = true;
    return &bus_mutex;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout)
{
    assert(semaphore == &bus_mutex && mutex_live && !owns_bus_mutex);
    const TickType_t ordinary = pdMS_TO_TICKS(20) ? pdMS_TO_TICKS(20) : 1U;
    assert(timeout == 0 || timeout == ordinary);
    assert(timeout != portMAX_DELAY);
    assert(pthread_mutex_lock(&gate) == 0);
    last_lock_timeout = timeout;
    assert(pthread_mutex_unlock(&gate) == 0);
    assert(pthread_mutex_lock(&semaphore->guard) == 0);
    if (semaphore->held && timeout == 0) {
        assert(pthread_mutex_unlock(&semaphore->guard) == 0);
        return pdFALSE;
    }
    const struct timespec limit = deadline_ms(timeout * HOST_TICK_MS);
    bool waited = false;
    while (semaphore->held) {
        if (!waited) {
            assert(pthread_mutex_lock(&gate) == 0);
            ++mutex_waiters;
            assert(pthread_cond_broadcast(&changed) == 0);
            assert(pthread_mutex_unlock(&gate) == 0);
            waited = true;
        }
        const int error = pthread_cond_timedwait(&semaphore->changed, &semaphore->guard, &limit);
        if (error == ETIMEDOUT) {
            assert(pthread_mutex_unlock(&semaphore->guard) == 0);
            return pdFALSE;
        }
        assert(error == 0);
    }
    semaphore->held = true;
    semaphore->owner = pthread_self();
    owns_bus_mutex = true;
    assert(pthread_mutex_unlock(&semaphore->guard) == 0);
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    assert(semaphore == &bus_mutex && owns_bus_mutex);
    assert(pthread_mutex_lock(&semaphore->guard) == 0);
    assert(semaphore->held && pthread_equal(semaphore->owner, pthread_self()));
    semaphore->held = false;
    owns_bus_mutex = false;
    assert(pthread_cond_broadcast(&semaphore->changed) == 0);
    assert(pthread_mutex_unlock(&semaphore->guard) == 0);
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    assert(semaphore == &bus_mutex && mutex_live && !semaphore->held);
    assert(pthread_mutex_destroy(&semaphore->guard) == 0);
    assert(pthread_cond_destroy(&semaphore->changed) == 0);
    mutex_live = false;
    ++mutex_deletes;
}

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *config,
                            i2c_master_bus_handle_t *out)
{
    assert(config && out);
    ++bus_creates;
    saved_bus = *config;
    assert(pthread_mutex_lock(&gate) == 0);
    new_bus_entered = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    while (block_new_bus && !release_new_bus) wait_changed();
    assert(pthread_mutex_unlock(&gate) == 0);
    if (new_bus_error != ESP_OK) {
        const esp_err_t error = new_bus_error;
        new_bus_error = ESP_OK;
        return error;
    }
    *out = &bus;
    return ESP_OK;
}

static unsigned endpoint(uint16_t address)
{
    if (address == 0x15) return 0;
    if (address == 0x18) return 1;
    assert(address == 0x19);
    return 2;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t handle,
                                   const i2c_device_config_t *config,
                                   i2c_master_dev_handle_t *out)
{
    assert(handle == &bus && config && out && owns_bus_mutex);
    const unsigned index = endpoint(config->device_address);
    ++registrations[index];
    saved_devices[index] = *config;
    if (add_device_error != ESP_OK) {
        const esp_err_t error = add_device_error;
        add_device_error = ESP_OK;
        return error;
    }
    devices[index].address = config->device_address;
    *out = &devices[index];
    return ESP_OK;
}

esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t handle)
{
    assert(handle == &bus);
    ++bus_deletes;
    return ESP_OK;
}
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device)
{
    assert(device >= devices && device < devices + 3);
    ++device_deletes;
    return ESP_OK;
}

static void transfer_enter(i2c_master_dev_handle_t device, int timeout_ms)
{
    assert(owns_bus_mutex && device >= devices && device < devices + 3);
    assert(timeout_ms == 20);
    assert(pthread_mutex_lock(&gate) == 0);
    last_driver_timeout = timeout_ms;
    ++transfer_calls;
    ++active_transfers;
    if (active_transfers > maximum_transfers) maximum_transfers = active_transfers;
    assert(active_transfers == 1);
    if (block_transfer) {
        block_transfer = false;
        transfer_entered = true;
        assert(pthread_cond_broadcast(&changed) == 0);
        while (!release_transfer) wait_changed();
    }
    assert(pthread_mutex_unlock(&gate) == 0);
}

static esp_err_t transfer_leave(esp_err_t error)
{
    assert(pthread_mutex_lock(&gate) == 0);
    assert(active_transfers == 1);
    --active_transfers;
    assert(pthread_mutex_unlock(&gate) == 0);
    return error;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device,
                                     const uint8_t *write_buffer, size_t write_size,
                                     uint8_t *read_buffer, size_t read_size,
                                     int timeout_ms)
{
    assert(write_buffer && write_size == 1 && read_buffer && read_size >= 1 && read_size <= 32);
    transfer_enter(device, timeout_ms);
    record(TRACE_READ, device->address, write_buffer[0], NULL, read_size);
    memset(read_buffer, 0xa5, read_size); /* Partial/error bytes must not escape. */
    if (transfer_error != ESP_OK) {
        const esp_err_t error = transfer_error;
        transfer_error = ESP_OK;
        return transfer_leave(error);
    }
    if (device->address == 0x15 && failed_read_count && write_buffer[0] == failed_read_register) {
        --failed_read_count;
        return transfer_leave(failed_read_error);
    }
    if (device->address == 0x15 && write_buffer[0] == 0xa7) {
        assert(read_size == 4);
        memcpy(read_buffer, identity, read_size);
    } else if (device->address == 0x15 && write_buffer[0] == 0xfe) {
        assert(read_size == 1);
        *read_buffer = auto_sleep;
    } else if (device->address == 0x15 && write_buffer[0] == 0x01) {
        assert(read_size == 6);
        memcpy(read_buffer, sample_data, read_size);
    } else {
        for (size_t i = 0; i < read_size; ++i) read_buffer[i] = (uint8_t)(write_buffer[0] + i);
    }
    return transfer_leave(ESP_OK);
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device,
                             const uint8_t *write_buffer, size_t write_size,
                             int timeout_ms)
{
    assert(write_buffer && write_size >= 1 && write_size <= 33);
    transfer_enter(device, timeout_ms);
    record(TRACE_WRITE, device->address, 0, write_buffer, write_size);
    if (transfer_error != ESP_OK) {
        const esp_err_t error = transfer_error;
        transfer_error = ESP_OK;
        return transfer_leave(error);
    }
    if (device->address == 0x15 && write_buffer[0] == 0xfe) {
        assert(write_size == 2 && write_buffer[1] == 1);
        if (fail_awake_write) { fail_awake_write = false; return transfer_leave(ESP_FAIL); }
    }
    return transfer_leave(ESP_OK);
}

esp_err_t i2c_master_probe(i2c_master_bus_handle_t handle, uint16_t address, int timeout_ms)
{
    assert(handle == &bus && owns_bus_mutex && timeout_ms == 3);
    assert(address >= 0x08 && address <= 0x77);
    assert(active_transfers == 0);
    ++probe_calls;
    probed_address = address;
    last_probe_timeout = timeout_ms;
    const esp_err_t error = probe_error;
    probe_error = ESP_OK;
    return error;
}

esp_err_t gpio_config(const gpio_config_t *config)
{
    assert(config && gpio_configs < sizeof(gpio_records) / sizeof(gpio_records[0]));
    gpio_records[gpio_configs++] = *config;
    record(TRACE_GPIO_CONFIG, (int)config->pin_bit_mask, config->mode, NULL, 0);
    return gpio_configs == fail_gpio_config_at ? ESP_FAIL : ESP_OK;
}
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    assert(pin == GPIO_NUM_2 && level <= 1);
    ++gpio_levels;
    record(TRACE_GPIO_LEVEL, pin, (int)level, NULL, 0);
    return gpio_levels == fail_gpio_level_at ? ESP_FAIL : ESP_OK;
}
void vTaskDelay(TickType_t ticks)
{
    ++delays;
    record(TRACE_DELAY, (int)ticks, 0, NULL, 0);
}
int64_t esp_timer_get_time(void) { return now_us; }

static void zero_bytes(const void *data, size_t length)
{
    const unsigned char *bytes = data;
    for (size_t i = 0; i < length; ++i) assert(bytes[i] == 0);
}

static void check_configuration(void)
{
    assert(saved_bus.i2c_port == I2C_NUM_0);
    assert(saved_bus.sda_io_num == GPIO_NUM_41 && saved_bus.scl_io_num == GPIO_NUM_40);
    assert(saved_bus.clk_source == I2C_CLK_SRC_DEFAULT && saved_bus.glitch_ignore_cnt == 7);
    assert(saved_bus.flags.enable_internal_pullup && saved_bus.trans_queue_depth == 0);
    for (unsigned i = 0; i < 3; ++i) if (registrations[i]) {
        assert(saved_devices[i].dev_addr_length == I2C_ADDR_BIT_LEN_7);
        assert(saved_devices[i].scl_speed_hz == 100000 && !saved_devices[i].flags.disable_ack_check);
        assert(endpoint(saved_devices[i].device_address) == i);
    }
    assert(!bus_deletes && !device_deletes);
}

static void *initialize_thread(void *opaque)
{
    esp_err_t *error = opaque;
    *error = ryz_board_i2c_init();
    return NULL;
}

static void case_init(void)
{
    uint8_t bytes[4];
    memset(bytes, 0xa5, sizeof(bytes));
    assert(ryz_board_i2c_read_reg(RYZ_BOARD_I2C_IMU_LOW, 0x10, bytes, sizeof(bytes)) == ESP_ERR_INVALID_STATE);
    zero_bytes(bytes, sizeof(bytes));
    assert(ryz_board_i2c_write(RYZ_BOARD_I2C_IMU_LOW, bytes, 1) == ESP_ERR_INVALID_STATE);
    assert(ryz_board_i2c_probe(0x18) == ESP_ERR_INVALID_STATE);
    fail_mutex_create = true;
    assert(ryz_board_i2c_init() == ESP_ERR_NO_MEM && mutex_creates == 1 && bus_creates == 0);
    new_bus_error = ESP_FAIL;
    assert(ryz_board_i2c_init() == ESP_FAIL && mutex_creates == 2 && mutex_deletes == 1);
    assert(bus_creates == 1);
    block_new_bus = true;
    new_bus_entered = false;
    esp_err_t error = ESP_FAIL;
    pthread_t thread;
    assert(pthread_create(&thread, NULL, initialize_thread, &error) == 0);
    assert(pthread_mutex_lock(&gate) == 0);
    while (!new_bus_entered) wait_changed();
    assert(pthread_mutex_unlock(&gate) == 0);
    assert(ryz_board_i2c_init() == ESP_ERR_INVALID_STATE);
    assert(ryz_board_i2c_probe(0x18) == ESP_ERR_INVALID_STATE);
    assert(pthread_mutex_lock(&gate) == 0);
    release_new_bus = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    assert(pthread_mutex_unlock(&gate) == 0);
    assert(pthread_join(thread, NULL) == 0 && error == ESP_OK);
    for (unsigned i = 0; i < 4; ++i) assert(ryz_board_i2c_init() == ESP_OK);
    assert(mutex_creates == 3 && bus_creates == 2 && mutex_deletes == 1);
    assert(!registrations[0] && !registrations[1] && !registrations[2]);
    check_configuration();
}

static void case_transfers(void)
{
    assert(ryz_board_i2c_init() == ESP_OK);
    uint8_t bytes[34];
    memset(bytes, 0xa5, sizeof(bytes));
    assert(ryz_board_i2c_read_reg((ryz_board_i2c_device_t)-1, 0, bytes, 4) == ESP_ERR_INVALID_ARG);
    zero_bytes(bytes, 4);
    memset(bytes, 0xa5, sizeof(bytes));
    assert(ryz_board_i2c_read_reg(RYZ_BOARD_I2C_DEVICE_COUNT, 0, bytes, 4) == ESP_ERR_INVALID_ARG);
    zero_bytes(bytes, 4);
    memset(bytes, 0xa5, sizeof(bytes));
    assert(ryz_board_i2c_read_reg(RYZ_BOARD_I2C_TOUCH, 0, bytes, 0) == ESP_ERR_INVALID_ARG);
    assert(ryz_board_i2c_read_reg(RYZ_BOARD_I2C_TOUCH, 0, bytes, 33) == ESP_ERR_INVALID_ARG);
    for (size_t i = 0; i < sizeof(bytes); ++i) assert(bytes[i] == 0xa5);
    assert(ryz_board_i2c_read_reg(RYZ_BOARD_I2C_TOUCH, 0, NULL, 1) == ESP_ERR_INVALID_ARG);
    assert(ryz_board_i2c_write((ryz_board_i2c_device_t)-1, bytes, 1) == ESP_ERR_INVALID_ARG);
    assert(ryz_board_i2c_write(RYZ_BOARD_I2C_DEVICE_COUNT, bytes, 1) == ESP_ERR_INVALID_ARG);
    assert(ryz_board_i2c_write(RYZ_BOARD_I2C_TOUCH, bytes, 0) == ESP_ERR_INVALID_ARG);
    assert(ryz_board_i2c_write(RYZ_BOARD_I2C_TOUCH, bytes, 34) == ESP_ERR_INVALID_ARG);
    assert(ryz_board_i2c_write(RYZ_BOARD_I2C_TOUCH, NULL, 1) == ESP_ERR_INVALID_ARG);
    assert(transfer_calls == 0 && !registrations[0] && !registrations[1] && !registrations[2]);
    add_device_error = ESP_ERR_NO_MEM;
    assert(ryz_board_i2c_read_reg(RYZ_BOARD_I2C_IMU_LOW, 0x10, bytes, 32) == ESP_ERR_NO_MEM);
    zero_bytes(bytes, 32);
    assert(registrations[1] == 1 && transfer_calls == 0);
    assert(ryz_board_i2c_read_reg(RYZ_BOARD_I2C_IMU_LOW, 0x10, bytes, 32) == ESP_OK);
    for (unsigned i = 0; i < 32; ++i) assert(bytes[i] == 0x10 + i);
    assert(registrations[1] == 2 && last_driver_timeout == 20);
    const TickType_t expected_ticks = pdMS_TO_TICKS(20) ? pdMS_TO_TICKS(20) : 1U;
    assert(last_lock_timeout == expected_ticks);
    assert(ryz_board_i2c_write(RYZ_BOARD_I2C_IMU_LOW, bytes, 33) == ESP_OK);
    assert(registrations[1] == 2);
    assert(trace[trace_count - 1].length == 33 && memcmp(trace[trace_count - 1].data, bytes, 33) == 0);
    assert(ryz_board_i2c_read_reg(RYZ_BOARD_I2C_TOUCH, 0xa7, bytes, 4) == ESP_OK);
    assert(memcmp(bytes, identity, 4) == 0);
    assert(ryz_board_i2c_read_reg(RYZ_BOARD_I2C_IMU_HIGH, 0x0f, bytes, 1) == ESP_OK);
    assert(registrations[0] == 1 && registrations[2] == 1);
    transfer_error = ESP_ERR_TIMEOUT;
    memset(bytes, 0xa5, sizeof(bytes));
    assert(ryz_board_i2c_read_reg(RYZ_BOARD_I2C_IMU_LOW, 0x10, bytes, 32) == ESP_ERR_TIMEOUT);
    zero_bytes(bytes, 32);
    assert(bytes[32] == 0xa5 && bytes[33] == 0xa5);
    transfer_error = ESP_FAIL;
    assert(ryz_board_i2c_write(RYZ_BOARD_I2C_IMU_HIGH, bytes, 1) == ESP_FAIL);
    assert(ryz_board_i2c_read_reg(RYZ_BOARD_I2C_IMU_LOW, 0x10, bytes, 1) == ESP_OK);
    assert(bus_creates == 1 && maximum_transfers == 1);
    check_configuration();
}

static void case_probe(void)
{
    assert(ryz_board_i2c_init() == ESP_OK);
    const uint8_t invalid[] = {0, 7, 0x78, 0x7f, 0xff};
    for (size_t i = 0; i < sizeof(invalid); ++i)
        assert(ryz_board_i2c_probe(invalid[i]) == ESP_ERR_INVALID_ARG);
    assert(probe_calls == 0);
    assert(ryz_board_i2c_probe(0x08) == ESP_OK && probed_address == 0x08);
    assert(last_lock_timeout == 0 && last_probe_timeout == 3);
    probe_error = ESP_ERR_NOT_FOUND;
    assert(ryz_board_i2c_probe(0x77) == ESP_ERR_NOT_FOUND && probed_address == 0x77);
    probe_error = ESP_ERR_TIMEOUT;
    assert(ryz_board_i2c_probe(0x18) == ESP_ERR_TIMEOUT);
    assert(!registrations[0] && !registrations[1] && !registrations[2]);
    check_configuration();
}

typedef struct { ryz_board_i2c_device_t device; esp_err_t error; uint8_t bytes[4]; } transfer_t;
static void *read_thread(void *opaque)
{
    transfer_t *request = opaque;
    request->error = ryz_board_i2c_read_reg(request->device, 0x20, request->bytes, sizeof(request->bytes));
    return NULL;
}

static void case_serialization(void)
{
    assert(ryz_board_i2c_init() == ESP_OK);
    block_transfer = true;
    transfer_t first = {.device = RYZ_BOARD_I2C_TOUCH, .error = ESP_FAIL};
    pthread_t first_thread, second_thread;
    assert(pthread_create(&first_thread, NULL, read_thread, &first) == 0);
    assert(pthread_mutex_lock(&gate) == 0);
    while (!transfer_entered) wait_changed();
    assert(pthread_mutex_unlock(&gate) == 0);
    const unsigned calls = transfer_calls;
    assert(ryz_board_i2c_probe(0x18) == ESP_ERR_TIMEOUT && probe_calls == 0);
    uint8_t bytes[4];
    memset(bytes, 0xa5, sizeof(bytes));
    assert(ryz_board_i2c_read_reg(RYZ_BOARD_I2C_IMU_LOW, 0x0f, bytes, sizeof(bytes)) == ESP_ERR_TIMEOUT);
    zero_bytes(bytes, sizeof(bytes));
    assert(transfer_calls == calls && registrations[1] == 0);
    transfer_t second = {.device = RYZ_BOARD_I2C_IMU_HIGH, .error = ESP_FAIL};
    const unsigned previous_waiters = mutex_waiters;
    assert(pthread_create(&second_thread, NULL, read_thread, &second) == 0);
    assert(pthread_mutex_lock(&gate) == 0);
    while (mutex_waiters == previous_waiters) wait_changed();
    assert(active_transfers == 1 && transfer_calls == calls && registrations[2] == 0);
    release_transfer = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    assert(pthread_mutex_unlock(&gate) == 0);
    assert(pthread_join(first_thread, NULL) == 0 && first.error == ESP_OK);
    assert(pthread_join(second_thread, NULL) == 0 && second.error == ESP_OK);
    assert(memcmp(first.bytes, "\x20\x21\x22\x23", 4) == 0);
    assert(memcmp(first.bytes, second.bytes, 4) == 0);
    assert(maximum_transfers == 1 && registrations[0] == 1 && registrations[2] == 1);
    assert(ryz_board_i2c_probe(0x18) == ESP_OK);
    check_configuration();
}

static void case_try_probe(void)
{
    assert(ryz_board_i2c_try_probe(0x18) == ESP_ERR_INVALID_STATE);
    const uint8_t invalid[] = {0, 7, 0x78, 0x7f, 0xff};
    for (size_t i = 0; i < sizeof(invalid); ++i)
        assert(ryz_board_i2c_try_probe(invalid[i]) == ESP_ERR_INVALID_ARG);
    assert(bus_creates == 0 && probe_calls == 0);
    assert(ryz_board_i2c_init() == ESP_OK);

    /* A real other thread owns the board mutex during its touch transaction.
     * Scan contention must be distinguishable from a completed failed probe. */
    block_transfer = true;
    transfer_t touch = {.device = RYZ_BOARD_I2C_TOUCH, .error = ESP_FAIL};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, read_thread, &touch) == 0);
    assert(pthread_mutex_lock(&gate) == 0);
    while (!transfer_entered) wait_changed();
    assert(pthread_mutex_unlock(&gate) == 0);
    assert(ryz_board_i2c_try_probe(0x18) == ESP_ERR_NOT_FINISHED);
    assert(last_lock_timeout == 0 && probe_calls == 0);
    assert(ryz_board_i2c_probe(0x18) == ESP_ERR_TIMEOUT && probe_calls == 0);
    assert(registrations[0] == 1 && registrations[1] == 0 && registrations[2] == 0);
    assert(pthread_mutex_lock(&gate) == 0);
    release_transfer = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    assert(pthread_mutex_unlock(&gate) == 0);
    assert(pthread_join(thread, NULL) == 0 && touch.error == ESP_OK);

    assert(ryz_board_i2c_try_probe(0x08) == ESP_OK && probed_address == 0x08);
    assert(last_lock_timeout == 0 && last_probe_timeout == 3);
    probe_error = ESP_ERR_NOT_FOUND;
    assert(ryz_board_i2c_try_probe(0x77) == ESP_ERR_NOT_FOUND && probed_address == 0x77);
    probe_error = ESP_ERR_TIMEOUT;
    assert(ryz_board_i2c_try_probe(0x18) == ESP_ERR_TIMEOUT);
    probe_error = ESP_ERR_INVALID_STATE;
    assert(ryz_board_i2c_try_probe(0x19) == ESP_ERR_INVALID_STATE);
    probe_error = ESP_FAIL;
    assert(ryz_board_i2c_try_probe(0x15) == ESP_FAIL);
    /* Every driver-result path releases the mutex and leaves shared resources. */
    assert(ryz_board_i2c_try_probe(0x15) == ESP_OK);
    assert(probe_calls == 6 && bus_creates == 1 && mutex_creates == 1);
    assert(registrations[0] == 1 && registrations[1] == 0 && registrations[2] == 0);
    check_configuration();
}

static void check_touch_start_trace(void)
{
    assert(trace_count == 10);
    assert(trace[0].kind == TRACE_GPIO_CONFIG && trace[0].first == (1 << 2));
    assert(trace[1].kind == TRACE_GPIO_CONFIG && trace[1].first == (1 << 1));
    assert(gpio_records[0].mode == GPIO_MODE_OUTPUT);
    assert(gpio_records[1].mode == GPIO_MODE_INPUT && gpio_records[1].pull_up_en == GPIO_PULLUP_ENABLE);
    assert(trace[2].kind == TRACE_GPIO_LEVEL && trace[2].first == 2 && trace[2].second == 0);
    assert(trace[3].kind == TRACE_DELAY && trace[3].first == (int)pdMS_TO_TICKS(10));
    assert(trace[4].kind == TRACE_GPIO_LEVEL && trace[4].first == 2 && trace[4].second == 1);
    assert(trace[5].kind == TRACE_DELAY && trace[5].first == (int)pdMS_TO_TICKS(50));
    assert(trace[6].kind == TRACE_READ && trace[6].first == 0x15 && trace[6].second == 0xa7 && trace[6].length == 4);
    assert(trace[7].kind == TRACE_WRITE && trace[7].first == 0x15 && trace[7].length == 2);
    assert(trace[7].data[0] == 0xfe && trace[7].data[1] == 1);
    assert(trace[8].kind == TRACE_READ && trace[8].second == 0xfe && trace[8].length == 1);
    assert(trace[9].kind == TRACE_READ && trace[9].second == 0x01 && trace[9].length == 6);
}

static void case_touch(uint8_t chip)
{
    identity[0] = chip;
    assert(ryz_touch_init() == ESP_OK);
    check_touch_start_trace();
    ryz_touch_info_t info;
    ryz_touch_get_info(&info);
    assert(info.ready && info.chip_id == chip && info.project_id == 0x12);
    assert(info.firmware_version == 0x34 && info.factory_id == 0x56);
    assert(info.disable_auto_sleep == 1 && info.reads == 1 && info.errors == 0);
    assert(ryz_touch_init() == ESP_OK && trace_count == 10);
    assert(gpio_configs == 2 && gpio_levels == 2 && delays == 2 && bus_creates == 1);
    sample_data[1] = 1; sample_data[3] = 12; sample_data[5] = 34;
    ryz_touch_sample_t sample;
    assert(ryz_touch_read(&sample) == ESP_OK && sample.pressed && sample.event == RYZ_TOUCH_DOWN);
    assert(sample.x == 12 && sample.y == 34 && sample.sampled_ms == 123456);
    assert(ryz_touch_read(&sample) == ESP_OK && sample.event == RYZ_TOUCH_MOVE);
    /* One shared-bus timeout is recoverable and must not terminate a running
     * touch application. The bounded retry still preserves the current press. */
    transfer_error = ESP_ERR_TIMEOUT;
    memset(&sample, 0xa5, sizeof(sample));
    assert(ryz_touch_read(&sample) == ESP_OK && sample.event == RYZ_TOUCH_MOVE);
    /* A persistent fault remains visible and clears the stale coordinate. */
    failed_read_register = 0x01;
    failed_read_count = 2;
    failed_read_error = ESP_ERR_TIMEOUT;
    memset(&sample, 0xa5, sizeof(sample));
    assert(ryz_touch_read(&sample) == ESP_ERR_TIMEOUT);
    zero_bytes(&sample, sizeof(sample));
    assert(ryz_touch_read(&sample) == ESP_OK && sample.event == RYZ_TOUCH_DOWN);
    sample_data[1] = 2;
    assert(ryz_touch_read(&sample) == ESP_ERR_INVALID_RESPONSE);
    zero_bytes(&sample, sizeof(sample));
    memset(sample_data, 0, sizeof(sample_data));
    assert(ryz_touch_read(&sample) == ESP_OK && sample.event == RYZ_TOUCH_NONE && !sample.pressed);
    sample_data[1] = 1;
    assert(ryz_touch_read(&sample) == ESP_OK && sample.event == RYZ_TOUCH_DOWN);
    memset(sample_data, 0, sizeof(sample_data));
    assert(ryz_touch_read(&sample) == ESP_OK && sample.event == RYZ_TOUCH_UP);
    assert(ryz_touch_read(NULL) == ESP_ERR_INVALID_ARG);
    ryz_touch_get_info(NULL);
    ryz_touch_get_info(&info);
    assert(info.ready && info.errors == 2 && info.presses == 3 && info.releases == 1);
    check_configuration();
}

static void case_touch_failure(unsigned stage)
{
    assert(ryz_board_i2c_init() == ESP_OK);
    uint8_t imu;
    assert(ryz_board_i2c_read_reg(RYZ_BOARD_I2C_IMU_LOW, 0x0f, &imu, 1) == ESP_OK);
    trace_count = 0;
    esp_err_t expected = ESP_FAIL;
    switch (stage) {
    case 0: fail_gpio_config_at = 1; break;
    case 1: fail_gpio_config_at = 2; break;
    case 2: fail_gpio_level_at = 1; break;
    case 3: fail_gpio_level_at = 2; break;
    case 4: add_device_error = ESP_ERR_NO_MEM; expected = ESP_ERR_NO_MEM; break;
    case 5: failed_read_register = 0xa7; failed_read_count = 1; failed_read_error = ESP_ERR_TIMEOUT; expected = ESP_ERR_TIMEOUT; break;
    case 6: identity[0] = 0x00; expected = ESP_ERR_NOT_SUPPORTED; break;
    case 7: fail_awake_write = true; break;
    case 8: failed_read_register = 0xfe; failed_read_count = 1; failed_read_error = ESP_ERR_TIMEOUT; expected = ESP_ERR_TIMEOUT; break;
    case 9: auto_sleep = 0; expected = ESP_ERR_INVALID_RESPONSE; break;
    case 10: failed_read_register = 0x01; failed_read_count = 2; failed_read_error = ESP_ERR_TIMEOUT; expected = ESP_ERR_TIMEOUT; break;
    case 11: sample_data[1] = 2; expected = ESP_ERR_INVALID_RESPONSE; break;
    default: abort();
    }
    assert(ryz_touch_init() == expected);
    ryz_touch_info_t info;
    ryz_touch_get_info(&info);
    assert(!info.ready);
    ryz_touch_sample_t sample;
    memset(&sample, 0xa5, sizeof(sample));
    assert(ryz_touch_read(&sample) == ESP_ERR_INVALID_STATE);
    zero_bytes(&sample, sizeof(sample));
    assert(bus_creates == 1 && bus_deletes == 0 && device_deletes == 0);
    assert(ryz_board_i2c_read_reg(RYZ_BOARD_I2C_IMU_LOW, 0x0f, &imu, 1) == ESP_OK && imu == 0x0f);
    assert(registrations[1] == 1);
    fail_gpio_config_at = fail_gpio_level_at = 0;
    identity[0] = 0xb6;
    auto_sleep = 1;
    memset(sample_data, 0, sizeof(sample_data));
    assert(ryz_touch_init() == ESP_OK);
    ryz_touch_get_info(&info);
    assert(info.ready && info.reads == 1 && info.errors == 0 && info.presses == 0);
    assert(bus_creates == 1 && registrations[0] == (stage == 4 ? 2U : 1U));
    assert(registrations[1] == 1 && mutex_creates == 1 && mutex_deletes == 0);
    check_configuration();
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "init")) case_init();
    else if (!strcmp(argv[1], "transfers")) case_transfers();
    else if (!strcmp(argv[1], "probe")) case_probe();
    else if (!strcmp(argv[1], "serialization")) case_serialization();
    else if (!strcmp(argv[1], "try_probe")) case_try_probe();
    else if (!strcmp(argv[1], "touch_d")) case_touch(0xb6);
    else if (!strcmp(argv[1], "touch_t")) case_touch(0xb5);
    else if (!strncmp(argv[1], "touch_failure_", 14)) case_touch_failure((unsigned)strtoul(argv[1] + 14, NULL, 10));
    else abort();
    assert(active_transfers == 0 && !owns_bus_mutex);
    /* Host process teardown only; production exposes no bus teardown API. */
    if (mutex_live) vSemaphoreDelete(&bus_mutex);
    printf("BOARD_I2C_PASS %s\n", argv[1]);
    return 0;
}
