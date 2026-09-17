/* Real V5 shell, LVGL/fonts/QR and Workbench control adapter. The network
 * service snapshot/admission is fake; no radio, NVS or device is accessed. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "workbench_network.h"
#include "ryz_v5_status.h"
#include "ryz_v5_render.h"
#include "ryz_font.h"
#include "ryz_display_host.h"

#ifdef NDEBUG
#error "Network setup checks require active assertions"
#endif

static ryz_system_services_snapshot_t services;
static ryz_system_ui_snapshot_t system_view;
static ryz_v5_status_t view;
static esp_err_t admission_error, snapshot_error;
static unsigned requests;
static ryz_system_network_action_t submitted;

esp_err_t ryz_system_services_get_snapshot(ryz_system_services_snapshot_t *out)
{ if(snapshot_error!=ESP_OK) return snapshot_error; *out=services; return ESP_OK; }
esp_err_t ryz_system_services_request_network(ryz_system_network_action_t action,uint32_t *out)
{
    ++requests; submitted=action;
    if(admission_error!=ESP_OK) return admission_error;
    if(services.network_operation.requested!=services.network_operation.completed) return ESP_ERR_INVALID_STATE;
    services.network_operation.requested++;
    services.network_operation.action=action;
    services.network_operation.phase=RYZ_SYSTEM_NETWORK_QUEUED;
    *out=services.network_operation.requested;
    return ESP_OK;
}

static bool label(lv_obj_t *o,const char *text)
{
    if(lv_obj_check_type(o,&lv_label_class) && !strcmp(lv_label_get_text(o),text)) return true;
    for(uint32_t i=0;i<lv_obj_get_child_count(o);++i)
        if(label(lv_obj_get_child(o,(int32_t)i),text)) return true;
    return false;
}
static void publish(void)
{
    ryz_workbench_network_controls(&services,&view.network);
    view.network.system=system_view;
    (void)ryz_v5_status_set(&view);
}
static void sample(ryz_touch_event_t event,unsigned x,unsigned y,ryz_system_ui_action_t expected)
{
    ryz_touch_sample_t s={.event=event,.pressed=event==RYZ_TOUCH_DOWN,
        .has_position=event==RYZ_TOUCH_DOWN,.x=(uint16_t)x,.y=(uint16_t)y};
    ryz_system_ui_action_t got=RYZ_SYSTEM_UI_ACTION_OTA_UPDATE;
    assert(ryz_system_ui_process_sample(&s,ESP_OK,&system_view,&got)==ESP_OK && got==expected);
}
static void tap(unsigned x,unsigned y,ryz_system_ui_action_t expected)
{ sample(RYZ_TOUCH_DOWN,x,y,RYZ_SYSTEM_UI_ACTION_NONE); sample(RYZ_TOUCH_UP,0,0,expected); }
static void draw(void) { publish(); assert(ryz_system_ui_render(&system_view)==ESP_OK); }
static void tick(void) { publish(); assert(ryz_system_ui_tick(&system_view)==ESP_OK); }
static void complete(esp_err_t error)
{
    services.network_operation.completed=services.network_operation.requested;
    services.network_operation.completed_action=services.network_operation.action;
    services.network_operation.result=error;
    services.network_operation.completed_network_valid=services.network_valid;
    services.network_operation.completed_network_state=services.network.state;
    services.network_operation.phase=error==ESP_OK?RYZ_SYSTEM_NETWORK_DONE:RYZ_SYSTEM_NETWORK_FAILED;
}
static void fresh(void)
{
    ryz_system_ui_reset(); ryz_host_display_reset();
    services=(ryz_system_services_snapshot_t){.provisioning_ready=true,.network_valid=true,
        .network={.enabled=true,.credentials_stored=true,.state=RYZ_PROVISIONING_ONLINE}};
    system_view=(ryz_system_ui_snapshot_t){.configured=true,.connected=true,.state=RYZ_SYSTEM_UI_NETWORK_ONLINE};
    view=(ryz_v5_status_t){.ble={.unavailable=true}};
    strcpy(view.network.sta_ssid,"SAMPLE NETWORK");
    strcpy(view.network.ipv4,"192.0.2.37");
    admission_error=snapshot_error=ESP_OK; requests=0;
    draw(); sample(RYZ_TOUCH_NONE,0,0,RYZ_SYSTEM_UI_ACTION_NONE);
    tap(200,220,RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_SETTINGS);
    tap(100,48,RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_WIFI_CONNECTED);
}
static void setup(void)
{
    tap(120,156,RYZ_SYSTEM_UI_ACTION_REPROVISION);
    ryz_ui_nav_token_t token=ryz_system_ui_page_token();
    uint32_t id=0;
    assert(ryz_workbench_network_request(RYZ_SYSTEM_UI_ACTION_REPROVISION,&id)==ESP_OK && id);
    assert(submitted==RYZ_SYSTEM_NETWORK_OPEN_AP);
    assert(ryz_system_ui_navigate(token,RYZ_SYSTEM_UI_ROUTE_AP_SETUP)==ESP_OK);
    draw(); sample(RYZ_TOUCH_NONE,0,0,RYZ_SYSTEM_UI_ACTION_NONE);
}
static void actual_ap(void)
{
    system_view.connected=false; system_view.ap_active=true;
    system_view.state=RYZ_SYSTEM_UI_NETWORK_AP_READY;
    services.network.state=RYZ_PROVISIONING_AP_READY;
    strcpy(system_view.ap_ssid,"SAMPLE-AP");
    strcpy(system_view.ap_password,"fixture-password");
    strcpy(system_view.portal_ip,"192.168.4.1");
}

static void pending_and_back(const char *directory)
{
    fresh(); assert(view.network.can_setup);
    ryz_host_display_save(directory,"v5-setup-connected-240");
    setup(); assert(view.network.busy && view.network.setup_pending);
    for(int phase=RYZ_SYSTEM_NETWORK_QUEUED;phase<=RYZ_SYSTEM_NETWORK_APPLYING;++phase) {
        services.network_operation.phase=phase; tick();
        assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_AP_SETUP);
        assert(label(lv_screen_active(),"SETUP PENDING"));
        assert(!label(lv_screen_active(),"SSID READY")); /* Pending is not AP success. */
        assert(!label(lv_screen_active(),"CONNECTED"));
    }
    ryz_host_display_save(directory,"v5-setup-pending-240");
    tap(120,156,RYZ_SYSTEM_UI_ACTION_NONE); assert(requests==1);
    const ryz_ui_nav_token_t old=ryz_system_ui_page_token();
    tap(20,16,RYZ_SYSTEM_UI_ACTION_NONE);
    /* Leaving the route never submits OFF or REPROVISION. No late callback
     * can steal the screen from HOME (actual radio state can still change). */
    assert(requests==1);
    assert(ryz_system_ui_navigate(ryz_system_ui_page_token(),RYZ_SYSTEM_UI_ROUTE_HOME)==ESP_OK);
    draw(); actual_ap(); complete(ESP_OK); tick();
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_HOME);
    assert(ryz_system_ui_navigate(old,RYZ_SYSTEM_UI_ROUTE_AP_SETUP)==ESP_ERR_INVALID_STATE);
    printf("PASS setup holds stale ONLINE, duplicate input inert, Back and late navigation safe\n");
}

