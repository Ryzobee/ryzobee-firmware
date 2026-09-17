#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Board-owned I2C0, SDA41/SCL40, 100kHz. No IDF handle is exposed and no
 * client may reset/reconfigure/delete this shared bus. Successful init is
 * retained for the boot lifetime, including when one peripheral fails.
 * init is retryable; simultaneous initialization returns INVALID_STATE.
 * All other calls require successful init and are synchronous/task-only.
 * This is a trusted C Interface, not a raw-register user Lua capability. */
typedef enum {
    RYZ_BOARD_I2C_TOUCH = 0,       /* CST816: 7-bit 0x15 */
    RYZ_BOARD_I2C_IMU_LOW,         /* LIS2DW12 SA0 low: 0x18 */
    RYZ_BOARD_I2C_IMU_HIGH,        /* LIS2DW12 SA0 high: 0x19 */
    RYZ_BOARD_I2C_DEVICE_COUNT,
} ryz_board_i2c_device_t;

#define RYZ_BOARD_I2C_READ_MAX 32U
#define RYZ_BOARD_I2C_WRITE_MAX 33U

esp_err_t ryz_board_i2c_init(void);
/* Serialize registration and transfers with a 20ms mutex acquisition budget
 * and a separate 20ms driver timeout. They are NOT an end-to-end 20ms promise.
 * At most three device handles are lazily registered; an address ACK is not
 * implied by registration. Failed reads clear the supplied valid-size buffer.
 * Transfers do not retain caller buffers or register asynchronous callbacks. */
esp_err_t ryz_board_i2c_read_reg(ryz_board_i2c_device_t device, uint8_t reg,
                                uint8_t *data, size_t length);
esp_err_t ryz_board_i2c_write(ryz_board_i2c_device_t device,
                             const uint8_t *data, size_t length);
/* Trusted scan admission for one 7-bit address, 0x08..0x77. A busy board
 * mutex returns NOT_FINISHED immediately without calling the IDF driver.
 * After acquiring it, pass through the synchronous 3ms IDF probe result:
 * only NOT_FOUND means no ACK; TIMEOUT is a real probe/driver timeout.
 * Invalid address -> INVALID_ARG; bus not ready -> INVALID_STATE.
 * Never registers a device or recreates/reconfigures the shared bus. Callers
 * must yield/retry contention and must not hold a whole-scan bus lock. */
esp_err_t ryz_board_i2c_try_probe(uint8_t address);
/* Probe one non-reserved 7-bit address, 0x08..0x77. Never changes bus pins,
 * clock or registered devices. Lock acquisition is nonblocking; the driver
 * probe timeout is 3ms. BUSY and physical timeout both return TIMEOUT, never
 * NOT_FOUND. A scan must yield/retry rather than hold a whole-scan bus lock;
 * a TIMEOUT must not be shown as an absent device. Compatibility wrapper:
 * maps try_probe's NOT_FINISHED to TIMEOUT, preserving existing IMU callers. */
esp_err_t ryz_board_i2c_probe(uint8_t address);

#ifdef __cplusplus
}
#endif
