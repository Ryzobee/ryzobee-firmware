/* Real production service-to-view mapping and LVGL pixels. Service readings,
 * timestamps and FPS samples below are explicit fixtures, never a board
 * temperature, radio measurement or actual panel-rate claim. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "workbench_telemetry.h"
#ifndef RYZ_TELEMETRY_MAP_ONLY
#include "ryz_v5_render.h"
#include "ryz_v5_widgets.h"
#include "ryz_v5_status.h"
#include "ryz_lvgl_system.h"
#include "ryz_display_host.h"
#include "ryz_pixels_host.h"
#endif

#ifdef NDEBUG
#error "Telemetry checks require assertions"
#endif

static unsigned groups;
static void passed(const char *name) { ++groups; printf("PASS %s\n", name); }

static ryz_system_services_snapshot_t fixture(void)
{
    ryz_system_services_snapshot_t s={0};
    s.network_valid=true;
    s.network.enabled=true;
    s.network.state=RYZ_PROVISIONING_ONLINE;
    s.network.link=(ryz_provisioning_link_info_t){
        .mac_valid=true,.rssi_valid=true,.ipv6_valid=true,.rssi_dbm=-52,
        .sampled_at_us=1000000,.mac="02:11:22:33:44:55",
        .ipv6="2001:0db8:1234:5678:abcd:1234:5678:9abc"};
    s.metrics=(ryz_system_metrics_snapshot_t){
        .temperature_valid=true,.temperature_deci_c=484,
        .load_valid=true,.load_permille=234,.sampled_at_us=1000000};
    return s;
}

static void fixed_dashboard(void)
{
    ryz_system_services_snapshot_t s=fixture();
    ryz_workbench_telemetry_t w={0};
    ryz_system_ui_snapshot_t ui={.connected=true};
    ryz_v5_network_model_t n={0};
    ryz_workbench_telemetry_update(&w,&s,1000000,NULL,&ui,&n);
    ryz_workbench_telemetry_psram(&w,1000000,1000,580,&ui);
    assert(ui.cpu_temperature_valid && ui.cpu_temperature_c==48);
    assert(ui.cpu_load_valid && ui.cpu_load_percent==23);
    assert(ui.psram_usage_valid && ui.psram_used_percent==42);
    assert(ui.home_selection.metric==RYZ_HOME_METRIC_NET && !ui.home_history_count);
    ryz_workbench_telemetry_update(&w,NULL,2000000,NULL,&ui,&n);
    ryz_workbench_telemetry_psram(&w,2000000,1000,500,&ui);
    assert(!ui.cpu_temperature_valid && !ui.cpu_load_valid && !ui.home_value_valid);
    assert(ui.psram_usage_valid && ui.psram_used_percent==50 && !ui.home_history_count);
    passed("fixed NET dashboard publishes all scalars; independent PSRAM survives service loss without becoming a plot");
}

static void availability(void)
{
    ryz_system_services_snapshot_t s=fixture();
    ryz_workbench_telemetry_t w={0};
    ryz_system_ui_snapshot_t ui={.connected=true};
    ryz_v5_network_model_t n={0};
    assert(ryz_workbench_telemetry_select(&w,(ryz_home_selection_t){.metric=RYZ_HOME_METRIC_TEMP},&ui));
    ryz_workbench_telemetry_update(&w,&s,1000000,NULL,&ui,&n);
    assert(ui.rssi_valid && ui.rssi_dbm==-52 && n.mac_valid && n.ipv6_valid);
    assert(!strcmp(n.mac,"02:11:22:33:44:55") && !strcmp(n.ipv6,s.network.link.ipv6));
    assert(ui.cpu_temperature_valid && ui.cpu_temperature_c==48);
    assert(!ui.cpu_load_valid && !ui.psram_usage_valid && !ui.display_fps_valid);
    ryz_workbench_telemetry_update(&w,&s,3500000,NULL,&ui,&n);
    assert(ui.rssi_valid && ui.cpu_temperature_valid); /* Inclusive 2.5s boundary. */
    ryz_workbench_telemetry_update(&w,&s,3500001,NULL,&ui,&n);
    assert(!ui.rssi_valid && !ui.cpu_load_valid && !ui.cpu_temperature_valid);
    assert(!n.mac_valid && !n.ipv6_valid && !n.mac[0] && !n.ipv6[0]);
    ryz_workbench_telemetry_update(&w,&s,999999,NULL,&ui,&n);
    assert(!ui.rssi_valid && !ui.cpu_temperature_valid && !ui.display_fps_valid);
    ryz_workbench_telemetry_update(&w,&s,1000000,NULL,&ui,&n);
    assert(ui.rssi_valid);
    ryz_workbench_telemetry_update(&w,NULL,1000001,NULL,&ui,&n);
    assert(!ui.rssi_valid && !ui.cpu_temperature_valid && !ui.cpu_load_valid);
    assert(ui.rssi_dbm==0 && ui.cpu_temperature_c==0 && ui.cpu_load_percent==0);
    passed("fresh, expired, future and absent service observations never retain old values");
}

static void state_and_quantization(void)
{
    ryz_workbench_telemetry_t w={0};
    ryz_system_ui_snapshot_t ui={.connected=true};
    ryz_v5_network_model_t n={0};
    for(int state=RYZ_PROVISIONING_UNCONFIGURED;state<=RYZ_PROVISIONING_OFF;++state) {
        ryz_system_services_snapshot_t s=fixture();
        s.network.state=state;
        ryz_workbench_telemetry_update(&w,&s,1000000,NULL,&ui,&n);
        assert(ui.rssi_valid==(state==RYZ_PROVISIONING_ONLINE));
        assert(n.mac_valid==ui.rssi_valid && n.ipv6_valid==ui.rssi_valid);
    }
    ryz_system_services_snapshot_t s=fixture();
    s.network_valid=false;
    ryz_workbench_telemetry_update(&w,&s,1000000,NULL,&ui,&n); assert(!ui.rssi_valid);
    s.network_valid=true; s.network.enabled=false;
    ryz_workbench_telemetry_update(&w,&s,1000000,NULL,&ui,&n); assert(!ui.rssi_valid);
    s.network.enabled=true; ui.connected=false;
    ryz_workbench_telemetry_update(&w,&s,1000000,NULL,&ui,&n); assert(!ui.rssi_valid);
    ui.connected=true;
    s.network.link.rssi_dbm=4; /* IDF permits slightly positive extreme RSSI. */
    s.network.link.ipv6_valid=false; s.network.link.last_error=ESP_FAIL;
    s.metrics.temperature_deci_c=-105; s.metrics.load_permille=235;
    assert(ryz_workbench_telemetry_select(&w,(ryz_home_selection_t){.metric=RYZ_HOME_METRIC_TEMP},&ui));
    ryz_workbench_telemetry_update(&w,&s,1000000,NULL,&ui,&n);
    assert(ui.rssi_valid && ui.rssi_dbm==4 && n.mac_valid && !n.ipv6_valid);
    assert(ui.cpu_temperature_c==-11 && !ui.cpu_load_valid);
    assert(ryz_workbench_telemetry_select(&w,(ryz_home_selection_t){.metric=RYZ_HOME_METRIC_CPU},&ui));
    ryz_workbench_telemetry_update(&w,&s,1000000,NULL,&ui,&n);
    assert(ui.cpu_load_valid && ui.cpu_load_percent==24 && !ui.cpu_temperature_valid);
    s.metrics.load_permille=1000;
    ryz_workbench_telemetry_update(&w,&s,1000000,NULL,&ui,&n);
    assert(ui.cpu_load_valid && ui.cpu_load_percent==100);
    s.metrics.load_permille=1001;
    memset(s.network.link.mac,'x',sizeof(s.network.link.mac));
    ryz_workbench_telemetry_update(&w,&s,1000000,NULL,&ui,&n);
    assert(!ui.cpu_load_valid && !n.mac_valid && !n.mac[0]);
    passed("radio/state gates, independent measurements, bounded strings and signed rounding");
}

