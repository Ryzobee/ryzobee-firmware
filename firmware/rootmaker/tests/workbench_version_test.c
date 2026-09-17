/* Actual Workbench formatter + original V5 Version view + locked LVGL/FreeType.
 * Partition/chip data are explicit DEMO inputs, not SDK or device evidence. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "workbench_version.h"
#include "ryz_v5_version.h"
#include "ryz_v5_widgets.h"
#include "ryz_v5_status.h"
#include "ryz_v5_render.h"
#include "ryz_display_host.h"
#include "ryz_pixels_host.h"

#ifdef NDEBUG
#error "Workbench Version checks require active assertions"
#endif

static unsigned groups;
static const char *output;

static void passed(const char *name)
{ ++groups; printf("PASS %s\n",name); }

static ryz_v5_version_model_t demo(void)
{
    ryz_v5_version_model_t view={.page=RYZ_V5_VERSION_INFO,
        .healthy_known=true,.healthy=false,.restart_ready=true,.attempt_id=17,
        .image_staged=true,.restart_error=ESP_ERR_TIMEOUT,.error=ESP_FAIL};
    strcpy(view.running,"0.8.0");
    strcpy(view.candidate,"0.9.0");
    strcpy(view.build,"DEMO INPUT");
    strcpy(view.idf,"DEMO IDF");
    return view;
}

static void only_hardware_fields(const ryz_v5_version_model_t *before,
                                const ryz_v5_version_model_t *after)
{
    ryz_v5_version_model_t unchanged;
    memcpy(&unchanged,after,sizeof(unchanged));
    memcpy(unchanged.slot,before->slot,sizeof(unchanged.slot));
    memcpy(unchanged.chip,before->chip,sizeof(unchanged.chip));
    unchanged.ab_slots=before->ab_slots;
    assert(!memcmp(&unchanged,before,sizeof(unchanged)));
}

static void maps_actual_identity(void)
{
    ryz_v5_version_model_t view=demo(), before=view;
    ryz_ota_running_info_t info={.partition_known=true,
        .running_subtype=0x11,.state=RYZ_OTA_IMAGE_STATE_VALID,.state_known=true,
        .ab_slots_known=true,.ab_slots=true};
    strcpy(info.running_label,"ota_1");
    ryz_workbench_version_hardware(&view,&info,true,101);
    assert(!strcmp(view.slot,"ota_1 / VALID"));
    assert(!strcmp(view.chip,"ESP32-S3 REV 1.1") && view.ab_slots);
    only_hardware_fields(&before,&view);
    assert(view.healthy_known && !view.healthy); /* VALID does not imply system health. */
    assert(!view.updated[0] && !view.channel[0]);
    passed("actual partition case/state and chip revision map without inventing health or release metadata");
}

static void expect_slot(ryz_ota_running_info_t *info,const char *expected)
{
    ryz_v5_version_model_t view=demo(), before=view;
    ryz_ota_running_info_t source;
    memcpy(&source,info,sizeof(source));
    strcpy(view.slot,"stale / VALID");
    before=view;
    ryz_workbench_version_hardware(&view,info,false,0);
    assert(!strcmp(view.slot,expected) && !view.chip[0]);
    assert(!memcmp(info,&source,sizeof(source)));
    assert(view.ab_slots==(info->ab_slots_known && info->ab_slots));
    only_hardware_fields(&before,&view);
}

