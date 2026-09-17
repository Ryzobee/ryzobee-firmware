#include "ryz_v5_status.h"
#include "ryz_v5_render_dirty.h"
#include <string.h>

static ryz_v5_status_t status={.ble={.unavailable=true}};
static uint64_t scripts_error_generation;
static esp_err_t scripts_error;
static int info_offsets[RYZ_SYSTEM_UI_ROUTE_COUNT];

void ryz_v5_detail_scroll_reset(ryz_system_ui_route_t route)
{
    if(route>=0 && route<RYZ_SYSTEM_UI_ROUTE_COUNT) info_offsets[route]=0;
}

bool ryz_v5_detail_scroll(ryz_system_ui_route_t route,int delta)
{
    int height,viewport=132;
    if(route==RYZ_SYSTEM_UI_ROUTE_WIFI_INFO) height=ryz_v5_network_info_height(&status.network);
    else if(route==RYZ_SYSTEM_UI_ROUTE_BLE_INFO) height=ryz_v5_ble_info_height(&status.ble);
    else if(route==RYZ_SYSTEM_UI_ROUTE_VERSION_INFO) height=ryz_v5_version_info_height(&status.version);
    else if(route==RYZ_SYSTEM_UI_ROUTE_VERSION_AVAILABLE) {
        height=ryz_v5_version_available_height(&status.version);
        viewport=48;
    }
    else return false;
    const int maximum=height>viewport?height-viewport:0;
    /* A changed description/field length may have shortened the viewport.
     * Continue from the displayed (clamped) position, never an obsolete one. */
    int current=info_offsets[route];
    if(current>maximum) current=maximum;
    int64_t next=(int64_t)current+delta;
    if(next<0) next=0;
    if(next>maximum) next=maximum;
    bool changed=next!=info_offsets[route];
    info_offsets[route]=(int)next;
    return changed;
}

void ryz_v5_scripts_admission_error(uint64_t generation, esp_err_t error)
{ scripts_error_generation=generation; scripts_error=error; }

void ryz_v5_scripts_route_snapshot(ryz_system_ui_route_t route, ryz_v5_scripts_snapshot_t *out)
{
    if(!out) return;
    *out=(ryz_v5_scripts_snapshot_t){0};
    if(route<RYZ_SYSTEM_UI_ROUTE_SCRIPTS_SETTINGS || route>RYZ_SYSTEM_UI_ROUTE_SCRIPTS_DELETE_ALL) return;
    out->page=(ryz_v5_scripts_page_t)(route-RYZ_SYSTEM_UI_ROUTE_SCRIPTS_SETTINGS);
    if(status.scripts.page!=out->page) return;
    *out=status.scripts;
    if(out->generation && out->generation==scripts_error_generation && scripts_error!=ESP_OK)
        out->error=scripts_error; /* View-only rejection, never a committed result. */
}

void ryz_v5_status_get(ryz_v5_status_t *out) { if(out) *out=status; }
bool ryz_v5_status_set(const ryz_v5_status_t *next)
{
    if(!next || memcmp(&status,next,sizeof(status))==0) return false;
    status=*next;
    return true;
}
bool ryz_v5_status_publish_visible(ryz_system_ui_route_t route,
                                  const ryz_v5_status_t *next)
{
    const bool changed = ryz_v5_status_visible_changed(route, &status, next);
    (void)ryz_v5_status_set(next);
    return changed;
}
void ryz_v5_status_toggle_device_id(void)
{ status.version.show_device_id=!status.version.show_device_id; }

static void select_page(ryz_system_ui_route_t route,ryz_v5_status_t *view)
{
    switch(route) {
    case RYZ_SYSTEM_UI_ROUTE_AP_SETUP: view->network.page=RYZ_V5_NETWORK_AP; break;
    case RYZ_SYSTEM_UI_ROUTE_WIFI_CONNECTED: view->network.page=RYZ_V5_NETWORK_CONNECTED; break;
    case RYZ_SYSTEM_UI_ROUTE_WIFI_OFF: view->network.page=RYZ_V5_NETWORK_OFF; break;
    case RYZ_SYSTEM_UI_ROUTE_WIFI_INFO: view->network.page=RYZ_V5_NETWORK_INFO; break;
    case RYZ_SYSTEM_UI_ROUTE_WIFI_JOINING: view->network.page=RYZ_V5_NETWORK_JOINING; break;
    case RYZ_SYSTEM_UI_ROUTE_WIFI_FAILED: view->network.page=RYZ_V5_NETWORK_FAILED; break;
    case RYZ_SYSTEM_UI_ROUTE_WIFI_READY: view->network.page=RYZ_V5_NETWORK_READY; break;
    case RYZ_SYSTEM_UI_ROUTE_WIFI_SERVER: view->network.page=RYZ_V5_NETWORK_FILE_SERVER; break;
    case RYZ_SYSTEM_UI_ROUTE_WIFI_NO_NETWORK: view->network.page=RYZ_V5_NETWORK_NO_NETWORK; break;
    default: break;
    }
    if(route>=RYZ_SYSTEM_UI_ROUTE_BLE_PAIRED && route<=RYZ_SYSTEM_UI_ROUTE_BLE_FORGET)
        view->ble.page=(ryz_v5_ble_page_t)(route-RYZ_SYSTEM_UI_ROUTE_BLE_PAIRED);
    if(route>=RYZ_SYSTEM_UI_ROUTE_VERSION_CURRENT && route<=RYZ_SYSTEM_UI_ROUTE_VERSION_FAILED)
        view->version.page=(ryz_v5_version_page_t)(route-RYZ_SYSTEM_UI_ROUTE_VERSION_CURRENT);
}