static void fps_mapping(void)
{
    ryz_workbench_telemetry_t w={0};
    ryz_system_ui_snapshot_t ui={0};
    ryz_v5_network_model_t n={0};
    uint16_t fps=17;
    ryz_workbench_telemetry_update(&w,NULL,0,&fps,&ui,&n);
    assert(ui.display_fps_valid && ui.display_fps==17);
    /* Service timestamps and availability must not recalculate LVGL FPS. */
    ryz_workbench_telemetry_update(&w,NULL,UINT64_MAX,&fps,&ui,&n);
    assert(ui.display_fps_valid && ui.display_fps==17);
    fps=0;
    ryz_workbench_telemetry_update(&w,NULL,1,&fps,&ui,&n);
    assert(ui.display_fps_valid && ui.display_fps==0);
    ryz_workbench_telemetry_update(&w,NULL,2,NULL,&ui,&n);
    assert(!ui.display_fps_valid && ui.display_fps==0);
    passed("official LVGL sample copied unchanged; valid zero and unavailable are distinct");
}

static void traffic_sample(ryz_system_services_snapshot_t *s, uint64_t time_us,
                            uint32_t epoch, uint64_t rx, uint64_t tx)
{
    s->network.link.traffic_valid=true;
    s->network.link.traffic_epoch=epoch;
    s->network.link.sampled_at_us=time_us;
    s->network.link.traffic_rx_bytes=rx;
    s->network.link.traffic_tx_bytes=tx;
}

static void traffic_windows(void)
{
    ryz_workbench_telemetry_t w={0};
    ryz_system_services_snapshot_t s=fixture();
    ryz_system_ui_snapshot_t ui={.connected=true};
    ryz_v5_network_model_t n={0};
    traffic_sample(&s,1000000,3,100,400);
    ryz_workbench_telemetry_update(&w,&s,1000000,NULL,&ui,&n);
    assert(!ui.home_value_valid && !ui.home_value && !ui.home_history_count);
    traffic_sample(&s,1500000,3,1100,1400);
    ryz_workbench_telemetry_update(&w,&s,1500000,NULL,&ui,&n);
    assert(ui.home_value_valid && ui.home_value==32 && ui.home_history_count==1);
    assert(ui.home_history[0]==32); /* 2000 combined bytes / 0.5s. */
    ryz_workbench_telemetry_update(&w,&s,1600000,NULL,&ui,&n);
    assert(ui.home_value_valid && ui.home_value==32 && ui.home_history_count==1);
    traffic_sample(&s,2000000,3,1100,1400);
    ryz_workbench_telemetry_update(&w,&s,2000000,NULL,&ui,&n);
    assert(ui.home_value_valid && !ui.home_value && ui.home_history_count==2);
    assert(ui.home_history[0]==32 && !ui.home_history[1]);
    passed("traffic uses measured combined deltas, counts repeated samples once and records real quiet zero");
}

static void traffic_unknown(const ryz_system_ui_snapshot_t *ui)
{
    assert(!ui->home_value_valid && !ui->home_value && !ui->home_history_count);
    for(unsigned i=0;i<RYZ_SYSTEM_UI_TRAFFIC_SAMPLES;++i) assert(!ui->home_history[i]);
}

static void traffic_ready(ryz_workbench_telemetry_t *w,
                           ryz_system_services_snapshot_t *s,
                           ryz_system_ui_snapshot_t *ui, ryz_v5_network_model_t *n)
{
    *w=(ryz_workbench_telemetry_t){0}; *s=fixture();
    *ui=(ryz_system_ui_snapshot_t){.connected=true}; *n=(ryz_v5_network_model_t){0};
    traffic_sample(s,1000000,3,100,200);
    ryz_workbench_telemetry_update(w,s,1000000,NULL,ui,n);
    traffic_sample(s,2000000,3,1100,1200);
    ryz_workbench_telemetry_update(w,s,2000000,NULL,ui,n);
    assert(ui->home_value_valid && ui->home_value==16 && ui->home_history_count==1);
}

static void traffic_quantization_and_history(void)
{
    ryz_workbench_telemetry_t w={0};
    ryz_system_services_snapshot_t s=fixture();
    ryz_system_ui_snapshot_t ui={.connected=true};
    ryz_v5_network_model_t n={0};
    traffic_sample(&s,0,1,0,0);
    ryz_workbench_telemetry_update(&w,&s,0,NULL,&ui,&n); traffic_unknown(&ui);
    traffic_sample(&s,250000,1,15,0);
    ryz_workbench_telemetry_update(&w,&s,250000,NULL,&ui,&n); traffic_unknown(&ui);
    traffic_sample(&s,499999,1,31,0);
    ryz_workbench_telemetry_update(&w,&s,499999,NULL,&ui,&n); traffic_unknown(&ui);
    traffic_sample(&s,500000,1,31,0);
    ryz_workbench_telemetry_update(&w,&s,500000,NULL,&ui,&n);
    assert(ui.home_value_valid && !ui.home_value && ui.home_history_count==1); /* 0.496 rounds down. */
    traffic_sample(&s,1012000,1,63,0);
    ryz_workbench_telemetry_update(&w,&s,1012000,NULL,&ui,&n);
    assert(ui.home_value==1 && ui.home_history_count==2); /* Exactly 0.5 rounds up. */
    traffic_sample(&s,3512000,1,1000063,0);
    ryz_workbench_telemetry_update(&w,&s,3512000,NULL,&ui,&n);
    assert(ui.home_value==3200 && ui.home_history_count==3); /* Inclusive 2.5s interval. */
    traffic_sample(&s,6012001,1,2000063,0);
    ryz_workbench_telemetry_update(&w,&s,6012001,NULL,&ui,&n); traffic_unknown(&ui);

    uint64_t bytes=0;
    w=(ryz_workbench_telemetry_t){0};
    traffic_sample(&s,0,7,0,0);
    ryz_workbench_telemetry_update(&w,&s,0,NULL,&ui,&n);
    for(unsigned i=1;i<=30;++i) {
        bytes+=125U*i; /* At 0.5s, 125-byte steps add 2 decimal kbit/s. */
        traffic_sample(&s,(uint64_t)i*500000,7,bytes,0);
        ryz_workbench_telemetry_update(&w,&s,(uint64_t)i*500000,NULL,&ui,&n);
    }
    assert(ui.home_history_count==24 && ui.home_value==60);
    for(unsigned i=0;i<24;++i) assert(ui.home_history[i]==14U+2U*i);
    passed("traffic interval bounds, frequent polls, nearest rounding and bounded oldest-to-newest history");
}

