#include "ryz_time_platform.h"

#include <stdlib.h>
#include <time.h>

#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

static SemaphoreHandle_t s_snapshot_mutex;
static ryz_time_platform_sync_sink_t s_sink;
static bool s_sntp_initialized;

int64_t ryz_time_platform_now_us(void)
{
    return esp_timer_get_time();
}

static void receive_sntp_time(struct timeval *time_value)
{
    if (s_sink != NULL) {
        const int64_t observed = ryz_time_platform_now_us();
        if (time_value == NULL || time_value->tv_usec < 0 || time_value->tv_usec >= 1000000) {
            s_sink(0, 0, observed);
        } else {
            s_sink((int64_t)time_value->tv_sec, (uint32_t)time_value->tv_usec, observed);
        }
    }
}

void ryz_time_platform_snapshot_lock(void)
{
    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
}

bool ryz_time_platform_snapshot_try_lock(void)
{
    return s_snapshot_mutex != NULL && xSemaphoreTake(s_snapshot_mutex, 0) == pdTRUE;
}

void ryz_time_platform_snapshot_unlock(void)
{
    xSemaphoreGive(s_snapshot_mutex);
}

esp_err_t ryz_time_platform_init(ryz_time_platform_sync_sink_t sink)
{
    if (sink == NULL) return ESP_ERR_INVALID_ARG;
    if (s_snapshot_mutex != NULL) return ESP_ERR_INVALID_STATE;
    s_snapshot_mutex = xSemaphoreCreateMutex();
    if (s_snapshot_mutex == NULL) return ESP_ERR_NO_MEM;
    if (setenv("TZ", CONFIG_RYZ_TIME_TIMEZONE, 1) != 0) {
        vSemaphoreDelete(s_snapshot_mutex);
        s_snapshot_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    tzset();
    s_sink = sink;
    return ESP_OK;
}

esp_err_t ryz_time_platform_start(void)
{
    if (!s_sntp_initialized) {
        esp_sntp_config_t config =
            ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_RYZ_NTP_SERVER);
        config.wait_for_sync = false;
        /* UTC anchors describe an applied clock, not a pending adjtime slew. */
        config.smooth_sync = false;
        config.sync_cb = receive_sntp_time;
        esp_err_t error = esp_netif_sntp_init(&config);
        if (error == ESP_OK) s_sntp_initialized = true;
        return error;
    }
    return esp_netif_sntp_start();
}
