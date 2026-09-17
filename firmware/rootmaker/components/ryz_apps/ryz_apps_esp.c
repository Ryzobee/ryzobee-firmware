#include "ryz_apps_platform.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_reader;

esp_err_t ryz_apps_platform_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    return s_lock == NULL ? ESP_ERR_NO_MEM : ESP_OK;
}

void ryz_apps_platform_deinit(void)
{
    if (s_lock != NULL) vSemaphoreDelete(s_lock);
    s_lock = NULL;
}

esp_err_t ryz_apps_platform_start(void (*entry)(void *))
{
    /* Directory scans and FreeType-independent metadata parsing must not run
     * at UI priority. Stack includes bounded page/detail value snapshots. */
    return xTaskCreate(entry, "ryz_apps", 8192, NULL, 3, &s_reader) == pdPASS ?
        ESP_OK : ESP_ERR_NO_MEM;
}

void ryz_apps_platform_lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
void ryz_apps_platform_unlock(void) { xSemaphoreGive(s_lock); }
void ryz_apps_platform_wake(void) { xTaskNotifyGive(s_reader); }
void ryz_apps_platform_wait(void)
{
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
}
