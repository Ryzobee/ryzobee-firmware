#include "ryz_system_metrics_platform.h"

#include <math.h>
#include <stddef.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_ipc.h"
#include "esp_timer.h"
#include "driver/temperature_sensor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if !CONFIG_IDF_TARGET_ESP32S3 || CONFIG_FREERTOS_SMP || CONFIG_FREERTOS_NUMBER_OF_CORES != 2
#error "System metrics require the audited ESP32-S3 dual-core IDF FreeRTOS port"
#endif
#if !CONFIG_ESP_IPC_ENABLE
#error "System LOAD requires the existing ESP IPC tasks"
#endif
#if !CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS || !CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER || !CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U64
#error "System LOAD requires ESP_TIMER runtime statistics with a 64-bit counter"
#endif
_Static_assert(sizeof(configRUN_TIME_COUNTER_TYPE)==sizeof(uint64_t),"Runtime counter must not wrap after 71 minutes");

/* Boot-lifetime handle, owned only by the existing system task. The driver
 * has no thread-safety protection and is never called by UI/get_snapshot.
 * Failed install/enable retry only on the next bounded sample, not a loop. */
static temperature_sensor_handle_t s_sensor;
static bool s_enabled;

esp_err_t ryz_system_metrics_platform_temperature(float *out_celsius)
{
    if(!out_celsius) return ESP_ERR_INVALID_ARG;
    *out_celsius=0;
    if(!s_sensor) {
        const temperature_sensor_config_t config=TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10,80);
        temperature_sensor_handle_t candidate=NULL;
        esp_err_t error=temperature_sensor_install(&config,&candidate);
        if(error!=ESP_OK) return error;
        if(!candidate) return ESP_ERR_NO_MEM;
        s_sensor=candidate;
    }
    if(!s_enabled) {
        esp_err_t error=temperature_sensor_enable(s_sensor);
        if(error!=ESP_OK) return error;
        s_enabled=true;
    }
    float value=NAN;
    esp_err_t error=temperature_sensor_get_celsius(s_sensor,&value);
    if(error!=ESP_OK) return error;
    if(!isfinite(value) || value < -40.0f || value > 125.0f) return ESP_ERR_INVALID_STATE;
    *out_celsius=value;
    return ESP_OK;
}

typedef struct {
    BaseType_t core;
    uint64_t idle_us;
    uint64_t sampled_at_us;
    esp_err_t error;
} idle_sample_t;

static void sample_idle_on_core(void *context)
{
    idle_sample_t *sample=context;
    if(xPortGetCoreID()!=sample->core) return;
    /* Executing in this core's existing IPC task forces its previous idle
     * slice to be accounted before the public getter. The dual-core IDF
     * kernel lock protects its U64 copy. No nested IPC or module locks. */
    uint64_t idle=ulTaskGetIdleRunTimeCounterForCore(sample->core);
    int64_t now=esp_timer_get_time();
    if(now<0 || idle>(uint64_t)now) return;
    sample->idle_us=idle;
    sample->sampled_at_us=(uint64_t)now;
    sample->error=ESP_OK;
}

esp_err_t ryz_system_metrics_platform_idle(uint64_t idle_us[2],uint64_t sampled_at_us[2])
{
    if(idle_us) memset(idle_us,0,2*sizeof(*idle_us));
    if(sampled_at_us) memset(sampled_at_us,0,2*sizeof(*sampled_at_us));
    if(!idle_us || !sampled_at_us) return ESP_ERR_INVALID_ARG;
    idle_sample_t samples[2]={{.core=0,.error=ESP_ERR_INVALID_STATE},
                              {.core=1,.error=ESP_ERR_INVALID_STATE}};
    for(unsigned core=0;core<2;++core) {
        /* Blocking ACK occurs after callback return, so the stack context
         * remains alive. SDK contention may delay the whole sampling attempt;
         * caller holds no snapshot lock and publishes its original age. */
        esp_err_t error=esp_ipc_call_blocking(core,sample_idle_on_core,&samples[core]);
        if(error!=ESP_OK) return error;
        if(samples[core].error!=ESP_OK) return samples[core].error;
    }
    for(unsigned core=0;core<2;++core) {
        idle_us[core]=samples[core].idle_us;
        sampled_at_us[core]=samples[core].sampled_at_us;
    }
    return ESP_OK;
}
