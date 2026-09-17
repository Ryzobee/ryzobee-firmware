/* Actual public Lua entrypoint + factory script + LVGL. Synthetic time,
 * pointer and sensor/frame replies are not physical-device acceptance. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_runtime.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "ryz_display_host.h"
#include "ryz_font.h"
#include "ryz_lvgl.h"

#ifdef NDEBUG
#error "Hardware script fixtures require assertions"
#endif
typedef struct { unsigned at; const char *action, *shot; } step_t;
static const step_t hub_steps[]={{200,"stop","hub"}};
static const step_t manual_steps[]={
    {200,"h1",NULL},{400,"primary","lcd"},{600,"h1",NULL},{800,"fail",NULL},
    {1000,"h1",NULL},{1200,"menu",NULL},{1400,"h1",NULL},{1600,"skip",NULL},
    {1800,"h5",NULL},{2000,"primary","battery"},{2200,"h5",NULL},{2400,"skip",NULL},
    {2600,"stop","manual-results"},
};
static const step_t touch_steps[]={
    {200,"h2",NULL},{400,"drag_g1","touch-empty"},{520,"blocked_primary",NULL},{600,"g1",NULL},
    {800,"g1",NULL},{1000,"g2",NULL},{1200,"g3",NULL},{1400,"g4",NULL},
    {1600,"g5",NULL},{1800,"g6",NULL},{2000,"g7",NULL},{2200,"g8",NULL},
    {2400,"g9",NULL},{2600,"primary","touch-covered"},{2800,"stop","touch-result"},
};
static const step_t imu_steps[]={
    {200,"h3",NULL},{400,"blocked_primary","imu-wait"},
    {1000,"primary","imu-motion"},{1200,"stop","imu-result"},
};
static const step_t imu_error_steps[]={
    {200,"h3",NULL},{400,"primary","imu-init-error"},{1200,"imu_fail",NULL},
    {1400,"primary","imu-read-error"},{2200,"imu_stale",NULL},
    {3400,"primary","imu-stale"},{3500,"imu_resume",NULL},
    {4500,"imu_close_fail",NULL},{4700,"primary",NULL},
    {5000,"primary","imu-close-error"},{5300,"stop","imu-recovered"},
};
static const step_t imu_still_steps[]={
    {200,"h3",NULL},{1600,"blocked_primary","imu-still"},{1800,"skip",NULL},{2000,"stop",NULL},
};
static const step_t imu_cancel_steps[]={{200,"h3",NULL},{400,"stop","imu-cancel"}};
static const step_t rgb_steps[]={
    {200,"h4",NULL},{400,"primary","rgb-start"},{460,"blocked_primary","rgb-pending"},
    {800,"primary","rgb-red"},{1200,"primary","rgb-green"},
    {1600,"primary","rgb-blue"},{2000,"stop","rgb-result"},
};
static const step_t summary_steps[]={
    {200,"h1",NULL},{400,"primary",NULL},{600,"h2",NULL},
    {800,"g1",NULL},{1000,"g2",NULL},{1200,"g3",NULL},{1400,"g4",NULL},
    {1600,"g5",NULL},{1800,"g6",NULL},{2000,"g7",NULL},{2200,"g8",NULL},
    {2400,"g9",NULL},{2600,"primary",NULL},{2800,"h3",NULL},{3800,"primary",NULL},
    {4000,"h4",NULL},{4200,"primary",NULL},{4600,"primary",NULL},{5000,"primary",NULL},
    {5400,"primary",NULL},{5800,"h5",NULL},{6000,"primary",NULL},
    {6200,"menu","summary"},{6400,"h1","completed-menu"},{6600,"primary",NULL},
    {6800,"again",NULL},{7000,"stop","run-again"},
};
static const step_t rgb_error_steps[]={
    {200,"h4",NULL},{400,"primary",NULL},{600,"primary","led-open-error"},
    {800,"primary","led-write-error"},{860,"led_status_fail",NULL},
    {1000,"primary","led-status-error"},{1060,"led_frame_fail",NULL},
    {1200,"primary","led-frame-error"},{1260,"led_mismatch",NULL},
    {1400,"primary","led-mismatch"},{1460,"led_stuck",NULL},
    {2600,"primary","led-timeout"},{2680,"led_resume",NULL},{3000,"fail",NULL},
    {3060,"led_close_fail",NULL},{3300,"primary","led-close-error"},
    {3500,"stop","led-failure-recorded"},
};
static const step_t rgb_cancel_steps[]={{200,"h4",NULL},{400,"primary",NULL},{460,"stop","led-cancel"}};
static const step_t unavailable_steps[]={
    {200,"h3",NULL},{400,"skip","imu-unavailable"},{600,"h4",NULL},
    {800,"primary",NULL},{1000,"skip","led-unavailable"},{1200,"stop","unavailable-result"},
};
static const step_t rgb_skip_steps[]={
    {200,"h4",NULL},{400,"primary",NULL},{800,"skip",NULL},{1100,"h4",NULL},
    {1300,"menu",NULL},{1500,"stop","led-return-menu"},
};
typedef struct {
    const char *name, *directory;
    const step_t *steps;
    size_t count, step;
    unsigned mounts, updates, closes, cleanups, hardware_calls, writes;
    bool down, cancel, imu_owned, moved;
    unsigned imu_reads, imu_deinits;
    unsigned init_failures, read_failures, deinit_failures;
    bool stale, still;
    uint32_t led_handle;
    unsigned led_due, led_closes;
    uint8_t led_rgb[3];
    unsigned led_open_fail, led_write_fail, led_status_fail, led_frame_fail, led_mismatch, led_close_fail;
    bool led_stuck, led_closing, unavailable;
    ryz_ui_scene_t scene;
} fixture_t;
static uint64_t now_ms(void *p) { (void)p; return esp_timer_get_time()/1000; }
static void *resize(void *p,size_t n) { if(!n){free(p);return NULL;} return realloc(p,n); }
static void pause_ms(void *p,unsigned ms) { (void)p; ryz_host_display_advance_ms(ms ? ms : 1); }
static bool cancelled(void *p) { return ((fixture_t *)p)->cancel; }
static const ryz_ui_object_t *object(fixture_t *p,const char *id)
{
    for(unsigned i=0;i<p->scene.object_count;i++)
        if(!strcmp(p->scene.objects[i].id,id)) return &p->scene.objects[i];
    fprintf(stderr,"missing object %s on %s\n",id,p->scene.id); abort();
}
static bool contains(fixture_t *p,const char *text)
{
    for(unsigned i=0;i<p->scene.object_count;i++)
        if(strstr(p->scene.objects[i].text,text)) return true;
    return false;
}
static void check_step(fixture_t *p)
{
    if(!strcmp(p->name,"hub")) {
        assert(contains(p,"ROOTMAKER SELF TEST") && contains(p,"0 / 5"));
        assert(contains(p,"LCD / PIXELS") && contains(p,"TOUCH / GRID"));
        assert(contains(p,"IMU / MOTION") && contains(p,"RGB LED") && contains(p,"BATTERY"));
        assert(!p->hardware_calls && !p->writes);
    } else if(!strcmp(p->name,"manual")) {
        unsigned at=p->steps[p->step].at;
        if(at==600) assert(!strcmp(object(p,"result1")->text,"PASS"));
        if(at==1000) assert(!strcmp(object(p,"result1")->text,"FAIL"));
        if(at==1400) assert(!strcmp(object(p,"result1")->text,"START"));
        if(at==2000) assert(contains(p,"NOT AVAILABLE") && !contains(p,"0%"));
        if(at==2200) assert(!strcmp(object(p,"result5")->text,"CHECK"));
        if(at==2600) assert(!strcmp(object(p,"result1")->text,"SKIP") && !strcmp(object(p,"result5")->text,"SKIP"));
        assert(!p->hardware_calls && !p->writes);
    } else if(!strcmp(p->name,"touch")) {
        unsigned at=p->steps[p->step].at;
        if(at==400 || at==520) assert(contains(p,"0 / 9"));
        if(at==1000) assert(contains(p,"1 / 9"));
        if(at==2600) assert(contains(p,"9 / 9") && object(p,"primary")->enabled);
        if(at==2800) assert(!strcmp(object(p,"result2")->text,"PASS"));
        assert(!p->hardware_calls && !p->writes);
    } else if(!strcmp(p->name,"imu")) {
        unsigned at=p->steps[p->step].at;
        if(at==400) assert(p->imu_owned && p->imu_reads && !object(p,"primary")->enabled);
        if(at==1000) assert(contains(p,"MOTION") && object(p,"primary")->enabled && p->imu_reads>=6);
        if(at==1200) assert(!strcmp(object(p,"result3")->text,"PASS") && p->imu_deinits==1 && !p->imu_owned);
    } else if(!strcmp(p->name,"imu-errors")) {
        unsigned at=p->steps[p->step].at;
        if(at==400) assert(contains(p,"INIT: FAILED") && !p->imu_owned);
        if(at==1400) assert(contains(p,"READ: FAILED") && contains(p,"--.-- g"));
        if(at==3400) assert(contains(p,"NO FRESH SAMPLE") && contains(p,"--.-- g"));
        if(at==5000) assert(contains(p,"STOP: FAILED") && p->imu_owned && contains(p,"--.-- g"));
        if(at==5300) assert(!strcmp(object(p,"result3")->text,"PASS") && !p->imu_owned);
    } else if(!strcmp(p->name,"imu-still")) {
        unsigned at=p->steps[p->step].at;
        if(at==1600) assert(!object(p,"primary")->enabled && p->imu_reads>=6);
        if(at==2000) assert(!strcmp(object(p,"result3")->text,"SKIP") && !p->imu_owned);
    } else if(!strcmp(p->name,"imu-cancel") && p->steps[p->step].at==400) {
        assert(p->imu_owned && p->imu_reads);
    } else if(!strcmp(p->name,"rgb")) {
        unsigned at=p->steps[p->step].at;
        if(at==400) assert(!p->led_handle && !p->writes && contains(p,"START"));
        if(at==460) assert(p->writes==1 && !object(p,"primary")->enabled);
        if(at==800) assert(p->writes==1 && p->led_rgb[0]==255 && contains(p,"RED"));
        if(at==1200) assert(p->writes==2 && p->led_rgb[1]==255 && contains(p,"GREEN"));
        if(at==1600) assert(p->writes==3 && p->led_rgb[2]==255 && contains(p,"BLUE"));
        if(at==2000) assert(p->writes==4 && !p->led_handle && p->led_closes==1 && !strcmp(object(p,"result4")->text,"PASS"));
    } else if(!strcmp(p->name,"summary")) {
        unsigned at=p->steps[p->step].at;
        if(at==6200) {
            assert(!strcmp(p->scene.id,"summary") && contains(p,"SELF TEST COMPLETE"));
            for(unsigned i=1;i<=4;i++) { char id[12]; snprintf(id,sizeof(id),"result%u",i); assert(!strcmp(object(p,id)->text,"PASS")); }
            assert(!strcmp(object(p,"result5")->text,"CHECK") && !p->led_handle && !p->imu_owned);
        }
        if(at==6400) assert(contains(p,"5 / 5"));
        if(at==7000) assert(contains(p,"0 / 5"));
    } else if(!strcmp(p->name,"rgb-errors")) {
        unsigned at=p->steps[p->step].at;
        if(at==600) assert(contains(p,"OPEN: FAILED") && !p->led_handle);
        if(at==800) assert(contains(p,"WRITE: FAILED") && p->led_handle);
        if(at==1000) assert(contains(p,"STATUS: FAILED"));
        if(at==1200) assert(contains(p,"FRAME FAILED"));
        if(at==1400) assert(contains(p,"FRAME MISMATCH"));
        if(at==2600) assert(contains(p,"FRAME TIMEOUT"));
        if(at==3300) assert(contains(p,"CLOSE: FAILED") && p->led_handle && p->led_closing);
        if(at==3500) assert(!strcmp(object(p,"result4")->text,"FAIL") && !p->led_handle);
    } else if(!strcmp(p->name,"rgb-cancel") && p->steps[p->step].at==460) {
        assert(p->led_handle && p->writes==1 && p->led_rgb[0]==255);
    } else if(!strcmp(p->name,"unavailable")) {
        unsigned at=p->steps[p->step].at;
        if(at==400) assert(contains(p,"INIT: UNAVAILABLE") && contains(p,"--.-- g"));
        if(at==1000) assert(contains(p,"OPEN: UNAVAILABLE") && !p->led_handle);
        if(at==1200) assert(!strcmp(object(p,"result3")->text,"SKIP") && !strcmp(object(p,"result4")->text,"SKIP"));
        assert(!p->writes);
    } else if(!strcmp(p->name,"rgb-skip")) {
        unsigned at=p->steps[p->step].at;
        if(at==1100) assert(!strcmp(object(p,"result4")->text,"SKIP") && !p->led_handle && p->writes==2);
        if(at==1500) assert(!strcmp(p->scene.id,"hub") && p->writes==2 && !p->led_handle);
    }
}
static int pointer_read(void *context,ryz_app_pointer_t *out)
{
    fixture_t *p=context; unsigned now=(unsigned)now_ms(p);
    *out=(ryz_app_pointer_t){.sampled_ms=now};
    if(p->step>=p->count || now<p->steps[p->step].at) return 0;
    const step_t *s=&p->steps[p->step];
    if(!p->down) {
        check_step(p);
        if(s->shot) {
            char name[100]; snprintf(name,sizeof(name),"hardware-fixture-%s-%s-240",p->name,s->shot);
            ryz_host_display_save(p->directory,name);
        }
        if(!strcmp(s->action,"stop")) { p->cancel=true; ++p->step; return 0; }
        if(!strncmp(s->action,"imu_",4)) {
            if(!strcmp(s->action,"imu_fail")) p->read_failures=1;
            else if(!strcmp(s->action,"imu_stale")) p->stale=true;
            else if(!strcmp(s->action,"imu_resume")) p->stale=false;
            else p->deinit_failures=1;
            ++p->step; return 0;
        }
        if(!strncmp(s->action,"led_",4)) {
            if(!strcmp(s->action,"led_status_fail")) p->led_status_fail=1;
            else if(!strcmp(s->action,"led_frame_fail")) p->led_frame_fail=1;
            else if(!strcmp(s->action,"led_mismatch")) p->led_mismatch=1;
            else if(!strcmp(s->action,"led_stuck")) p->led_stuck=true;
            else if(!strcmp(s->action,"led_resume")) p->led_stuck=false;
            else p->led_close_fail=1;
            ++p->step; return 0;
        }
        bool blocked=!strncmp(s->action,"blocked_",8),drag=!strncmp(s->action,"drag_",5);
        const ryz_ui_object_t *o=object(p,s->action+(blocked ? 8 : drag ? 5 : 0));
        assert(o->kind==RYZ_UI_BUTTON && o->visible && o->enabled==!blocked);
        *out=(ryz_app_pointer_t){.phase=RYZ_APP_POINTER_DOWN,.pressed=true,.has_position=true,
            .x=o->x+o->width/2,.y=o->y+o->height/2,.sampled_ms=now};
        p->down=true; p->moved=false;
    } else if(now>=s->at+40) { out->phase=RYZ_APP_POINTER_UP; p->down=false; ++p->step; }
    else if(!strncmp(s->action,"drag_",5) && !p->moved && now>=s->at+20) {
        *out=(ryz_app_pointer_t){.phase=RYZ_APP_POINTER_MOVE,.pressed=true,.has_position=true,
            .x=120,.y=100,.sampled_ms=now}; p->moved=true;
    }
    else out->pressed=true;
    return 0;
}
static void check_layout(const ryz_ui_scene_t *s)
{
    lv_obj_t *root=lv_screen_active();
    assert(s->object_count<=32 && lv_obj_get_child_count(root)==s->object_count);
    for(unsigned i=0;i<s->object_count;i++) {
        const ryz_ui_object_t *o=&s->objects[i];
        assert(!o->parent[0]);
        if(o->kind!=RYZ_UI_LABEL && o->kind!=RYZ_UI_BUTTON) continue;
        lv_obj_t *label=lv_obj_get_child(root,i);
        if(o->kind==RYZ_UI_BUTTON) label=lv_obj_get_child(label,0);
        lv_point_t size;
        lv_text_get_size(&size,o->text,lv_obj_get_style_text_font(label,0),0,0,LV_COORD_MAX,0);
        int width=o->width-(o->kind==RYZ_UI_BUTTON ? 8 : 0);
        if(size.x>width || size.y>o->height)
            fprintf(stderr,"CLIP %s: %s actual=%dx%d box=%dx%d\n",o->id,o->text,(int)size.x,(int)size.y,width,o->height);
        assert(size.x<=width && size.y<=o->height);
    }
}
static int mount(void *context,const ryz_ui_scene_t *s)
{
    fixture_t *p=context; int error=ryz_lvgl_mount(s);
    if(!error) { p->scene=*s; ++p->mounts; check_layout(s); }
    return error;
}
static int update(void *context,const ryz_ui_scene_t *s)
{
    fixture_t *p=context; assert(p->scene.generation==s->generation);
    int error=ryz_lvgl_update(s);
    if(!error) { p->scene=*s; ++p->updates; check_layout(s); }
    return error;
}
static int pump(void *p) { (void)p; return ryz_lvgl_pump(); }
static void close_ui(void *context)
{ fixture_t *p=context; ++p->closes; ryz_lvgl_release(); }
static void cleanup(void *context)
{ fixture_t *p=context; ++p->cleanups; p->imu_owned=false; assert(!p->led_handle); }
static ryz_peripheral_result_t peripheral(void *context,const ryz_peripheral_request_t *q,
    ryz_peripheral_reply_t *out)
{
    fixture_t *p=context; memset(out,0,sizeof(*out));
    if(p->unavailable) return RYZ_PERIPHERAL_UNAVAILABLE;
    if(q->kind!=RYZ_PERIPHERAL_LED) return RYZ_PERIPHERAL_UNAVAILABLE;
    if(q->op==RYZ_PERIPHERAL_OPEN) {
        if(p->led_open_fail) { --p->led_open_fail; return RYZ_PERIPHERAL_FAILED; }
        assert(q->config.led.board && !p->led_handle);
        p->led_closing=false;
        out->handle=p->led_handle=UINT32_C(4000000001); return RYZ_PERIPHERAL_OK;
    }
    assert(p->led_handle && q->handle==p->led_handle);
    if(q->op==RYZ_PERIPHERAL_CLOSE) {
        p->led_closing=true;
        if(p->led_close_fail) { --p->led_close_fail; return RYZ_PERIPHERAL_FAILED; }
        p->led_handle=0; ++p->led_closes; return RYZ_PERIPHERAL_OK;
    }
    assert(!p->led_closing);
    if(q->op==RYZ_PERIPHERAL_WRITE) {
        if(p->led_write_fail) { --p->led_write_fail; return RYZ_PERIPHERAL_FAILED; }
        assert(q->length==3); memcpy(p->led_rgb,q->data,3); ++p->writes;
        p->led_due=(unsigned)now_ms(p)+100; return RYZ_PERIPHERAL_OK;
    }
    assert(q->op==RYZ_PERIPHERAL_STATUS);
    if(p->led_status_fail) { --p->led_status_fail; return RYZ_PERIPHERAL_FAILED; }
    out->led.state=!p->led_stuck && now_ms(p)>=p->led_due ? RYZ_PERIPHERAL_LED_READY : RYZ_PERIPHERAL_LED_PENDING;
    if(p->led_frame_fail) { --p->led_frame_fail; out->led.state=RYZ_PERIPHERAL_LED_FAILED; }
    out->led.output_known=out->led.state==RYZ_PERIPHERAL_LED_READY;
    out->led.red=p->led_rgb[0]; out->led.green=p->led_rgb[1]; out->led.blue=p->led_rgb[2];
    if(p->led_mismatch) { --p->led_mismatch; out->led.state=RYZ_PERIPHERAL_LED_READY; out->led.output_known=true; out->led.red=123; }
    return RYZ_PERIPHERAL_OK;
}
static ryz_lua_hardware_result_t hardware(void *context,ryz_lua_hardware_op_t op,
    ryz_lua_hardware_value_t *out)
{
    fixture_t *p=context; ++p->hardware_calls;
    if(p->unavailable) return RYZ_LUA_HW_UNAVAILABLE;
    if(op==RYZ_LUA_IMU_INIT) {
        if(p->init_failures) { --p->init_failures; return RYZ_LUA_HW_FAILED; }
        p->imu_owned=true; p->imu_reads=0; return RYZ_LUA_HW_OK;
    }
    if(op==RYZ_LUA_IMU_DEINIT) {
        if(p->deinit_failures) { --p->deinit_failures; return RYZ_LUA_HW_FAILED; }
        p->imu_owned=false; ++p->imu_deinits; return RYZ_LUA_HW_OK;
    }
    if(op!=RYZ_LUA_IMU_READ) return RYZ_LUA_HW_UNAVAILABLE;
    if(!p->imu_owned) return RYZ_LUA_HW_NOT_INITIALIZED;
    if(p->read_failures) { --p->read_failures; return RYZ_LUA_HW_FAILED; }
    unsigned n=++p->imu_reads;
    *out=(ryz_lua_hardware_value_t){.x_mg=(int)(n%4)*300-450,
        .y_mg=(int)((n+1)%4)*300-450,.z_mg=500+(int)((n+2)%4)*250,
        .timestamp_us=UINT64_C(9000000000)+now_ms(p)*1000,
        .sequence=UINT32_C(4000000000)+n};
    if(p->stale) { out->sequence=UINT32_C(4000000000); out->timestamp_us=UINT64_C(9000000000); }
    if(p->still) { out->x_mg=0; out->y_mg=0; out->z_mg=1000; }
    return RYZ_LUA_HW_OK;
}

int main(int argc,char **argv)
{
    assert(argc==4);
    FILE *f=fopen(argv[1],"rb"); assert(f);
    char source[RYZ_LUA_SOURCE_MAX+1]; size_t n=fread(source,1,sizeof(source),f);
    assert(!ferror(f) && fclose(f)==0 && n && n<=RYZ_LUA_SOURCE_MAX);
    fixture_t p={.name=argv[2],.directory=argv[3],.steps=hub_steps,.count=1};
    if(!strcmp(p.name,"manual")) { p.steps=manual_steps; p.count=sizeof(manual_steps)/sizeof(*manual_steps); }
    else if(!strcmp(p.name,"touch")) { p.steps=touch_steps; p.count=sizeof(touch_steps)/sizeof(*touch_steps); }
    else if(!strcmp(p.name,"imu")) { p.steps=imu_steps; p.count=sizeof(imu_steps)/sizeof(*imu_steps); }
    else if(!strcmp(p.name,"imu-errors")) { p.steps=imu_error_steps; p.count=sizeof(imu_error_steps)/sizeof(*imu_error_steps); p.init_failures=1; }
    else if(!strcmp(p.name,"imu-still")) { p.steps=imu_still_steps; p.count=sizeof(imu_still_steps)/sizeof(*imu_still_steps); p.still=true; }
    else if(!strcmp(p.name,"imu-cancel")) { p.steps=imu_cancel_steps; p.count=2; }
    else if(!strcmp(p.name,"rgb")) { p.steps=rgb_steps; p.count=sizeof(rgb_steps)/sizeof(*rgb_steps); }
    else if(!strcmp(p.name,"summary")) { p.steps=summary_steps; p.count=sizeof(summary_steps)/sizeof(*summary_steps); }
    else if(!strcmp(p.name,"rgb-errors")) { p.steps=rgb_error_steps; p.count=sizeof(rgb_error_steps)/sizeof(*rgb_error_steps); p.led_open_fail=p.led_write_fail=1; }
    else if(!strcmp(p.name,"rgb-cancel")) { p.steps=rgb_cancel_steps; p.count=3; }
    else if(!strcmp(p.name,"unavailable")) { p.steps=unavailable_steps; p.count=sizeof(unavailable_steps)/sizeof(*unavailable_steps); p.unavailable=true; }
    else if(!strcmp(p.name,"rgb-skip")) { p.steps=rgb_skip_steps; p.count=sizeof(rgb_skip_steps)/sizeof(*rgb_skip_steps); }
    else assert(!strcmp(p.name,"hub"));
    assert(ryz_font_init()==ESP_OK); ryz_host_display_reset();
    const ryz_app_platform_t platform={.context=&p,.resize=resize,.now_ms=now_ms,
        .pause_ms=pause_ms,.cancelled=cancelled,.pointer_read=pointer_read,
        .ui_mount=mount,.ui_update=update,.ui_pump=pump,.ui_close=close_ui,
        .cleanup=cleanup,.hardware_call=hardware,.peripheral_call=peripheral,.tool_call=NULL,.seed=1};
    ryz_lua_result_t result;
    ryz_app_execute(source,n,"@tool_hardware.lua",15000,&platform,&result);
    if(!result.ok && strcmp(result.phase,"stopped")) fprintf(stderr,"Lua %s: %s\n",result.phase,result.error);
    assert(p.mounts && p.closes==1 && p.cleanups==1 && p.step==p.count);
    assert(!result.ok && !strcmp(result.phase,"stopped"));
    assert(result.peak_bytes<RYZ_LUA_HEAP_LIMIT);
    printf("HARDWARE_SCRIPT_PASS %s mounts=%u updates=%u peak=%zu bytes=%zu elapsed=%u SIMULATED INPUT ONLY\n",
        p.name,p.mounts,p.updates,result.peak_bytes,n,result.elapsed_ms);
    return 0;
}
