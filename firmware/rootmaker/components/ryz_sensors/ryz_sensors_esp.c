#include "ryz_sensors_platform.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static SemaphoreHandle_t s_mutex;

esp_err_t ryz_sensors_platform_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    return s_mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

void ryz_sensors_platform_deinit(void)
{
    if (s_mutex) vSemaphoreDelete(s_mutex);
    s_mutex = NULL;
}

esp_err_t ryz_sensors_platform_start(void (*entry)(void *))
{
    /* The IMU driver has one owner, below the UI task priority. I2C0 is shared
     * by the board Adapter; this task must never recreate or delete that bus. */
    return xTaskCreate(entry, "ryz-sensors", 4096, NULL, 3, NULL) == pdPASS ?
        ESP_OK : ESP_ERR_NO_MEM;
}

void ryz_sensors_platform_lock(void) { xSemaphoreTake(s_mutex, portMAX_DELAY); }
void ryz_sensors_platform_unlock(void) { xSemaphoreGive(s_mutex); }
int64_t ryz_sensors_platform_now_ms(void) { return esp_timer_get_time() / 1000; }
void ryz_sensors_platform_wait_ms(uint32_t milliseconds)
{
    TickType_t ticks = pdMS_TO_TICKS(milliseconds);
    vTaskDelay(ticks ? ticks : 1);
}
