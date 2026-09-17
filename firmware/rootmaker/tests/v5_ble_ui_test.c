/* V5 BLE view rendered by production LVGL/static glyphs and exact Figma
 * assets. Models below are explicitly DEMO fixtures, not a Bluetooth stack or
 * evidence of device pairing, RF, a real peer or actual countdown timing. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ryz_v5_ble.h"
#include "ryz_v5_widgets.h"
#include "ryz_v5_assets.h"
#include "ryz_lvgl_system.h"
#include "ryz_display_host.h"
#include "ryz_pixels_host.h"

#ifdef NDEBUG
#error "V5 BLE UI checks require assertions"
#endif

static unsigned groups;
static const char *output;
static void passed(const char *name) { ++groups; printf("PASS %s\n",name); }

static ryz_v5_ble_model_t demo(ryz_v5_ble_page_t page)
{
    ryz_v5_ble_model_t value={.page=page,.enabled=page!=RYZ_V5_BLE_OFF,
        .bonded=page!=RYZ_V5_BLE_EMPTY && page!=RYZ_V5_BLE_PAIRING,
        .linked=page==RYZ_V5_BLE_PAIRED || page==RYZ_V5_BLE_INFO,
        .pairing_active=page==RYZ_V5_BLE_PAIRING,.rssi_valid=true,.rssi_dbm=-54,
        .remaining_valid=true,.remaining_seconds=24,.pairing_window_seconds=30,
        .reconnect_known=true,.reconnect_enabled=true};
    strcpy(value.local_name,"DEMO-RYZOBEE");
    strcpy(value.peer_name,"DEMO-PEER");
    strcpy(value.local_address,"00:11:22:33:44:55");
    strcpy(value.mode,"DEMO BONDED MODE");
    return value;
}

static bool label(lv_obj_t *root,const char *text)
{
    for (uint32_t i=0;i<lv_obj_get_child_count(root);++i) {
        lv_obj_t *child=lv_obj_get_child(root,(int32_t)i);
        if (lv_obj_check_type(child,&lv_label_class) && !strcmp(lv_label_get_text(child),text)) return true;
        if (label(child,text)) return true;
    }
    return false;
}

static lv_obj_t *label_object(lv_obj_t *root,const char *text)
{
    for(uint32_t i=0;i<lv_obj_get_child_count(root);++i) {
        lv_obj_t *child=lv_obj_get_child(root,(int32_t)i);
        if(lv_obj_check_type(child,&lv_label_class) && !strcmp(lv_label_get_text(child),text)) return child;
        lv_obj_t *found=label_object(child,text);
        if(found) return found;
    }
    return NULL;
}

static bool image_at(lv_obj_t *root,const lv_image_dsc_t *asset,int x,int y)
{
    for (uint32_t i=0;i<lv_obj_get_child_count(root);++i) {
        lv_obj_t *child=lv_obj_get_child(root,(int32_t)i);
        if (lv_obj_check_type(child,&lv_image_class) && lv_image_get_src(child)==asset &&
            lv_obj_get_x(child)==x && lv_obj_get_y(child)==y) return true;
    }
    return false;
}

static uint16_t rgb565(uint32_t rgb)
{ return (uint16_t)(((rgb>>8)&0xf800)|((rgb>>5)&0x07e0)|((rgb>>3)&0x001f)); }

static void check_tree(lv_obj_t *root)
{
    for (uint32_t i=0;i<lv_obj_get_child_count(root);++i) {
        lv_obj_t *child=lv_obj_get_child(root,(int32_t)i);
        assert(lv_obj_get_x(child)>=0 && lv_obj_get_y(child)>=32);
        assert(lv_obj_get_x(child)+lv_obj_get_width(child)<=240);
        assert(lv_obj_get_y(child)+lv_obj_get_height(child)<=240);
        assert(!lv_obj_has_flag(child,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE));
    }
}

static lv_obj_t *render(const ryz_v5_ble_model_t *model,const char *name)
{
    ryz_v5_ble_model_t before=*model;
    lv_obj_t *root=NULL;
    assert(ryz_lvgl_system_create(&root)==ESP_OK && root);
    ryz_v5_widgets_begin();
    assert(ryz_v5_ble_draw(root,model)==ESP_OK);
    assert(memcmp(&before,model,sizeof(before))==0);
    size_t shows=ryz_host_display_shows();
    assert(ryz_lvgl_system_commit(root,NULL,NULL,NULL)==ESP_OK);
    assert(ryz_v5_widgets_status()==ESP_OK && ryz_host_display_shows()>shows);
    check_tree(root);
    if (output && name) ryz_host_display_save(output,name);
    return root;
}

static void hits(void)
{
    ryz_v5_ble_model_t value=demo(RYZ_V5_BLE_PAIRED);
    assert(ryz_v5_ble_hit(NULL,10,10)==RYZ_V5_BLE_ACTION_NONE);
    value.page=(ryz_v5_ble_page_t)99;
    assert(ryz_v5_ble_hit(&value,10,10)==RYZ_V5_BLE_ACTION_NONE);
    assert(!strcmp(ryz_v5_ble_title(&value),"BLE"));
    for (int p=RYZ_V5_BLE_PAIRED;p<=RYZ_V5_BLE_FORGET;++p) {
        value=demo((ryz_v5_ble_page_t)p);
        assert(ryz_v5_ble_title(&value)[0]);
        const ryz_v5_ble_action_t back=p==RYZ_V5_BLE_PAIRING?
            RYZ_V5_BLE_ACTION_CANCEL_PAIR:RYZ_V5_BLE_ACTION_BACK;
        assert(ryz_v5_ble_hit(&value,0,0)==back);
        assert(ryz_v5_ble_hit(&value,43,31)==back);
        assert(ryz_v5_ble_hit(&value,44,31)==RYZ_V5_BLE_ACTION_NONE);
        assert(ryz_v5_ble_hit(&value,-1,60)==RYZ_V5_BLE_ACTION_NONE);
        assert(ryz_v5_ble_hit(&value,240,60)==RYZ_V5_BLE_ACTION_NONE);
        assert(ryz_v5_ble_hit(&value,60,240)==RYZ_V5_BLE_ACTION_NONE);
    }
    value=demo(RYZ_V5_BLE_PAIRED);
    assert(ryz_v5_ble_hit(&value,214,45)==RYZ_V5_BLE_ACTION_DISABLE);
    assert(ryz_v5_ble_hit(&value,20,112)==RYZ_V5_BLE_ACTION_INFO);
    assert(ryz_v5_ble_hit(&value,20,146)==RYZ_V5_BLE_ACTION_REPLACE);
    assert(ryz_v5_ble_hit(&value,20,180)==RYZ_V5_BLE_ACTION_FORGET);
    value=demo(RYZ_V5_BLE_OFF);
    assert(ryz_v5_ble_hit(&value,214,45)==RYZ_V5_BLE_ACTION_ENABLE);
    assert(ryz_v5_ble_hit(&value,50,200)==RYZ_V5_BLE_ACTION_NONE);
    value=demo(RYZ_V5_BLE_EMPTY);
    assert(ryz_v5_ble_hit(&value,28,172)==RYZ_V5_BLE_ACTION_PAIR);
    for(int y=156;y<216;++y) for(int x=12;x<228;++x) {
        const bool hit=x>=20 && x<220 && y>=164 && y<208;
        assert(ryz_v5_ble_hit(&value,x,y)==(hit?RYZ_V5_BLE_ACTION_PAIR:RYZ_V5_BLE_ACTION_NONE));
    }
    value.enabled=false;
    assert(ryz_v5_ble_hit(&value,20,164)==RYZ_V5_BLE_ACTION_NONE);
    value.enabled=true; value.checking_state=true;
    assert(ryz_v5_ble_hit(&value,219,207)==RYZ_V5_BLE_ACTION_NONE);
    value.checking_state=false; value.unavailable=true;
    assert(ryz_v5_ble_hit(&value,20,164)==RYZ_V5_BLE_ACTION_NONE);
    value.unavailable=false; value.bonded=true;
    assert(ryz_v5_ble_hit(&value,219,207)==RYZ_V5_BLE_ACTION_REPLACE);
    value=demo(RYZ_V5_BLE_PAIRING);
    assert(ryz_v5_ble_hit(&value,30,220)==RYZ_V5_BLE_ACTION_CANCEL_PAIR);
    value.pairing_active=false;
    assert(ryz_v5_ble_hit(&value,30,220)==RYZ_V5_BLE_ACTION_BACK);
    value=demo(RYZ_V5_BLE_REPLACE);
    assert(ryz_v5_ble_hit(&value,50,180)==RYZ_V5_BLE_ACTION_BACK);
    assert(ryz_v5_ble_hit(&value,150,180)==RYZ_V5_BLE_ACTION_CONFIRM_REPLACE);
    value=demo(RYZ_V5_BLE_SUCCESS);
    assert(ryz_v5_ble_hit(&value,50,184)==RYZ_V5_BLE_ACTION_DONE);
    value=demo(RYZ_V5_BLE_INFO);
    assert(ryz_v5_ble_hit(&value,30,194)==RYZ_V5_BLE_ACTION_FORGET);
    assert(ryz_v5_ble_hit(&value,30,193)==RYZ_V5_BLE_ACTION_NONE);
    value=demo(RYZ_V5_BLE_FORGET);
    assert(ryz_v5_ble_hit(&value,50,178)==RYZ_V5_BLE_ACTION_BACK);
    assert(ryz_v5_ble_hit(&value,104,178)==RYZ_V5_BLE_ACTION_CONFIRM_FORGET);
    passed("all eight original pages have bounded pure hit actions");
}

static void unavailable(void)
{
    for (int p=RYZ_V5_BLE_PAIRED;p<=RYZ_V5_BLE_FORGET;++p) {
        ryz_v5_ble_model_t value=demo((ryz_v5_ble_page_t)p);
        value.unavailable=true; /* Deliberately retain contradictory positive flags. */
        const size_t heap=ryz_host_heap_blocks(), shows=ryz_host_display_shows();
        for (int y=0;y<240;++y) for (int x=0;x<240;++x) {
            ryz_v5_ble_action_t action=ryz_v5_ble_hit(&value,x,y);
            assert(action==RYZ_V5_BLE_ACTION_NONE || action==RYZ_V5_BLE_ACTION_BACK ||
                   action==RYZ_V5_BLE_ACTION_INFO || action==RYZ_V5_BLE_ACTION_DONE);
        }
        assert(ryz_host_heap_blocks()==heap && ryz_host_display_shows()==shows);
    }
    passed("unavailable overrides every mutation over all 460800 page pixels");
}

