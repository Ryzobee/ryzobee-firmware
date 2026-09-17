#include "ryz_i2c_scan_idf.h"

#include <stdatomic.h>
#include "sdkconfig.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_private.h"

#if !CONFIG_IDF_TARGET_ESP32S3 || ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(5, 5, 4)
#error "Custom I2C diagnostics require the audited ESP32-S3 / ESP-IDF 5.5.4"
#endif
#if CONFIG_PM_ENABLE || CONFIG_FREERTOS_SMP || CONFIG_FREERTOS_UNICORE
#error "Custom I2C teardown requires the audited dual-core, non-SMP, non-PM configuration"
#endif

/* Not caller memory; survives failures. The two slots are chosen against the
 * driver's ACTUAL retained pointer, not blindly alternated per invocation. */
static uint8_t s_address_bytes[2];

bool ryz_i2c_scan_idf_owner_valid(void)
{
    /* Unbound tasks always use IPC in esp_intr_free(), even when currently on
     * the interrupt's core. Pinning removes its fallible IPC branch; with PM
     * off, same-core non-NULL interrupt free has no error return path. */
    return xTaskGetCoreID(NULL) == 1 && xPortGetCoreID() == 1;
}

bool ryz_i2c_scan_idf_controller_available(void)
{
    /* 5.5.4 duplicate acquire hands new_master_bus an existing base even when
     * returning INVALID_STATE. Its failure cleanup can deinit the old pins.
     * Avoid entering that path. Caller holds the shared I2C1 controller lease
     * before this check and creation; this extra unlocked SDK observation
     * rejects unbrokered owners but does not replace the cross-writer lease. */
    return ryz_i2c_scan_idf_owner_valid() && !i2c_bus_occupied(I2C_NUM_1);
}

esp_err_t ryz_i2c_scan_idf_probe(i2c_master_bus_handle_t bus,
                              i2c_master_dev_handle_t device, uint8_t address)
{
    if (!bus || !device || address < 0x08 || address > 0x77) return ESP_ERR_INVALID_ARG;
    if (!ryz_i2c_scan_idf_owner_valid() || bus->async_trans || device->master_bus != bus) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t *byte = bus->i2c_ops[1].data == &s_address_bytes[0] ?
        &s_address_bytes[1] : &s_address_bytes[0];
    *byte = (uint8_t)(address << 1);
    i2c_operation_job_t operations[] = {
        {.command = I2C_MASTER_CMD_START},
        {.command = I2C_MASTER_CMD_WRITE, .write = {
            .ack_check = true, .data = byte, .total_bytes = 1}},
        {.command = I2C_MASTER_CMD_STOP},
    };
    const esp_err_t error = i2c_master_execute_defined_operations(device, operations, 3, 3);
    /* 5.5.4 synchronous_transaction copies zeroed operations before bus clear;
     * WRITE then consumes its one byte. Pointer identity proves this call got
     * past memcpy; bytes_used proves it got past preflight. Reading status
     * alone would misclassify a prior NACK after a new pre-lock/clear failure.
     * Synchronous return disables interrupts and resets hardware on error.
     * No SDK private state is modified here. */
    const i2c_operation_t *write = &bus->i2c_ops[1];
    const bool fresh = write->data == byte && write->total_bytes == 1 &&
        write->bytes_used == 1 && write->hw_cmd.op_code == I2C_LL_CMD_WRITE;
    const i2c_master_status_t status = atomic_load(&bus->status);
    if (error == ESP_OK) return fresh && status == I2C_STATUS_DONE ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (error == ESP_ERR_INVALID_STATE && fresh) {
        if (status == I2C_STATUS_ACK_ERROR) return ESP_ERR_NOT_FOUND;
        if (status == I2C_STATUS_TIMEOUT) return ESP_ERR_TIMEOUT;
    }
    return error;
}
