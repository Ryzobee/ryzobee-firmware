#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "board_i2c.h"
#include "imu.h"
#include "freertos/task.h"

/* A register-level board Adapter: real production imu.c, no live I2C device.
 * The peer obeys ST's reset, auto-increment, data-ready and little-endian
 * contracts. Error injection deliberately models IDF 5.5.4 INVALID_STATE
 * for a failed acknowledged transaction, distinct from probe NOT_FOUND. */
static uint8_t registers[2][64];
static esp_err_t probes[2] = {ESP_ERR_NOT_FOUND, ESP_OK};
static unsigned reset_reads[2];
static unsigned writes;
static unsigned reads;
static unsigned probe_calls;
static unsigned reset_writes;
static unsigned delays;
static int64_t now_us;
static esp_err_t init_error;
static esp_err_t read_error;
static int read_error_reg = -1;
static esp_err_t write_error;
static int write_error_reg = -1;
static bool reset_stuck;
static bool corrupt_config;
static bool enforce_deadline;
static bool gate_armed;
static bool gate_entered;
static bool gate_release;
static pthread_mutex_t gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_condition = PTHREAD_COND_INITIALIZER;

int64_t esp_timer_get_time(void) { return now_us; }
void vTaskDelay(TickType_t ticks) { now_us += (int64_t)ticks * 1000; ++delays; }
esp_err_t ryz_board_i2c_init(void) { return init_error; }

esp_err_t ryz_board_i2c_probe(uint8_t address)
{
    assert(address == 0x18 || address == 0x19);
    ++probe_calls;
    return probes[address - 0x18];
}

esp_err_t ryz_board_i2c_read_reg(ryz_board_i2c_device_t device, uint8_t reg,
                                uint8_t *data, size_t length)
{
    assert(device == RYZ_BOARD_I2C_IMU_LOW || device == RYZ_BOARD_I2C_IMU_HIGH);
    assert(data && length && length <= RYZ_BOARD_I2C_READ_MAX);
    assert(reg + length <= sizeof(registers[0]));
    unsigned slot = (unsigned)device - RYZ_BOARD_I2C_IMU_LOW;
    assert(probes[slot] == ESP_OK); /* No WHO_AM_I read before candidate ACK. */
    ++reads;
    memset(data, 0, length);
    pthread_mutex_lock(&gate_mutex);
    if (gate_armed) {
        gate_entered = true;
        pthread_cond_broadcast(&gate_condition);
        while (!gate_release) pthread_cond_wait(&gate_condition, &gate_mutex);
        gate_armed = false;
    }
    pthread_mutex_unlock(&gate_mutex);
    if (read_error_reg == reg && read_error != ESP_OK) return read_error;
    if (reg == 0x21 && (registers[slot][0x21] & 0x40)) {
        ++reset_reads[slot];
        if (enforce_deadline) now_us += 100000;
        if (!reset_stuck) {
            uint8_t id = registers[slot][0x0f];
            memset(registers[slot], 0, sizeof(registers[slot]));
            registers[slot][0x0f] = id;
            registers[slot][0x21] = 0x04; /* IF_ADD_INC reset value. */
        }
    }
    if (length > 1) assert(registers[slot][0x21] & 0x04);
    memcpy(data, registers[slot] + reg, length);
    if (corrupt_config && reg == 0x20 && length == 6) data[5] ^= 0x10;
    if (reg == 0x28) registers[slot][0x27] &= (uint8_t)~1U;
    return ESP_OK;
}

esp_err_t ryz_board_i2c_write(ryz_board_i2c_device_t device,
                             const uint8_t *data, size_t length)
{
    assert(device == RYZ_BOARD_I2C_IMU_LOW || device == RYZ_BOARD_I2C_IMU_HIGH);
    assert(data && length >= 2 && length <= RYZ_BOARD_I2C_WRITE_MAX);
    unsigned slot = (unsigned)device - RYZ_BOARD_I2C_IMU_LOW;
    assert(probes[slot] == ESP_OK && registers[slot][0x0f] == 0x44);
    assert(data[0] + length - 1 <= sizeof(registers[0]));
    ++writes;
    if (write_error_reg == data[0] && write_error != ESP_OK) return write_error;
    if (length > 2) assert(registers[slot][0x21] & 0x04);
    memcpy(registers[slot] + data[0], data + 1, length - 1);
    if (data[0] == 0x21 && data[1] == 0x40) ++reset_writes;
    return ESP_OK;
}

static void expect_zero(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    for (size_t i = 0; i < size; ++i) assert(bytes[i] == 0);
}

static ryz_imu_info_t info(void)
{
    ryz_imu_info_t result;
    unsigned io_before = reads + writes + probe_calls;
    assert(ryz_imu_get_info(&result) == ESP_OK);
    assert(reads + writes + probe_calls == io_before);
    return result;
}

static void fresh(unsigned slot)
{
    /* Independent ST example values: +4096 counts = +999.424mg;
     * -4096 = -999.424mg, -8192 = -1998.848mg, packed left by two. */
    const uint8_t axes[] = {0x00, 0x40, 0x00, 0xc0, 0x00, 0x80};
    memcpy(registers[slot] + 0x28, axes, sizeof(axes));
    registers[slot][0x27] |= 1;
    now_us += 40000;
}

