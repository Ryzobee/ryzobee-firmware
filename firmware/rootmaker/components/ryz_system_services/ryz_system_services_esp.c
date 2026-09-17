#include "ryz_system_services_platform.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static SemaphoreHandle_t s_mutex;

esp_err_t ryz_system_services_platform_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    return s_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

void ryz_system_services_platform_deinit(void)
{
    vSemaphoreDelete(s_mutex);
    s_mutex = NULL;
}

esp_err_t ryz_system_services_platform_start(void (*entry)(void *))
{
    /* Above the Lua/UI task (4), sleeping between cycles. Dependency calls
     * can take longer than the 200ms sampling interval. This task
     * never acquires the display, touch or LVGL owner, and has no core affinity
     * requirement. The existing SPI owner must retain its initialized core. */
    return xTaskCreate(entry, "ryz-system", 6144, NULL, 5, NULL) == pdPASS
        ? ESP_OK : ESP_ERR_NO_MEM;
}

void ryz_system_services_platform_lock(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

void ryz_system_services_platform_unlock(void)
{
    xSemaphoreGive(s_mutex);
}

int64_t ryz_system_services_platform_now_us(void)
{
    return esp_timer_get_time();
}

void ryz_system_services_platform_wait_ms(uint32_t milliseconds)
{
    TickType_t ticks = pdMS_TO_TICKS(milliseconds);
    vTaskDelay(ticks != 0 ? ticks : 1);
}

void ryz_system_services_platform_report(
    ryz_system_services_stage_t stage, esp_err_t error, uint32_t suppressed)
{
    static const char *const names[] = {
        "provisioning init", "provisioning start", "network snapshot",
        "time init", "time network", "time snapshot", "OTA init",
        "OTA prerequisites", "reprovision", "network control", "boot network",
    };
    _Static_assert(sizeof(names) / sizeof(names[0]) == RYZ_SYSTEM_STAGE_COUNT,
                   "Every system stage needs a non-secret diagnostic label");
    const char *name = (unsigned int)stage < RYZ_SYSTEM_STAGE_COUNT
        ? names[stage] : "unknown";
    ESP_LOGW("ryz-system", "%s: %s (0x%x); %lu repeats suppressed",
             name, esp_err_to_name(error), (unsigned int)error,
             (unsigned long)suppressed);
}
