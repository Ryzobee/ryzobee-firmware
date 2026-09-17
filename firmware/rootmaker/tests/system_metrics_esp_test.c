#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ryz_system_metrics_platform.h"
#include "driver/temperature_sensor.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_ipc.h"

struct temperature_sensor_obj_t { int marker; };
static struct temperature_sensor_obj_t sensor={0x53454e53};
static unsigned installs,enables,reads,idles;
static esp_err_t install_error,enable_error,read_error;
static bool null_install;
static float raw=37.125f;
static uint64_t idle[2];
static int64_t now=INT64_C(6000000000);
static bool in_ipc;
static BaseType_t ipc_core;
static unsigned ipc_calls;
static int ipc_fail_core=-1;
static esp_err_t ipc_error=ESP_ERR_INVALID_STATE;
static int wrong_core=-1;
static int omitted_core=-1;
static int64_t ipc_time_offset[2];

esp_err_t esp_ipc_call_blocking(uint32_t core,esp_ipc_func_t callback,void *context)
{
    assert(core<2 && callback && context && !in_ipc);
    ++ipc_calls;
    if((int)core==ipc_fail_core) return ipc_error;
    in_ipc=true; ipc_core=(BaseType_t)core;
    if((int)core!=omitted_core) callback(context);
    in_ipc=false;
    return ESP_OK;
}

BaseType_t xPortGetCoreID(void)
{
    assert(in_ipc);
    return ipc_core==wrong_core ? 1-ipc_core : ipc_core;
}

esp_err_t temperature_sensor_install(const temperature_sensor_config_t *config,
                                      temperature_sensor_handle_t *out)
{
    ++installs;
    assert(config && out && !*out);
    assert(config->range_min==-10 && config->range_max==80);
    assert(config->clk_src==TEMPERATURE_SENSOR_CLK_SRC_DEFAULT && !config->flags.allow_pd);
    if(install_error!=ESP_OK) return install_error;
    *out=null_install ? NULL : &sensor;
    return ESP_OK;
}

esp_err_t temperature_sensor_enable(temperature_sensor_handle_t handle)
{
    assert(handle==&sensor); ++enables;
    return enable_error;
}

esp_err_t temperature_sensor_get_celsius(temperature_sensor_handle_t handle,float *out)
{
    assert(handle==&sensor && out); ++reads;
    *out=raw; /* Actual SDK can write the output before reporting failure. */
    return read_error;
}

configRUN_TIME_COUNTER_TYPE ulTaskGetIdleRunTimeCounterForCore(BaseType_t core)
{
    assert(core==0 || core==1);
    /* RED on the old remote direct-read Adapter: sampling must first execute
     * in the target IPC task so the scheduler has closed its idle slice.
     * Host asserts the call seam, not the real SDK scheduling implementation. */
    assert(in_ipc && core==ipc_core); ++idles;
    return idle[core];
}

int64_t esp_timer_get_time(void)
{
    assert(in_ipc); /* The counter and endpoint must both be read locally. */
    return now+ipc_time_offset[ipc_core];
}

static void temperature_case(void)
{
    float out=123;
    assert(ryz_system_metrics_platform_temperature(NULL)==ESP_ERR_INVALID_ARG && !installs);
    assert(ryz_system_metrics_platform_temperature(&out)==ESP_OK && out==raw);
    assert(installs==1 && enables==1 && reads==1);
    raw=41;
    assert(ryz_system_metrics_platform_temperature(&out)==ESP_OK && out==41);
    assert(installs==1 && enables==1 && reads==2);
    read_error=ESP_FAIL; raw=100;
    assert(ryz_system_metrics_platform_temperature(&out)==ESP_FAIL && out==0);
    read_error=ESP_OK;
    const float bad[]={NAN,INFINITY,-INFINITY,-40.01f,125.01f};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        raw=bad[i]; out=123;
        assert(ryz_system_metrics_platform_temperature(&out)==ESP_ERR_INVALID_STATE && out==0);
    }
    raw=-40; assert(ryz_system_metrics_platform_temperature(&out)==ESP_OK && out==-40);
    raw=125; assert(ryz_system_metrics_platform_temperature(&out)==ESP_OK && out==125);
    assert(installs==1 && enables==1);
}

