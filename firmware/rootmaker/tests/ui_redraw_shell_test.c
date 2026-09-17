#define main original_shell_main
#include "v5_shell_ui_test.c"
#undef main
#define RYZ_REDRAW_FULL_ORACLE
static void audit_full_render(void);
#include "ui_redraw_measure.h"
static void audit_full_render(void)
{
    assert(ryz_v5_release()==ESP_OK);
    assert(ryz_v5_render(ryz_system_ui_current_route(),&system_view,NULL,NULL,NULL)==ESP_OK);
}

static void service_render(void)
{
    bool dirty=ryz_v5_status_publish_visible(ryz_system_ui_current_route(),&service_view);
    if(dirty) assert(ryz_system_ui_render(&system_view)==ESP_OK);
}

int main(int argc,char **argv)
{
    (void)argc; (void)argv;
    fresh(); system_view.cpu_temperature_valid=true; system_view.cpu_temperature_c=48;
    system_view.cpu_load_valid=true; system_view.cpu_load_percent=20;
    assert(ryz_system_ui_render(&system_view)==ESP_OK);
    ryz_system_ui_snapshot_t old=system_view;
    audit_begin(); system_view.cpu_temperature_c=49;
    if(ryz_v5_system_visible_changed(RYZ_SYSTEM_UI_ROUTE_HOME,&old,&system_view))
        assert(ryz_system_ui_render(&system_view)==ESP_OK);
    audit_end("HOME temperature 48->49");
    old=system_view;
    audit_begin(); system_view.cpu_load_percent=21;
    if(ryz_v5_system_visible_changed(RYZ_SYSTEM_UI_ROUTE_HOME,&old,&system_view))
        assert(ryz_system_ui_render(&system_view)==ESP_OK);
    audit_end("HOME load 20->21");
    owner_page(RYZ_SYSTEM_UI_ROUTE_SETTINGS);
    strcpy(system_view.time_hhmm,"18:43"); assert(ryz_system_ui_render(&system_view)==ESP_OK);
    old=system_view;
    audit_begin(); strcpy(system_view.time_hhmm,"18:44");
    if(ryz_v5_system_visible_changed(RYZ_SYSTEM_UI_ROUTE_SETTINGS,&old,&system_view))
        assert(ryz_system_ui_render(&system_view)==ESP_OK);
    audit_end("SETTINGS header minute");
    service_view.ble=(ryz_v5_ble_model_t){.enabled=true,.pairing_active=true,
        .remaining_valid=true,.remaining_seconds=30,.pairing_window_seconds=30};
    publish(); owner_page(RYZ_SYSTEM_UI_ROUTE_BLE_PAIRING);
    audit_begin(); service_view.ble.remaining_seconds=29; service_render();
    audit_end("BLE pairing 30->29");
    service_view.version=(ryz_v5_version_model_t){.wifi_ready=true,.cancel_ready=true,
        .attempt_id=17,.downloaded_bytes=500,.total_bytes=100000,.running="0.9.0",.candidate="0.9.1"};
    publish(); owner_page(RYZ_SYSTEM_UI_ROUTE_VERSION_DOWNLOADING);
    audit_begin(); service_view.version.downloaded_bytes=501; service_render();
    audit_end("OTA bytes 500->501");
    audit_begin(); service_view.version.downloaded_bytes=1500; service_render();
    audit_end("OTA progress 0->1 percent");
    service_view.version=(ryz_v5_version_model_t){.running="0.9.0",.build="Sep 14 2026",
        .idf="5.5.4",.slot="ota_0",.uptime="00:00:01",.chip="ESP32-S3"};
    publish(); owner_page(RYZ_SYSTEM_UI_ROUTE_VERSION_INFO);
    audit_begin(); strcpy(service_view.version.uptime,"00:00:02"); service_render();
    audit_end("VERSION info uptime +1s");
    sample(RYZ_TOUCH_DOWN,true,180,170);
    audit_begin(); sample(RYZ_TOUCH_MOVE,true,180,140); audit_end("VERSION info scroll 30px");
    audit_begin(); sample(RYZ_TOUCH_MOVE,true,180,130); audit_end("VERSION info scroll 10px");
    sample(RYZ_TOUCH_UP,false,0,0);
    assert(ryz_v5_release()==ESP_OK);
    printf("AUDIT_GLOBAL_CASES %u\n",audit_global);
    return audit_global ? 1 : 0;
}