static void unavailable_diagnostics(void)
{
    ryz_v5_ble_model_t m=demo(RYZ_V5_BLE_OFF);
    m.unavailable=true;
    lv_obj_t *root=render(&m,"v5_ble_unavailable_unknown");
    assert(label(root,"N/A") && label(root,"BLE UNAVAILABLE"));
    assert(!label(root,"--") && !label(root,"BLE OFF") && !label(root,"RESTART DEVICE"));
    assert(ryz_v5_ble_hit(&m,213,46)==RYZ_V5_BLE_ACTION_NONE);
    m.failure=RYZ_V5_BLE_FAILURE_START;
    strcpy(m.error_code,"E_00000103");
    root=render(&m,"v5_ble_init_failed");
    assert(label(root,"ERR") && label(root,"BLE INIT FAILED") && label(root,"E_00000103"));
    assert(label(root,"RESTART DEVICE") && label(root,"DEPENDENT SETTINGS") && label(root,"LOCKED"));
    assert(!label(root,"--") && !label(root,"BLE OFF") && !label(root,"RETRY"));
    for(int y=0;y<240;++y) for(int x=0;x<240;++x) {
        const ryz_v5_ble_action_t action=ryz_v5_ble_hit(&m,x,y);
        assert(action==RYZ_V5_BLE_ACTION_NONE || action==RYZ_V5_BLE_ACTION_BACK);
    }
    m.failure=RYZ_V5_BLE_FAILURE_UNKNOWN;
    strcpy(m.error_code,"SDK_0000002A");
    root=render(&m,"v5_ble_service_failed");
    assert(label(root,"BLE FAILED") && label(root,"SDK_0000002A"));
    memset(m.error_code,'x',sizeof(m.error_code));
    root=render(&m,NULL);
    assert(label(root,"N/A") && label(root,"BLE UNAVAILABLE") && !label(root,"RESTART DEVICE"));
    m=demo(RYZ_V5_BLE_OFF);
    root=render(&m,"v5_ble_off_known");
    assert(label(root,"BLE OFF") && !label(root,"N/A") && !label(root,"ERR"));
    assert(ryz_v5_ble_hit(&m,213,46)==RYZ_V5_BLE_ACTION_ENABLE);
    passed("unavailable is not an OFF switch; actual init/service errors remain visible with no retry admission");
}