static void states_and_availability(void)
{
    const struct { ryz_ota_image_state_t state; const char *text; } states[]={
        {RYZ_OTA_IMAGE_STATE_NEW,"demo / NEW"},
        {RYZ_OTA_IMAGE_STATE_PENDING_VERIFY,"demo / PENDING"},
        {RYZ_OTA_IMAGE_STATE_VALID,"demo / VALID"},
        {RYZ_OTA_IMAGE_STATE_INVALID,"demo / INVALID"},
        {RYZ_OTA_IMAGE_STATE_ABORTED,"demo / ABORTED"},
        {RYZ_OTA_IMAGE_STATE_UNDEFINED,"demo / UNDEFINED"},
    };
    ryz_ota_running_info_t info={.partition_known=true,.state_known=true};
    strcpy(info.running_label,"demo");
    for(size_t i=0;i<sizeof(states)/sizeof(states[0]);++i) {
        info.state=states[i].state;
        expect_slot(&info,states[i].text);
    }
    info.state=(ryz_ota_image_state_t)999;
    expect_slot(&info,"demo / --");
    info.state=RYZ_OTA_IMAGE_STATE_VALID;
    info.state_known=false;
    expect_slot(&info,"demo / --");
    info.state_known=true;
    info.state_error=ESP_ERR_NOT_SUPPORTED;
    expect_slot(&info,"demo / --");
    info.state_error=ESP_FAIL;
    expect_slot(&info,"demo / --");
    info.partition_known=false;
    expect_slot(&info,"");
    for(unsigned bits=0;bits<4;++bits) {
        info.ab_slots_known=(bits&1U)!=0;
        info.ab_slots=(bits&2U)!=0;
        expect_slot(&info,""); /* Topology and running-partition observations are independent. */
    }
    ryz_v5_version_model_t view=demo();
    strcpy(view.slot,"old / VALID"); view.ab_slots=true;
    const ryz_v5_version_model_t before=view;
    ryz_workbench_version_hardware(&view,NULL,true,300);
    assert(!view.slot[0] && !view.ab_slots && !strcmp(view.chip,"ESP32-S3 REV 3.0"));
    only_hardware_fields(&before,&view);
    ryz_workbench_version_hardware(NULL,&info,true,300);
    passed("all image states / unsupported and unknown records / revoked partition / independent A-B facts");
}

static void labels_and_fallbacks(void)
{
    ryz_ota_running_info_t info={.partition_known=true,.running_subtype=0x10};
    strcpy(info.running_label,"MiXeD DATA");
    expect_slot(&info,"MiXeD DATA / --");
    strcpy(info.running_label," A ");
    expect_slot(&info," A  / --"); /* No invented trim policy. */
    strcpy(info.running_label,"DEMO_partition01");
    assert(strlen(info.running_label)==16);
    info.state_known=true; info.state=RYZ_OTA_IMAGE_STATE_UNDEFINED;
    expect_slot(&info,"DEMO_partition01 / UNDEFINED");
    info.state_known=false;
    for(unsigned invalid=0;invalid<5;++invalid) {
        memset(info.running_label,0,sizeof(info.running_label));
        if(invalid==0) memset(info.running_label,'x',sizeof(info.running_label));
        if(invalid==1) strcpy(info.running_label,"bad\nlabel");
        if(invalid==2) { strcpy(info.running_label,"bad"); info.running_label[2]=0x7f; }
        if(invalid==3) { strcpy(info.running_label,"bad"); info.running_label[2]=(char)0x80; }
        /* invalid==4 is the empty label. */
        expect_slot(&info,"OTA_0 / --");
    }
    memcpy(info.running_label,"ok\0\n",4);
    expect_slot(&info,"ok / --"); /* Bytes after a valid terminator are not text. */
    info.running_label[0]=0;
    const char *ota[]={"OTA_0 / --","OTA_1 / --","OTA_2 / --","OTA_3 / --",
        "OTA_4 / --","OTA_5 / --","OTA_6 / --","OTA_7 / --","OTA_8 / --","OTA_9 / --",
        "OTA_10 / --","OTA_11 / --","OTA_12 / --","OTA_13 / --","OTA_14 / --","OTA_15 / --"};
    for(unsigned i=0;i<16;++i) {
        info.running_subtype=(uint8_t)(0x10U+i);
        expect_slot(&info,ota[i]);
    }
    info.running_subtype=0; expect_slot(&info,"FACTORY / --");
    info.running_subtype=0x20; expect_slot(&info,"TEST / --");
    info.running_subtype=1; expect_slot(&info,"APP / --");
    info.running_subtype=0xff; expect_slot(&info,"APP / --");
    passed("complete 16-byte printable labels preserve case; malformed labels use actual subtype fallback");
}

