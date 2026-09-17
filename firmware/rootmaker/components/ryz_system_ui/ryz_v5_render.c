#include "ryz_v5_render.h"
#include "ryz_v5_widgets.h"
#include "ryz_v5_assets.h"
#include "ryz_v5_apps.h"
#include "ryz_v5_status.h"
#include "ryz_v5_password.h"
#include "ryz_v5_fault.h"
#include "ryz_v5_display.h"
#include "ryz_lvgl_system.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BOX(x,y,w,h,c) ryz_v5_box(root,x,y,w,h,c,0,0)
#define TEXT(x,y,w,h,t,f,p,c,a) ryz_v5_label(root,x,y,w,h,t,f,p,c,a)
#define ICON(x,y,n,c) ryz_v5_image(root,x,y,&ryz_v5_asset_##n,c)
#define LEFT LV_TEXT_ALIGN_LEFT
#define RIGHT LV_TEXT_ALIGN_RIGHT
#define CENTER LV_TEXT_ALIGN_CENTER
#define MED RYZ_FONT_BODY_MEDIUM
#define SEMI RYZ_FONT_BODY_SEMIBOLD
#define DISPLAY RYZ_FONT_DISPLAY

/* Retain only an actually committed Apps page. Deletion revokes the pointer,
 * including direct bridge release and replacement by Lua/boot/other pages. */
static lv_obj_t *apps_root;
static TaskHandle_t apps_owner;
static bool apps_wifi, apps_ble;
static char apps_time[6];
static lv_obj_t *retained_root;
static TaskHandle_t retained_owner;
static ryz_system_ui_route_t retained_route;

static bool retain_route(ryz_system_ui_route_t route)
{
    return route==RYZ_SYSTEM_UI_ROUTE_HOME || route==RYZ_SYSTEM_UI_ROUTE_SETTINGS ||
        route==RYZ_SYSTEM_UI_ROUTE_DISPLAY || route==RYZ_SYSTEM_UI_ROUTE_BLE_PAIRING ||
        route==RYZ_SYSTEM_UI_ROUTE_VERSION_INFO || route==RYZ_SYSTEM_UI_ROUTE_VERSION_DOWNLOADING;
}

static bool header_ble(const ryz_v5_ble_model_t *ble)
{ return !ble->unavailable && ble->enabled && ble->bonded && ble->linked; }

static void apps_root_deleted(lv_event_t *event)
{
    if(lv_event_get_target_obj(event)==apps_root) { apps_root=NULL; apps_owner=NULL; }
    if(lv_event_get_target_obj(event)==retained_root) { retained_root=NULL; retained_owner=NULL; }
}

/* Geometry: Figma 55:1728 and its current V5 Header, not V5.1. */
static void header(lv_obj_t *root, const char *title, bool back,
                   const ryz_system_ui_snapshot_t *s,const ryz_v5_ble_model_t *ble)
{
    BOX(0,0,240,32,V5_BLACK);
    if (back) {
        ICON(9,8,back,V5_ORANGE);
    } else {
        lv_obj_t *plate = BOX(4,4,56,24,V5_ORANGE);
        ryz_v5_radius(plate,2);
        TEXT(4,4,56,24,"RYZOBEE",DISPLAY,16,V5_BLACK,CENTER);
    }
    TEXT(back?44:64,4,back?88:68,24,title,DISPLAY,20,V5_WHITE,LEFT);
    BOX(4,31,236,1,V5_BORDER);
    /* Current V5 TOP/CHILD Header (98:3989 / 98:4007): 16px icons,
     * status group at (136,8). Round fractional Figma positions to pixels;
     * exported battery vectors include their stroke bounds. */
    ICON(136,8,wifi_status,s->connected?V5_BODY:V5_DISABLED);
    ICON(154,8,ble_status,!ble->unavailable && ble->enabled && ble->bonded && ble->linked?
        V5_BODY:V5_DISABLED);
    /* Export the exact outline/terminal separately: no fake 75% fill. */
    ICON(172,10,battery_outline,V5_DISABLED);
    ICON(191,14,battery_terminal,V5_DISABLED);
    TEXT(196,8,36,16,s->time_hhmm[0]?s->time_hhmm:"--:--",SEMI,14,V5_ORANGE,RIGHT);
}