static void pixels(void)
{
    assert(ryz_v5_ble_draw(NULL,NULL)==ESP_ERR_INVALID_ARG);
    assert(ryz_font_init()==ESP_OK);
    for (int p=RYZ_V5_BLE_PAIRED;p<=RYZ_V5_BLE_FORGET;++p) {
        ryz_v5_ble_model_t value=demo((ryz_v5_ble_page_t)p);
        char name[48];
        snprintf(name,sizeof(name),"v5_ble_b%d_demo",p+1);
        lv_obj_t *root=render(&value,name);
            assert(ryz_host_display_pixel(0,216)==rgb565(p==RYZ_V5_BLE_INFO?V5_BLACK:V5_BORDER));
        if (p==RYZ_V5_BLE_PAIRING) {
            assert(label(root,"00:24"));
            assert(image_at(root,&ryz_v5_asset_ble_pair,100,70));
            assert(label(root,"DEMO-RYZOBEE") && label(root,"NEW DEVICE"));
        } else if (p==RYZ_V5_BLE_SUCCESS) {
            assert(image_at(root,&ryz_v5_asset_ble_success,103,75));
            assert(label(root,"DEMO-PEER"));
        } else if (p==RYZ_V5_BLE_OFF) {
            assert(image_at(root,&ryz_v5_asset_ble_pair,100,71));
            assert(!label(root,"B"));
        } else if (p==RYZ_V5_BLE_PAIRED) {
            assert(label(root,"DEMO-PEER") && label(root,"LINKED"));
            assert(ryz_host_display_pixel(8,70)==rgb565(V5_GREEN));
        }
        value.unavailable=true;
        snprintf(name,sizeof(name),"v5_ble_b%d_unavailable",p+1);
        root=render(&value,name);
        assert(!label(root,"DEMO-PEER") && !label(root,"LINKED") && !label(root,"00:24"));
        assert(!label(root,"PAIRED DEVICE EXISTS"));
        assert(!image_at(root,&ryz_v5_asset_ble_success,103,75));
        if(p==RYZ_V5_BLE_INFO) {
            /* INFO is now seven independently unknown rows, not a footer.
             * Its SERVICE row remains available through continuous scroll. */
            const char *fields[]={"LOCAL","MAC","PEER","RSSI","ROLE","BOND","SERVICE"};
            for(unsigned i=0;i<7;++i) assert(label(root,fields[i]));
            assert(label(root,"--") && !label(root,"SAVED") && !label(root,"READY"));
            assert(ryz_v5_ble_info_height(&value)>132);
            assert(ryz_v5_ble_hit(&value,100,200)==RYZ_V5_BLE_ACTION_NONE);
            value.info_scroll=ryz_v5_ble_info_height(&value)-132;
            root=render(&value,"v5_ble_info_unavailable_scrolled");
            assert(label(root,"SERVICE") && label(root,"--"));
        } else assert(label(root,"SERVICE UNAVAILABLE"));
    }
    passed("16 real LVGL RGB565 frames / original assets / unavailable has no fabricated peer or success");
}

