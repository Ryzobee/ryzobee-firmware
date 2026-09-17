#pragma once
#include "esp_err.h"
/* Host ABI-shaped seam only; not a copy of the complete SDK bus layout. */
typedef int i2c_port_num_t;
typedef enum { I2C_BUS_MODE_MASTER, I2C_BUS_MODE_SLAVE } i2c_bus_mode_t;
typedef struct i2c_bus_t { i2c_port_num_t port_num; } *i2c_bus_handle_t;
esp_err_t i2c_acquire_bus_handle(i2c_port_num_t port_num,
                               i2c_bus_handle_t *out, i2c_bus_mode_t mode);
