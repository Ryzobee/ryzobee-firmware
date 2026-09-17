#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERROR_CHECK(expression) do { if ((expression) != ESP_OK) abort(); } while (0)
const char *esp_err_to_name(esp_err_t error);

typedef struct { unsigned revision; } esp_chip_info_t;
void esp_chip_info(esp_chip_info_t *info);
esp_err_t esp_flash_get_size(void *chip, uint32_t *size);
size_t esp_psram_get_size(void);
uint32_t esp_random(void);
const char *esp_get_idf_version(void);
int64_t esp_timer_get_time(void);

#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_8BIT 4
void *heap_caps_realloc(void *ptr, size_t size, unsigned capabilities);
void heap_caps_free(void *ptr);
size_t heap_caps_get_free_size(unsigned capabilities);

typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
void vTaskDelay(TickType_t ticks);
void runtime_owner_test_yield(void);
#define taskYIELD() runtime_owner_test_yield()