static void traffic_availability(void)
{
    ryz_workbench_telemetry_t w;
    ryz_system_services_snapshot_t s;
    ryz_system_ui_snapshot_t ui;
    ryz_v5_network_model_t n;
    for(unsigned gate=0;gate<9;++gate) {
        traffic_ready(&w,&s,&ui,&n);
        uint64_t now=2000000;
        if(gate==1) s.network_valid=false;
        if(gate==2) s.network.enabled=false;
        if(gate==3) s.network.state=RYZ_PROVISIONING_OFF;
        if(gate==4) ui.connected=false;
        if(gate==5) s.network.link.traffic_valid=false;
        if(gate==6) s.network.link.traffic_epoch=0;
        if(gate==7) now=1999999;
        if(gate==8) now=4500001;
        ryz_workbench_telemetry_update(&w,gate==0 ? NULL : &s,now,NULL,&ui,&n);
        traffic_unknown(&ui);
        s=fixture(); ui.connected=true;
        traffic_sample(&s,5000000,3,5100,5200);
        ryz_workbench_telemetry_update(&w,&s,5000000,NULL,&ui,&n); traffic_unknown(&ui);
    }
    for(int state=RYZ_PROVISIONING_UNCONFIGURED;state<=RYZ_PROVISIONING_OFF;++state) {
        if(state==RYZ_PROVISIONING_ONLINE) continue;
        traffic_ready(&w,&s,&ui,&n); s.network.state=state;
        ryz_workbench_telemetry_update(&w,&s,2000000,NULL,&ui,&n); traffic_unknown(&ui);
    }
    traffic_ready(&w,&s,&ui,&n);
    ryz_workbench_telemetry_update(&w,&s,4500000,NULL,&ui,&n);
    assert(ui.home_value_valid && ui.home_value==16 && ui.home_history_count==1);
    ryz_workbench_telemetry_update(&w,&s,4500001,NULL,&ui,&n); traffic_unknown(&ui);
    passed("all traffic admission gates and source freshness revoke history, recovery needs a new baseline");
}

static void traffic_identity_resets(void)
{
    ryz_workbench_telemetry_t w;
    ryz_system_services_snapshot_t s;
    ryz_system_ui_snapshot_t ui;
    ryz_v5_network_model_t n;
    for(unsigned reset=0;reset<6;++reset) {
        traffic_ready(&w,&s,&ui,&n);
        uint64_t now=2100000;
        if(reset==0) s.network.link.traffic_epoch=4;
        if(reset==1) s.network.link.sampled_at_us=1999999;
        if(reset==2) s.network.link.traffic_rx_bytes=1099;
        if(reset==3) s.network.link.traffic_tx_bytes=1199;
        if(reset==4) s.network.link.traffic_rx_bytes=1101; /* Changed duplicate. */
        if(reset==5) {
            ryz_workbench_telemetry_update(&w,&s,3000000,NULL,&ui,&n);
            now=2900000; /* Host clock rolls back while the source remains fresh. */
        }
        ryz_workbench_telemetry_update(&w,&s,now,NULL,&ui,&n); traffic_unknown(&ui);
        traffic_sample(&s,3500000,s.network.link.traffic_epoch,
                       s.network.link.traffic_rx_bytes+1000,s.network.link.traffic_tx_bytes+1000);
        ryz_workbench_telemetry_update(&w,&s,3500000,NULL,&ui,&n);
        assert(ui.home_value_valid && ui.home_history_count==1); /* No prior-identity history. */
    }
    traffic_ready(&w,&s,&ui,&n);
    traffic_sample(&s,2100000,3,1200,1300);
    ryz_workbench_telemetry_update(&w,&s,2100000,NULL,&ui,&n);
    assert(ui.home_value_valid && ui.home_history_count==1);
    traffic_sample(&s,2200000,3,1150,1350); /* Above baseline, below last short sample. */
    ryz_workbench_telemetry_update(&w,&s,2200000,NULL,&ui,&n); traffic_unknown(&ui);
    passed("epoch, sample/owner clock, individual counter and inconsistent-duplicate resets cannot join curves");
}

static void traffic_arithmetic_bounds(void)
{
    ryz_workbench_telemetry_t w;
    ryz_system_services_snapshot_t s;
    ryz_system_ui_snapshot_t ui;
    ryz_v5_network_model_t n;
    for(unsigned failure=0;failure<3;++failure) {
        traffic_ready(&w,&s,&ui,&n);
        uint64_t rx=UINT64_MAX, tx=UINT64_MAX;
        if(failure==1) { rx=1100+UINT64_MAX/8000+1; tx=1200; }
        if(failure==2) { rx=1100+((uint64_t)UINT32_MAX+1)*125; tx=1200; }
        traffic_sample(&s,3000000,3,rx,tx);
        ryz_workbench_telemetry_update(&w,&s,3000000,NULL,&ui,&n); traffic_unknown(&ui);
        traffic_sample(&s,4000000,3,rx,tx);
        ryz_workbench_telemetry_update(&w,&s,4000000,NULL,&ui,&n);
        assert(ui.home_value_valid && !ui.home_value && ui.home_history_count==1);
    }
    w=(ryz_workbench_telemetry_t){0}; s=fixture(); ui.connected=true;
    traffic_sample(&s,1000000,9,UINT64_MAX-200,UINT64_MAX-400);
    ryz_workbench_telemetry_update(&w,&s,1000000,NULL,&ui,&n); traffic_unknown(&ui);
    traffic_sample(&s,1500000,9,UINT64_MAX-75,UINT64_MAX-275);
    ryz_workbench_telemetry_update(&w,&s,1500000,NULL,&ui,&n);
    assert(ui.home_value_valid && ui.home_value==4); /* Sum absolute counters would overflow. */
    w=(ryz_workbench_telemetry_t){0};
    traffic_sample(&s,UINT64_MAX-1000000,10,0,0);
    ryz_workbench_telemetry_update(&w,&s,UINT64_MAX-1000000,NULL,&ui,&n); traffic_unknown(&ui);
    traffic_sample(&s,UINT64_MAX,10,(uint64_t)UINT32_MAX*125,0);
    ryz_workbench_telemetry_update(&w,&s,UINT64_MAX,NULL,&ui,&n);
    assert(ui.home_value_valid && ui.home_value==UINT32_MAX);
    passed("traffic sum, scale and result overflow reject safely while large cumulative counters remain usable");
}