static void test_fresh(void)
{
    assert(ryz_imu_init() == ESP_OK);
    ryz_imu_info_t state = info();
    assert(state.ready && state.address == 0x19 && state.who_am_i == 0x44);
    assert(state.odr_hz == 25 && state.full_scale_g == 2 && state.resolution_bits == 14);
    assert(registers[1][0x20] == 0x34 && registers[1][0x21] == 0x0c);
    assert(registers[1][0x25] == 0x40 && registers[1][0x2e] == 0);
    assert(registers[1][0x23] == 0 && registers[1][0x24] == 0 && registers[1][0x3f] == 0);
    ryz_imu_sample_t sample;
    memset(&sample, 0xa5, sizeof(sample));
    assert(ryz_imu_read_sample(&sample) == ESP_ERR_NOT_FINISHED);
    expect_zero(&sample, sizeof(sample));
    fresh(1);
    assert(ryz_imu_read_sample(&sample) == ESP_ERR_NOT_FINISHED);
    expect_zero(&sample, sizeof(sample));
    assert(info().samples == 0);
    fresh(1);
    assert(ryz_imu_read_sample(&sample) == ESP_OK);
    assert(fabsf(sample.x_mg - 999.424f) < 0.001f);
    assert(fabsf(sample.y_mg + 999.424f) < 0.001f);
    assert(fabsf(sample.z_mg + 1998.848f) < 0.001f);
    assert(sample.timestamp_us == (uint64_t)now_us && sample.sequence == 1);
    assert(ryz_imu_read_sample(&sample) == ESP_ERR_NOT_FINISHED);
    expect_zero(&sample, sizeof(sample));
    assert(info().samples == 1 && info().errors == 0);
    unsigned resets = reset_writes;
    assert(ryz_imu_init() == ESP_OK && reset_writes == resets + 1);
    fresh(1);
    assert(ryz_imu_read_sample(&sample) == ESP_ERR_NOT_FINISHED);
    fresh(1);
    assert(ryz_imu_read_sample(&sample) == ESP_OK && sample.sequence == 2);
}

static void test_identification(void)
{
    probes[1] = ESP_ERR_NOT_FOUND;
    assert(ryz_imu_init() == ESP_ERR_NOT_FOUND);
    assert(writes == 0 && reads == 0 && !info().ready);
    probes[0] = ESP_ERR_TIMEOUT;
    assert(ryz_imu_init() == ESP_ERR_TIMEOUT && writes == 0 && reads == 0);
    probes[0] = ESP_OK;
    registers[0][0x0f] = 0x33;
    assert(ryz_imu_init() == ESP_ERR_NOT_SUPPORTED && writes == 0);
    registers[0][0x0f] = 0x44;
    probes[1] = ESP_ERR_TIMEOUT; /* One valid ID never masks the other's timeout. */
    assert(ryz_imu_init() == ESP_ERR_TIMEOUT && writes == 0);
    probes[1] = ESP_OK;
    registers[1][0x0f] = 0x44;
    assert(ryz_imu_init() == ESP_ERR_INVALID_STATE && writes == 0);
    probes[1] = ESP_ERR_NOT_FOUND;
    read_error_reg = 0x0f;
    read_error = ESP_ERR_INVALID_STATE;
    assert(ryz_imu_init() == ESP_ERR_INVALID_STATE && writes == 0);
    read_error = ESP_ERR_TIMEOUT;
    assert(ryz_imu_init() == ESP_ERR_TIMEOUT && writes == 0);
    read_error = ESP_OK;
    assert(ryz_imu_init() == ESP_OK && info().address == 0x18);
}

static void test_reset_and_config(void)
{
    reset_stuck = true;
    assert(ryz_imu_init() == ESP_ERR_TIMEOUT);
    assert(reset_reads[1] == 10 && !info().ready && writes == 2);
    assert(registers[1][0x20] == 0x04); /* One best-effort power-down. */
    reset_stuck = false;
    enforce_deadline = true;
    unsigned before = reset_reads[1];
    assert(ryz_imu_init() == ESP_ERR_TIMEOUT && reset_reads[1] == before + 1);
    enforce_deadline = false;
    corrupt_config = true;
    assert(ryz_imu_init() == ESP_ERR_INVALID_RESPONSE && !info().ready);
    assert(registers[1][0x20] == 0x04);
    corrupt_config = false;
    write_error_reg = 0x2e;
    write_error = ESP_FAIL;
    assert(ryz_imu_init() == ESP_FAIL && info().last_error == ESP_FAIL);
    write_error = ESP_OK;
    assert(ryz_imu_init() == ESP_OK && info().errors == 4);
}

