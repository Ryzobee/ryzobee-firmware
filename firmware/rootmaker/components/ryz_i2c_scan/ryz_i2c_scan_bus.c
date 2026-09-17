#include "ryz_i2c_scan_bus.h"
#include "ryz_i2c_scan_idf.h"
#include "ryz_tool_pins.h"
#include "board_i2c.h"
#include "driver/gpio.h"
#include "esp_private/esp_gpio_reserve.h"

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_device;
static uint32_t s_lease, s_controller_lease;
static int s_sda, s_scl;
static bool s_restore_pins, s_active, s_default;

esp_err_t ryz_i2c_scan_validate_config(const ryz_i2c_scan_config_t *config)
{
    if (!config || (config->hz != 100000 && config->hz != 400000)) return ESP_ERR_INVALID_ARG;
    if (config->sda == 41 && config->scl == 40 && config->hz == 100000) return ESP_OK;
    if (config->sda == config->scl ||
        ryz_tool_pin_reason(config->sda) != RYZ_TOOL_PIN_AVAILABLE ||
        ryz_tool_pin_reason(config->scl) != RYZ_TOOL_PIN_AVAILABLE) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}
esp_err_t ryz_i2c_scan_bus_begin(const ryz_i2c_scan_config_t *config)
{
    esp_err_t error = ryz_i2c_scan_bus_end();
    if (error != ESP_OK) return error;
    error = ryz_i2c_scan_validate_config(config);
    if (error != ESP_OK) return error;
    if (config->sda == 41 && config->scl == 40) {
        error = ryz_board_i2c_init();
        if (error == ESP_OK) s_active = s_default = true;
        return error;
    }
    if (!ryz_i2c_scan_idf_owner_valid()) return ESP_ERR_INVALID_STATE;
    error = ryz_tool_pins_claim_pair(config->sda, config->scl, &s_lease);
    if (error != ESP_OK) return error;
    s_sda = config->sda;
    s_scl = config->scl;
    error = ryz_tool_pins_claim_controller(RYZ_TOOL_CONTROLLER_I2C1, &s_controller_lease);
    if (error != ESP_OK) {
        const esp_err_t cleanup = ryz_i2c_scan_bus_end();
        return cleanup == ESP_OK ? error : cleanup;
    }
    if (esp_gpio_is_reserved((UINT64_C(1) << s_sda) | (UINT64_C(1) << s_scl)) ||
        !ryz_i2c_scan_idf_controller_available()) {
        error = ryz_i2c_scan_bus_end();
        return error == ESP_OK ? ESP_ERR_INVALID_STATE : error;
    }
    /* Even failed creation may have changed GPIO before IDF's own cleanup. */
    s_restore_pins = true;
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_1, .sda_io_num = s_sda, .scl_io_num = s_scl,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .trans_queue_depth = 0, .flags.enable_internal_pullup = true,
    };
    error = i2c_new_master_bus(&bus_config, &s_bus);
    if (error != ESP_OK) return error;
    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = I2C_DEVICE_ADDRESS_NOT_USED,
        .scl_speed_hz = config->hz, .scl_wait_us = 3000,
    };
    error = i2c_master_bus_add_device(s_bus, &device_config, &s_device);
    if (error == ESP_OK) s_active = true;
    return error;
}
esp_err_t ryz_i2c_scan_bus_probe(uint8_t address)
{
    if (address < RYZ_I2C_SCAN_FIRST || address > RYZ_I2C_SCAN_LAST) return ESP_ERR_INVALID_ARG;
    if (!s_active) return ESP_ERR_INVALID_STATE;
    return s_default ? ryz_board_i2c_try_probe(address) :
        ryz_i2c_scan_idf_probe(s_bus, s_device, address);
}
esp_err_t ryz_i2c_scan_bus_end(void)
{
    s_active = s_default = false;
    if (ryz_i2c_scan_bus_resources_held() && !ryz_i2c_scan_idf_owner_valid()) return ESP_ERR_INVALID_STATE;
    esp_err_t error;
    if (s_device) {
        error = i2c_master_bus_rm_device(s_device);
        if (error != ESP_OK) return error;
        s_device = NULL;
    }
    if (s_bus) {
        /* Audited same-core/non-PM IDF path only. Do not generalize this to
         * arbitrary IDF destruction failures: that SDK can suppress a failed
         * cross-core interrupt free after consuming its wrapper. */
        error = i2c_del_master_bus(s_bus);
        if (error != ESP_OK) return error;
        s_bus = NULL;
    }
    if (s_restore_pins) {
        error = gpio_reset_pin(s_sda);
        if (error != ESP_OK) return error;
        error = gpio_reset_pin(s_scl);
        if (error != ESP_OK) return error;
        s_restore_pins = false;
    }
    if (s_lease) {
        error = ryz_tool_pins_release(s_lease);
        if (error != ESP_OK) return error;
        s_lease = 0;
    }
    if (s_controller_lease) {
        error = ryz_tool_pins_release_controller(s_controller_lease);
        if (error != ESP_OK) return error;
        s_controller_lease = 0;
    }
    return ESP_OK;
}
bool ryz_i2c_scan_bus_resources_held(void)
{
    return s_bus || s_device || s_lease || s_controller_lease || s_restore_pins;
}