static void navigation(lv_obj_t *root, ryz_system_ui_route_t route)
{
    BOX(0,196,240,44,V5_BLACK);
    BOX(0,196,240,1,V5_BORDER);
    BOX(80,196,1,44,V5_BORDER);
    BOX(160,196,1,44,V5_BORDER);
    const unsigned active = route == RYZ_SYSTEM_UI_ROUTE_HOME ? 0 :
        route == RYZ_SYSTEM_UI_ROUTE_APPS ? 1 : 2;
    BOX((int)active*80,238,80,2,V5_ORANGE);
    ICON(32,201,home,active==0?V5_ORANGE:V5_MUTED);
    ICON(112,201,apps,active==1?V5_ORANGE:V5_MUTED);
    ICON(192,201,settings,active==2?V5_ORANGE:V5_MUTED);
    const char *labels[] = {"HOME","APPS","SETTING"};
    for (unsigned i=0;i<3;++i)
        TEXT((int)i*80,218,80,16,labels[i],MED,12,
             active==i?V5_ORANGE:V5_MUTED,CENTER);
}

typedef struct {
    uint8_t count;
    lv_point_t points[RYZ_SYSTEM_UI_TRAFFIC_SAMPLES];
} home_trend_plot_t;

static void trend_plot_event(lv_event_t *event)
{
    home_trend_plot_t *plot = lv_event_get_user_data(event);
    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        lv_free(plot);
        return;
    }
    if (lv_event_get_code(event) != LV_EVENT_DRAW_MAIN) return;
    lv_area_t area;
    lv_obj_get_coords(lv_event_get_target_obj(event), &area);
    lv_layer_t *layer = lv_event_get_layer(event);
    if (plot->count == 1) {
        lv_draw_rect_dsc_t dot;
        lv_draw_rect_dsc_init(&dot);
        dot.bg_color = lv_color_hex(V5_ORANGE);
        lv_area_t pixel = {area.x1 + plot->points[0].x,
                          area.y1 + plot->points[0].y,
                          area.x1 + plot->points[0].x,
                          area.y1 + plot->points[0].y};
        lv_draw_rect(layer, &dot, &pixel);
    }
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_hex(V5_ORANGE);
    line.width = 1;
    for (unsigned i = 1; i < plot->count; ++i) {
        line.p1.x = area.x1 + plot->points[i-1].x;
        line.p1.y = area.y1 + plot->points[i-1].y;
        line.p2.x = area.x1 + plot->points[i].x;
        line.p2.y = area.y1 + plot->points[i].y;
        lv_draw_line(layer, &line);
    }
}

static bool home_value_in_range(ryz_home_metric_t metric, int64_t value)
{
    if(metric==RYZ_HOME_METRIC_NET) return value>=0 && value<=UINT32_MAX;
    if(metric==RYZ_HOME_METRIC_TEMP) return value>=INT16_MIN && value<=INT16_MAX;
    return (metric==RYZ_HOME_METRIC_CPU || metric==RYZ_HOME_METRIC_PSRAM) &&
        value>=0 && value<=100;
}

