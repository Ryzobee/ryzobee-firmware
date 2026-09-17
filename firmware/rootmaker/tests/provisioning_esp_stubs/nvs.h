#pragma once
#include "sdk.h"

#define ESP_ERR_NVS_TYPE_MISMATCH 0x1103
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out);
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value);
