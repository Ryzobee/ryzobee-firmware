/* Actual V5 Version view + locked LVGL/FreeType + exported Figma assets.
 * All version/update values are explicit DEMO fixtures, not OTA service or
 * device evidence. No network, download, reboot or firmware writes occur. */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "ryz_v5_version.h"
#include "ryz_v5_widgets.h"
#include "ryz_v5_assets.h"
#include "ryz_lvgl_system.h"
#include "ryz_display_host.h"
#include "ryz_pixels_host.h"

#ifdef NDEBUG
#error "Version UI assertions must remain enabled"
#endif

static unsigned groups;
static const char *output;
static const char device_id[]="0123456789abcdef0123456789abcdef"
                              "fedcba9876543210fedcba9876543210";

static void passed(const char *name) { ++groups; printf("PASS %s\n",name); }
static uint16_t rgb565(uint32_t rgb)
{ return (uint16_t)(((rgb>>8)&0xf800)|((rgb>>5)&0x07e0)|((rgb>>3)&0x001f)); }

static ryz_v5_version_model_t demo(ryz_v5_version_page_t page)
{
    ryz_v5_version_model_t value={.page=page,.check_ready=true,.checked=true,.up_to_date=true,
        .healthy_known=true,.healthy=true,.wifi_ready=true,.power_known=true,.power_ready=true,
        .ab_slots=true,.update_ready=true,.cancel_ready=true,.restart_ready=true,.retry_ready=true,
        .attempt_id=17,.image_staged=page==RYZ_V5_VERSION_REBOOT,
        .downloaded_bytes=2800,.total_bytes=4100,.error=ESP_FAIL};
    strcpy(value.running,"0.8.0");
    strcpy(value.candidate,page==RYZ_V5_VERSION_CURRENT?"0.8.0":"0.9.0");
    strcpy(value.updated,"DEMO DATE"); strcpy(value.channel,"DEMO CHANNEL");
    strcpy(value.build,"DEMO BUILD"); strcpy(value.idf,"DEMO IDF");
    strcpy(value.slot,"DEMO OTA_0 VALID"); strcpy(value.uptime,"02:18:43");
    strcpy(value.chip,"DEMO ESP32-S3");
    assert(strlen(device_id)==64);
    strcpy(value.device_id,device_id);
    strcpy(value.failure_title,"CONNECTION LOST");
    strcpy(value.failure_body,"DEMO FAILURE: the current image was not changed. Check Wi-Fi and retry.");
    return value;
}

