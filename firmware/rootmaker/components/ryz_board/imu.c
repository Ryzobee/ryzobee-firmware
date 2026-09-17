#include "imu.h"

#include <stdatomic.h>
#include <string.h>
#include "board_i2c.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Register contract: ST LIS2DW12 DS11811 Rev9, AN5038 Rev6. The reference
 * Arduino BSP at 8ec48fd instantiates LIS2DW12Sensor; it does not establish the
 * installed PCB's SA0 strap or mounting transform. Conversion agrees with ST
 * lis2dw12-pid d0a476d31f5fed47333e23fd72927f78bdd6e9b3 (HP14, +/-2g).
 * This is a bounded polling subset, not the Arduino driver's error handling.
 */
enum {
    WHO_AM_I = 0x0f, EXPECTED_ID = 0x44,
    CTRL1 = 0x20, CTRL2 = 0x21, CTRL6 = 0x25,
    STATUS = 0x27, OUT_X_L = 0x28, FIFO_CTRL = 0x2e, CTRL7 = 0x3f,
    SOFT_RESET = 0x40, BDU_INCREMENT = 0x0c,
    HP_POWER_DOWN = 0x04, HP_25HZ = 0x34, LPF_ODR_DIV4_2G = 0x40,
    RESET_READ_LIMIT = 10,
};

static atomic_flag s_guard = ATOMIC_FLAG_INIT;
static ryz_imu_info_t s_info = {.last_error = ESP_ERR_INVALID_STATE};
static ryz_board_i2c_device_t s_device = RYZ_BOARD_I2C_IMU_LOW;
static bool s_discard_first;

static bool enter(void)
{
    return !atomic_flag_test_and_set_explicit(&s_guard, memory_order_acquire);
}

static void leave(void)
{
    atomic_flag_clear_explicit(&s_guard, memory_order_release);
}

static void increment(uint32_t *value)
{
    if (*value != UINT32_MAX) ++*value;
}

static esp_err_t failure(esp_err_t error)
{
    s_info.ready = false;
    s_info.last_error = error;
    increment(&s_info.errors);
    return error;
}

static void delay_ms(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks ? ticks : 1);
}

static esp_err_t read_reg(uint8_t reg, uint8_t *data, size_t length)
{
    return ryz_board_i2c_read_reg(s_device, reg, data, length);
}

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    const uint8_t data[] = {reg, value};
    return ryz_board_i2c_write(s_device, data, sizeof(data));
}

static esp_err_t write_checked(uint8_t reg, uint8_t value)
{
    esp_err_t error = write_reg(reg, value);
    uint8_t actual = 0;
    if (error == ESP_OK) error = read_reg(reg, &actual, 1);
    if (error == ESP_OK && actual != value) error = ESP_ERR_INVALID_RESPONSE;
    return error;
}

static esp_err_t identify(void)
{
    const ryz_board_i2c_device_t devices[] = {
        RYZ_BOARD_I2C_IMU_LOW, RYZ_BOARD_I2C_IMU_HIGH,
    };
    const uint8_t addresses[] = {0x18, 0x19};
    unsigned matches = 0;
    unsigned responding = 0;
    unsigned selected = 0;
    for (unsigned i = 0; i < 2; ++i) {
        /* IDF 5.5.4 combined transfers can collapse NACK and physical timeout
         * into INVALID_STATE. Only a probe's explicit NOT_FOUND proves NACK;
         * neither a probe timeout nor a failed ID read is treated as absence. */
        esp_err_t error = ryz_board_i2c_probe(addresses[i]);
        if (error == ESP_ERR_NOT_FOUND) continue;
        if (error != ESP_OK) return error;
        uint8_t id = 0;
        error = ryz_board_i2c_read_reg(devices[i], WHO_AM_I, &id, 1);
        if (error != ESP_OK) return error;
        ++responding;
        if (id == EXPECTED_ID) {
            ++matches;
            selected = i;
        }
    }
    if (matches > 1) return ESP_ERR_INVALID_STATE;
    if (!matches) return responding ? ESP_ERR_NOT_SUPPORTED : ESP_ERR_NOT_FOUND;
    s_device = devices[selected];
    s_info.address = addresses[selected];
    s_info.who_am_i = EXPECTED_ID;
    return ESP_OK;
}