static void unknowns(void)
{
    ryz_v5_ble_model_t value=demo(RYZ_V5_BLE_INFO);
    value.rssi_valid=false;
    value.local_name[0]=value.local_address[0]=value.mode[0]=0;
    memset(value.peer_name,'x',sizeof(value.peer_name)); /* Bounded unterminated producer input. */
    lv_obj_t *root=render(&value,NULL);
    assert(label(root,"--") && !label(root,"-54 dBm"));
    value=demo(RYZ_V5_BLE_PAIRING);
    value.remaining_valid=false;
    root=render(&value,NULL);
    assert(label(root,"--:--") && !label(root,"00:24"));
    value.remaining_valid=true; value.pairing_active=false;
    root=render(&value,NULL);
    assert(label(root,"--:--") && !label(root,"00:24"));
    value=demo(RYZ_V5_BLE_SUCCESS); value.bonded=false;
    root=render(&value,NULL);
    assert(!image_at(root,&ryz_v5_asset_ble_success,103,75));
    assert(!label(root,"PAIRED"));
    passed("unknown strings/RSSI/time and unconfirmed bond cannot become a success");
}

static ryz_v5_ble_model_t closure_demo(ryz_v5_ble_state_t state);

static void resources(void)
{
    assert(ryz_lvgl_system_release()==ESP_OK);
    ryz_v5_widgets_release();
    const size_t bytes=ryz_host_heap_bytes(), blocks=ryz_host_heap_blocks();
    for (unsigned loop=0;loop<3;++loop) {
        for (int page=RYZ_V5_BLE_PAIRED;page<=RYZ_V5_BLE_FORGET;++page) {
            ryz_v5_ble_model_t value=demo((ryz_v5_ble_page_t)page);
            (void)render(&value,NULL);
        }
        for(int state=RYZ_V5_BLE_STATE_SAVED_WAITING;state<=RYZ_V5_BLE_STATE_FORGETTING;++state) {
            ryz_v5_ble_model_t value=closure_demo((ryz_v5_ble_state_t)state);
            (void)render(&value,NULL);
        }
        assert(ryz_lvgl_system_release()==ESP_OK);
        ryz_v5_widgets_release();
        assert(ryz_host_heap_bytes()==bytes && ryz_host_heap_blocks()==blocks);
    }
    passed("font/root release across 48 legacy/closure page switches keeps only shared display allocation");
}