static esp_err_t trend_plot(lv_obj_t *root, const ryz_system_ui_snapshot_t *s)
{
    ryz_home_metric_t metric=s->home_selection.metric;
    if ((metric==RYZ_HOME_METRIC_NET && !s->connected) ||
        !s->home_value_valid || !s->home_history_count ||
        s->home_history_count > RYZ_SYSTEM_UI_TRAFFIC_SAMPLES ||
        !home_value_in_range(metric,s->home_value)) return ESP_OK;
    int64_t low=0,high=metric==RYZ_HOME_METRIC_NET?1000:100;
    for(unsigned i=0;i<s->home_history_count;++i) {
        int64_t value=s->home_history[i];
        if(!home_value_in_range(metric,value)) return ESP_OK;
        if(value<low) low=value;
        if(value>high) high=value;
    }
    home_trend_plot_t points={.count=s->home_history_count};
    for (unsigned i = 0; i < points.count; ++i) {
        unsigned slot = RYZ_SYSTEM_UI_TRAFFIC_SAMPLES - points.count + i;
        points.points[i].x = 1 + (int32_t)(slot * 159 / (RYZ_SYSTEM_UI_TRAFFIC_SAMPLES - 1));
        points.points[i].y = 34 - (int32_t)((s->home_history[i]-low) * 33 / (high-low));
    }
    /* Own the immutable points with this candidate root. A cancelled commit
     * may leave the previous root alive; it must not borrow our stack/state. */
    lv_obj_t *object = BOX(66,54,162,36,V5_SURFACE);
    if (!object) return ESP_ERR_NO_MEM;
    home_trend_plot_t *plot=NULL;
    for(uint32_t i=0;i<lv_obj_get_event_count(object);++i) {
        lv_event_dsc_t *event=lv_obj_get_event_dsc(object,i);
        if(lv_event_dsc_get_cb(event)==trend_plot_event) plot=lv_event_dsc_get_user_data(event);
    }
    if(!plot) {
        plot=lv_malloc(sizeof(*plot));
        if(!plot) return ESP_ERR_NO_MEM;
        *plot=points;
        lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
        if(!lv_obj_add_event_cb(object,trend_plot_event,LV_EVENT_ALL,plot)) {
            lv_free(plot); return ESP_ERR_NO_MEM;
        }
    } else if(memcmp(plot,&points,sizeof(points))) {
        *plot=points; lv_obj_invalidate(object);
    }
    return ESP_OK;
}

