#pragma once

#include "driver/i2c_master.h"

/* Private, synchronous, single-worker address-only Adapter. Reads the pinned
 * IDF's terminal diagnostic state; never writes private driver fields. */
esp_err_t ryz_i2c_scan_idf_probe(i2c_master_bus_handle_t bus,
                              i2c_master_dev_handle_t device, uint8_t address);
/* Dynamic I2C teardown is only audited on the pinned core: IDF 5.5.4 can
 * suppress a cross-core interrupt-free error after freeing the bus wrapper. */
bool ryz_i2c_scan_idf_owner_valid(void);
/* Includes native master/slave/initializing owners, unlike the public master
 * handle getter. Observation only: all future custom bus creators must share
 * the tool ownership policy; this is not an atomic SDK claim. */
bool ryz_i2c_scan_idf_controller_available(void);