static void selected_metric_lifecycle(void)
{
    ryz_workbench_telemetry_t w;
    ryz_system_services_snapshot_t s;
    ryz_system_ui_snapshot_t ui;
    ryz_v5_network_model_t n;
    traffic_ready(&w,&s,&ui,&n);
    ryz_workbench_telemetry_psram(&w,2000000,1000,580,&ui);
    assert(!ryz_workbench_telemetry_select(&w,(ryz_home_selection_t){0},&ui));
    assert(ui.home_history_count==1 && ui.home_value==16);
    const ryz_home_selection_t temp={.metric=RYZ_HOME_METRIC_TEMP,
        .generation=1,.selected_at_us=2500000};
    assert(ryz_workbench_telemetry_select(&w,temp,&ui));
    traffic_unknown(&ui);
    assert(!w.traffic.baseline_valid && !w.psram_sampled);
    assert(ui.home_selection.metric==RYZ_HOME_METRIC_TEMP && ui.home_selection.generation==1);
    s.metrics.sampled_at_us=2000000; s.metrics.temperature_deci_c=-105;
    ryz_workbench_telemetry_update(&w,&s,2500000,NULL,&ui,&n);
    traffic_unknown(&ui); /* A fresh service cache can still predate this selection. */
    s.metrics.sampled_at_us=2500000;
    ryz_workbench_telemetry_update(&w,&s,2500000,NULL,&ui,&n);
    assert(ui.home_value_valid && ui.home_value==-11 && ui.home_history_count==1);
    assert(ui.home_history[0]==-11 && !w.traffic.baseline_valid);
    assert(!ryz_workbench_telemetry_select(&w,temp,&ui));
    ryz_workbench_telemetry_update(&w,&s,2700000,NULL,&ui,&n);
    assert(ui.home_history_count==1); /* Same source sample is not another point. */
    s.metrics.sampled_at_us=3000000; s.metrics.temperature_deci_c=125;
    ryz_workbench_telemetry_update(&w,&s,3000000,NULL,&ui,&n);
    assert(ui.home_history_count==1 && ui.home_value==-11); /* 1s selected cadence. */
    s.metrics.sampled_at_us=3500000;
    ryz_workbench_telemetry_update(&w,&s,3500000,NULL,&ui,&n);
    assert(ui.home_history_count==2 && ui.home_value==13 && ui.home_history[0]==-11);

    assert(ryz_workbench_telemetry_select(&w,(ryz_home_selection_t){
        .metric=RYZ_HOME_METRIC_CPU,.generation=2,.selected_at_us=3600000},&ui));
    traffic_unknown(&ui);
    ryz_workbench_telemetry_update(&w,&s,3600000,NULL,&ui,&n); traffic_unknown(&ui);
    s.metrics.sampled_at_us=4000000; s.metrics.load_permille=0;
    ryz_workbench_telemetry_update(&w,&s,4000000,NULL,&ui,&n);
    assert(ui.home_value_valid && !ui.home_value && ui.home_history_count==1); /* Real 0%. */
    s.metrics.sampled_at_us=5000000; s.metrics.load_permille=1000;
    ryz_workbench_telemetry_update(&w,&s,5000000,NULL,&ui,&n);
    assert(ui.home_history_count==2 && ui.home_value==100 && !ui.home_history[0]);

    assert(ryz_workbench_telemetry_select(&w,(ryz_home_selection_t){
        .metric=RYZ_HOME_METRIC_TEMP,.generation=3,.selected_at_us=5100000},&ui));
    traffic_unknown(&ui); /* Returning to TEMP does not restore its -11/13 samples. */
    ryz_workbench_telemetry_update(&w,&s,5100000,NULL,&ui,&n); traffic_unknown(&ui);
    s.metrics.sampled_at_us=6000000; s.metrics.temperature_deci_c=-20;
    ryz_workbench_telemetry_update(&w,&s,6000000,NULL,&ui,&n);
    assert(ui.home_history_count==1 && ui.home_value==-2);
    assert(ryz_workbench_telemetry_select(&w,(ryz_home_selection_t){
        .metric=RYZ_HOME_METRIC_TEMP,.generation=4,.selected_at_us=6100000},&ui));
    traffic_unknown(&ui); /* New generation revokes even a same-metric recording. */

    assert(ryz_workbench_telemetry_select(&w,(ryz_home_selection_t){
        .metric=RYZ_HOME_METRIC_NET,.generation=5,.selected_at_us=6200000},&ui));
    traffic_sample(&s,6000000,3,2000,2000);
    ryz_workbench_telemetry_update(&w,&s,6200000,NULL,&ui,&n);
    traffic_unknown(&ui); assert(!w.traffic.baseline_valid);
    traffic_sample(&s,7000000,3,3000,3000);
    ryz_workbench_telemetry_update(&w,&s,7000000,NULL,&ui,&n); traffic_unknown(&ui);
    traffic_sample(&s,8000000,3,4000,4000);
    ryz_workbench_telemetry_update(&w,&s,8000000,NULL,&ui,&n);
    assert(ui.home_history_count==1 && ui.home_value==16);
    s.network.state=RYZ_PROVISIONING_OFF; ui.connected=false;
    ryz_workbench_telemetry_update(&w,&s,8000001,NULL,&ui,&n); traffic_unknown(&ui);
    passed("one selected history: identical selection retained, metric/generation changes clear, old caches rejected, switch-back restarts");
}