static esp_err_t home(lv_obj_t *root, const ryz_system_ui_snapshot_t *s)
{
    const ryz_home_metric_t selected=s->home_selection.metric;
    const bool net=selected==RYZ_HOME_METRIC_NET;
    const bool valid=s->home_value_valid && home_value_in_range(selected,s->home_value);
    BOX(8,34,224,74,V5_SURFACE);
    ICON(14,48,logo,V5_ORANGE);
    BOX(60,34,1,74,V5_BORDER);
    TEXT(14,91,40,16,s->connected?"LINK":"IDLE",MED,12,V5_MUTED,CENTER);
    ICON(66,36,wifi,net?V5_ORANGE:V5_MUTED);
    TEXT(84,35,38,16,"WLAN",SEMI,12,net?V5_ORANGE:V5_MUTED,LEFT);
    char rate[12] = "--";
    if (net && s->connected && valid) {
        if (s->home_value < 9950) {
            unsigned tenths = (unsigned)(s->home_value + 50U) / 100U;
            snprintf(rate,sizeof(rate),"%u.%u",tenths/10,tenths%10);
        } else {
            uint64_t whole = ((uint64_t)s->home_value + 500U) / 1000U;
            if (whole <= 999) snprintf(rate,sizeof(rate),"%u",(unsigned)whole);
            else snprintf(rate,sizeof(rate),"HI"); /* Explicitly outside the fixed V5 slot. */
        }
    }
    /* Three Teko 22px digits need 30px. Grow into the unused left gap while
     * preserving the original right edge (200), unit gap and text size. */
    TEXT(170,32,30,24,rate,DISPLAY,22,V5_WHITE,RIGHT);
    TEXT(202,36,30,16,"Mb/s",MED,12,V5_MUTED,RIGHT);
    ICON(66,54,grid,V5_BORDER);
    esp_err_t plot_error = trend_plot(root,s);
    if (plot_error != ESP_OK) return plot_error;
    if(net && !s->connected)
        TEXT(78,62,138,20,"OFFLINE",MED,12,V5_MUTED,CENTER);
    TEXT(66,91,34,16,s->connected?"LINK":"OFF",MED,12,
         s->connected?V5_GREEN:V5_MUTED,LEFT);
    char rssi[16]="-- dBm";
    if(s->connected && s->rssi_valid)
        snprintf(rssi,sizeof(rssi),"%d dBm",(int)s->rssi_dbm);
    TEXT(104,91,56,16,rssi,SEMI,12,V5_WHITE,LEFT);
    TEXT(166,91,18,16,"AP",MED,12,V5_MUTED,LEFT);
    TEXT(184,91,48,16,s->ap_active?"READY":"OFF",SEMI,12,V5_WHITE,RIGHT);
    BOX(8,108,224,1,V5_BORDER);
    BOX(8,112,224,44,V5_SURFACE);
    BOX(8,112,224,1,V5_BORDER);
    BOX(8,155,224,1,V5_BORDER);
    BOX(80,112,1,44,V5_BORDER);
    BOX(156,112,1,44,V5_BORDER);
    ICON(12,116,temp,net || selected==RYZ_HOME_METRIC_TEMP?V5_ORANGE:V5_MUTED);
    ICON(84,116,load,net || selected==RYZ_HOME_METRIC_CPU?V5_ORANGE:V5_MUTED);
    ICON(160,116,psram,net || selected==RYZ_HOME_METRIC_PSRAM?V5_ORANGE:V5_MUTED);
    TEXT(30,114,42,16,"CPU",MED,12,selected==RYZ_HOME_METRIC_TEMP?V5_ORANGE:V5_MUTED,LEFT);
    TEXT(102,114,46,16,"LOAD",MED,12,selected==RYZ_HOME_METRIC_CPU?V5_ORANGE:V5_MUTED,LEFT);
    TEXT(178,114,46,16,"PSRAM",MED,12,selected==RYZ_HOME_METRIC_PSRAM?V5_ORANGE:V5_MUTED,LEFT);
    char temperature[16]="--",load[12]="--",psram[12]="--";
    if(net && s->cpu_temperature_valid)
        snprintf(temperature,sizeof(temperature),"%d \xc2\xb0" "C",(int)s->cpu_temperature_c);
    if(net && s->cpu_load_valid && s->cpu_load_percent<=100)
        snprintf(load,sizeof(load),"%u%%",(unsigned)s->cpu_load_percent);
    if(net && s->psram_usage_valid && s->psram_used_percent<=100)
        snprintf(psram,sizeof(psram),"%u%%",(unsigned)s->psram_used_percent);
    /* Teko's native degree plus ASCII C; chip-internal, not ambient temperature. */
    if(selected==RYZ_HOME_METRIC_TEMP && valid)
        snprintf(temperature,sizeof(temperature),"%d \xc2\xb0" "C",(int)s->home_value);
    if(selected==RYZ_HOME_METRIC_CPU && valid)
        snprintf(load,sizeof(load),"%u%%",(unsigned)s->home_value);
    if(selected==RYZ_HOME_METRIC_PSRAM && valid)
        snprintf(psram,sizeof(psram),"%u%%",(unsigned)s->home_value);
    TEXT(12,130,60,24,temperature,DISPLAY,24,V5_WHITE,LEFT);
    TEXT(84,130,64,24,load,DISPLAY,24,V5_WHITE,LEFT);
    TEXT(160,130,64,24,psram,DISPLAY,24,V5_WHITE,LEFT);
    ICON(8,159,storage,V5_ORANGE);
    TEXT(26,158,92,18,"STORAGE",MED,14,V5_WHITE,LEFT);
    unsigned used=0;
    unsigned bar_width=0;
    char value[24]="-- USED";
    if(s->storage_ready && s->storage_total_bytes) {
        size_t bytes=s->storage_used_bytes>s->storage_total_bytes?
            s->storage_total_bytes:s->storage_used_bytes;
        used=(unsigned)((uint64_t)bytes*100/s->storage_total_bytes);
        if(bytes && !used) {
            snprintf(value,sizeof(value),"<1%% USED");
            bar_width=1;
        } else {
            snprintf(value,sizeof(value),"%u%% USED",used);
            bar_width=224*used/100;
        }
    }
    TEXT(122,158,110,18,value,MED,14,V5_MUTED,RIGHT);
    BOX(8,181,224,6,V5_BORDER);
    if(bar_width) BOX(8,181,(int)bar_width,6,V5_ORANGE);
    for(int x=64;x<=176;x+=56) BOX(x,181,1,6,V5_BG);
    return ESP_OK;
}

