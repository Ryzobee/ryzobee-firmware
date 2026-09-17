/* Production native UI/gesture/controller + real LVGL/FreeType. Credentials,
 * touch and BSP endpoints are deliberate fixtures, never device passwords. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "display.h"
#include "ryz_system_ui.h"
#include "ryz_v5_password.h"
#include "ryz_v5_status.h"
#include "ryz_v5_render.h"
#include "ryz_display_host.h"
#include "ryz_pixels_host.h"
#include "ryz_font.h"

#ifdef NDEBUG
#error "Password UI checks require active assertions"
#endif

static ryz_system_ui_snapshot_t system_view;
static ryz_v5_status_t view;
static ryz_v5_password_metadata_t source;
static esp_err_t inspect_error,copy_error;
static unsigned copies,groups;
static char fixture[64];

static void passed(const char *name) { ++groups; printf("PASS %s\n",name); }
static esp_err_t inspect(void *unused,ryz_v5_password_metadata_t *out)
{ (void)unused; *out=source; return inspect_error; }
static esp_err_t copy(void *unused,uint64_t epoch,char *out,size_t capacity)
{
    (void)unused;
    ++copies;
    assert(capacity==64);
    memset(out,0,capacity);
    /* Even an erroneous provider which writes output cannot paint it. */
    if(copy_error!=ESP_OK) { memcpy(out,"SAMPLE-FAILED-COPY",19); return copy_error; }
    if(!source.available || epoch!=source.epoch) return ESP_ERR_INVALID_STATE;
    memcpy(out,fixture,sizeof(fixture));
    return ESP_OK;
}

static lv_obj_t *label(lv_obj_t *root,const char *value)
{
    if(lv_obj_check_type(root,&lv_label_class) &&
       !strcmp(lv_label_get_text(root),value)) return root;
    for(uint32_t i=0;i<lv_obj_get_child_count(root);++i) {
        lv_obj_t *found=label(lv_obj_get_child(root,(int32_t)i),value);
        if(found) return found;
    }
    return NULL;
}

static bool contains(const void *memory,size_t capacity,const char *needle)
{
    size_t length=strlen(needle);
    if(!length || length>capacity) return false;
    const unsigned char *bytes=memory;
    for(size_t i=0;i<=capacity-length;++i)
        if(!memcmp(bytes+i,needle,length)) return true;
    return false;
}

static esp_err_t sample(ryz_touch_event_t event,bool pressed,bool positioned,
                        uint16_t x,uint16_t y)
{
    ryz_touch_sample_t point={.event=event,.pressed=pressed,.has_position=positioned,.x=x,.y=y};
    ryz_system_ui_action_t action=RYZ_SYSTEM_UI_ACTION_OTA_UPDATE;
    esp_err_t error=ryz_system_ui_process_sample(&point,ESP_OK,&system_view,&action);
    assert(action==RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_take_action()==RYZ_SYSTEM_UI_ACTION_NONE);
    return error;
}
static void released(void) { assert(sample(RYZ_TOUCH_NONE,false,false,0,0)==ESP_OK); }
static void hold(void)
{
    assert(sample(RYZ_TOUCH_DOWN,true,true,170,196)==ESP_OK);
    assert(ryz_v5_password_held());
}
static void publish(void) { view.network.system=system_view; (void)ryz_v5_status_set(&view); }
static void fresh(void)
{
    ryz_host_heap_allow();
    ryz_host_display_fail_privacy(ESP_OK);
    assert(ryz_system_ui_reset_checked()==ESP_OK);
    ryz_host_display_reset();
    inspect_error=copy_error=ESP_OK;
    source=(ryz_v5_password_metadata_t){true,7};
    copies=0;
    memset(fixture,0,sizeof(fixture));
    strcpy(fixture,"SAMPLE@pass-123");
    const ryz_v5_password_binding_t reader={.inspect=inspect,.copy=copy};
    ryz_v5_password_bind(&reader);
    system_view=(ryz_system_ui_snapshot_t){.connected=true,.configured=true,
        .state=RYZ_SYSTEM_UI_NETWORK_ONLINE,.time_hhmm="18:43"};
    memset(&view,0,sizeof(view));
    view.ble.unavailable=true;
    view.network.enabled=true;
    strcpy(view.network.sta_ssid,"SAMPLE NETWORK");
    strcpy(view.network.ipv4,"192.0.2.37");
    publish();
    assert(ryz_system_ui_navigate(ryz_system_ui_page_token(),RYZ_SYSTEM_UI_ROUTE_WIFI_INFO)==ESP_OK);
    assert(ryz_system_ui_render(&system_view)==ESP_OK);
    released();
    /* Real Info content scrolls; bring PASS above the fixed actions. */
    assert(sample(RYZ_TOUCH_DOWN,true,true,180,172)==ESP_OK);
    assert(sample(RYZ_TOUCH_MOVE,true,true,180,62)==ESP_OK);
    assert(sample(RYZ_TOUCH_UP,false,false,0,0)==ESP_OK);
    assert(ryz_v5_password_can_hold() && !ryz_v5_password_held());
    assert(label(lv_screen_active(),"************"));
    assert(!copies);
}