static void scalar_invalidations(void)
{
    for(unsigned metric=RYZ_HOME_METRIC_TEMP;metric<=RYZ_HOME_METRIC_CPU;++metric) {
        for(unsigned failure=0;failure<7;++failure) {
            ryz_workbench_telemetry_t w={0};
            ryz_system_services_snapshot_t s=fixture();
            ryz_system_ui_snapshot_t ui={0};
            ryz_v5_network_model_t n={0};
            assert(ryz_workbench_telemetry_select(&w,(ryz_home_selection_t){
                .metric=(ryz_home_metric_t)metric,.generation=1},&ui));
            ryz_workbench_telemetry_update(&w,&s,1000000,NULL,&ui,&n);
            s.metrics.sampled_at_us=2000000;
            ryz_workbench_telemetry_update(&w,&s,2000000,NULL,&ui,&n);
            assert(ui.home_history_count==2);
            uint64_t now=2000000;
            if(failure==0) { s.metrics.temperature_valid=false; s.metrics.load_valid=false; }
            if(failure==1) now=4500001; /* Expired. */
            if(failure==2) now=1999999; /* Future relative to owner clock. */
            if(failure==3) s.metrics.sampled_at_us=1999999; /* Source clock rollback. */
            if(failure==4) {
                ryz_workbench_telemetry_update(&w,&s,2200000,NULL,&ui,&n);
                now=2100000; /* Owner rollback with a still-fresh source. */
            }
            if(failure==5) s.metrics.sampled_at_us=now=5000001; /* Unobserved gap. */
            if(failure==6) { ++s.metrics.temperature_deci_c; s.metrics.load_permille=1000;
                /* Change the rounded temperature too. */ s.metrics.temperature_deci_c=500; }
            ryz_workbench_telemetry_update(&w,&s,now,NULL,&ui,&n);
            if(failure<3) traffic_unknown(&ui);
            else assert(ui.home_history_count==1); /* Fresh scalar can begin a new, unjoined series. */
            s.metrics.temperature_valid=s.metrics.load_valid=true;
            s.metrics.sampled_at_us=6000000;
            ryz_workbench_telemetry_update(&w,&s,6000000,NULL,&ui,&n);
            assert(ui.home_value_valid && ui.home_history_count<=2);
            ryz_workbench_telemetry_update(&w,NULL,6000001,NULL,&ui,&n); traffic_unknown(&ui);
        }
    }
    passed("TEMP/CPU invalid, expired, future, clock-reset and changed-duplicate samples never join earlier curves");
}

static void selected_psram(void)
{
    ryz_workbench_telemetry_t w={0};
    ryz_system_ui_snapshot_t ui={0};
    ryz_v5_network_model_t n={0};
    ryz_workbench_telemetry_psram(&w,1000000,1000,580,&ui);
    assert(!ui.home_history_count && w.psram_sampled && ui.psram_usage_valid);
    assert(ui.psram_used_percent==42); /* Fixed NET dashboard also publishes memory. */
    const ryz_home_selection_t psram={.metric=RYZ_HOME_METRIC_PSRAM,
        .generation=1,.selected_at_us=1500000};
    assert(ryz_workbench_telemetry_select(&w,psram,&ui));
    assert(!w.psram_sampled); traffic_unknown(&ui);
    ryz_workbench_telemetry_update(&w,NULL,1500000,NULL,&ui,&n); traffic_unknown(&ui);
    ryz_workbench_telemetry_psram(&w,1500000,1000,500,&ui);
    assert(ui.home_history_count==1 && ui.home_value==50);
    ryz_workbench_telemetry_psram(&w,2499999,1000,0,&ui);
    assert(ui.home_history_count==1 && ui.home_value==50);
    ryz_workbench_telemetry_psram(&w,2500000,1000,0,&ui);
    assert(ui.home_history_count==2 && ui.home_value==100);
    assert(!ryz_workbench_telemetry_select(&w,psram,&ui));
    assert(ui.home_history_count==2);
    ryz_workbench_telemetry_psram(&w,2500001,0,0,&ui); traffic_unknown(&ui);
    ryz_workbench_telemetry_psram(&w,3500001,1000,1000,&ui);
    assert(ui.home_value_valid && !ui.home_value && ui.home_history_count==1);
    ryz_workbench_telemetry_psram(&w,2000000,1000,700,&ui);
    assert(ui.home_history_count==1 && ui.home_value==30); /* New series on rollback. */
    ryz_workbench_telemetry_psram(&w,1000000,1000,800,&ui);
    traffic_unknown(&ui); /* Time before selection cannot repopulate it. */
    assert(ryz_workbench_telemetry_select(&w,(ryz_home_selection_t){
        .metric=RYZ_HOME_METRIC_NET,.generation=2,.selected_at_us=3000000},&ui));
    assert(ryz_workbench_telemetry_select(&w,(ryz_home_selection_t){
        .metric=RYZ_HOME_METRIC_PSRAM,.generation=3,.selected_at_us=4000000},&ui));
    assert(!w.psram_sampled); traffic_unknown(&ui);
    ryz_workbench_telemetry_psram(&w,4000000,1000,250,&ui);
    assert(ui.home_history_count==1 && ui.home_value==75);
    passed("PSRAM selected-only 1s recording, real zero, cache reset, invalidation and switch-back without restoration");
}

static void service_failure_psram_recovery(void)
{
    ryz_workbench_telemetry_t w={0};
    ryz_system_services_snapshot_t s=fixture();
    ryz_system_ui_snapshot_t ui={.connected=true};
    ryz_v5_network_model_t n={0};
    assert(ryz_workbench_telemetry_select(&w,(ryz_home_selection_t){
        .metric=RYZ_HOME_METRIC_CPU,.generation=1},&ui));
    ryz_workbench_telemetry_update(&w,&s,1000000,NULL,&ui,&n);
    assert(ui.home_value_valid && ui.home_value==23);
    ryz_workbench_telemetry_update(&w,NULL,1100000,NULL,&ui,&n);
    traffic_unknown(&ui);
    assert(!ui.rssi_valid && !ui.cpu_temperature_valid && !ui.cpu_load_valid);

    const ryz_home_selection_t psram={.metric=RYZ_HOME_METRIC_PSRAM,
        .generation=2,.selected_at_us=1200000};
    assert(ryz_workbench_telemetry_select(&w,psram,&ui));
    ryz_workbench_telemetry_update(&w,NULL,1200000,NULL,&ui,&n);
    traffic_unknown(&ui);
    ryz_workbench_telemetry_psram(&w,1200000,1000,580,&ui);
    assert(ui.home_value_valid && ui.home_value==42 && ui.home_history_count==1);
    assert(!ui.rssi_valid && !ui.cpu_temperature_valid && !ui.cpu_load_valid);
    assert(!ryz_workbench_telemetry_select(&w,psram,&ui));
    ryz_workbench_telemetry_update(&w,NULL,2200000,NULL,&ui,&n);
    ryz_workbench_telemetry_psram(&w,2200000,1000,500,&ui);
    assert(ui.home_value==50 && ui.home_history_count==2 && ui.home_history[0]==42);

    /* An independently recovered service must not erase a PSRAM recording. */
    s.metrics.sampled_at_us=3000000;
    ryz_workbench_telemetry_update(&w,&s,3000000,NULL,&ui,&n);
    assert(ui.home_history_count==2 && ui.home_value==50);
    for(unsigned metric=RYZ_HOME_METRIC_NET;metric<=RYZ_HOME_METRIC_CPU;++metric) {
        assert(ryz_workbench_telemetry_select(&w,(ryz_home_selection_t){
            .metric=(ryz_home_metric_t)metric,.generation=3+metric,
            .selected_at_us=3200000},&ui));
        ryz_workbench_telemetry_update(&w,NULL,3200000,NULL,&ui,&n);
        traffic_unknown(&ui);
        assert(!ui.psram_usage_valid && !w.psram_sampled);
    }
    assert(ryz_workbench_telemetry_select(&w,(ryz_home_selection_t){
        .metric=RYZ_HOME_METRIC_PSRAM,.generation=6,.selected_at_us=3300000},&ui));
    ryz_workbench_telemetry_update(&w,NULL,3300000,NULL,&ui,&n);
    traffic_unknown(&ui);
    ryz_workbench_telemetry_psram(&w,3300000,1000,250,&ui);
    assert(ui.home_history_count==1 && ui.home_value==75);
    passed("NULL service revokes dependent metrics while selected heap recording starts, advances and recovers independently");
}