static ryz_v5_ble_model_t closure_demo(ryz_v5_ble_state_t state)
{
    ryz_v5_ble_model_t m=demo(RYZ_V5_BLE_PAIRED);
    m.state=state; m.bond_known=true; m.service_known=true; m.service_ready=false;
    m.retry_allowed=true; m.operation_id=71; m.linked=false;
    switch(state) {
    case RYZ_V5_BLE_STATE_LINKED_SERVICE_WAIT: m.linked=true; break;
    case RYZ_V5_BLE_STATE_PAIR_TIMEOUT:
        m.page=RYZ_V5_BLE_PAIRING; m.bonded=false; m.remaining_seconds=0; break;
    case RYZ_V5_BLE_STATE_PAIR_FAILED:
        m.page=RYZ_V5_BLE_PAIRING; m.bonded=false; m.failure=RYZ_V5_BLE_FAILURE_AUTH;
        strcpy(m.error_code,"E_AUTH"); break;
    case RYZ_V5_BLE_STATE_PAIRING_AFTER_REPLACE:
        m.page=RYZ_V5_BLE_PAIRING; m.bonded=false; m.old_bond_removed=true;
        m.pairing_active=true; m.remaining_seconds=30; break;
    case RYZ_V5_BLE_STATE_FORGET_FAILED:
        m.page=RYZ_V5_BLE_FORGET; strcpy(m.error_code,"E_STORE"); break;
    case RYZ_V5_BLE_STATE_SERVICE_FAILED:
        m.linked=true; strcpy(m.error_code,"E_GATT"); break;
    case RYZ_V5_BLE_STATE_FORGETTING: m.page=RYZ_V5_BLE_FORGET; break;
    default: break;
    }
    return m;
}