static void failure_retry(const char *directory)
{
    fresh(); setup(); complete(ESP_FAIL); tick();
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_WIFI_FAILED);
    assert(view.network.setup_failed && view.network.can_retry);
    assert(system_view.connected); /* OTA cleanup rejection kept real STA online. */
    assert(label(lv_screen_active(),"SETUP FAILED"));
    assert(label(lv_screen_active(),"CHECK AP SETUP") && label(lv_screen_active(),"AP LINK"));
    assert(!label(lv_screen_active(),"CHECK NETWORK"));
    ryz_host_display_save(directory,"v5-setup-failed-240");
    sample(RYZ_TOUCH_NONE,0,0,RYZ_SYSTEM_UI_ACTION_NONE);
    tap(170,187,RYZ_SYSTEM_UI_ACTION_NETWORK_RETRY);
    uint32_t id=0;
    assert(ryz_workbench_network_request(RYZ_SYSTEM_UI_ACTION_NETWORK_RETRY,&id)==ESP_OK);
    assert(submitted==RYZ_SYSTEM_NETWORK_OPEN_AP); draw(); tick();
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_WIFI_FAILED);
    assert(label(lv_screen_active(),"SETUP PENDING") && !label(lv_screen_active(),"CONNECTION FAILED"));
    actual_ap(); complete(ESP_OK); tick();
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_AP_SETUP);
    assert(!view.network.setup_failed && label(lv_screen_active(),"SAMPLE-AP"));
    ryz_host_display_save(directory,"v5-setup-ready-240");
    sample(RYZ_TOUCH_NONE,0,0,RYZ_SYSTEM_UI_ACTION_NONE);
    const unsigned prior_requests=requests;
    tap(20,16,RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route()!=RYZ_SYSTEM_UI_ROUTE_WIFI_FAILED);
    tick();
    assert(ryz_system_ui_current_route()!=RYZ_SYSTEM_UI_ROUTE_AP_SETUP);
    assert(requests==prior_requests && system_view.ap_active);
    /* AP command succeeded; a later STA failure retries STA, not opening AP. */
    system_view.ap_active=false; system_view.state=RYZ_SYSTEM_UI_NETWORK_FAILED;
    services.network.state=RYZ_PROVISIONING_FAILED; tick();
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_WIFI_FAILED);
    assert(ryz_workbench_network_request(RYZ_SYSTEM_UI_ACTION_NETWORK_RETRY,&id)==ESP_OK);
    assert(submitted==RYZ_SYSTEM_NETWORK_ON);
    printf("PASS setup failure remains separate from link state; retry chooses actual failed action\n");
}