#ifndef RYZ_TELEMETRY_MAP_ONLY
static lv_obj_t *find_label(lv_obj_t *root,const char *text)
{
    for(uint32_t i=0;i<lv_obj_get_child_count(root);++i) {
        lv_obj_t *o=lv_obj_get_child(root,(int32_t)i);
        if(lv_obj_check_type(o,&lv_label_class) && !strcmp(lv_label_get_text(o),text)) return o;
        lv_obj_t *found=find_label(o,text);
        if(found) return found;
    }
    return NULL;
}

static void fits(lv_obj_t *root,const char *text)
{
    lv_obj_t *label=find_label(root,text); assert(label);
    lv_point_t extent;
    lv_text_get_size(&extent,text,lv_obj_get_style_text_font(label,0),0,0,
        LV_COORD_MAX,LV_TEXT_FLAG_NONE);
    if(extent.x>lv_obj_get_width(label) || extent.y>lv_obj_get_height(label))
        fprintf(stderr,"clipped %s: %ldx%ld in %ldx%ld\n",text,(long)extent.x,
            (long)extent.y,(long)lv_obj_get_width(label),(long)lv_obj_get_height(label));
    assert(extent.x<=lv_obj_get_width(label) && extent.y<=lv_obj_get_height(label));
}

static void pixels(const char *directory)
{
    assert(ryz_font_init()==ESP_OK);
    ryz_host_display_reset();
    ryz_system_services_snapshot_t s=fixture();
    ryz_workbench_telemetry_t w={0};
    ryz_system_ui_snapshot_t ui={.connected=true,.state=RYZ_SYSTEM_UI_NETWORK_ONLINE,
        .time_hhmm="18:43",.firmware_version="0.9.0"};
    ryz_v5_network_model_t n={.page=RYZ_V5_NETWORK_INFO,.enabled=true};
    strcpy(n.sta_ssid,"TEST FIXTURE"); strcpy(n.ipv4,"192.0.2.37");
    assert(ryz_workbench_telemetry_select(&w,(ryz_home_selection_t){.metric=RYZ_HOME_METRIC_TEMP},&ui));
    ryz_workbench_telemetry_update(&w,&s,1000000,&(const uint16_t){7},&ui,&n);
    ryz_workbench_telemetry_update(&w,&s,2000000,&(const uint16_t){7},&ui,&n);
    ryz_workbench_telemetry_psram(&w,2000000,1000,580,&ui);
    assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_HOME,&ui,NULL,NULL,NULL)==ESP_OK);
    lv_obj_t *root=lv_screen_active();
    fits(root,"-52 dBm"); fits(root,"48 \xc2\xb0" "C");
    assert(!find_label(root,"23%") && !find_label(root,"42%"));
    ryz_host_display_save(directory,"v5-telemetry-home-240");
    s.network.link.rssi_dbm=-128; s.metrics.temperature_deci_c=-100;
    s.metrics.load_permille=1000;
    ryz_workbench_telemetry_update(&w,&s,2000000,&(const uint16_t){7},&ui,&n);
    assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_HOME,&ui,NULL,NULL,NULL)==ESP_OK);
    root=lv_screen_active(); fits(root,"-128 dBm"); fits(root,"-10 \xc2\xb0" "C");
    assert(!find_label(root,"100%"));
    ryz_host_display_save(directory,"v5-telemetry-extremes-240");
    n.system=ui;
    ryz_v5_status_t views={.network=n};
    views.ble.unavailable=true;
    (void)ryz_v5_status_set(&views);
    assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_WIFI_INFO,&ui,NULL,NULL,NULL)==ESP_OK);
    root=lv_screen_active();
    fits(root,"02:11:22:33:44:55");
    lv_obj_t *ip=find_label(root,"2001:0db8:1234:5678:abcd:1234:5678:9abc");
    assert(ip && lv_label_get_long_mode(ip)==LV_LABEL_LONG_WRAP);
    assert(lv_obj_get_width(ip)==152 && lv_obj_get_height(ip)>=40);
    ryz_host_display_save(directory,"v5-telemetry-network-240");
    ryz_workbench_telemetry_update(&w,&s,3500001,&(const uint16_t){7},&ui,&n);
    assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_HOME,&ui,NULL,NULL,NULL)==ESP_OK);
    root=lv_screen_active(); fits(root,"-- dBm");
    assert(!find_label(root,"-128 dBm") && !find_label(root,"-10 \xc2\xb0" "C") && !find_label(root,"100%"));
    ryz_host_display_save(directory,"v5-telemetry-stale-240");
    assert(ryz_v5_release()==ESP_OK);
    passed("selected same-source observations fit original cells, hide unselected values and expire to unknown");
}

static void storage_pixels(const char *directory)
{
    const struct { size_t used, total; const char *label; unsigned width; } cases[] = {
        {0,10000,"0% USED",0}, {1,10000,"<1% USED",1},
        {555,1048576,"<1% USED",1}, {99,10000,"<1% USED",1},
        {100,10000,"1% USED",2}, {10000,10000,"100% USED",224},
        {10001,10000,"100% USED",224}, {1,0,"-- USED",0},
    };
    for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        ryz_system_ui_snapshot_t ui={.storage_ready=true,
            .storage_used_bytes=cases[i].used,.storage_total_bytes=cases[i].total};
        assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_HOME,&ui,NULL,NULL,NULL)==ESP_OK);
        fits(lv_screen_active(),cases[i].label);
        for (unsigned x=8;x<232;++x) {
            bool ink=cases[i].width && x<8+cases[i].width && x!=64 && x!=120 && x!=176;
            assert((ryz_host_display_pixel(x,183)==0xfb40)==ink);
        }
        if(i==2) ryz_host_display_save(directory,"v5-storage-subpercent-240");
    }
    ryz_system_ui_snapshot_t unavailable={.storage_used_bytes=555,.storage_total_bytes=1048576};
    assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_HOME,&unavailable,NULL,NULL,NULL)==ESP_OK);
    fits(lv_screen_active(),"-- USED");
    assert(ryz_v5_release()==ESP_OK);
    passed("storage distinguishes zero/subpercent/unavailable; original label bounds and 1px minimum bar");
}

