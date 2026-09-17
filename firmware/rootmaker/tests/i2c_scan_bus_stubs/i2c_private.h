#pragma once
#include <stdatomic.h>
#include "driver/i2c_master.h"
enum { I2C_LL_CMD_WRITE = 1 };
typedef enum { I2C_STATUS_IDLE, I2C_STATUS_DONE, I2C_STATUS_ACK_ERROR, I2C_STATUS_TIMEOUT } i2c_master_status_t;
typedef struct {
    struct { unsigned op_code; } hw_cmd;
    uint8_t *data;
    size_t total_bytes, bytes_used;
} i2c_operation_t;
struct i2c_master_bus_t {
    _Atomic i2c_master_status_t status;
    i2c_operation_t i2c_ops[3];
    bool async_trans;
};
struct i2c_master_dev_t { struct i2c_master_bus_t *master_bus; };
bool i2c_bus_occupied(int port);