static esp_err_t configure(void)
{
    esp_err_t error = write_reg(CTRL2, SOFT_RESET);
    if (error != ESP_OK) return error;
    const int64_t started = esp_timer_get_time();
    bool reset_done = false;
    for (unsigned attempt = 0; attempt < RESET_READ_LIMIT; ++attempt) {
        if (esp_timer_get_time() - started >= 100000) return ESP_ERR_TIMEOUT;
        uint8_t ctrl2 = 0;
        error = read_reg(CTRL2, &ctrl2, 1);
        if (error != ESP_OK) return error;
        if (esp_timer_get_time() - started >= 100000) return ESP_ERR_TIMEOUT;
        if (!(ctrl2 & SOFT_RESET)) {
            reset_done = true;
            break;
        }
        if (attempt + 1 < RESET_READ_LIMIT) delay_ms(2);
    }
    if (!reset_done) return ESP_ERR_TIMEOUT;

    /* Enable auto-increment before any burst. Program only documented control
     * registers, with conversion stopped until every critical value is read
     * back. Reserved bits, self-test, interrupts and user offsets stay off. */
    error = write_checked(CTRL2, BDU_INCREMENT);
    if (error != ESP_OK) return error;
    const uint8_t controls[] = {
        CTRL1, HP_POWER_DOWN, BDU_INCREMENT, 0, 0, 0, LPF_ODR_DIV4_2G,
    };
    error = ryz_board_i2c_write(s_device, controls, sizeof(controls));
    if (error != ESP_OK) return error;
    uint8_t actual[6] = {0};
    error = read_reg(CTRL1, actual, sizeof(actual));
    if (error != ESP_OK) return error;
    if (memcmp(actual, controls + 1, sizeof(actual))) return ESP_ERR_INVALID_RESPONSE;
    error = write_checked(FIFO_CTRL, 0);
    if (error == ESP_OK) error = write_checked(CTRL7, 0);
    if (error == ESP_OK) error = write_checked(CTRL1, HP_25HZ);
    return error;
}

esp_err_t ryz_imu_init(void)
{
    if (!enter()) return ESP_ERR_TIMEOUT;
    s_info.ready = false;
    s_info.address = 0;
    s_info.who_am_i = 0;
    s_info.resolution_bits = 0;
    s_info.full_scale_g = 0;
    s_info.odr_hz = 0;
    s_discard_first = false;
    esp_err_t error = ryz_board_i2c_init();
    if (error == ESP_OK) {
        /* ST's maximum power-on boot time, also safe for explicit retries. */
        delay_ms(20);
        error = identify();
    }
    if (error == ESP_OK) error = configure();
    if (error != ESP_OK) {
        /* One bounded best-effort peripheral power-down only. Keep the original
         * failure as the cause; no claim is made that this write succeeded. */
        if (s_info.address) (void)write_reg(CTRL1, HP_POWER_DOWN);
        failure(error);
    } else {
        s_info.ready = true;
        s_info.resolution_bits = 14;
        s_info.full_scale_g = 2;
        s_info.odr_hz = 25;
        s_info.last_error = ESP_OK;
        /* AN5038 Table13: HP25Hz with LPF ODR/4 discards one fresh sample. */
        s_discard_first = true;
    }
    leave();
    return error;
}

esp_err_t ryz_imu_deinit(void)
{
    if (!enter()) return ESP_ERR_TIMEOUT;
    esp_err_t error = s_info.address ? write_checked(CTRL1, HP_POWER_DOWN) : ESP_OK;
    s_info.ready = false;
    s_discard_first = false;
    if (error != ESP_OK) failure(error);
    else s_info.last_error = ESP_ERR_INVALID_STATE;
    leave();
    return error;
}

static float axis_mg(const uint8_t *bytes)
{
    const uint16_t packed = (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
    /* Explicit 14-bit sign extension avoids implementation-defined shifting
     * of a negative signed integer. The two low bits are not resolution. */
    int32_t raw14 = (int32_t)(packed >> 2);
    if (raw14 >= 8192) raw14 -= 16384;
    return (float)raw14 * 0.244f;
}

esp_err_t ryz_imu_read_sample(ryz_imu_sample_t *sample)
{
    if (!sample) return ESP_ERR_INVALID_ARG;
    memset(sample, 0, sizeof(*sample));
    if (!enter()) return ESP_ERR_TIMEOUT;
    if (!s_info.ready) {
        leave();
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t status = 0;
    esp_err_t error = read_reg(STATUS, &status, 1);
    if (error == ESP_OK && !(status & 1)) {
        leave();
        return ESP_ERR_NOT_FINISHED;
    }
    uint8_t bytes[6] = {0};
    if (error == ESP_OK) error = read_reg(OUT_X_L, bytes, sizeof(bytes));
    if (error != ESP_OK) {
        failure(error);
    } else if (s_discard_first) {
        s_discard_first = false;
        error = ESP_ERR_NOT_FINISHED;
    } else {
        increment(&s_info.samples);
        s_info.last_error = ESP_OK;
        sample->x_mg = axis_mg(bytes);
        sample->y_mg = axis_mg(bytes + 2);
        sample->z_mg = axis_mg(bytes + 4);
        sample->timestamp_us = (uint64_t)esp_timer_get_time();
        sample->sequence = s_info.samples;
    }
    leave();
    return error;
}

esp_err_t ryz_imu_get_info(ryz_imu_info_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;
    memset(info, 0, sizeof(*info));
    if (!enter()) return ESP_ERR_TIMEOUT;
    *info = s_info;
    leave();
    return ESP_OK;
}
