#include "ryz_monitor_platform.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static SemaphoreHandle_t s_mutex, s_event;
esp_err_t ryz_monitor_platform_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    s_event = xSemaphoreCreateBinary();
    if (!s_event) { vSemaphoreDelete(s_mutex); s_mutex = NULL; return ESP_ERR_NO_MEM; }
    return ESP_OK;
}
void ryz_monitor_platform_deinit(void)
{
    if (s_event) vSemaphoreDelete(s_event);
    if (s_mutex) vSemaphoreDelete(s_mutex);
    s_event = s_mutex = NULL;
}
esp_err_t ryz_monitor_platform_start(void (*entry)(void *))
{
    return xTaskCreatePinnedToCore(entry, "ryz-monitor", 4096, NULL, 2, NULL, 1) == pdPASS ?
        ESP_OK : ESP_ERR_NO_MEM;
}
void ryz_monitor_platform_lock(void) { xSemaphoreTake(s_mutex, portMAX_DELAY); }
void ryz_monitor_platform_unlock(void) { xSemaphoreGive(s_mutex); }
void ryz_monitor_platform_wake(void) { xSemaphoreGive(s_event); }
void ryz_monitor_platform_wait(void) { xSemaphoreTake(s_event, portMAX_DELAY); }
void ryz_monitor_platform_delay_ms(uint32_t ms)
{
    TickType_t ticks = (TickType_t)(((uint64_t)ms * configTICK_RATE_HZ + 999) / 1000);
    vTaskDelay(ticks ? ticks : 1);
}
int64_t ryz_monitor_platform_now_ms(void) { return esp_timer_get_time() / 1000; }