static void closure_states(void)
{
    const char *expected[]={"", "WAITING FOR CLIENT","OPEN CLIENT APP","PAIRING TIMED OUT",
        "PAIRING FAILED","OLD BOND REMOVED","COULD NOT FORGET","CLIENT NOT READY","FORGETTING DEVICE"};
    for(int state=RYZ_V5_BLE_STATE_SAVED_WAITING;state<=RYZ_V5_BLE_STATE_FORGETTING;++state) {
        ryz_v5_ble_model_t m=closure_demo((ryz_v5_ble_state_t)state);
        char name[64]; snprintf(name,sizeof(name),"v5_ble_closure_%d_demo",state);
        lv_obj_t *root=render(&m,name);
        assert(label(root,expected[state]));
        if(state==RYZ_V5_BLE_STATE_SAVED_WAITING) {
            assert(label(root,"SAVED") && label(root,"BONDED / RSSI --") && !label(root,"LINKED"));
            assert(!label(root,"AUTO RECONNECT ON") && !label(root,"NO BOND"));
        } else if(state==RYZ_V5_BLE_STATE_LINKED_SERVICE_WAIT) {
            assert(label(root,"LINKED") && label(root,"BONDED / SERVICE WAIT"));
            assert(!label(root,"BLE READY") && !label(root,"SERVICE READY"));
        } else if(state==RYZ_V5_BLE_STATE_PAIRING_AFTER_REPLACE) {
            assert(label(root,"00:30") && label(root,"DEMO-RYZOBEE") && label(root,"NO BOND TO KEEP"));
            assert(image_at(root,&ryz_v5_asset_ble_pair,100,70));
            lv_obj_t *cancel=label_object(root,"CANCEL");
            assert(cancel && lv_color_eq(lv_obj_get_style_text_color(cancel,0),lv_color_hex(V5_ORANGE)));
            assert(ryz_v5_ble_hit(&m,20,16)==RYZ_V5_BLE_ACTION_CANCEL_PAIR);
            assert(ryz_v5_ble_hit(&m,20,228)==RYZ_V5_BLE_ACTION_CANCEL_PAIR);
        } else if(state==RYZ_V5_BLE_STATE_FORGETTING) {
            for(int y=0;y<240;++y) for(int x=0;x<240;++x)
                assert(ryz_v5_ble_hit(&m,x,y)==RYZ_V5_BLE_ACTION_NONE);
            assert(label(root,"REMOVING...") && !label(root,"CANCEL"));
        } else if(state==RYZ_V5_BLE_STATE_SERVICE_FAILED) {
            assert(ryz_v5_ble_hit(&m,170,190)==RYZ_V5_BLE_ACTION_INFO);
            assert(label(root,"BOND KEPT") && !label(root,"RETRY"));
        } else {
            assert(ryz_v5_ble_hit(&m,170,190)==(state==RYZ_V5_BLE_STATE_FORGET_FAILED?
                RYZ_V5_BLE_ACTION_RETRY_FORGET:RYZ_V5_BLE_ACTION_RETRY_PAIR));
            m.retry_allowed=false;
            assert(ryz_v5_ble_hit(&m,170,190)==RYZ_V5_BLE_ACTION_NONE);
        }
        m.unavailable=true; /* Positive enum plus stale facts must not win. */
        snprintf(name,sizeof(name),"v5_ble_closure_%d_unavailable",state);
        root=render(&m,name);
        assert(!label(root,expected[state]) && !label(root,"DEMO-PEER") && !label(root,"00:30"));
        assert(!label(root,"E_AUTH") && !label(root,"E_STORE") && !label(root,"E_GATT"));
        for(int y=0;y<240;++y) for(int x=0;x<240;++x) {
            ryz_v5_ble_action_t action=ryz_v5_ble_hit(&m,x,y);
            assert(action==RYZ_V5_BLE_ACTION_NONE || action==RYZ_V5_BLE_ACTION_BACK ||
                action==RYZ_V5_BLE_ACTION_INFO || action==RYZ_V5_BLE_ACTION_DONE);
        }
    }
    passed("eight closure states / real fact predicates / all-pixel unavailable and pending write gates");
}