static void settings(lv_obj_t *root, const ryz_system_ui_snapshot_t *s,const ryz_v5_ble_model_t *ble)
{
    const char *labels[]={"WI-FI","BLE","SCRIPTS","DISPLAY","VERSION"};
    const char *values[]={s->state==RYZ_SYSTEM_UI_NETWORK_OFF?"OFF":"ON",
                           ble->unavailable?"--":ble->enabled?"ON":"OFF",
                           "--","SETUP",s->firmware_version[0]?s->firmware_version:"--"};
    for(int i=0;i<5;++i) {
        int y=32+i*33,h=i==4?32:33;
        if(i==0) BOX(8,y,224,h,V5_SELECTED);
        BOX(8,y,i==0?3:1,h,i==0?V5_ORANGE:V5_BORDER);
        BOX(8,y+h-1,224,1,V5_BORDER);
        char number[4]; snprintf(number,sizeof(number),"%02d",i+1);
        TEXT(16,y,22,h,i==0?">":number,SEMI,12,
             i==0?V5_ORANGE:V5_DISABLED,LEFT);
        TEXT(44,y,112,h,labels[i],i==0?SEMI:MED,14,
             i==0?V5_WHITE:V5_BODY,LEFT);
        TEXT(160,y,64,h,values[i],SEMI,12,
             i==0?V5_ORANGE:V5_MUTED,RIGHT);
    }
}

bool ryz_v5_needs_cleanup(void)
{ return ryz_v5_password_needs_cleanup() || ryz_v5_fault_needs_cleanup(); }

esp_err_t ryz_v5_release(void)
{
    (void)ryz_v5_password_revoke();
    esp_err_t error=ryz_lvgl_system_release();
    if(error==ESP_OK) {
        ryz_v5_widgets_release();
        error=ryz_v5_password_cleanup();
        if(error==ESP_OK) error=ryz_v5_fault_cleanup();
    }
    return error;
}