static void chip_revisions(void)
{
    const struct { uint16_t revision; const char *text; } cases[]={
        {0,"ESP32-S3 REV 0.0"},{99,"ESP32-S3 REV 0.99"},
        {100,"ESP32-S3 REV 1.0"},{101,"ESP32-S3 REV 1.1"},
        {309,"ESP32-S3 REV 3.9"},{UINT16_MAX,"ESP32-S3 REV 655.35"},
    };
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        ryz_v5_version_model_t view=demo(), before=view;
        ryz_workbench_version_hardware(&view,NULL,true,cases[i].revision);
        assert(!strcmp(view.chip,cases[i].text));
        assert(memchr(view.chip,0,sizeof(view.chip)) && !view.slot[0] && !view.ab_slots);
        only_hardware_fields(&before,&view);
        ryz_workbench_version_hardware(&view,NULL,false,cases[i].revision);
        assert(!view.chip[0]);
        only_hardware_fields(&before,&view);
    }
    passed("chip MXX revision formatting includes the complete uint16 boundary and clears unsupported models");
}

static lv_obj_t *find_label(lv_obj_t *root,const char *text)
{
    for(uint32_t i=0;i<lv_obj_get_child_count(root);++i) {
        lv_obj_t *child=lv_obj_get_child(root,(int32_t)i);
        if(lv_obj_check_type(child,&lv_label_class) && !strcmp(lv_label_get_text(child),text))
            return child;
        lv_obj_t *found=find_label(child,text);
        if(found) return found;
    }
    return NULL;
}

static void fits(lv_obj_t *label,int width,int row_y,int row_height)
{
    assert(label && lv_obj_get_width(label)==width);
    assert(lv_obj_get_y(label)>=row_y &&
           lv_obj_get_y(label)+lv_obj_get_height(label)<=row_y+row_height);
    lv_point_t extent;
    lv_text_get_size(&extent,lv_label_get_text(label),lv_obj_get_style_text_font(label,0),
        lv_obj_get_style_text_letter_space(label,0),lv_obj_get_style_text_line_space(label,0),
        LV_COORD_MAX,LV_TEXT_FLAG_NONE);
    bool scroll=lv_label_get_long_mode(label)==LV_LABEL_LONG_SCROLL_CIRCULAR;
    if((extent.x>width && !scroll) || extent.y>lv_obj_get_height(label))
        fprintf(stderr,"clipped hardware text %s: %ldx%ld in %dx%ld\n",lv_label_get_text(label),
            (long)extent.x,(long)extent.y,width,(long)lv_obj_get_height(label));
    assert((extent.x<=width || scroll) && extent.y<=lv_obj_get_height(label));
}

static lv_obj_t *render(const ryz_v5_version_model_t *view,const char *name)
{
    ryz_v5_version_model_t before;
    memcpy(&before,view,sizeof(before));
    ryz_system_ui_snapshot_t system={.time_hhmm="18:43"};
    ryz_v5_status_t status={.version=*view};
    (void)ryz_v5_status_set(&status);
    assert(view->page==RYZ_V5_VERSION_INFO || view->page==RYZ_V5_VERSION_CURRENT);
    const ryz_system_ui_route_t route=view->page==RYZ_V5_VERSION_INFO?
        RYZ_SYSTEM_UI_ROUTE_VERSION_INFO:RYZ_SYSTEM_UI_ROUTE_VERSION_CURRENT;
    size_t shows=ryz_host_display_shows();
    assert(ryz_v5_render(route,&system,NULL,NULL,NULL)==ESP_OK);
    assert(ryz_v5_widgets_status()==ESP_OK);
    assert(ryz_host_display_shows()>shows);
    assert(!memcmp(view,&before,sizeof(before)));
    if(output && name) ryz_host_display_save(output,name);
    return lv_screen_active();
}

