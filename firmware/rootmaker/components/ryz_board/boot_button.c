#include "boot_button.h"

#include "driver/gpio.h"

static bool ready;

esp_err_t ryz_boot_button_init(void)
{
    if (ready) return ESP_OK;
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << RYZ_BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t error = gpio_config(&config);
    if (error == ESP_OK) ready = true;
    return error;
}

esp_err_t ryz_boot_button_read(bool *pressed)
{
    if (!pressed) return ESP_ERR_INVALID_ARG;
    if (!ready) {
        *pressed = false;
        return ESP_ERR_INVALID_STATE;
    }
    const int level = gpio_get_level(RYZ_BOOT_BUTTON_GPIO);
    if (level < 0) {
        *pressed = false;
        return ESP_FAIL;
    }
    *pressed = level == 0;
    return ESP_OK;
}