static void retry_case(void)
{
    float out=123;
    install_error=ESP_ERR_NO_MEM;
    assert(ryz_system_metrics_platform_temperature(&out)==ESP_ERR_NO_MEM && out==0);
    assert(installs==1 && !enables && !reads);
    install_error=ESP_OK; null_install=true;
    assert(ryz_system_metrics_platform_temperature(&out)==ESP_ERR_NO_MEM && out==0);
    null_install=false; enable_error=ESP_FAIL;
    assert(ryz_system_metrics_platform_temperature(&out)==ESP_FAIL && out==0);
    assert(installs==3 && enables==1 && !reads);
    enable_error=ESP_OK;
    assert(ryz_system_metrics_platform_temperature(&out)==ESP_OK && out==raw);
    assert(installs==3 && enables==2 && reads==1);
}

static void idle_case(void)
{
    uint64_t out[2]={123,456},time[2]={789,999};
    assert(ryz_system_metrics_platform_idle(NULL,time)==ESP_ERR_INVALID_ARG && !time[0] && !time[1]);
    assert(ryz_system_metrics_platform_idle(out,NULL)==ESP_ERR_INVALID_ARG && !idles);
    assert(!out[0] && !out[1] && !ipc_calls);
    idle[0]=UINT64_C(5000000000); idle[1]=UINT64_C(4500000000);
    ipc_time_offset[1]=200000;
    assert(ryz_system_metrics_platform_idle(out,time)==ESP_OK);
    assert(out[0]==idle[0] && out[1]==idle[1] && time[0]==(uint64_t)now && idles==2);
    assert(time[1]==(uint64_t)now+200000 && ipc_calls==2);
    ipc_time_offset[1]=0;
    now=-1;
    assert(ryz_system_metrics_platform_idle(out,time)==ESP_ERR_INVALID_STATE);
    assert(!out[0] && !out[1] && !time[0] && !time[1]);
    now=100;
    assert(ryz_system_metrics_platform_idle(out,time)==ESP_ERR_INVALID_STATE);
    assert(!out[0] && !out[1] && !time[0] && !time[1]);
    idle[0]=20; idle[1]=50;
    assert(ryz_system_metrics_platform_idle(out,time)==ESP_OK && out[0]==20 && out[1]==50);
    assert(time[0]==100 && time[1]==100);
}

static void ipc_failure_case(void)
{
    uint64_t out[2],time[2];
    idle[0]=100; idle[1]=200;
    for(int core=0;core<2;++core) {
        ipc_fail_core=core; ipc_error=ESP_FAIL;
        unsigned previous_calls=ipc_calls,previous_reads=idles;
        memset(out,1,sizeof(out)); memset(time,1,sizeof(time));
        assert(ryz_system_metrics_platform_idle(out,time)==ESP_FAIL);
        assert(!out[0] && !out[1] && !time[0] && !time[1]);
        assert(ipc_calls==previous_calls+(unsigned)core+1 && idles==previous_reads+(unsigned)core);
    }
    ipc_fail_core=-1;
    for(int core=0;core<2;++core) {
        wrong_core=core;
        assert(ryz_system_metrics_platform_idle(out,time)==ESP_ERR_INVALID_STATE);
        assert(!out[0] && !out[1] && !time[0] && !time[1]);
        wrong_core=-1; omitted_core=core;
        assert(ryz_system_metrics_platform_idle(out,time)==ESP_ERR_INVALID_STATE);
        assert(!out[0] && !out[1] && !time[0] && !time[1]);
        omitted_core=-1;
    }
    /* First-core success must never leak when the second observation is bad. */
    idle[1]=(uint64_t)now+1;
    assert(ryz_system_metrics_platform_idle(out,time)==ESP_ERR_INVALID_STATE);
    assert(!out[0] && !out[1] && !time[0] && !time[1]);
    idle[1]=200;
    assert(ryz_system_metrics_platform_idle(out,time)==ESP_OK);
    assert(out[0]==100 && out[1]==200 && time[0]==(uint64_t)now && time[1]==(uint64_t)now);
}

int main(int argc,char **argv)
{
    assert(argc==2);
    if(!strcmp(argv[1],"temperature")) temperature_case();
    else if(!strcmp(argv[1],"retry")) retry_case();
    else if(!strcmp(argv[1],"idle")) idle_case();
    else if(!strcmp(argv[1],"ipc_failure")) ipc_failure_case();
    else assert(false);
    printf("SYSTEM_METRICS_ESP_PASS %s\n",argv[1]);
    return 0;
}