static void test_read_failure(void)
{
    ryz_imu_sample_t sample;
    assert(ryz_imu_init() == ESP_OK);
    fresh(1);
    assert(ryz_imu_read_sample(&sample) == ESP_ERR_NOT_FINISHED);
    fresh(1);
    read_error_reg = 0x28;
    read_error = ESP_ERR_INVALID_STATE;
    memset(&sample, 0xa5, sizeof(sample));
    assert(ryz_imu_read_sample(&sample) == ESP_ERR_INVALID_STATE);
    expect_zero(&sample, sizeof(sample));
    assert(!info().ready && info().errors == 1 && info().samples == 0);
    unsigned before = reads;
    assert(ryz_imu_read_sample(&sample) == ESP_ERR_INVALID_STATE && reads == before);
    read_error = ESP_OK;
    assert(ryz_imu_init() == ESP_OK);
    read_error_reg = 0x27;
    read_error = ESP_ERR_TIMEOUT;
    assert(ryz_imu_read_sample(&sample) == ESP_ERR_TIMEOUT);
    assert(!info().ready && info().last_error == ESP_ERR_TIMEOUT);
    expect_zero(&sample, sizeof(sample));
}

static void *initialize_thread(void *unused)
{
    (void)unused;
    assert(ryz_imu_init() == ESP_OK);
    return NULL;
}

static void test_concurrent(void)
{
    gate_armed = true;
    pthread_t thread;
    assert(pthread_create(&thread, NULL, initialize_thread, NULL) == 0);
    pthread_mutex_lock(&gate_mutex);
    while (!gate_entered) pthread_cond_wait(&gate_condition, &gate_mutex);
    pthread_mutex_unlock(&gate_mutex);
    ryz_imu_sample_t sample;
    ryz_imu_info_t state;
    memset(&sample, 0xa5, sizeof(sample));
    memset(&state, 0xa5, sizeof(state));
    assert(ryz_imu_init() == ESP_ERR_TIMEOUT);
    assert(ryz_imu_read_sample(&sample) == ESP_ERR_TIMEOUT);
    assert(ryz_imu_get_info(&state) == ESP_ERR_TIMEOUT);
    expect_zero(&sample, sizeof(sample));
    expect_zero(&state, sizeof(state));
    pthread_mutex_lock(&gate_mutex);
    gate_release = true;
    pthread_cond_broadcast(&gate_condition);
    pthread_mutex_unlock(&gate_mutex);
    assert(pthread_join(thread, NULL) == 0 && info().ready);
}

static void test_bounds(void)
{
    ryz_imu_sample_t sample;
    assert(ryz_imu_get_info(NULL) == ESP_ERR_INVALID_ARG);
    assert(ryz_imu_read_sample(NULL) == ESP_ERR_INVALID_ARG);
    assert(ryz_imu_read_sample(&sample) == ESP_ERR_INVALID_STATE);
    expect_zero(&sample, sizeof(sample));
    assert(!info().ready && info().last_error == ESP_ERR_INVALID_STATE);
    init_error = ESP_ERR_NO_MEM;
    assert(ryz_imu_init() == ESP_ERR_NO_MEM);
    assert(!delays && !reads && !writes && !probe_calls);
    init_error = ESP_OK;
    assert(ryz_imu_init() == ESP_OK);
    /* Max positive, smallest negative and zero; low unused bits ignored. */
    const uint8_t axes[] = {0xff, 0x7f, 0xff, 0xff, 0x03, 0x00};
    fresh(1);
    assert(ryz_imu_read_sample(&sample) == ESP_ERR_NOT_FINISHED);
    memcpy(registers[1] + 0x28, axes, sizeof(axes));
    registers[1][0x27] = 1;
    assert(ryz_imu_read_sample(&sample) == ESP_OK);
    assert(fabsf(sample.x_mg - 1998.604f) < 0.001f);
    assert(fabsf(sample.y_mg + 0.244f) < 0.001f && sample.z_mg == 0.0f);
}

static void test_deinit(void)
{
    assert(ryz_imu_deinit() == ESP_OK && !writes);
    assert(ryz_imu_init() == ESP_OK);
    assert(ryz_imu_deinit() == ESP_OK && !info().ready);
    assert(registers[1][0x20] == 0x04);
    ryz_imu_sample_t sample;
    assert(ryz_imu_read_sample(&sample) == ESP_ERR_INVALID_STATE);
    assert(ryz_imu_init() == ESP_OK);
    write_error_reg = 0x20;
    write_error = ESP_FAIL;
    assert(ryz_imu_deinit() == ESP_FAIL && !info().ready && info().last_error == ESP_FAIL);
    write_error = ESP_OK;
    assert(ryz_imu_deinit() == ESP_OK);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    registers[1][0x0f] = 0x44;
    if (!strcmp(argv[1], "fresh")) test_fresh();
    else if (!strcmp(argv[1], "identification")) test_identification();
    else if (!strcmp(argv[1], "reset_config")) test_reset_and_config();
    else if (!strcmp(argv[1], "read_failure")) test_read_failure();
    else if (!strcmp(argv[1], "concurrent")) test_concurrent();
    else if (!strcmp(argv[1], "bounds")) test_bounds();
    else if (!strcmp(argv[1], "deinit")) test_deinit();
    else assert(false);
    printf("IMU_PASS %s\n", argv[1]);
    return 0;
}
