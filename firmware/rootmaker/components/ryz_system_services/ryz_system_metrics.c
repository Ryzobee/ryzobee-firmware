#include "ryz_system_metrics.h"
#include "ryz_system_metrics_platform.h"
#include "ryz_system_services_platform.h"

#include <math.h>
#include <string.h>

#define METRICS_INTERVAL_US INT64_C(1000000)

static void temperature(ryz_system_metrics_snapshot_t *out)
{
    float value=NAN;
    out->temperature_error=ryz_system_metrics_platform_temperature(&value);
    if(out->temperature_error!=ESP_OK) return;
    /* The ESP32-S3 hardware range, not an invented healthy/unsafe threshold.
     * The SDK can write out before returning FAIL; only success is eligible. */
    if(!isfinite(value) || value < -40.0f || value > 125.0f) {
        out->temperature_error=ESP_ERR_INVALID_STATE;
        return;
    }
    float deci=value*10.0f;
    out->temperature_deci_c=(int16_t)(deci+(deci>=0 ? 0.5f : -0.5f));
    out->temperature_valid=true;
}

static void load(ryz_system_metrics_t *state,ryz_system_metrics_snapshot_t *out)
{
    uint64_t idle[2]={0},now[2]={0};
    out->load_error=ryz_system_metrics_platform_idle(idle,now);
    if(out->load_error!=ESP_OK) { state->baseline_valid=false; return; }
    /* Idle is boot-lifetime time, never more than the same monotonic clock.
     * Reject torn/reset/foreign-clock observations rather than clamp to 0%. */
    if(idle[0]>now[0] || idle[1]>now[1]) {
        state->baseline_valid=false;
        out->load_error=ESP_ERR_INVALID_STATE;
        return;
    }
    out->load_error=ESP_ERR_NOT_FINISHED;
    if(state->baseline_valid) {
        uint64_t busy_ppm[2]={0};
        bool valid=true;
        for(unsigned core=0;core<2;++core) {
            uint64_t elapsed=now[core]-state->baseline_at_us[core];
            if(now[core]<=state->baseline_at_us[core] || idle[core]<state->idle_us[core] ||
               elapsed>UINT64_MAX/2000000 || idle[core]-state->idle_us[core]>elapsed) {
                valid=false;
                break;
            }
            uint64_t busy=elapsed-(idle[core]-state->idle_us[core]);
            busy_ppm[core]=(busy*1000000+elapsed/2)/elapsed;
        }
        if(valid) {
            /* Each core has its own interval: serial IPC dispatch latency is
             * not another core's idle time. Average at ppm precision before
             * rounding to permille; one busy + one idle core yields 500. */
            out->load_permille=(uint16_t)((busy_ppm[0]+busy_ppm[1]+1000)/2000);
            out->load_valid=true;
            out->load_error=ESP_OK;
        } else {
            out->load_error=ESP_ERR_INVALID_STATE;
        }
    }
    state->baseline_valid=true;
    memcpy(state->baseline_at_us,now,sizeof(now));
    memcpy(state->idle_us,idle,sizeof(idle));
}

void ryz_system_metrics_poll(ryz_system_metrics_t *state,ryz_system_metrics_snapshot_t *out)
{
    if(!state || !out) return;
    int64_t now=ryz_system_services_platform_now_us();
    if(state->attempted && now>=0 && state->attempted_at_us>=0 &&
       now>=state->attempted_at_us &&
       (uint64_t)now-(uint64_t)state->attempted_at_us<METRICS_INTERVAL_US) {
        *out=state->snapshot;
        return;
    }
    bool reversed=state->attempted && now<state->attempted_at_us;
    state->attempted=true;
    state->attempted_at_us=now;
    ryz_system_metrics_snapshot_t next={0};
    if(now<0) {
        next.temperature_error=next.load_error=ESP_ERR_INVALID_STATE;
        state->baseline_valid=false;
    } else {
        next.sampled_at_us=(uint64_t)now;
        temperature(&next);
        load(state,&next);
        if(reversed) {
            next.load_valid=false;
            next.load_permille=0;
            next.load_error=ESP_ERR_INVALID_STATE;
        }
    }
    state->snapshot=next;
    *out=next;
}
