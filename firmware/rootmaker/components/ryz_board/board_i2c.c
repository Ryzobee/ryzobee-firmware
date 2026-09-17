#include "board_i2c.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

enum { UNINITIALIZED, INITIALIZING, READY };
static atomic_int s_state = ATOMIC_VAR_INIT(UNINITIALIZED);
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_devices[RYZ_BOARD_I2C_DEVICE_COUNT];
static SemaphoreHandle_t s_mutex;
static const uint8_t s_addresses[RYZ_BOARD_I2C_DEVICE_COUNT] = {0x15, 0x18, 0x19};

esp_err_t ryz_board_i2c_init(void)
{
    int expected = UNINITIALIZED;
    if (!atomic_compare_exchange_strong(&s_state, &expected, INITIALIZING)) {
        return expected == READY ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    s_mutex = xSemaphoreCreateMutex();
    esp_err_t error = s_mutex ? ESP_OK : ESP_ERR_NO_MEM;
    if (error == ESP_OK) {
        const i2c_master_bus_config_t config = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = GPIO_NUM_41, .scl_io_num = GPIO_NUM_40,
            .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        error = i2c_new_master_bus(&config, &s_bus);
    }
    if (error != ESP_OK) {
        if (s_mutex) vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        s_bus = NULL;
        atomic_store(&s_state, UNINITIALIZED);
        return error;
    }
    atomic_store(&s_state, READY);
    return ESP_OK;
}

static esp_err_t lock_bus(bool probe)
{
    if (atomic_load(&s_state) != READY) return ESP_ERR_INVALID_STATE;
    TickType_t ticks = probe ? 0 : pdMS_TO_TICKS(20);
    if (!probe && !ticks) ticks = 1;
    if (xSemaphoreTake(s_mutex, ticks) == pdTRUE) return ESP_OK;
    return probe ? ESP_ERR_NOT_FINISHED : ESP_ERR_TIMEOUT;
}

static esp_err_t device_locked(ryz_board_i2c_device_t device)
{
    if (s_devices[device]) return ESP_OK;
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = s_addresses[device],
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t handle = NULL;
    esp_err_t error = i2c_master_bus_add_device(s_bus, &config, &handle);
    if (error == ESP_OK) s_devices[device] = handle;
    return error;
}

esp_err_t ryz_board_i2c_read_reg(ryz_board_i2c_device_t device, uint8_t reg,
                                uint8_t *data, size_t length)
{
    if (!data || !length || length > RYZ_BOARD_I2C_READ_MAX) return ESP_ERR_INVALID_ARG;
    memset(data, 0, length);
    if ((unsigned)device >= RYZ_BOARD_I2C_DEVICE_COUNT) return ESP_ERR_INVALID_ARG;
    esp_err_t error = lock_bus(false);
    if (error != ESP_OK) return error;
    error = device_locked(device);
    if (error == ESP_OK) {
        error = i2c_master_transmit_receive(s_devices[device], &reg, 1,
                                            data, length, 20);
    }
    if (error != ESP_OK) memset(data, 0, length);
    xSemaphoreGive(s_mutex);
    return error;
}

esp_err_t ryz_board_i2c_write(ryz_board_i2c_device_t device,
                             const uint8_t *data, size_t length)
{
    if ((unsigned)device >= RYZ_BOARD_I2C_DEVICE_COUNT || !data || !length ||
        length > RYZ_BOARD_I2C_WRITE_MAX) return ESP_ERR_INVALID_ARG;
    esp_err_t error = lock_bus(false);
    if (error != ESP_OK) return error;
    error = device_locked(device);
    if (error == ESP_OK) {
        error = i2c_master_transmit(s_devices[device], data, length, 20);
    }
    xSemaphoreGive(s_mutex);
    return error;
}

esp_err_t ryz_board_i2c_try_probe(uint8_t address)
{
    if (address < 0x08 || address > 0x77) return ESP_ERR_INVALID_ARG;
    esp_err_t error = lock_bus(true);
    if (error != ESP_OK) return error;
    error = i2c_master_probe(s_bus, address, 3);
    xSemaphoreGive(s_mutex);
    return error;
}

esp_err_t ryz_board_i2c_probe(uint8_t address)
{
    const esp_err_t error = ryz_board_i2c_try_probe(address);
    return error == ESP_ERR_NOT_FINISHED ? ESP_ERR_TIMEOUT : error;
}
