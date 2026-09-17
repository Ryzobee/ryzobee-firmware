#pragma once

#include "esp_err.h"
#include "touch_protocol.h"

typedef struct {
    bool ready;
    uint8_t chip_id;
    uint8_t project_id;
    uint8_t firmware_version;
    uint8_t factory_id;
    uint8_t disable_auto_sleep;
    uint32_t reads;
    uint32_t errors;
    uint32_t presses;
    uint32_t releases;
} ryz_touch_info_t;

/* Main task only. Owns I2C0 for now; only addresses the onboard 0x15 device.
 * Future IMU/power drivers must share this bus, not initialize I2C0 again.
 * read is a current sample, not a queue or an exactly-once event API. Event
 * phases are normalized from transitions between successful samples. */
esp_err_t ryz_touch_init(void);
esp_err_t ryz_touch_read(ryz_touch_sample_t *sample);
/* Sole SPI/input Owner, after display rotation is confirmed. No I2C work.
 * Caller must swallow any in-progress contact across this coordinate change. */
esp_err_t ryz_touch_set_rotation(uint8_t rotation);
void ryz_touch_get_info(ryz_touch_info_t *info);