static void pixels(void)
{
    assert(ryz_font_init()==ESP_OK);
    /* The display keeps its boot-lifetime draw buffer; compare released
     * views only after that persistent infrastructure has been created. */
    ryz_v5_version_model_t warm=demo();
    (void)render(&warm,NULL);
    assert(ryz_v5_release()==ESP_OK);
    const size_t bytes=ryz_host_heap_bytes(),blocks=ryz_host_heap_blocks();
    const struct { const char *label; ryz_ota_image_state_t state; bool known; const char *name; } cases[]={
        {"ota_1",RYZ_OTA_IMAGE_STATE_VALID,true,"v5-hardware-valid-demo-240"},
        {"ota_0",RYZ_OTA_IMAGE_STATE_PENDING_VERIFY,true,"v5-hardware-pending-demo-240"},
        {"factory",RYZ_OTA_IMAGE_STATE_VALID,false,"v5-hardware-unknown-demo-240"},
        {"DEMO_partition01",RYZ_OTA_IMAGE_STATE_UNDEFINED,true,"v5-hardware-long-label-demo-240"},
    };
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        ryz_v5_version_model_t view=demo();
        view.healthy_known=false; view.healthy=false;
        ryz_ota_running_info_t info={.partition_known=true,.state=cases[i].state,
            .state_known=cases[i].known,.state_error=cases[i].known?ESP_OK:ESP_ERR_NOT_SUPPORTED,
            .ab_slots_known=true,.ab_slots=i!=2};
        strcpy(info.running_label,cases[i].label);
        ryz_workbench_version_hardware(&view,&info,true,i==3?UINT16_MAX:101);
        lv_obj_t *root=render(&view,cases[i].name);
        lv_obj_t *slot=find_label(root,view.slot),*chip=find_label(root,view.chip);
        assert(slot && lv_obj_get_x(slot)==68);
        assert(chip && lv_obj_get_x(chip)==0);
        assert(lv_obj_get_width(slot)==152 && lv_label_get_long_mode(slot)==LV_LABEL_LONG_WRAP);
        assert(lv_obj_get_height(slot)>=20);
        fits(chip,224,ryz_v5_version_info_height(&view)-24,24);
        assert(find_label(root,"DEMO INPUT") && !find_label(root,"HEALTHY"));
        assert(!find_label(root,"PENDING")); /* Health is still unknown, even for a VALID OTA record. */
        assert(!view.updated[0] && !view.channel[0]);
        assert(ryz_host_display_pixel(8,93)==(uint16_t)0x39c7); /* First 36px info row separator. */
        assert(ryz_v5_release()==ESP_OK);
        assert(ryz_host_heap_bytes()==bytes && ryz_host_heap_blocks()==blocks);
    }
    ryz_v5_version_model_t view=demo();
    view.page=RYZ_V5_VERSION_CURRENT;
    ryz_ota_running_info_t info={.partition_known=true,.running_subtype=0x10,
        .state_known=true,.state=RYZ_OTA_IMAGE_STATE_VALID};
    ryz_workbench_version_hardware(&view,&info,true,101);
    lv_obj_t *root=render(&view,NULL);
    assert(find_label(root,"UPDATED") && find_label(root,"CHANNEL"));
    unsigned unknown_rows=0;
    for(uint32_t i=0;i<lv_obj_get_child_count(root);++i) {
        lv_obj_t *label=lv_obj_get_child(root,(int32_t)i);
        if(lv_obj_check_type(label,&lv_label_class) && lv_obj_get_x(label)==76 &&
           !strcmp(lv_label_get_text(label),"--")) ++unknown_rows;
    }
    assert(unknown_rows==2 && !find_label(root,"UP TO DATE"));
    assert(ryz_v5_release()==ESP_OK);
    assert(ryz_host_heap_bytes()==bytes && ryz_host_heap_blocks()==blocks);
    passed("real Version Info fonts fit original 152px slot / 142px footer; CURRENT metadata stays unknown");
}

int main(int argc,char **argv)
{
    assert(argc==1 || argc==2);
    output=argc==2?argv[1]:NULL;
    maps_actual_identity(); states_and_availability(); labels_and_fallbacks(); chip_revisions(); pixels();
    printf("WORKBENCH_VERSION_PASS %u groups; DEMO service inputs only\n",groups);
    return 0;
}
