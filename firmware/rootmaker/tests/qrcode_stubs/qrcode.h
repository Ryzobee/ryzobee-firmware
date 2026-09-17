#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef const uint8_t *esp_qrcode_handle_t;

typedef struct {
    union {
        void (*display_func)(esp_qrcode_handle_t qrcode);
        void (*display_func_with_cb)(esp_qrcode_handle_t qrcode,
                                     void *user_data);
    };
    int max_qrcode_version;
    int qrcode_ecc_level;
    void *user_data;
} esp_qrcode_config_t;

enum {
    ESP_QRCODE_ECC_LOW = 0,
    ESP_QRCODE_ECC_MED,
    ESP_QRCODE_ECC_QUART,
    ESP_QRCODE_ECC_HIGH,
};

#define ESP_QRCODE_CONFIG_DEFAULT()                                            \
    (esp_qrcode_config_t) {                                                    \
        .display_func = NULL, .max_qrcode_version = 10,                        \
        .qrcode_ecc_level = ESP_QRCODE_ECC_LOW, .user_data = NULL              \
    }

esp_err_t esp_qrcode_generate(esp_qrcode_config_t *config, const char *text);
int esp_qrcode_get_size(esp_qrcode_handle_t qrcode);
bool esp_qrcode_get_module(esp_qrcode_handle_t qrcode, int x, int y);