esp_err_t ryz_v5_render(ryz_system_ui_route_t route,
                       const ryz_system_ui_snapshot_t *snapshot,
                       bool (*cancelled)(void *),void *context,
                       bool *cancelled_out)
{
    if(cancelled_out) *cancelled_out=false;
    if(!snapshot || route<0 || route>=RYZ_SYSTEM_UI_ROUTE_COUNT) {
        if(ryz_v5_needs_cleanup()) (void)ryz_v5_release();
        return ESP_ERR_INVALID_ARG;
    }
    ryz_v5_status_t state;
    ryz_v5_status_get(&state);
    (void)ryz_v5_password_refresh(route==RYZ_SYSTEM_UI_ROUTE_WIFI_INFO &&
        snapshot->connected && state.network.enabled && !state.network.busy);
    if((!ryz_v5_password_held() && ryz_v5_password_needs_cleanup()) ||
       (route!=RYZ_SYSTEM_UI_ROUTE_SCRIPT_FAULT && ryz_v5_fault_needs_cleanup())) {
        /* This mandatory redaction is never cancelled by an arriving job. */
        esp_err_t cleaned=ryz_v5_release();
        if(cleaned!=ESP_OK) return cleaned;
    }
    if(route==RYZ_SYSTEM_UI_ROUTE_APPS && apps_root && apps_owner==xTaskGetCurrentTaskHandle() &&
       apps_wifi==snapshot->connected && apps_ble==header_ble(&state.ble) &&
       !memcmp(apps_time,snapshot->time_hhmm,sizeof(apps_time))) {
        /* No mutation before a pending-job cancellation. On any transfer
         * failure discard the optimized root; the existing shell error path
         * revokes input and repairs pixels with a complete checked frame. */
        if(cancelled && cancelled(context)) {
            if(cancelled_out) *cancelled_out=true;
            return ESP_ERR_TIMEOUT;
        }
        if(ryz_v5_apps_update(apps_root)) {
            lv_timer_ready(lv_display_get_refr_timer(lv_obj_get_display(apps_root)));
            esp_err_t error=ryz_lvgl_system_pump(cancelled,context,cancelled_out);
            if(error==ESP_OK) error=ryz_v5_widgets_status();
            if(error!=ESP_OK) (void)ryz_v5_release();
            return error;
        }
    }
    const bool retain=retain_route(route);
    bool reuse=retain && retained_root && retained_owner==xTaskGetCurrentTaskHandle() && retained_route==route;
    if(reuse && cancelled && cancelled(context)) {
        if(cancelled_out) *cancelled_out=true;
        return ESP_ERR_TIMEOUT;
    }
retry_full:;
    lv_obj_t *root=reuse?retained_root:NULL;
    esp_err_t error=ESP_OK;
    if(!reuse) error=ryz_lvgl_system_create(&root);
    if(error!=ESP_OK) {
        if(ryz_v5_needs_cleanup()) (void)ryz_v5_release();
        return error;
    }
    ryz_v5_widgets_begin();
    if(retain) {
        error=ryz_v5_widgets_retain_begin(root,reuse);
        if(error!=ESP_OK) { (void)ryz_v5_release(); return error; }
    }
    if(!reuse) lv_obj_set_style_bg_color(root,lv_color_hex(V5_BG),0);
    if(route==RYZ_SYSTEM_UI_ROUTE_SCRIPT_FAULT) error=ryz_v5_fault_draw(root,snapshot);
    else if(route==RYZ_SYSTEM_UI_ROUTE_DISPLAY) error=ryz_v5_display_draw(root);
    else if(route==RYZ_SYSTEM_UI_ROUTE_HOME) error=home(root,snapshot);
    else if(route==RYZ_SYSTEM_UI_ROUTE_SETTINGS) settings(root,snapshot,&state.ble);
    else if(route==RYZ_SYSTEM_UI_ROUTE_APPS) error=ryz_v5_apps_draw(root);
    else error=ryz_v5_detail_draw(root,route,snapshot);
    bool apps=route==RYZ_SYSTEM_UI_ROUTE_APPS;
    bool detail=route>=RYZ_SYSTEM_UI_ROUTE_AP_SETUP;
    if(route!=RYZ_SYSTEM_UI_ROUTE_SCRIPT_FAULT) header(root,route==RYZ_SYSTEM_UI_ROUTE_DISPLAY?"DISPLAY":apps?ryz_v5_apps_title():
           detail?ryz_v5_detail_title(route):
           route==RYZ_SYSTEM_UI_ROUTE_HOME?"HOME":"SETTINGS",
           (apps && ryz_v5_apps_has_back()) || detail,snapshot,&state.ble);
    if(!detail && (!apps || ryz_v5_apps_has_navigation())) navigation(root,route);
    if(retain) {
        esp_err_t retained_error=ryz_v5_widgets_retain_end();
        if(reuse && retained_error==ESP_ERR_NOT_SUPPORTED) {
            /* Conditional content changed topology. No partial frame was
             * flushed. Discard and rebuild; never keep stale controls. */
            error=ryz_v5_release();
            if(error!=ESP_OK) return error;
            reuse=false; goto retry_full;
        }
        if(error==ESP_OK) error=retained_error;
    }
    if(error==ESP_OK) error=ryz_v5_widgets_status();
    if(error!=ESP_OK) { (void)ryz_v5_release(); return error; }
    if(!reuse && (apps || retain) &&
       !lv_obj_add_event_cb(root,apps_root_deleted,LV_EVENT_DELETE,NULL)) {
        (void)ryz_v5_release(); return ESP_ERR_NO_MEM;
    }
    if(reuse) {
        lv_timer_ready(lv_display_get_refr_timer(lv_obj_get_display(root)));
        error=ryz_lvgl_system_pump(cancelled,context,cancelled_out);
    } else error=ryz_lvgl_system_commit(root,cancelled,context,cancelled_out);
    if(error==ESP_OK) error=ryz_v5_widgets_status();
    if(error==ESP_OK && (!cancelled_out || !*cancelled_out) && apps) {
        apps_root=root; apps_owner=xTaskGetCurrentTaskHandle();
        apps_wifi=snapshot->connected; apps_ble=header_ble(&state.ble);
        memcpy(apps_time,snapshot->time_hhmm,sizeof(apps_time));
    }
    if(error==ESP_OK && (!cancelled_out || !*cancelled_out) && retain) {
        retained_root=root; retained_owner=xTaskGetCurrentTaskHandle(); retained_route=route;
    }
    if(error!=ESP_OK && (retain || ryz_v5_needs_cleanup()))
        (void)ryz_v5_release(); /* Old logical root is not a safe rollback. */
    return error;
}