esp_err_t ryz_v5_detail_draw(lv_obj_t *root,ryz_system_ui_route_t route,
                             const ryz_system_ui_snapshot_t *system)
{
    if(!system || route<0 || route>=RYZ_SYSTEM_UI_ROUTE_COUNT) return ESP_ERR_INVALID_ARG;
    ryz_v5_status_t view=status;
    select_page(route,&view);
    if(route>=RYZ_SYSTEM_UI_ROUTE_AP_SETUP && route<=RYZ_SYSTEM_UI_ROUTE_WIFI_NO_NETWORK) {
        ryz_v5_network_model_t model=view.network;
        model.info_scroll=info_offsets[route];
        model.system=*system;
        return ryz_v5_network_draw(root,&model);
    }
    if(route>=RYZ_SYSTEM_UI_ROUTE_BLE_PAIRED && route<=RYZ_SYSTEM_UI_ROUTE_BLE_FORGET) {
        ryz_v5_ble_model_t model=view.ble;
        model.info_scroll=info_offsets[route];
        return ryz_v5_ble_draw(root,&model);
    }
    if(route>=RYZ_SYSTEM_UI_ROUTE_SCRIPTS_SETTINGS && route<=RYZ_SYSTEM_UI_ROUTE_SCRIPTS_DELETE_ALL) {
        ryz_v5_scripts_snapshot_t scripts;
        ryz_v5_scripts_route_snapshot(route,&scripts);
        return ryz_v5_scripts_draw(root,&scripts);
    }
    if(route>=RYZ_SYSTEM_UI_ROUTE_VERSION_CURRENT && route<=RYZ_SYSTEM_UI_ROUTE_VERSION_FAILED) {
        ryz_v5_version_model_t model=view.version;
        model.info_scroll=info_offsets[route];
        return ryz_v5_version_draw(root,&model);
    }
    return ESP_ERR_INVALID_ARG;
}

const char *ryz_v5_detail_title(ryz_system_ui_route_t route)
{
    ryz_v5_status_t view=status;
    select_page(route,&view);
    if(route==RYZ_SYSTEM_UI_ROUTE_WIFI_FAILED &&
       (view.network.setup_failed || view.network.setup_pending)) return "AP LINK";
    if(route<=RYZ_SYSTEM_UI_ROUTE_WIFI_NO_NETWORK) return ryz_v5_network_title(view.network.page);
    if(route<=RYZ_SYSTEM_UI_ROUTE_BLE_FORGET) return ryz_v5_ble_title(&view.ble);
    if(route<=RYZ_SYSTEM_UI_ROUTE_SCRIPTS_DELETE_ALL) {
        ryz_v5_scripts_snapshot_t scripts;
        ryz_v5_scripts_route_snapshot(route,&scripts);
        return ryz_v5_scripts_title(&scripts);
    }
    return ryz_v5_version_title(view.version.page);
}

ryz_v5_network_action_t ryz_v5_network_route_hit(ryz_system_ui_route_t route,uint16_t x,uint16_t y)
{ ryz_v5_status_t view=status; select_page(route,&view); return ryz_v5_network_hit(&view.network,x,y); }
ryz_v5_ble_action_t ryz_v5_ble_route_hit(ryz_system_ui_route_t route,uint16_t x,uint16_t y)
{ ryz_v5_status_t view=status; select_page(route,&view); return ryz_v5_ble_hit(&view.ble,x,y); }
ryz_v5_version_action_t ryz_v5_version_route_hit(ryz_system_ui_route_t route,uint16_t x,uint16_t y)
{ ryz_v5_status_t view=status; select_page(route,&view); return ryz_v5_version_hit(&view.version,x,y); }
bool ryz_v5_scripts_route_hit(ryz_system_ui_route_t route,uint16_t x,uint16_t y,
                               ryz_v5_scripts_intent_t *out)
{
    ryz_v5_scripts_snapshot_t scripts;
    ryz_v5_scripts_route_snapshot(route,&scripts);
    return ryz_v5_scripts_hit(&scripts,x,y,out);
}
