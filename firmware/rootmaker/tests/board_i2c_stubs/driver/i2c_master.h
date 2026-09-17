#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"
enum { I2C_NUM_0 = 0, I2C_CLK_SRC_DEFAULT = 0, I2C_ADDR_BIT_LEN_7 = 0 };
typedef struct host_i2c_bus *i2c_master_bus_handle_t;
typedef struct host_i2c_device *i2c_master_dev_handle_t;
typedef struct {
    int i2c_port;
    gpio_num_t sda_io_num;
    gpio_num_t scl_io_num;
    int clk_source;
    uint8_t glitch_ignore_cnt;
    int intr_priority;
    size_t trans_queue_depth;
    struct { bool enable_internal_pullup; bool allow_pd; } flags;
} i2c_master_bus_config_t;
typedef struct {
    int dev_addr_length;
    uint16_t device_address;
    uint32_t scl_speed_hz;
    uint32_t scl_wait_us;
    struct { bool disable_ack_check; } flags;
} i2c_device_config_t;
esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *config,
                            i2c_master_bus_handle_t *out);
esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t bus);
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
                                   const i2c_device_config_t *config,
                                   i2c_master_dev_handle_t *out);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device,
                                     const uint8_t *write_buffer, size_t write_size,
                                     uint8_t *read_buffer, size_t read_size,
                                     int timeout_ms);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device,
                             const uint8_t *write_buffer, size_t write_size,
                             int timeout_ms);
esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address,
                          int timeout_ms);