static void fast_join(void)
{
    fresh(); setup();
    /* Phone finishes before the first completed OPEN_AP snapshot reaches UI. */
    complete(ESP_OK); tick();
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_WIFI_READY);
    fresh(); setup(); complete(ESP_OK);
    system_view.connected=false; system_view.state=RYZ_SYSTEM_UI_NETWORK_CREDENTIALS_RECEIVED;
    tick(); assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_WIFI_JOINING);
    printf("PASS completed fast submission follows real JOIN/READY, not forced AP ready\n");
}

static void admission(void)
{
    fresh(); uint32_t id=99;
    admission_error=ESP_ERR_TIMEOUT;
    assert(ryz_workbench_network_request(RYZ_SYSTEM_UI_ACTION_REPROVISION,&id)==ESP_ERR_TIMEOUT && id==0);
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_WIFI_CONNECTED && services.network.credentials_stored);
    const unsigned before=requests;
    assert(ryz_workbench_network_request(RYZ_SYSTEM_UI_ACTION_NETWORK_CANCEL,&id)==ESP_ERR_NOT_SUPPORTED);
    assert(ryz_workbench_network_request(RYZ_SYSTEM_UI_ACTION_NETWORK_RETRY,&id)==ESP_ERR_INVALID_STATE);
    snapshot_error=ESP_FAIL;
    assert(ryz_workbench_network_request(RYZ_SYSTEM_UI_ACTION_NETWORK_RETRY,&id)==ESP_FAIL);
    assert(requests==before);
    services.network_valid=false; publish();
    assert(view.network.busy && !view.network.enabled && !view.network.can_setup && !view.network.can_retry);
    printf("PASS rejected admission never navigates, no cancel alias or retry from stale/unavailable state\n");
}

static void superseded_setup_failure(void)
{
    fresh(); setup(); complete(ESP_FAIL); tick();
    assert(view.network.setup_failed);
    ++services.network.revision; tick(); /* Ordinary ONLINE telemetry is not a new failure. */
    assert(view.network.setup_failed);
    services.network.state=RYZ_PROVISIONING_FAILED;
    services.network.last_error=ESP_FAIL; services.network.detail=201;
    system_view.state=RYZ_SYSTEM_UI_NETWORK_FAILED; system_view.connected=false;
    tick();
    assert(!view.network.setup_failed && view.network.can_retry && view.network.operation_error==ESP_OK);
    draw(); assert(label(lv_screen_active(),"CONNECTION FAILED") && !label(lv_screen_active(),"SETUP FAILED"));
    uint32_t id=0;
    assert(ryz_workbench_network_request(RYZ_SYSTEM_UI_ACTION_NETWORK_RETRY,&id)==ESP_OK);
    assert(submitted==RYZ_SYSTEM_NETWORK_ON);
    fresh(); setup();
    services.network.state=RYZ_PROVISIONING_FAILED; services.network.enabled=false;
    system_view.state=RYZ_SYSTEM_UI_NETWORK_FAILED; system_view.connected=false;
    complete(ESP_FAIL); tick();
    assert(view.network.setup_failed && view.network.can_retry);
    assert(ryz_workbench_network_request(RYZ_SYSTEM_UI_ACTION_NETWORK_RETRY,&id)==ESP_OK);
    assert(submitted==RYZ_SYSTEM_NETWORK_OPEN_AP);
    /* If the completion sample was unavailable, preserve a known command
     * failure conservatively, never claim that opening AP succeeded. */
    complete(ESP_FAIL); services.network_operation.completed_network_valid=false;
    services.network.state=RYZ_PROVISIONING_ONLINE; publish();
    assert(view.network.setup_failed);
    printf("PASS completed setup failure belongs to its captured network state, not a later STA failure\n");
}

