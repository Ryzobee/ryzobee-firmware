#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
typedef unsigned nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c
esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *out);
esp_err_t nvs_get_blob(nvs_handle_t handle,const char *key,void *out,size_t *size);
esp_err_t nvs_set_blob(nvs_handle_t handle,const char *key,const void *data,size_t size);
esp_err_t nvs_commit(nvs_handle_t handle);
void nvs_close(nvs_handle_t handle);
