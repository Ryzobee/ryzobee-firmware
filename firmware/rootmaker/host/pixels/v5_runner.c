#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ryz_v5_render.h"
#include "ryz_v5_apps.h"
#include "ryz_display_host.h"
#include "ryz_pixels_host.h"
#include "ryz_font.h"
#include "ryz_v5_status.h"
#include "ryz_v5_widgets.h"

/* Explicit deterministic data fixture. These values never enter firmware
 * defaults and are not evidence of a physical sensor, network or Store. */
static ryz_v5_apps_snapshot_t fixture;
static esp_err_t get(void *unused, ryz_v5_apps_snapshot_t *out)
{ (void)unused; *out=fixture; return ESP_OK; }
static esp_err_t submit(void *unused, const ryz_v5_apps_intent_t *intent)
{ (void)unused; (void)intent; return ESP_ERR_NOT_SUPPORTED; }

static void render(ryz_system_ui_route_t route,
                    const ryz_system_ui_snapshot_t *snapshot,
                    const char *directory,const char *name)
{
    bool cancelled=true;
    unsigned fonts_before=ryz_v5_widgets_font_count();
    esp_err_t error=ryz_v5_render(route,snapshot,NULL,NULL,&cancelled);
    if(error!=ESP_OK) fprintf(stderr,"V5 render error=%d route=%d fonts=%u\n",
        error,route,fonts_before);
    assert(error==ESP_OK);
    assert(!cancelled);
    ryz_host_display_save(directory,name);
}