static void closure_unknowns(void)
{
    const ryz_v5_ble_state_t states[]={RYZ_V5_BLE_STATE_SAVED_WAITING,RYZ_V5_BLE_STATE_LINKED_SERVICE_WAIT,
        RYZ_V5_BLE_STATE_PAIR_TIMEOUT,RYZ_V5_BLE_STATE_PAIR_FAILED,RYZ_V5_BLE_STATE_PAIRING_AFTER_REPLACE,
        RYZ_V5_BLE_STATE_FORGET_FAILED,RYZ_V5_BLE_STATE_SERVICE_FAILED};
    for(unsigned i=0;i<sizeof(states)/sizeof(states[0]);++i) {
        ryz_v5_ble_model_t m=closure_demo(states[i]);
        m.enabled=false; m.bond_known=false; m.bonded=false;
        lv_obj_t *root=render(&m,NULL);
        assert(label(root,"STATE UNKNOWN") && !label(root,"NO DEVICE SAVED") && !label(root,"BOND STILL SAVED"));
        assert(ryz_v5_ble_hit(&m,170,190)==(states[i]==RYZ_V5_BLE_STATE_SERVICE_FAILED?
            RYZ_V5_BLE_ACTION_INFO:RYZ_V5_BLE_ACTION_NONE));
    }
    ryz_v5_ble_model_t m=closure_demo(RYZ_V5_BLE_STATE_FORGETTING);
    m.checking_state=true;
    lv_obj_t *root=render(&m,"v5_ble_forget_outcome_unknown");
    assert(label(root,"CHECKING SAVED STATE") && label(root,"CHECKING...") && label(root,"STATE UNKNOWN"));
    assert(!label(root,"FORGETTING DEVICE") && !label(root,"BOND STILL SAVED"));
    assert(ryz_v5_ble_hit(&m,20,16)==RYZ_V5_BLE_ACTION_NONE);
    /* Unknown accepted outcome wins even if a stale failure enum remains. */
    m.state=RYZ_V5_BLE_STATE_FORGET_FAILED;
    root=render(&m,NULL);
    assert(label(root,"CHECKING...") && !label(root,"BACK") && !label(root,"RETRY"));
    assert(ryz_v5_ble_hit(&m,20,16)==RYZ_V5_BLE_ACTION_NONE);
    m.page=RYZ_V5_BLE_INFO;
    assert(ryz_v5_ble_hit(&m,100,200)==RYZ_V5_BLE_ACTION_NONE && ryz_v5_ble_hit(&m,20,16)==RYZ_V5_BLE_ACTION_NONE);
    m.linked=m.service_ready=true;
    root=render(&m,NULL);
    assert(!label(root,"SAVED") && !label(root,"NONE") && !label(root,"READY") && !label(root,"LINKED"));
    m.page=RYZ_V5_BLE_FORGET; m.state=RYZ_V5_BLE_STATE_LEGACY;
    root=render(&m,NULL);
    assert(label(root,"CHECKING...") && !label(root,"BACK") && !label(root,"RETRY"));
    m=closure_demo(RYZ_V5_BLE_STATE_FORGETTING);
    memset(m.peer_name,'W',sizeof(m.peer_name)-1); m.peer_name[sizeof(m.peer_name)-1]=0;
    root=render(&m,"v5_ble_forget_long_peer");
    lv_obj_t *last=label_object(root,"Please wait for the result.");
    /* The static Noto glyph box may exceed the 18 px baseline pitch. Check
     * the actual Figma body viewport bottom (96 + 72), not that pitch. */
    assert(last && lv_obj_get_y(last)+lv_obj_get_height(last)<=168);
    assert(label(root,"and removing its pairing.") && !label(root,m.peer_name));
    bool elided=false;
    for(uint32_t i=0;i<lv_obj_get_child_count(root);++i) {
        lv_obj_t *child=lv_obj_get_child(root,(int32_t)i);
        if(lv_obj_check_type(child,&lv_label_class) && !strncmp(lv_label_get_text(child),"Disconnecting ",14)) {
            assert(lv_label_get_long_mode(child)==LV_LABEL_LONG_DOT);
            assert(strlen(lv_label_get_text(child))<14+strlen(m.peer_name));
            elided=true;
        }
    }
    assert(elided);
    m=closure_demo(RYZ_V5_BLE_STATE_PAIR_FAILED); m.failure=RYZ_V5_BLE_FAILURE_START; m.error_code[0]=0;
    root=render(&m,"v5_ble_pair_start_failure");
    assert(label(root,"Pairing could not start.\nCheck Bluetooth and\ntry again.") && label(root,"--"));
    assert(!label(root,"E_AUTH") && !label(root,"Phone / PC rejected\npairing. Check the app\nand try again."));
    m.failure=RYZ_V5_BLE_FAILURE_SAVE; root=render(&m,"v5_ble_pair_save_failure");
    assert(label(root,"Could not save pairing.\nCheck the device and\ntry again."));
    m.failure=RYZ_V5_BLE_FAILURE_UNKNOWN; root=render(&m,NULL);
    assert(label(root,"Pairing did not finish.\nCheck the phone / PC\napp and try again."));
    m=closure_demo(RYZ_V5_BLE_STATE_PAIRING_AFTER_REPLACE); m.remaining_valid=false;
    root=render(&m,NULL); assert(label(root,"--:--") && !label(root,"00:30"));
    passed("closure unknowns keep no bond/linked/saved claim; failure source and countdown remain explicit");
}

