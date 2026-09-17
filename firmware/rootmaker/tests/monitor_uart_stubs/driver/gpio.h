#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef int gpio_num_t;
typedef struct {
    uint64_t pin_bit_mask;
    int mode, pull_up_en, pull_down_en, intr_type;
} gpio_config_t;
enum { GPIO_MODE_INPUT = 1, GPIO_PULLUP_ENABLE = 1,
       GPIO_PULLDOWN_DISABLE = 0, GPIO_INTR_DISABLE = 0 };
esp_err_t gpio_config(const gpio_config_t *config);
esp_err_t gpio_reset_pin(gpio_num_t pin);
