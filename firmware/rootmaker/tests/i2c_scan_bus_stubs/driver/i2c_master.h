#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct i2c_master_bus_t *i2c_master_bus_handle_t;
typedef struct i2c_master_dev_t *i2c_master_dev_handle_t;
enum { I2C_NUM_1 = 1, I2C_CLK_SRC_DEFAULT = 1, I2C_ADDR_BIT_LEN_7 = 0 };
#define I2C_DEVICE_ADDRESS_NOT_USED 0xffff
typedef struct {
    int i2c_port, sda_io_num, scl_io_num, clk_source, glitch_ignore_cnt;
    size_t trans_queue_depth;
    struct { bool enable_internal_pullup; } flags;
} i2c_master_bus_config_t;
typedef struct {
    int dev_addr_length;
    uint16_t device_address;
    uint32_t scl_speed_hz, scl_wait_us;
    struct { bool disable_ack_check; } flags;
} i2c_device_config_t;
typedef enum { I2C_MASTER_CMD_START, I2C_MASTER_CMD_WRITE, I2C_MASTER_CMD_STOP } i2c_master_command_t;
typedef struct {
    i2c_master_command_t command;
    struct { bool ack_check; uint8_t *data; size_t total_bytes; } write;
} i2c_operation_job_t;
esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *, i2c_master_bus_handle_t *);
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t, const i2c_device_config_t *, i2c_master_dev_handle_t *);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t);
esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t);
esp_err_t i2c_master_execute_defined_operations(i2c_master_dev_handle_t, i2c_operation_job_t *, size_t, int);