static void numeric_comparison(void)
{
    ryz_v5_ble_model_t saved=closure_demo(RYZ_V5_BLE_STATE_SAVED_WAITING);
    saved.page=RYZ_V5_BLE_REPLACE;
    lv_obj_t *warning_root=render(&saved,NULL);
    assert(label(warning_root,"REPLACE BOND?"));
    assert(ryz_v5_ble_hit(&saved,150,185)==RYZ_V5_BLE_ACTION_CONFIRM_REPLACE);
    saved.page=RYZ_V5_BLE_FORGET;
    assert(ryz_v5_ble_hit(&saved,150,185)==RYZ_V5_BLE_ACTION_CONFIRM_FORGET);
    ryz_v5_ble_model_t m=demo(RYZ_V5_BLE_PAIRING);
    m.state=RYZ_V5_BLE_STATE_VERIFY_CODE; m.operation_id=91;
    m.compare_pending=true; m.compare_value=42719; m.remaining_seconds=18;
    lv_obj_t *root=render(&m,"v5_ble_verify_code_demo");
    assert(label(root,"042719") && label(root,"VERIFY CODE") && label(root,"00:18"));
    assert(label(root,"Match this code on\nboth devices.") && label(root,"ONLY IF CODES MATCH"));
    assert(ryz_v5_ble_hit(&m,50,190)==RYZ_V5_BLE_ACTION_REJECT_CODE);
    assert(ryz_v5_ble_hit(&m,150,190)==RYZ_V5_BLE_ACTION_CONFIRM_CODE);
    assert(ryz_v5_ble_hit(&m,20,16)==RYZ_V5_BLE_ACTION_CANCEL_PAIR);
    m.compare_value=0; root=render(&m,NULL); assert(label(root,"000000"));
    m.compare_value=999999; root=render(&m,NULL); assert(label(root,"999999"));
    m.compare_value=1000000;
    assert(ryz_v5_ble_hit(&m,150,190)==RYZ_V5_BLE_ACTION_NONE);
    m.compare_value=42719; m.remaining_seconds=0;
    assert(ryz_v5_ble_hit(&m,150,190)==RYZ_V5_BLE_ACTION_NONE);
    m.remaining_seconds=18; m.compare_pending=false;
    assert(ryz_v5_ble_hit(&m,150,190)==RYZ_V5_BLE_ACTION_NONE);
    m.compare_pending=true; m.operation_id=0;
    assert(ryz_v5_ble_hit(&m,150,190)==RYZ_V5_BLE_ACTION_NONE);
    m.operation_id=91; m.unavailable=true; root=render(&m,NULL);
    assert(!label(root,"042719") && !label(root,"CONFIRM"));
    m.unavailable=false;
    for(int i=0;i<2;++i) {
        m.state=i?RYZ_V5_BLE_STATE_STOPPING:RYZ_V5_BLE_STATE_COMMITTING;
        m.checking_state=true;
        root=render(&m,i?"v5_ble_stopping_demo":"v5_ble_committing_demo");
        assert(label(root,i?"STOPPING...":"SAVING..."));
        for(int y=0;y<240;++y) for(int x=0;x<240;++x)
            assert(ryz_v5_ble_hit(&m,x,y)==RYZ_V5_BLE_ACTION_NONE);
    }
    ryz_v5_ble_bind_services(NULL);
    assert(ryz_v5_ble_submit(RYZ_V5_BLE_ACTION_CONFIRM_CODE,91,42719)==ESP_ERR_INVALID_STATE);
    passed("six-digit comparison / leading zeros / stale predicates / commit-stop action lock");
}

int main(int argc,char **argv)
{
    assert(argc==1 || argc==2);
    output=argc==2?argv[1]:NULL;
    hits(); unavailable(); unavailable_diagnostics(); pixels(); unknowns(); closure_states(); closure_unknowns(); numeric_comparison(); resources();
    printf("V5_BLE_UI_PASS %u groups\n",groups);
    return 0;
}