static void visibility_and_pixels(const char *directory)
{
    fresh();
    if(directory) ryz_host_display_save(directory,"v5-password-masked-240");
    hold();
    assert(copies==1);
    lv_obj_t *shown=label(lv_screen_active(),fixture);
    assert(shown);
    const char *owned=lv_label_get_text(shown);
    ryz_v5_status_t public_view;
    ryz_v5_status_get(&public_view);
    assert(!contains(&public_view,sizeof(public_view),fixture));
    assert(!contains(&system_view,sizeof(system_view),fixture));
    if(directory) ryz_host_display_save(directory,"v5-password-held-240");
    assert(sample(RYZ_TOUCH_NONE,true,true,170,196)==ESP_OK);
    assert(ryz_system_ui_tick(&system_view)==ESP_OK && copies==1);
    assert(sample(RYZ_TOUCH_UP,false,false,0,0)==ESP_OK);
    assert(!ryz_v5_password_held() && !ryz_v5_password_needs_cleanup());
    for(size_t i=0;i<65;++i) assert(!owned[i]); /* Static storage remains owned. */
    assert(label(lv_screen_active(),"************") && !label(lv_screen_active(),fixture));
    if(directory) ryz_host_display_save(directory,"v5-password-released-240");
    passed("explicit hold, static owned text, no snapshot copy and UP wipe");
}

static void contact_revocation(void)
{
    fresh(); hold();
    assert(sample(RYZ_TOUCH_MOVE,true,true,103,196)==ESP_OK);
    assert(!ryz_v5_password_held());
    assert(sample(RYZ_TOUCH_MOVE,true,true,170,196)==ESP_OK);
    assert(!ryz_v5_password_held() && copies==1);
    assert(sample(RYZ_TOUCH_UP,false,true,170,196)==ESP_OK);
    hold();
    assert(sample(RYZ_TOUCH_NONE,false,false,0,0)==ESP_OK);
    assert(!ryz_v5_password_held());
    hold();
    assert(sample(RYZ_TOUCH_NONE,true,true,170,100)==ESP_OK);
    assert(!ryz_v5_password_held());
    released(); hold();
    /* Duplicate DOWN is cancellation, never a new authorization. */
    assert(sample(RYZ_TOUCH_DOWN,true,true,170,196)==ESP_OK);
    assert(!ryz_v5_password_held());
    assert(sample(RYZ_TOUCH_MOVE,true,true,170,196)==ESP_OK);
    assert(!ryz_v5_password_held());
    released(); hold();
    assert(sample(RYZ_TOUCH_MOVE,true,false,0,0)==ESP_OK);
    assert(!ryz_v5_password_held());
    passed("MOVE/NONE/duplicate contact cancellation never re-arms without DOWN");
}

static void identities_and_failures(void)
{
    fresh(); hold();
    source.epoch++;
    assert(ryz_system_ui_tick(&system_view)==ESP_OK);
    assert(!ryz_v5_password_held() && copies==1);
    assert(sample(RYZ_TOUCH_MOVE,true,true,170,196)==ESP_OK && copies==1);
    released(); hold();
    inspect_error=ESP_ERR_TIMEOUT;
    assert(ryz_system_ui_tick(&system_view)==ESP_OK);
    assert(!ryz_v5_password_held());
    inspect_error=ESP_OK;
    assert(ryz_system_ui_tick(&system_view)==ESP_OK);
    assert(!ryz_v5_password_held());
    released(); hold();
    system_view.connected=false; publish();
    assert(ryz_system_ui_render(&system_view)==ESP_OK);
    assert(!ryz_v5_password_held() && label(lv_screen_active(),"--"));
    fresh(); copy_error=ESP_FAIL;
    assert(sample(RYZ_TOUCH_DOWN,true,true,170,196)==ESP_OK);
    assert(!ryz_v5_password_held() && !label(lv_screen_active(),"SAMPLE-FAILED-COPY"));
    assert(!ryz_v5_password_needs_cleanup());
    fresh(); hold();
    view.network.busy=true; publish();
    assert(ryz_system_ui_tick(&system_view)==ESP_OK && !ryz_v5_password_held());
    passed("epoch/availability/busy/offline revocation and partial copy failure");
}

static bool cancel_now(void *unused) { (void)unused; return true; }