static bool orange_pixel(unsigned x, unsigned y)
{
    const uint16_t orange=(uint16_t)(((V5_ORANGE>>19)&31U)<<11 |
                                    ((V5_ORANGE>>10)&63U)<<5 |
                                    ((V5_ORANGE>>3)&31U));
    return ryz_host_display_pixel(x,y)==orange;
}

static unsigned graph_ink(void)
{
    unsigned count=0;
    for(unsigned y=54;y<=89;++y) for(unsigned x=66;x<=227;++x) {
        if(!orange_pixel(x,y)) continue;
        assert(x>=67 && x<=226 && y>=55 && y<=88);
        ++count;
    }
    return count;
}

static unsigned graph_blended_ink(void)
{
    unsigned count=0;
    for(unsigned y=54;y<=89;++y) for(unsigned x=66;x<=227;++x) {
        uint16_t pixel=ryz_host_display_pixel(x,y);
        /* The real LVGL 1px diagonal is antialiased over the gray grid.
         * Normalize RGB565 channel widths before comparing its orange tint. */
        unsigned red=((pixel>>11)&31U)*255U/31U;
        unsigned green=((pixel>>5)&63U)*255U/63U;
        unsigned blue=(pixel&31U)*255U/31U;
        if(!(red>green && green>blue)) continue;
        assert(x>=67 && x<=226 && y>=55 && y<=88);
        ++count;
    }
    return count;
}

static void graph_horizontal(unsigned first_x, unsigned y)
{
    unsigned count=graph_ink();
    assert(count>=226U-first_x && count<=227U-first_x);
    for(unsigned row=54;row<=89;++row) for(unsigned x=66;x<=227;++x) {
        if(orange_pixel(x,row)) assert(row==y && x>=first_x && x<=226);
    }
    assert(orange_pixel(first_x,y) && orange_pixel(225,y));
}

static void traffic_pixels(const char *directory)
{
    assert(ryz_font_init()==ESP_OK);
    ryz_workbench_telemetry_t w={0};
    ryz_system_services_snapshot_t s=fixture();
    ryz_system_ui_snapshot_t ui={.connected=true,.state=RYZ_SYSTEM_UI_NETWORK_ONLINE,
        .time_hhmm="18:43",.firmware_version="0.9.0"};
    ryz_v5_network_model_t n={0};
    uint64_t bytes=0;
    traffic_sample(&s,1000000,12,0,0);
    ryz_workbench_telemetry_update(&w,&s,1000000,&(const uint16_t){7},&ui,&n);
    for(unsigned i=1;i<=24;++i) {
        bytes+=12500U*i;
        uint64_t sample_us=(uint64_t)(i+1U)*1000000;
        traffic_sample(&s,sample_us,12,bytes,0);
        s.metrics.sampled_at_us=sample_us;
        ryz_workbench_telemetry_update(&w,&s,sample_us,&(const uint16_t){7},&ui,&n);
    }
    /* Explicit counter fixtures produce 100..2400 kbit/s through the real
     * mapper. These pictures are not measurements from an ESP32 radio. */
    assert(ui.home_value==2400 && ui.home_history_count==24);
    assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_HOME,&ui,NULL,NULL,NULL)==ESP_OK);
    fits(lv_screen_active(),"2.4"); fits(lv_screen_active(),"Mb/s");
    ryz_host_display_save(directory,"v5-home-traffic-fixture-240");
    assert(graph_blended_ink()>20);

    const struct { uint32_t kbps; const char *label; } formats[]={
        {0,"0.0"},{9900,"9.9"},{9950,"10"},{999000,"999"},{1000000,"HI"},
    };
    for(unsigned i=0;i<sizeof(formats)/sizeof(formats[0]);++i) {
        ui.home_value=formats[i].kbps;
        for(unsigned j=0;j<24;++j) ui.home_history[j]=formats[i].kbps;
        assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_HOME,&ui,NULL,NULL,NULL)==ESP_OK);
        fits(lv_screen_active(),formats[i].label);
    }
    ui.home_value=500;
    for(unsigned i=0;i<24;++i) ui.home_history[i]=500;
    assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_HOME,&ui,NULL,NULL,NULL)==ESP_OK);
    graph_horizontal(67,72); /* Half of the minimum 1000-kbit/s plot scale. */
    ui.home_value=2000;
    for(unsigned i=0;i<24;++i) ui.home_history[i]=2000;
    assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_HOME,&ui,NULL,NULL,NULL)==ESP_OK);
    graph_horizontal(67,55); /* The scale grows to fit 2000 kbit/s. */
    ui.home_value=0; ui.home_history_count=2;
    ui.home_history[0]=ui.home_history[1]=0;
    assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_HOME,&ui,NULL,NULL,NULL)==ESP_OK);
    graph_horizontal(219,88); /* Two samples occupy the two rightmost slots. */
    ui.home_history_count=1;
    assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_HOME,&ui,NULL,NULL,NULL)==ESP_OK);
    fits(lv_screen_active(),"0.0");
    assert(graph_ink()==1 && orange_pixel(226,88));
    ryz_host_display_save(directory,"v5-home-traffic-quiet-fixture-240");
    ui.home_value_valid=false;
    assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_HOME,&ui,NULL,NULL,NULL)==ESP_OK);
    fits(lv_screen_active(),"--"); assert(!graph_blended_ink());
    ryz_host_display_save(directory,"v5-home-traffic-unknown-fixture-240");
    ui.connected=false; ui.home_value_valid=true;
    assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_HOME,&ui,NULL,NULL,NULL)==ESP_OK);
    fits(lv_screen_active(),"--"); assert(!graph_blended_ink());
    assert(ryz_v5_release()==ESP_OK);

    lv_mem_monitor_t before, after;
    lv_mem_monitor(&before);
    const size_t external_bytes=ryz_host_heap_bytes(), external_blocks=ryz_host_heap_blocks();
    ui.connected=true; ui.home_value_valid=true; ui.home_history_count=24;
    for(unsigned i=0;i<24;++i) ui.home_history[i]=(i+1U)*100;
    /* Warm the retained update path before measuring steady-state teardown.
     * Host TLSF may retain a larger backing block for an existing persistent
     * LVGL allocation (observed 24 bytes, no additional live block). Bound that
     * warm-up too; the following 32 complete cycles must be exactly stable. */
    ui.home_value=2400;
    assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_HOME,&ui,NULL,NULL,NULL)==ESP_OK);
    ui.home_value=0;
    assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_HOME,&ui,NULL,NULL,NULL)==ESP_OK);
    assert(ryz_v5_release()==ESP_OK);
    lv_mem_monitor(&after);
    assert(after.used_cnt==before.used_cnt && after.free_size+64>=before.free_size);
    before=after;
    for(unsigned i=0;i<32;++i) {
        ui.home_value=2400;
        assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_HOME,&ui,NULL,NULL,NULL)==ESP_OK);
        ui.home_value=0;
        assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_HOME,&ui,NULL,NULL,NULL)==ESP_OK);
        assert(ryz_v5_release()==ESP_OK);
        assert(lv_mem_test()==LV_RESULT_OK);
        lv_mem_monitor(&after);
        assert(after.free_size==before.free_size && after.used_cnt==before.used_cnt);
        assert(ryz_host_heap_bytes()==external_bytes && ryz_host_heap_blocks()==external_blocks);
    }
    passed("WLAN fixtures fit original labels, bounded 1px measured plots, unknown gating and owned-root cleanup");
}