int main(int argc,char **argv)
{
    assert(argc==2);
    ryz_host_display_reset();
    /* System pages must work before (or without) the Lua FreeType engine. */
    assert(!ryz_font_ready());
    const size_t baseline=ryz_host_heap_blocks();
    ryz_system_ui_snapshot_t state={.storage_ready=true,
        .storage_total_bytes=1024*1024,.storage_used_bytes=153600,
        .state=RYZ_SYSTEM_UI_NETWORK_AP_READY,.ap_active=true,.time_hhmm="18:43",
        .firmware_version=RYZ_HOST_FIRMWARE_VERSION};
    assert(!strcmp(state.firmware_version, RYZ_HOST_FIRMWARE_VERSION));
    render(RYZ_SYSTEM_UI_ROUTE_HOME,&state,argv[1],"v5-home");
    render(RYZ_SYSTEM_UI_ROUTE_SETTINGS,&state,argv[1],"v5-settings");

    fixture.open=true;
    fixture.token.page=(ryz_ui_nav_token_t){.route=2,.generation=1};
    fixture.token.reader_request_id=1;
    fixture.page_limit=4;
    fixture.reader.view=RYZ_APPS_VIEW_CATALOG;
    fixture.reader.state=RYZ_APPS_READY;
    fixture.reader.store_ready=true;
    fixture.reader.page.revision=1;
    fixture.reader.page.count=3;
    fixture.reader.page.total=3;
    const char *names[]={"tool_hardware.lua","tool_i2c.lua","tool_monitor.lua"};
    for(unsigned i=0;i<3;++i) {
        snprintf(fixture.reader.page.entries[i].name,
                 sizeof(fixture.reader.page.entries[i].name),"%s",names[i]);
        fixture.reader.page.entries[i].bytes=192;
    }
    const ryz_v5_apps_binding_t binding={.get=get,.submit=submit};
    ryz_v5_apps_bind(&binding);
    (void)ryz_v5_apps_tick(0);
    render(RYZ_SYSTEM_UI_ROUTE_APPS,&state,argv[1],"v5-apps");
    const ryz_touch_sample_t release={.event=RYZ_TOUCH_UP};
    const ryz_touch_sample_t launcher={.event=RYZ_TOUCH_DOWN,.pressed=true,
        .has_position=true,.x=70,.y=54};
    const ryz_touch_sample_t press={.event=RYZ_TOUCH_DOWN,.pressed=true,
        .has_position=true,.x=70,.y=70};
    (void)ryz_v5_apps_pointer(&release,20);
    (void)ryz_v5_apps_pointer(&launcher,30);
    (void)ryz_v5_apps_pointer(&release,40);
    (void)ryz_v5_apps_tick(40);
    render(RYZ_SYSTEM_UI_ROUTE_APPS,&state,argv[1],"v5-lua-files");
    (void)ryz_v5_apps_pointer(&press,100);
    (void)ryz_v5_apps_tick(700);
    render(RYZ_SYSTEM_UI_ROUTE_APPS,&state,argv[1],"v5-app-hold");
    (void)ryz_v5_apps_pointer(&release,701);
    /* Start the independent detail fixture through the launcher, discarding
     * the intentionally unsupported request from the hold preview. */
    ryz_v5_apps_reset();
    (void)ryz_v5_apps_tick(702);
    (void)ryz_v5_apps_pointer(&release,703);
    (void)ryz_v5_apps_pointer(&launcher,704);
    (void)ryz_v5_apps_pointer(&release,705);
    fixture.reader.view=RYZ_APPS_VIEW_DETAIL;
    fixture.token.reader_request_id=2;
    fixture.reader.detail.file.entry=fixture.reader.page.entries[1];
    fixture.reader.detail.source_valid=true;
    fixture.reader.detail.revision=1;
    memset(fixture.reader.detail.file.sha256,'a',64);
    fixture.reader.detail.file.sha256[64]='\0';
    snprintf(fixture.reader.detail.metadata.description,
        sizeof(fixture.reader.detail.metadata.description),"I2C tool placeholder. Not implemented yet.");
    snprintf(fixture.reader.detail.metadata.author,
        sizeof(fixture.reader.detail.metadata.author),"RyzoBee");
    snprintf(fixture.reader.detail.metadata.version,
        sizeof(fixture.reader.detail.metadata.version),"0.0.0-placeholder");
    (void)ryz_v5_apps_tick(720);
    (void)ryz_v5_apps_pointer(&release,721);
    render(RYZ_SYSTEM_UI_ROUTE_APPS,&state,argv[1],"v5-app-detail");
    const ryz_touch_sample_t delete_press={.event=RYZ_TOUCH_DOWN,.pressed=true,
        .has_position=true,.x=30,.y=210};
    (void)ryz_v5_apps_pointer(&delete_press,730);
    (void)ryz_v5_apps_pointer(&release,740);
    render(RYZ_SYSTEM_UI_ROUTE_APPS,&state,argv[1],"v5-app-delete");

    /* All remaining original V5 pages, explicit view fixtures only. This
     * exercises a single continuous font/object lifetime, not a per-page
     * process that could hide a finite-cache exhaustion or leaked QR mask. */
    ryz_v5_status_t views={0};
    views.network.enabled=true;
    views.network.can_setup=true;
    views.network.can_cancel=true;
    views.network.can_retry=true;
    strcpy(views.network.sta_ssid,"Ryzobee-Test");
    strcpy(views.network.ipv4,"192.0.2.10");
    views.network.elapsed_valid=true;
    views.network.elapsed_ms=18000;
    views.ble.enabled=true;
    views.ble.bonded=true;
    views.ble.linked=true;
    views.ble.pairing_active=true;
    views.ble.remaining_valid=true;
    views.ble.remaining_seconds=18;
    views.ble.pairing_window_seconds=30;
    strcpy(views.ble.local_name,"Ryzobee-Test");
    strcpy(views.ble.peer_name,"TEST PHONE");
    strcpy(views.ble.mode,"TEST FIXTURE");
    views.scripts.generation=1;
    views.scripts.boot_known=true;
    views.scripts.boot_bytes_known=true;
    strcpy(views.scripts.boot_name,"tool_i2c.lua");
    views.scripts.boot_bytes=192;
    views.scripts.store_ready=true;
    views.scripts.catalog_state=RYZ_APPS_READY;
    views.scripts.catalog=fixture.reader.page;
    views.scripts.selected=fixture.reader.detail.file;
    views.scripts.selected_identity_valid=true;
    views.scripts.selected_source_valid=true;
    views.scripts.selected_revision=1;
    views.scripts.can_change_boot=true;
    views.scripts.can_clear_boot=true;
    strcpy(views.scripts.boot_name,"boot.lua");
    views.scripts.boot_revision=1;
    memset(views.scripts.boot_sha256,'a',64);
    views.scripts.can_restart=true;
    strcpy(views.scripts.result_name,"tool_i2c.lua");
    strcpy(views.version.running,RYZ_HOST_FIRMWARE_VERSION);
    assert(!strcmp(views.version.running, RYZ_HOST_FIRMWARE_VERSION));
    strcpy(views.version.candidate,"0.9.1");
    strcpy(views.version.build,"TEST FIXTURE");
    strcpy(views.version.idf,"v5.5.4");
    strcpy(views.version.channel,"TEST");
    views.version.wifi_ready=true;
    views.version.ab_slots=true;
    views.version.check_ready=true;
    views.version.update_ready=true;
    views.version.cancel_ready=true;
    views.version.restart_ready=true;
    views.version.attempt_id=17; /* Explicit demo identity, not an OTA operation. */
    views.version.retry_ready=true;
    views.version.downloaded_bytes=680000;
    views.version.total_bytes=1000000;
    state.configured=true;
    strcpy(state.ap_ssid,"Ryzobee-Test");
    strcpy(state.ap_password,"test-only-123");
    strcpy(state.portal_ip,"192.168.4.1");
    for(int route=RYZ_SYSTEM_UI_ROUTE_AP_SETUP;route<RYZ_SYSTEM_UI_ROUTE_COUNT;++route) {
        views.version.image_staged=route==RYZ_SYSTEM_UI_ROUTE_VERSION_REBOOT;
        if(route>=RYZ_SYSTEM_UI_ROUTE_SCRIPTS_SETTINGS && route<=RYZ_SYSTEM_UI_ROUTE_SCRIPTS_DELETE_ALL)
            views.scripts.page=(ryz_v5_scripts_page_t)(route-RYZ_SYSTEM_UI_ROUTE_SCRIPTS_SETTINGS);
        state.state=route==RYZ_SYSTEM_UI_ROUTE_WIFI_OFF?RYZ_SYSTEM_UI_NETWORK_OFF:RYZ_SYSTEM_UI_NETWORK_AP_READY;
        state.configured=route!=RYZ_SYSTEM_UI_ROUTE_WIFI_NO_NETWORK;
        state.connected=route==RYZ_SYSTEM_UI_ROUTE_WIFI_CONNECTED || route==RYZ_SYSTEM_UI_ROUTE_WIFI_INFO ||
            route==RYZ_SYSTEM_UI_ROUTE_WIFI_READY;
        views.network.enabled=route!=RYZ_SYSTEM_UI_ROUTE_WIFI_OFF;
        views.ble.enabled=route!=RYZ_SYSTEM_UI_ROUTE_BLE_OFF;
        views.ble.bonded=route!=RYZ_SYSTEM_UI_ROUTE_BLE_EMPTY && route!=RYZ_SYSTEM_UI_ROUTE_BLE_PAIRING;
        if(route==RYZ_SYSTEM_UI_ROUTE_SCRIPTS_SAVED) {
            views.scripts.operation=RYZ_V5_SCRIPTS_OP_BOOT;
            views.scripts.result=RYZ_V5_SCRIPTS_COMMITTED;
        } else views.scripts.result=RYZ_V5_SCRIPTS_IDLE;
        (void)ryz_v5_status_set(&views);
        char name[40]; snprintf(name,sizeof(name),"v5-route-%02d",route);
        printf("Rendering %s\n",name); fflush(stdout);
        render((ryz_system_ui_route_t)route,&state,argv[1],name);
        if(route==RYZ_SYSTEM_UI_ROUTE_VERSION_REBOOT) {
            render((ryz_system_ui_route_t)route,&state,argv[1],"v5-restart-ready");
            views.version.restart_error=ESP_ERR_TIMEOUT;
            (void)ryz_v5_status_set(&views);
            render((ryz_system_ui_route_t)route,&state,argv[1],"v5-restart-busy");
            views.version.restart_error=ESP_FAIL;
            (void)ryz_v5_status_set(&views);
            render((ryz_system_ui_route_t)route,&state,argv[1],"v5-restart-failed");
            views.version.restart_error=ESP_OK;
        }
    }
    printf("V5 full palette fonts=%u\n",ryz_v5_widgets_font_count());
    assert(!ryz_font_ready());
    /* Same real page roots/fonts repeat without permanent per-page growth. */
    for(unsigned i=0;i<5;++i) {
        assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_HOME,&state,NULL,NULL,NULL)==ESP_OK);
        assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_SETTINGS,&state,NULL,NULL,NULL)==ESP_OK);
    }
    assert(ryz_v5_release()==ESP_OK);
    ryz_v5_apps_reset();
    /* Only the shared display allocation is retained. System fonts are
     * immutable flash data, not per-page heap allocations. */
    size_t retained=ryz_host_heap_blocks();
    assert(retained>=baseline);
    assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_HOME,&state,NULL,NULL,NULL)==ESP_OK);
    assert(ryz_v5_release()==ESP_OK);
    assert(ryz_host_heap_blocks()==retained);
    printf("V5_PIXELS_PASS sample fixtures; shared retained=%zu peak=%zu\n",
        retained,ryz_host_heap_peak());
    return 0;
}
