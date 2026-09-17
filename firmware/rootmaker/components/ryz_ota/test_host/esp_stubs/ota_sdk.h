#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include "esp_err.h"

typedef int BaseType_t;
typedef unsigned TickType_t;
typedef void (*TaskFunction_t)(void *);
typedef void *TaskHandle_t;
typedef pthread_mutex_t *SemaphoreHandle_t;
#define pdPASS 1
#define pdFALSE 0
#define portMAX_DELAY UINT32_MAX
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex);
void vSemaphoreDelete(SemaphoreHandle_t mutex);
BaseType_t xTaskCreate(TaskFunction_t entry, const char *name, uint32_t stack,
                      void *argument, unsigned priority, TaskHandle_t *handle);
void vTaskDelete(TaskHandle_t handle);

typedef struct { char project_name[32]; char version[32]; } esp_app_desc_t;
typedef enum { ESP_PARTITION_TYPE_APP = 0, ESP_PARTITION_TYPE_DATA = 1 } esp_partition_type_t;
typedef enum {
    ESP_PARTITION_SUBTYPE_APP_FACTORY = 0, ESP_PARTITION_SUBTYPE_DATA_OTA = 0,
    ESP_PARTITION_SUBTYPE_APP_OTA_0 = 0x10, ESP_PARTITION_SUBTYPE_APP_OTA_1 = 0x11,
} esp_partition_subtype_t;
typedef struct {
    void *flash_chip;
    esp_partition_type_t type;
    esp_partition_subtype_t subtype;
    uint32_t address, size, erase_size;
    char label[17];
    bool encrypted, readonly;
} esp_partition_t;
typedef enum {
    ESP_OTA_IMG_NEW = 0, ESP_OTA_IMG_PENDING_VERIFY = 1, ESP_OTA_IMG_VALID = 2,
    ESP_OTA_IMG_INVALID = 3, ESP_OTA_IMG_ABORTED = 4, ESP_OTA_IMG_UNDEFINED = 0xffffffffU,
} esp_ota_img_states_t;
const esp_partition_t *esp_partition_find_first(
    esp_partition_type_t type, esp_partition_subtype_t subtype, const char *label);
typedef void *esp_https_ota_handle_t;
typedef struct {
    const char *url;
    int timeout_ms;
    bool disable_auto_redirect, keep_alive_enable;
    void (*crt_bundle_attach)(void);
} esp_http_client_config_t;
typedef struct { const esp_http_client_config_t *http_config; } esp_https_ota_config_t;
#define ESP_ERR_HTTPS_OTA_IN_PROGRESS 0x9001
#define ESP_ERR_NOT_FOUND 0x105
esp_err_t esp_https_ota_begin(const esp_https_ota_config_t *config, esp_https_ota_handle_t *out);
esp_err_t esp_https_ota_get_img_desc(esp_https_ota_handle_t handle, esp_app_desc_t *out);
int esp_https_ota_get_image_size(esp_https_ota_handle_t handle);
int esp_https_ota_get_image_len_read(esp_https_ota_handle_t handle);
esp_err_t esp_https_ota_perform(esp_https_ota_handle_t handle);
bool esp_https_ota_is_complete_data_received(esp_https_ota_handle_t handle);
esp_err_t esp_https_ota_abort(esp_https_ota_handle_t handle);
esp_err_t esp_https_ota_finish(esp_https_ota_handle_t handle);
const esp_app_desc_t *esp_app_get_description(void);
const esp_partition_t *esp_ota_get_running_partition(void);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *partition, esp_ota_img_states_t *out);
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void);
void esp_restart(void);
void esp_crt_bundle_attach(void);
