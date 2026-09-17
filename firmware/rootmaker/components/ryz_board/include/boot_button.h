#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* ESP32-S3 ROM BOOT button convention. This is a runtime input only; the
 * firmware never drives GPIO0 and therefore does not alter download mode. */
#define RYZ_BOOT_BUTTON_GPIO 0

esp_err_t ryz_boot_button_init(void);
esp_err_t ryz_boot_button_read(bool *pressed);
