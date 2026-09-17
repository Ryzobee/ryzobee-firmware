#pragma once
/* Only the clock enum beneath the actual locked SDK public tsens header is
 * substituted. Host never claims to sample a real ADC/calibration/allocator. */
typedef enum { TEMPERATURE_SENSOR_CLK_SRC_DEFAULT=1 } temperature_sensor_clk_src_t;
#define SOC_TEMPERATURE_SENSOR_INTR_SUPPORT 0