static lv_obj_t *find_label(lv_obj_t *root,const char *text)
{
    for (uint32_t i=0;i<lv_obj_get_child_count(root);++i) {
        lv_obj_t *child=lv_obj_get_child(root,(int32_t)i);
        if (lv_obj_check_type(child,&lv_label_class) && !strcmp(lv_label_get_text(child),text)) return child;
        lv_obj_t *found=find_label(child,text);
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

static lv_obj_t *render(const ryz_v5_version_model_t *model,const char *name)
{
    const ryz_v5_version_model_t before=*model;
    lv_obj_t *root=NULL;
    assert(ryz_lvgl_system_create(&root)==ESP_OK);
    ryz_v5_widgets_begin();
    assert(ryz_v5_version_draw(root,model)==ESP_OK);
    assert(memcmp(&before,model,sizeof(before))==0);
    size_t shows=ryz_host_display_shows();
    assert(ryz_lvgl_system_commit(root,NULL,NULL,NULL)==ESP_OK);
    assert(ryz_v5_widgets_status()==ESP_OK && ryz_host_display_shows()>shows);
    check_tree(root);
    if (name && output) ryz_host_display_save(output,name);
    return root;
}

static void actions(void)
{
    const struct {ryz_v5_version_page_t page; int x,y; ryz_v5_version_action_t action;} entries[]={
        {RYZ_V5_VERSION_CURRENT,40,184,RYZ_V5_VERSION_ACTION_CHECK},
        {RYZ_V5_VERSION_AVAILABLE,90,188,RYZ_V5_VERSION_ACTION_UPDATE},
        {RYZ_V5_VERSION_DOWNLOADING,50,188,RYZ_V5_VERSION_ACTION_CANCEL},
        {RYZ_V5_VERSION_REBOOT,90,188,RYZ_V5_VERSION_ACTION_RESTART},
        {RYZ_V5_VERSION_FAILED,90,178,RYZ_V5_VERSION_ACTION_RETRY}};
    assert(ryz_v5_version_hit(NULL,0,0)==RYZ_V5_VERSION_ACTION_NONE);
    for (size_t i=0;i<sizeof(entries)/sizeof(entries[0]);++i) {
        ryz_v5_version_model_t value=demo(entries[i].page);
        assert(ryz_v5_version_hit(&value,entries[i].x,entries[i].y)==entries[i].action);
        value.check_ready=value.update_ready=value.cancel_ready=value.restart_ready=value.retry_ready=false;
        size_t shows=ryz_host_display_shows(), blocks=ryz_host_heap_blocks();
        for (uint16_t y=0;y<240;++y) for (uint16_t x=0;x<240;++x) {
            ryz_v5_version_action_t action=ryz_v5_version_hit(&value,x,y);
            assert(action==RYZ_V5_VERSION_ACTION_NONE || action==RYZ_V5_VERSION_ACTION_INFO ||
                   action==RYZ_V5_VERSION_ACTION_LATER);
        }
        assert(ryz_host_display_shows()==shows && ryz_host_heap_blocks()==blocks);
        assert(ryz_v5_version_hit(&value,240,0)==RYZ_V5_VERSION_ACTION_NONE);
        assert(ryz_v5_version_hit(&value,0,240)==RYZ_V5_VERSION_ACTION_NONE);
    }
    ryz_v5_version_model_t value=demo(RYZ_V5_VERSION_INFO);
    assert(ryz_v5_version_hit(&value,32,194)==RYZ_V5_VERSION_ACTION_DEVICE_ID);
    assert(ryz_v5_version_hit(&value,208,235)==RYZ_V5_VERSION_ACTION_DEVICE_ID);
    assert(ryz_v5_version_hit(&value,208,193)==RYZ_V5_VERSION_ACTION_NONE);
    value.device_id[0]=0;
    assert(ryz_v5_version_hit(&value,32,194)==RYZ_V5_VERSION_ACTION_NONE);
    passed("six-page pure hit testing / disabled native actions across 288000 coordinates");
}

static void pages(void)
{
    assert(ryz_font_init()==ESP_OK);
    assert(ryz_v5_version_draw(NULL,NULL)==ESP_ERR_INVALID_ARG);
    for (int p=RYZ_V5_VERSION_CURRENT;p<=RYZ_V5_VERSION_FAILED;++p) {
        ryz_v5_version_model_t value=demo((ryz_v5_version_page_t)p);
        assert(!strcmp(ryz_v5_version_title(value.page),p<=RYZ_V5_VERSION_INFO?"VERSION":"UPDATE"));
        char name[48]; snprintf(name,sizeof(name),"v5_version_v%d_demo",p+1);
        lv_obj_t *root=render(&value,name);
        assert(ryz_host_display_pixel(0,216)==rgb565(p==RYZ_V5_VERSION_INFO?V5_BLACK:V5_BORDER));
        if (p==RYZ_V5_VERSION_CURRENT) assert(find_label(root,"V0.8.0") &&
            find_label(root,"LATEST V0.8.0") && find_label(root,"UP TO DATE"));
        if (p==RYZ_V5_VERSION_INFO) assert(find_label(root,"V0.8.0") &&
            find_label(root,"HEALTHY") && find_label(root,"DEMO BUILD"));
        if (p==RYZ_V5_VERSION_AVAILABLE) {
            assert(find_label(root,"WIFI OK") && find_label(root,"POWER OK"));
            assert(find_label(root,"V0.9.0") && find_label(root,"UI SAMPLE VERSION"));
            assert(find_label(root,"Wi-Fi required · keep USB power connected · device restarts after confirmation."));
            assert(ryz_host_display_pixel(90,190)==rgb565(V5_WARNING));
        }
        if (p==RYZ_V5_VERSION_DOWNLOADING) assert(find_label(root,"68%"));
        if (p==RYZ_V5_VERSION_REBOOT) assert(image_at(root,&ryz_v5_asset_version_ready,105,71));
        if (p==RYZ_V5_VERSION_FAILED) assert(ryz_host_display_pixel(8,80)==rgb565(V5_RED));
    }
    passed("six original 240x240 body frames / exact assets / actual RGB565 pixels");
}

static void unavailable(void)
{
    ryz_v5_version_model_t value={.page=RYZ_V5_VERSION_CURRENT};
    strcpy(value.running,"0.7.0");
    lv_obj_t *root=render(&value,"v5_version_no_source");
    assert(find_label(root,"NOT CHECKED") && find_label(root,"UPDATE SOURCE --"));
    assert(!find_label(root,"UP TO DATE") && !find_label(root,"0.9.0"));
    assert(ryz_host_display_pixel(30,190)==rgb565(V5_SELECTED));
    assert(ryz_v5_version_hit(&value,50,190)==RYZ_V5_VERSION_ACTION_NONE);
    value.page=RYZ_V5_VERSION_INFO;
    root=render(&value,NULL);
    assert(!find_label(root,"HEALTHY") && !find_label(root,"PENDING"));
    value.page=RYZ_V5_VERSION_AVAILABLE;
    root=render(&value,NULL);
    assert(find_label(root,"WIFI --") && find_label(root,"POWER --") && find_label(root,"SLOT --"));
    assert(!find_label(root,"POWER OK"));
    value.page=RYZ_V5_VERSION_FAILED;
    root=render(&value,"v5_version_failure_no_source");
    assert(find_label(root,"UPDATE UNAVAILABLE"));
    assert(find_label(root,"No update source is configured. No download was started."));
    assert(ryz_v5_version_hit(&value,90,180)==RYZ_V5_VERSION_ACTION_NONE);
    passed("no source has no fake latest/healthy/power or enabled update actions");
}

static void complete_id(void)
{
    ryz_v5_version_model_t value=demo(RYZ_V5_VERSION_INFO);
    lv_obj_t *root=render(&value,NULL);
    assert(find_label(root,"SHOW DEVICE ID") && !find_label(root,device_id));
    value.show_device_id=true;
    root=render(&value,"v5_version_complete_id");
    lv_obj_t *id=find_label(root,device_id);
    assert(id && !find_label(root,"SHOW DEVICE ID"));
    assert(lv_obj_get_x(id)==12 && lv_obj_get_width(id)==216);
    assert(strlen(lv_label_get_text(id))==64);
    assert(lv_label_get_long_mode(id)==LV_LABEL_LONG_SCROLL_CIRCULAR);
    assert(lv_obj_get_style_text_font(id,0)->line_height>=12);
    value.show_device_id=false;
    root=render(&value,NULL);
    assert(!find_label(root,device_id) && find_label(root,"SHOW DEVICE ID"));
    passed("complete 64-byte ID is hidden by default and readable in a scrolling 12px cell");
}

static uint64_t footer_pixels(void)
{
    uint64_t hash=UINT64_C(1469598103934665603);
    for(unsigned y=188;y<240;++y) for(unsigned x=0;x<240;++x) {
        hash^=ryz_host_display_pixel(x,y);
        hash*=UINT64_C(1099511628211);
    }
    return hash;
}

static void available_description_scroll(void)
{
    static const char safety[]=
        "Wi-Fi required · keep USB power connected · device restarts after confirmation.";
    static const char description[]=
        "DEMO: Wi-Fi is required.\nKeep USB power connected.\nFinish all running scripts.\n"
        "Save your current files.\nRestart follows confirmation.";
    ryz_v5_version_model_t value=demo(RYZ_V5_VERSION_AVAILABLE);
    assert(sizeof(description)<=sizeof(value.update_description));
    strcpy(value.update_description,description);
    const int height=ryz_v5_version_available_height(&value);
    assert(height>48);
    lv_obj_t *root=render(&value,"v5_version_available_long_top");
    lv_obj_t *body=find_label(root,description);
    assert(body && lv_label_get_long_mode(body)==LV_LABEL_LONG_WRAP);
    assert(lv_obj_get_y(body)==0 && lv_obj_get_width(body)==192 && lv_obj_get_height(body)==height);
    assert(!find_label(root,safety));
    lv_obj_t *viewport=lv_obj_get_parent(body);
    assert(lv_obj_get_x(viewport)==24 && lv_obj_get_y(viewport)==118);
    assert(lv_obj_get_width(viewport)==192 && lv_obj_get_height(viewport)==48);
    assert(!lv_obj_has_flag(viewport,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_OVERFLOW_VISIBLE));
    const uint64_t footer=footer_pixels();
    const uint16_t badge=ryz_host_display_pixel(20,170);
    value.info_scroll=INT_MAX;
    root=render(&value,"v5_version_available_long_bottom");
    body=find_label(root,description);
    assert(body && lv_obj_get_y(body)==48-height);
    lv_area_t bounds;
    lv_obj_get_coords(body,&bounds);
    assert(bounds.y2==165);
    assert(footer_pixels()==footer && ryz_host_display_pixel(20,170)==badge);
    assert(ryz_v5_version_hit(&value,48,202)==RYZ_V5_VERSION_ACTION_LATER);
    assert(ryz_v5_version_hit(&value,160,202)==RYZ_V5_VERSION_ACTION_UPDATE);
    for(uint16_t y=118;y<166;++y) for(uint16_t x=24;x<216;++x)
        assert(ryz_v5_version_hit(&value,x,y)==RYZ_V5_VERSION_ACTION_NONE);
    value.info_scroll=INT_MIN;
    root=render(&value,NULL);
    assert(lv_obj_get_y(find_label(root,description))==0);
    strcpy(value.update_description,"DEMO: Short safety text.");
    value.info_scroll=INT_MAX;
    assert(ryz_v5_version_available_height(&value)==48);
    root=render(&value,"v5_version_available_short");
    body=find_label(root,value.update_description);
    assert(body && lv_obj_get_y(body)==0 && lv_obj_get_height(body)==48);
    assert(ryz_host_display_pixel(218,118)==rgb565(V5_BLACK));
    /* An unterminated Owner snapshot must never read beyond its fixed field. */
    memset(value.update_description,'X',sizeof(value.update_description));
    root=render(&value,"v5_version_available_invalid_description");
    assert(find_label(root,safety));
    value.update_description[0]=0;
    root=render(&value,NULL);
    assert(find_label(root,safety));
    passed("AVAILABLE bounded static-font description wraps and clamps in 192x48; actions stay fixed");
}

static void progress(void)
{
    ryz_v5_version_model_t value=demo(RYZ_V5_VERSION_DOWNLOADING);
    value.total_bytes=0;
    lv_obj_t *root=render(&value,"v5_version_total_unknown");
    assert(find_label(root,"--%") && !find_label(root,"100%"));
    assert(ryz_host_display_pixel(120,138)==rgb565(V5_SELECTED));
    value.downloaded_bytes=200; value.total_bytes=100;
    root=render(&value,NULL);
    assert(find_label(root,"100%") && ryz_host_display_pixel(210,138)==rgb565(V5_ORANGE));
    value.downloaded_bytes=UINT32_C(42949673); value.total_bytes=1;
    root=render(&value,"v5_version_progress_saturated");
    assert(find_label(root,"100%")); /* Clamp the uint64 ratio before narrowing. */
    assert(ryz_host_display_pixel(210,138)==rgb565(V5_ORANGE));
    value=demo(RYZ_V5_VERSION_DOWNLOADING); value.verifying=true; value.cancel_ready=false;
    root=render(&value,NULL);
    assert(find_label(root,"VERIFYING"));
    assert(ryz_v5_version_hit(&value,50,190)==RYZ_V5_VERSION_ACTION_NONE);
    value.verifying=false; value.cancelling=true;
    root=render(&value,NULL);
    assert(find_label(root,"CANCELLING"));
    passed("unknown total / bounded uint64 saturation / verifying and cancelling are explicit");
}

static void restart_requires_staged_image(void)
{
    ryz_v5_version_model_t value=demo(RYZ_V5_VERSION_REBOOT);
    value.image_staged=false;
    lv_obj_t *root=render(&value,"v5_version_restart_not_ready");
    assert(find_label(root,"IMAGE NOT READY") && find_label(root,"NOT READY"));
    assert(!find_label(root,"IMAGE VERIFIED"));
    assert(!image_at(root,&ryz_v5_asset_version_ready,105,71));
    assert(ryz_v5_version_hit(&value,160,202)==RYZ_V5_VERSION_ACTION_NONE);
    assert(ryz_v5_version_hit(&value,40,202)==RYZ_V5_VERSION_ACTION_LATER);
    passed("REBOOT without a staged image never claims verification or enables restart");
}

static void label_fits(lv_obj_t *label,int width,int height)
{
    assert(label);
    lv_point_t extent;
    lv_text_get_size(&extent,lv_label_get_text(label),lv_obj_get_style_text_font(label,0),
        lv_obj_get_style_text_letter_space(label,0),lv_obj_get_style_text_line_space(label,0),
        width,LV_TEXT_FLAG_NONE);
    if(extent.x>width || extent.y>height)
        fprintf(stderr,"clipped restart text %s: %ldx%ld in %dx%d\n",lv_label_get_text(label),
                (long)extent.x,(long)extent.y,width,height);
    assert(extent.x<=width && extent.y<=height);
}

static void restart_geometry(lv_obj_t *root,const char *body,const char *foot,bool enabled)
{
    lv_obj_t *paragraph=find_label(root,body), *title=find_label(root,"READY TO REBOOT");
    assert(paragraph && lv_obj_get_x(paragraph)==28 && lv_obj_get_y(paragraph)==146);
    assert(lv_obj_get_width(paragraph)==184 && lv_obj_get_height(paragraph)==34);
    assert(lv_label_get_long_mode(paragraph)==LV_LABEL_LONG_WRAP);
    label_fits(paragraph,184,34);
    assert(title && lv_color_eq(lv_obj_get_style_text_color(title,0),lv_color_hex(V5_WHITE)));
    label_fits(title,192,lv_obj_get_height(title));
    label_fits(find_label(root,foot),142,24);
    bool original_button=false;
    for(uint32_t i=0;i<lv_obj_get_child_count(root);++i) {
        lv_obj_t *child=lv_obj_get_child(root,(int32_t)i);
        if(lv_obj_check_type(child,&lv_obj_class) && lv_obj_get_x(child)==88 &&
           lv_obj_get_y(child)==188 && lv_obj_get_width(child)==144 && lv_obj_get_height(child)==28)
            original_button=true;
    }
    assert(original_button);
    assert(ryz_host_display_pixel(90,190)==rgb565(enabled?V5_ORANGE:V5_SELECTED));
    assert(find_label(root,"IMAGE VERIFIED") && !find_label(root,"UPDATE FAILED"));
    assert(image_at(root,&ryz_v5_asset_version_ready,105,71));
    unsigned green=0;
    for(unsigned y=71;y<106;++y) for(unsigned x=105;x<136;++x)
        if(ryz_host_display_pixel(x,y)==rgb565(V5_GREEN)) ++green;
    assert(green>0);
}

static void restart_states(void)
{
    const struct {
        esp_err_t error; bool pending; const char *body,*foot,*image;
    } states[]={
        {ESP_OK,false,"The new image is staged. Restart to switch A/B slot.",
            "A/B IMAGE STAGED","v5_version_restart_ready"},
        {ESP_ERR_TIMEOUT,false,"A task or file operation is busy.\nFinish it, then retry.",
            "RESTART BUSY","v5_version_restart_busy"},
        {ESP_FAIL,false,"Restart failed.\nStaged image unchanged.",
            "RESTART FAILED","v5_version_restart_failed"},
        {ESP_ERR_INVALID_STATE,false,"Restart is blocked.\nCheck storage and retry.",
            "RESTART BLOCKED",NULL},
        {ESP_FAIL,true,"Restart is in progress.\nPlease wait.",
            "RESTART PENDING",NULL},
    };
    for(size_t i=0;i<sizeof(states)/sizeof(states[0]);++i) {
        ryz_v5_version_model_t value=demo(RYZ_V5_VERSION_REBOOT);
        value.restart_error=states[i].error;
        value.restart_pending=states[i].pending;
        lv_obj_t *root=render(&value,states[i].image);
        restart_geometry(root,states[i].body,states[i].foot,!states[i].pending);
        assert(ryz_v5_version_hit(&value,160,202)==(states[i].pending?
            RYZ_V5_VERSION_ACTION_NONE:RYZ_V5_VERSION_ACTION_RESTART));
        assert(ryz_v5_version_hit(&value,40,202)==RYZ_V5_VERSION_ACTION_LATER);
    }
    ryz_v5_version_model_t value=demo(RYZ_V5_VERSION_REBOOT);
    value.ab_slots=false;
    lv_obj_t *root=render(&value,NULL);
    assert(find_label(root,"IMAGE STAGED") && !find_label(root,"A/B IMAGE STAGED"));
    for(unsigned combination=0;combination<16;++combination) {
        value.image_staged=(combination&1U)!=0;
        value.restart_ready=(combination&2U)!=0;
        value.restart_pending=(combination&4U)!=0;
        value.attempt_id=(combination&8U)?17:0;
        assert(ryz_v5_version_hit(&value,160,202)==(combination==11?
            RYZ_V5_VERSION_ACTION_RESTART:RYZ_V5_VERSION_ACTION_NONE));
    }
    passed("staged restart errors/pending retain verified image; real fonts fit original paragraph/button geometry");
}

static void resources(void)
{
    assert(ryz_lvgl_system_release()==ESP_OK); ryz_v5_widgets_release();
    const size_t bytes=ryz_host_heap_bytes(), blocks=ryz_host_heap_blocks();
    for (unsigned loop=0;loop<3;++loop) {
        for (int page=RYZ_V5_VERSION_CURRENT;page<=RYZ_V5_VERSION_FAILED;++page) {
            ryz_v5_version_model_t value=demo((ryz_v5_version_page_t)page);
            (void)render(&value,NULL);
        }
        assert(ryz_lvgl_system_release()==ESP_OK); ryz_v5_widgets_release();
        assert(ryz_host_heap_bytes()==bytes && ryz_host_heap_blocks()==blocks);
    }
    passed("18 page switches release fonts/roots without accumulating Host heap blocks");
}

int main(int argc,char **argv)
{
    assert(argc==1 || argc==2);
    output=argc==2?argv[1]:NULL;
    actions(); pages(); unavailable(); complete_id(); available_description_scroll(); progress();
    restart_requires_staged_image(); restart_states(); resources();
    printf("V5_VERSION_UI_PASS %u groups\n",groups);
    return 0;
}