static void selected_metric_pixels(const char *directory)
{
    ryz_workbench_telemetry_t w={0};
    ryz_system_services_snapshot_t s=fixture();
    ryz_system_ui_snapshot_t ui={.connected=true,.state=RYZ_SYSTEM_UI_NETWORK_ONLINE,
        .time_hhmm="18:43",.storage_ready=true,.storage_used_bytes=8192,.storage_total_bytes=1048576};
    ryz_v5_network_model_t n={0};
    static const char *names[]={"v5-home-net-240","v5-home-temp-240",
        "v5-home-cpu-240","v5-home-psram-240"};
    static const char *values[]={"2.4",("-10 \xc2\xb0" "C"),"100%","42%"};
    for(unsigned metric=0;metric<RYZ_HOME_METRIC_COUNT;++metric) {
        uint64_t base=UINT64_C(1000000)+metric*UINT64_C(30000000);
        assert(ryz_workbench_telemetry_select(&w,(ryz_home_selection_t){
            .metric=(ryz_home_metric_t)metric,.generation=metric+1,.selected_at_us=base},&ui));
        traffic_unknown(&ui);
        if(metric==RYZ_HOME_METRIC_TEMP) {
            assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_HOME,&ui,NULL,NULL,NULL)==ESP_OK);
            assert(!graph_blended_ink() && !find_label(lv_screen_active(),"2.4"));
            ryz_host_display_save(directory,"v5-home-switched-empty-240");
        }
        uint64_t bytes=0;
        for(unsigned i=0;i<=24;++i) {
            uint64_t at=base+i*UINT64_C(1000000);
            bytes+=12500U*i;
            traffic_sample(&s,at,1,bytes,0);
            s.metrics.sampled_at_us=at;
            s.metrics.temperature_deci_c=(int16_t)(140-(int)i*10);
            s.metrics.load_permille=(uint16_t)(i*1000/24);
            ryz_workbench_telemetry_update(&w,&s,at,NULL,&ui,&n);
            ryz_workbench_telemetry_psram(&w,at,1000,1000-i*420/24,&ui);
        }
        assert(ui.home_history_count==24 && ui.home_value_valid);
        assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_HOME,&ui,NULL,NULL,NULL)==ESP_OK);
        fits(lv_screen_active(),values[metric]);
        assert(graph_blended_ink()>10);
        for(unsigned other=0;other<RYZ_HOME_METRIC_COUNT;++other)
            if(other!=metric && metric!=RYZ_HOME_METRIC_NET)
                assert(!find_label(lv_screen_active(),values[other]));
        ryz_host_display_save(directory,names[metric]);
    }
    assert(ryz_workbench_telemetry_select(&w,(ryz_home_selection_t){
        .metric=RYZ_HOME_METRIC_NET,.generation=5,.selected_at_us=125000000},&ui));
    ui.connected=false;
    assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_HOME,&ui,NULL,NULL,NULL)==ESP_OK);
    fits(lv_screen_active(),"OFFLINE");
    assert(!graph_blended_ink() && !find_label(lv_screen_active(),"0.0"));
    ryz_host_display_save(directory,"v5-home-net-offline-240");
    assert(ryz_v5_release()==ESP_OK);
    passed("four selected-only native 240px fixtures, shared graph, switch-empty and NET OFFLINE without synthetic zero");
}
#endif

static void psram_mapping(void)
{
    ryz_workbench_telemetry_t w={0};
    ryz_system_ui_snapshot_t ui={0};
    assert(ryz_workbench_telemetry_select(&w,(ryz_home_selection_t){.metric=RYZ_HOME_METRIC_PSRAM},&ui));
    ryz_workbench_telemetry_psram(&w,1000000,1000,580,&ui);
    assert(ui.psram_usage_valid && ui.psram_used_percent==42);
    ryz_workbench_telemetry_psram(&w,1999999,1000,0,&ui);
    assert(ui.psram_used_percent==42);
    ryz_workbench_telemetry_psram(&w,2000000,1000,0,&ui);
    assert(ui.psram_usage_valid && ui.psram_used_percent==100);
    ryz_workbench_telemetry_psram(&w,3000000,1000,1000,&ui);
    assert(ui.psram_usage_valid && ui.psram_used_percent==0);
    ryz_workbench_telemetry_psram(&w,4000000,1000,994,&ui);
    assert(ui.psram_used_percent==1);
    ryz_workbench_telemetry_psram(&w,1,1000,500,&ui);
    assert(ui.psram_used_percent==50); /* Clock rollback resamples. */
    ryz_workbench_telemetry_psram(&w,2,0,0,&ui);
    assert(!ui.psram_usage_valid);
    ryz_workbench_telemetry_psram(&w,1000002,1000,1001,&ui);
    assert(!ui.psram_usage_valid);
    ryz_workbench_telemetry_psram(&w,2000002,UINT64_MAX,0,&ui);
    assert(!ui.psram_usage_valid);
    ryz_workbench_telemetry_psram(&w,3000002,2097152,1879088,&ui);
    assert(ui.psram_usage_valid && ui.psram_used_percent==10);
    passed("real PSRAM used ratio, 1s cadence, rounding, invalid input and clock reset");
}

int main(int argc,char **argv)
{
    psram_mapping();
    assert(argc==2);
    (void)argv;
    availability(); state_and_quantization(); fps_mapping(); traffic_windows();
    traffic_quantization_and_history(); traffic_availability();
    traffic_identity_resets(); traffic_arithmetic_bounds();
    fixed_dashboard(); selected_metric_lifecycle(); scalar_invalidations(); selected_psram();
    service_failure_psram_recovery();
#ifndef RYZ_TELEMETRY_MAP_ONLY
    pixels(argv[1]);
    storage_pixels(argv[1]);
    traffic_pixels(argv[1]);
    selected_metric_pixels(argv[1]);
#endif
    printf("V5_TELEMETRY_PASS groups=%u\n",groups);
    return 0;
}
