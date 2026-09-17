#pragma once

#include <stdbool.h>
#include "ryz_i2c_scan.h"

/* Internal single-worker Interface. begin first retries prior teardown, then
 * borrows default I2C0 or claims two pins and creates synchronous I2C1 for this
 * scan. No externally accessible handles, callbacks, or register/data writes.
 * Custom probes send only START/address+W/STOP at the configured 100/400kHz.
 * Default shared bus remains fixed at 100kHz and is never destroyed/reset. */
esp_err_t ryz_i2c_scan_bus_begin(const ryz_i2c_scan_config_t *config);
esp_err_t ryz_i2c_scan_bus_probe(uint8_t address);
/* Successful cleanup means no custom bus/device/pin lease remains. On failure
 * retain valid remaining resources for a later begin/end retry; never pretend
 * pins are free or destroy a possibly live handle. No default bus teardown. */
esp_err_t ryz_i2c_scan_bus_end(void);
bool ryz_i2c_scan_bus_resources_held(void);