static void navigation_errors_and_handoff(void)
{
    fresh(); hold();
    const char *owned=lv_label_get_text(label(lv_screen_active(),fixture));
    assert(ryz_system_ui_navigate(ryz_system_ui_page_token(),RYZ_SYSTEM_UI_ROUTE_HOME)==ESP_OK);
    for(size_t i=0;i<65;++i) assert(!owned[i]);
    assert(!ryz_v5_password_needs_cleanup());
    assert(ryz_system_ui_render(&system_view)==ESP_OK);
    fresh(); hold();
    ryz_system_ui_action_t action;
    assert(ryz_system_ui_process_sample(NULL,ESP_FAIL,&system_view,&action)==ESP_FAIL);
    assert(!ryz_v5_password_held() && !ryz_v5_password_needs_cleanup());
    fresh(); hold();
    bool cancelled=false;
    assert(ryz_system_ui_render_checked(&system_view,cancel_now,NULL,&cancelled)==ESP_ERR_TIMEOUT);
    assert(cancelled && !ryz_v5_password_held() && !ryz_v5_password_needs_cleanup());
    fresh(); hold();
    assert(sample(RYZ_TOUCH_UP,true,false,0,0)==ESP_ERR_INVALID_ARG);
    assert(!ryz_v5_password_held() && !ryz_v5_password_needs_cleanup());
    fresh(); hold();
    ryz_host_display_fail_show(ESP_FAIL,160);
    assert(ryz_system_ui_render(&system_view)!=ESP_OK);
    assert(!ryz_v5_password_held() && !ryz_v5_password_needs_cleanup());
    fresh(); hold();
    ryz_host_display_fail_privacy(ESP_FAIL);
    assert(ryz_system_ui_reset_checked()==ESP_FAIL);
    assert(!ryz_v5_password_held() && ryz_v5_password_needs_cleanup());
    assert(ryz_host_display_privacy_blocked());
    uint16_t pixel=0xffff;
    assert(ryz_display_read_pixel(200,166,&pixel)==ESP_ERR_INVALID_STATE);
    assert(ryz_system_ui_render(&system_view)!=ESP_OK);
    ryz_host_display_fail_privacy(ESP_OK);
    assert(ryz_system_ui_reset_checked()==ESP_OK);
    assert(!ryz_host_display_privacy_blocked());
    for(unsigned y=0;y<240;++y) for(unsigned x=0;x<240;++x) {
        assert(ryz_display_read_pixel((int)x,(int)y,&pixel)==ESP_OK);
        assert(!pixel);
    }
    passed("navigation, read/transfer failures and checked handoff privacy barrier");
}

static void text_boundaries(const char *directory)
{
    fresh();
    memcpy(fixture,"SAMPLE",6);
    for(size_t i=6;i<63;++i) fixture[i]=(char)('0'+i%10);
    fixture[63]=0;
    char expected[65];
    snprintf(expected,sizeof(expected),"%s",fixture);
    hold();
    lv_obj_t *shown=label(lv_screen_active(),expected);
    assert(shown);
    lv_area_t bounds; lv_obj_get_coords(shown,&bounds);
    assert(bounds.x1==76 && bounds.x2==227 && bounds.y1>=158 && bounds.y2<190);
    const lv_font_t *font=lv_obj_get_style_text_font(shown,0);
    assert(font->line_height>=14 && font->line_height<=24);
    assert(lv_label_get_long_mode(shown)==LV_LABEL_LONG_SCROLL_CIRCULAR);
    assert(strlen(lv_label_get_text(shown))==63);
    assert(sample(RYZ_TOUCH_UP,false,false,0,0)==ESP_OK);
    assert(!ryz_v5_password_held() && !label(lv_screen_active(),expected));
    if(directory) ryz_host_display_save(directory,"v5-password-long-240");
    fresh(); fixture[0]=0; hold();
    assert(label(lv_screen_active(),"OPEN NETWORK"));
    fresh(); fixture[4]=(char)0xc3;
    assert(sample(RYZ_TOUCH_DOWN,true,true,170,196)==ESP_OK);
    assert(!ryz_v5_password_held() && label(lv_screen_active(),"ASCII FONT ONLY"));
    fresh(); memset(fixture,'x',sizeof(fixture));
    assert(sample(RYZ_TOUCH_DOWN,true,true,170,196)==ESP_OK);
    assert(!ryz_v5_password_held());
    passed("63 complete ASCII bytes in scrolled private cell, open and unsupported data");
}

int main(int argc,char **argv)
{
    assert(ryz_font_init()==ESP_OK);
    const char *directory=argc>1 ? argv[1] : NULL;
    visibility_and_pixels(directory);
    contact_revocation();
    identities_and_failures();
    navigation_errors_and_handoff();
    text_boundaries(directory);
    assert(ryz_system_ui_reset_checked()==ESP_OK);
    ryz_v5_password_bind(NULL);
    printf("V5_PASSWORD_UI_PASS groups=%u\n",groups);
    return 0;
}