static void switch_failure_feedback(const char *directory)
{
    fresh();
    uint32_t id=0;
    assert(ryz_workbench_network_request(RYZ_SYSTEM_UI_ACTION_NETWORK_OFF,&id)==ESP_OK);
    complete(ESP_FAIL); draw();
    assert(view.network.enabled && !view.network.busy);
    assert(label(lv_screen_active(),"OFF FAILED") && label(lv_screen_active(),"ONLINE"));
    ryz_host_display_save(directory,"v5-preference-off-failed-240");

    services.network.enabled=false; services.network.state=RYZ_PROVISIONING_OFF;
    system_view.connected=false; system_view.state=RYZ_SYSTEM_UI_NETWORK_OFF;
    complete(ESP_OK); tick();
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_WIFI_OFF);
    sample(RYZ_TOUCH_NONE,0,0,RYZ_SYSTEM_UI_ACTION_NONE);
    tap(214,46,RYZ_SYSTEM_UI_ACTION_NETWORK_ON);
    assert(ryz_workbench_network_request(RYZ_SYSTEM_UI_ACTION_NETWORK_ON,&id)==ESP_OK);
    draw(); assert(label(lv_screen_active(),"PLEASE WAIT"));
    complete(ESP_FAIL); draw();
    assert(!view.network.enabled && !view.network.busy);
    assert(label(lv_screen_active(),"START FAILED") && label(lv_screen_active(),"RADIO OFF"));
    assert(!label(lv_screen_active(),"PLEASE WAIT"));
    ryz_host_display_save(directory,"v5-preference-on-failed-240");
    /* A failure leaves the same switch retryable. A new request hides the
     * old error while busy; it never fabricates an enabled/online radio. */
    sample(RYZ_TOUCH_NONE,0,0,RYZ_SYSTEM_UI_ACTION_NONE);
    tap(214,46,RYZ_SYSTEM_UI_ACTION_NETWORK_ON);
    assert(ryz_workbench_network_request(RYZ_SYSTEM_UI_ACTION_NETWORK_ON,&id)==ESP_OK);
    draw();
    assert(label(lv_screen_active(),"PLEASE WAIT") && !label(lv_screen_active(),"START FAILED"));
    assert(!view.network.enabled);
    puts("PASS ON/OFF failure stays visible without replacing actual radio state, explicit retry clears old hint");
}

static void initialization_failure_feedback(const char *directory)
{
    fresh();
    assert(ryz_system_ui_navigate(ryz_system_ui_page_token(),RYZ_SYSTEM_UI_ROUTE_HOME)==ESP_OK);
    services=(ryz_system_services_snapshot_t){0};
    system_view=(ryz_system_ui_snapshot_t){.state=RYZ_SYSTEM_UI_NETWORK_UNCONFIGURED};
    draw(); sample(RYZ_TOUCH_NONE,0,0,RYZ_SYSTEM_UI_ACTION_NONE);
    tap(200,220,RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_SETTINGS);
    tap(100,48,RYZ_SYSTEM_UI_ACTION_NONE); draw();
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_WIFI_NO_NETWORK);
    assert(label(lv_screen_active(),"PLEASE WAIT"));
    services.errors[RYZ_SYSTEM_PROVISIONING_INIT]=ESP_FAIL;
    draw();
    assert(view.network.busy && !view.network.can_setup && !view.network.can_retry);
    assert(label(lv_screen_active(),"INIT FAILED"));
    assert(!label(lv_screen_active(),"PLEASE WAIT"));
    assert(label(lv_screen_active(),"WI-FI UNAVAILABLE"));
    assert(!label(lv_screen_active(),"NO NETWORK"));
    assert(label(lv_screen_active(),"Wi-Fi could not initialize.\nSaved network state is unknown."));
    sample(RYZ_TOUCH_NONE,0,0,RYZ_SYSTEM_UI_ACTION_NONE);
    tap(214,46,RYZ_SYSTEM_UI_ACTION_NONE);
    assert(requests==0);
    ryz_host_display_save(directory,"v5-preference-init-failed-240");
    /* Owner retries initialization. A restored OFF preference is settled,
     * not a radio failure and not an endlessly pending operation. */
    services.provisioning_ready=services.network_valid=true;
    services.errors[RYZ_SYSTEM_PROVISIONING_INIT]=ESP_OK;
    services.network.state=RYZ_PROVISIONING_OFF;
    system_view.state=RYZ_SYSTEM_UI_NETWORK_OFF;
    tick(); draw();
    assert(!view.network.busy && !view.network.enabled);
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_WIFI_OFF);
    assert(label(lv_screen_active(),"RADIO OFF") && !label(lv_screen_active(),"INIT FAILED"));
    puts("PASS init error is visible while unavailable controls stay locked, recovery restores saved OFF view");
}

int main(int argc,char **argv)
{
    assert(argc==2 && ryz_font_init()==ESP_OK);
    pending_and_back(argv[1]); failure_retry(argv[1]); fast_join(); admission(); superseded_setup_failure();
    switch_failure_feedback(argv[1]); initialization_failure_feedback(argv[1]);
    ryz_system_ui_reset(); assert(ryz_v5_release()==ESP_OK);
    puts("V5_NETWORK_SETUP_PASS 7 groups; real UI/adapter, fake native admission and panel");
    return 0;
}
