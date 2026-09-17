/* Real factory Lua and LVGL. Only the public peripheral/time/pointer boundary
 * is simulated. This is not an electrical or physical-device acceptance test. */
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
#error "I2C script fixture requires assertions"
#endif
typedef struct { unsigned at; const char *action; } step_t;
static const step_t pin_steps[] = {
    {2800,"pins"},{3000,"sda"},{3200,"p13"},{3400,"use"},
    {3600,"scl"},{3800,"blocked_p13"},{4000,"p14"},{4200,"cancel"},
    {4400,"scl"},{4600,"p14"},{4800,"use"},{5000,"clock"},
    {5200,"c400"},{5400,"use"},{5600,"submit"},{8300,"pins"},
    {8500,"internal"},{11200,"stop"},
};
static const step_t error_steps[] = {
    {200,"scan"},{3000,"scan"},{3500,"scan"},{6200,"scan"},
    {6500,"scan"},{6800,"stop"},
};
static const step_t close_steps[] = {{2800,"scan"},{3200,"scan"},{5900,"stop"}};
static const step_t picker_steps[] = {
    {2800,"pins"},{3000,"sda"},{3200,"next"},{3400,"next"},
    {3600,"p48"},{3800,"use"},{4000,"sda"},{4200,"next"},{4400,"cancel"},
    {4600,"scl"},{4800,"p13"},{5000,"use"},{5200,"submit"},{8000,"stop"},
};
typedef struct {
    const char *name, *directory;
    unsigned mounts, updates, ui_closes, cleans, probes, opens, closes;
    uint32_t handle;
    bool cancel, down;
    unsigned step, next_address, scan_probes, attempts, close_failures;
    bool moved;
    ryz_peripheral_config_t config;
    ryz_ui_scene_t scene;
} fixture_t;
static uint64_t now_ms(void *p) { (void)p; return esp_timer_get_time()/1000; }
static void *resize(void *p,size_t n) { if(!n){free(p);return NULL;} return realloc(p,n); }
static void pause_ms(void *p,unsigned ms) { (void)p; ryz_host_display_advance_ms(ms ? ms : 1); }
static bool cancelled(void *p) { return ((fixture_t *)p)->cancel; }
static bool contains(const fixture_t *p,const char *text)
{
    for(unsigned i=0;i<p->scene.object_count;i++)
        if(strstr(p->scene.objects[i].text,text)) return true;
    return false;
}
static const ryz_ui_object_t *object(fixture_t *p,const char *id)
{
    for(unsigned i=0;i<p->scene.object_count;i++)
        if(!strcmp(p->scene.objects[i].id,id)) return &p->scene.objects[i];
    fprintf(stderr,"missing object %s on %s\n",id,p->scene.id); abort();
}
static int pointer_read(void *context,ryz_app_pointer_t *out)
{
    fixture_t *p=context;
    *out=(ryz_app_pointer_t){.sampled_ms=(uint32_t)now_ms(p)};
    if(!strcmp(p->name,"cancel")) {
        if(now_ms(p)>=300) {
            assert(p->handle && p->scan_probes>0 && p->scan_probes<112);
            ryz_host_display_save(p->directory,"i2c-fixture-boot-cancel-240"); p->cancel=true;
        }
        return 0;
    }
    if(!strcmp(p->name,"repeat")) {
        if(now_ms(p)<2800+p->step*2800) return 0;
        if(!p->down) {
            assert(!p->handle && p->opens==p->step+1 && p->closes==p->opens);
            assert(contains(p,"112 ACK") && contains(p,"READ-ONLY SCAN"));
            if(p->step==8) { p->cancel=true; return 0; }
            const ryz_ui_object_t *o=object(p,"scan");
            *out=(ryz_app_pointer_t){.phase=RYZ_APP_POINTER_DOWN,.pressed=true,.has_position=true,
                .x=o->x+o->width/2,.y=o->y+o->height/2,.sampled_ms=(uint32_t)now_ms(p)};
            p->down=true;
        } else if(now_ms(p)>=2840+p->step*2800) { out->phase=RYZ_APP_POINTER_UP; p->down=false; ++p->step; }
        else out->pressed=true;
        return 0;
    }
    if(!strcmp(p->name,"scroll")) {
        if(now_ms(p)<2800+p->step*80) return 0;
        if(!p->down) {
            assert(p->opens==1 && p->closes==1 && contains(p,"112 ACK"));
            if(!p->step) ryz_host_display_save(p->directory,"i2c-fixture-scroll-first-240");
            if(p->step==33) {
                assert(contains(p,"0x77") && !contains(p,"0x08"));
                ryz_host_display_save(p->directory,"i2c-fixture-scroll-last-240");
            }
            if(p->step==34) { assert(p->opens==1); p->cancel=true; return 0; }
            *out=(ryz_app_pointer_t){.phase=RYZ_APP_POINTER_DOWN,.pressed=true,.has_position=true,
                .x=64,.y=p->step==33 ? 118 : 164,.sampled_ms=(uint32_t)now_ms(p)};
            p->down=true; p->moved=false;
        } else if(!p->moved) {
            *out=(ryz_app_pointer_t){.phase=RYZ_APP_POINTER_MOVE,.pressed=true,.has_position=true,
                .x=64,.y=p->step==33 ? 198 : 14,.sampled_ms=(uint32_t)now_ms(p)};
            p->moved=true;
        } else { out->phase=RYZ_APP_POINTER_UP; p->down=false; ++p->step; }
        return 0;
    }
    if(!strcmp(p->name,"errors") || !strcmp(p->name,"close-errors")) {
        bool errors=!strcmp(p->name,"errors");
        const step_t *steps=errors ? error_steps : close_steps;
        size_t count=errors ? sizeof(error_steps)/sizeof(*error_steps) : sizeof(close_steps)/sizeof(*close_steps);
        if(p->step>=count || now_ms(p)<steps[p->step].at) return 0;
        const step_t *s=&steps[p->step];
        if(!p->down) {
            if(errors && s->at==200) {
                assert(contains(p,"OPEN: BUSY") && !p->handle);
                assert(contains(p,"SCAN INCOMPLETE") && !contains(p,"NO ADDRESSES"));
                ryz_host_display_save(p->directory,"i2c-fixture-open-busy-240");
            }
            if(errors && s->at==3500) {
                assert(contains(p,"PARTIAL: TIMEOUT") && contains(p,"1 FOUND") && !p->handle);
                ryz_host_display_save(p->directory,"i2c-fixture-partial-timeout-240");
            }
            if(errors && (s->at==3000 || s->at==6200)) assert(contains(p,"1 FOUND") && contains(p,"READ-ONLY SCAN"));
            if(!errors && s->at==2800) {
                assert(contains(p,"CLOSE: FAILED") && p->handle && !object(p,"pins")->enabled);
                ryz_host_display_save(p->directory,"i2c-fixture-close-failed-240");
            }
            if(!errors && s->at==3200) assert(!p->handle && contains(p,"RELEASED / SCAN AGAIN"));
            if(!strcmp(s->action,"stop")) {
                if(errors) {
                    assert(contains(p,"CANCELLED") && contains(p,"1 FOUND") && contains(p,"PREVIOUS RESULTS RESTORED"));
                    assert(p->scan_probes<112 && !p->handle);
                    ryz_host_display_save(p->directory,"i2c-fixture-cancel-restored-240");
                } else assert(!p->handle && contains(p,"READ-ONLY SCAN"));
                ++p->step; p->cancel=true; return 0;
            }
            const ryz_ui_object_t *o=object(p,s->action);
            assert(o->enabled);
            *out=(ryz_app_pointer_t){.phase=RYZ_APP_POINTER_DOWN,.pressed=true,.has_position=true,
                .x=o->x+o->width/2,.y=o->y+o->height/2,.sampled_ms=(uint32_t)now_ms(p)};
            p->down=true;
        } else if(now_ms(p)>=s->at+40) { out->phase=RYZ_APP_POINTER_UP; p->down=false; ++p->step; }
        else out->pressed=true;
        return 0;
    }
    if(!strcmp(p->name,"pins") || !strcmp(p->name,"picker")) {
        bool picker=!strcmp(p->name,"picker");
        const step_t *steps=picker ? picker_steps : pin_steps;
        size_t count=picker ? sizeof(picker_steps)/sizeof(*picker_steps) : sizeof(pin_steps)/sizeof(*pin_steps);
        if(p->step>=count || now_ms(p)<steps[p->step].at) return 0;
        const step_t *s=&steps[p->step];
            if(!p->down) {
            if(s->at==3000) {
                assert(!object(p,"submit")->enabled && p->opens==1);
                ryz_host_display_save(p->directory,"i2c-fixture-pins-empty-240");
            }
            if(!picker && s->at==4400) assert(contains(p,"GPIO13") && !object(p,"submit")->enabled && p->opens==1);
            if(s->at==5600) {
                assert(contains(p,"GPIO13") && contains(p,"GPIO14") && contains(p,"400 kHz"));
                assert(p->opens==1 && !p->handle);
                ryz_host_display_save(p->directory,"i2c-fixture-pins-ready-240");
            }
            if(picker && s->at==3600) ryz_host_display_save(p->directory,"i2c-fixture-pin-picker-last-240");
            if(!picker && s->at==5200) ryz_host_display_save(p->directory,"i2c-fixture-clock-picker-240");
            if(s->at==8300) {
                assert(contains(p,"CUSTOM I2C1") && contains(p,"GPIO13 / GPIO14"));
                assert(contains(p,"400 kHz") && p->opens==2 && p->closes==2);
                assert(!p->config.i2c.board && p->config.i2c.sda==13 && p->config.i2c.scl==14 && p->config.i2c.frequency_hz==400000);
                ryz_host_display_save(p->directory,"i2c-fixture-custom-complete-240");
            }
            if(!strcmp(s->action,"stop")) {
                if(picker) assert(contains(p,"GPIO48 / GPIO13") && p->opens==2 && p->closes==2);
                else assert(contains(p,"INTERNAL I2C0") && p->opens==3 && p->closes==3);
                ++p->step; p->cancel=true; return 0;
            }
            bool blocked=!strncmp(s->action,"blocked_",8);
            const ryz_ui_object_t *o=object(p,s->action+(blocked ? 8 : 0));
            assert(o->kind==RYZ_UI_BUTTON && o->visible && o->enabled==!blocked);
            *out=(ryz_app_pointer_t){.phase=RYZ_APP_POINTER_DOWN,.pressed=true,.has_position=true,
                .x=o->x+o->width/2,.y=o->y+o->height/2,.sampled_ms=(uint32_t)now_ms(p)};
            p->down=true;
        } else if(now_ms(p)>=s->at+40) {
            out->phase=RYZ_APP_POINTER_UP; p->down=false; ++p->step;
        } else out->pressed=true;
        return 0;
    }
    if(now_ms(p)<3000) return 0;
    assert(contains(p,"INTERNAL I2C0") && contains(p,"GPIO41 / GPIO40"));
    if(!strcmp(p->name,"empty")) assert(contains(p,"0 FOUND") && contains(p,"NO ADDRESSES"));
    else {
        assert(contains(p,"0x15") && contains(p,"ADDRESS") && contains(p,"ACK"));
        assert(!contains(p,"CST816") && contains(p,"1 FOUND"));
    }
    assert(p->probes==112 && p->opens==1 && p->closes==1 && !p->handle);
    char shot[96]; snprintf(shot,sizeof(shot),"i2c-fixture-%s-complete-240",p->name);
    ryz_host_display_save(p->directory,shot);
    p->cancel=true;
    return 0;
}
static void check_layout(const ryz_ui_scene_t *s)
{
    lv_obj_t *root=lv_screen_active(), *nodes[RYZ_UI_OBJECT_MAX];
    unsigned top=0, children[RYZ_UI_OBJECT_MAX]={0};
    for(unsigned i=0;i<s->object_count;++i) {
        const ryz_ui_object_t *o=&s->objects[i]; nodes[i]=NULL;
        if(o->parent[0]) {
            for(unsigned j=0;j<i;++j) if(!strcmp(s->objects[j].id,o->parent))
                nodes[i]=lv_obj_get_child(nodes[j],children[j]++);
        } else nodes[i]=lv_obj_get_child(root,top++);
        assert(nodes[i]);
        if(!o->visible || (o->kind!=RYZ_UI_LABEL && o->kind!=RYZ_UI_BUTTON)) continue;
        lv_obj_t *label=o->kind==RYZ_UI_BUTTON ? lv_obj_get_child(nodes[i],0) : nodes[i];
        const lv_font_t *font=lv_obj_get_style_text_font(label,0);
        lv_point_t size; lv_text_get_size(&size,o->text,font,0,0,LV_COORD_MAX,0);
        int width=o->width-(o->kind==RYZ_UI_BUTTON ? 8 : 0);
        if(size.x>width || size.y>o->height)
            fprintf(stderr,"CLIP %s text=%s actual=%dx%d box=%dx%u\n",o->id,o->text,
                (int)size.x,(int)size.y,width,o->height);
        assert(size.x<=width && size.y<=o->height);
    }
}
static int mount(void *context,const ryz_ui_scene_t *s)
{
    fixture_t *p=context; int e=ryz_lvgl_mount(s); if(e) return e;
    p->scene=*s; ++p->mounts; check_layout(s); return 0;
}
static int update(void *context,const ryz_ui_scene_t *s)
{
    fixture_t *p=context; int e=ryz_lvgl_update(s); if(e) return e;
    p->scene=*s; ++p->updates; check_layout(s); return 0;
}
static int pump(void *p) { (void)p; return ryz_lvgl_pump(); }
static void close_ui(void *context) { fixture_t *p=context; ++p->ui_closes; ryz_lvgl_release(); }
static void cleanup(void *context) { fixture_t *p=context; ++p->cleans; assert(!p->handle); }
static ryz_peripheral_result_t peripheral(void *context,const ryz_peripheral_request_t *q,
    ryz_peripheral_reply_t *out)
{
    fixture_t *p=context; memset(out,0,sizeof(*out));
    assert(q->kind==RYZ_PERIPHERAL_I2C);
    if(q->op==RYZ_PERIPHERAL_OPEN) {
        ++p->attempts;
        if(!strcmp(p->name,"errors") && p->attempts==1) return RYZ_PERIPHERAL_BUSY;
        assert(!p->handle); p->config=q->config; p->scan_probes=0; p->next_address=8;
        out->handle=p->handle=++p->opens; return RYZ_PERIPHERAL_OK;
    }
    assert(q->handle==p->handle && p->handle);
    if(q->op==RYZ_PERIPHERAL_CLOSE) {
        if(!strcmp(p->name,"close-errors") && !p->close_failures++) return RYZ_PERIPHERAL_FAILED;
        p->handle=0; ++p->closes; return RYZ_PERIPHERAL_OK;
    }
    assert(q->op==RYZ_PERIPHERAL_PROBE && q->address==p->next_address++);
    assert(q->timeout_ms<=20); ++p->probes; ++p->scan_probes; ryz_host_display_advance_ms(2);
    if(!strcmp(p->name,"errors") && p->opens==2 && q->address==0x18) return RYZ_PERIPHERAL_TIMEOUT;
    return (!strcmp(p->name,"scroll") || !strcmp(p->name,"repeat") || (strcmp(p->name,"empty") && q->address==0x15)) ?
        RYZ_PERIPHERAL_OK : RYZ_PERIPHERAL_NOT_FOUND;
}
int main(int argc,char **argv)
{
    assert(argc==4); FILE *f=fopen(argv[1],"rb"); assert(f);
    char source[RYZ_LUA_SOURCE_MAX+1]; size_t n=fread(source,1,sizeof(source),f);
    assert(!ferror(f) && fclose(f)==0 && n && n<=RYZ_LUA_SOURCE_MAX);
    fixture_t p={.name=argv[2],.directory=argv[3]};
    assert(!strcmp(p.name,"happy") || !strcmp(p.name,"pins") || !strcmp(p.name,"errors") ||
        !strcmp(p.name,"close-errors") || !strcmp(p.name,"cancel") || !strcmp(p.name,"scroll") || !strcmp(p.name,"empty") || !strcmp(p.name,"picker") || !strcmp(p.name,"repeat"));
    assert(ryz_font_init()==ESP_OK); ryz_host_display_reset();
    const ryz_app_platform_t platform={.context=&p,.resize=resize,.now_ms=now_ms,
        .pause_ms=pause_ms,.cancelled=cancelled,.pointer_read=pointer_read,
        .ui_mount=mount,.ui_update=update,.ui_pump=pump,.ui_close=close_ui,
        .cleanup=cleanup,.peripheral_call=peripheral,.tool_call=NULL,.seed=1};
    ryz_lua_result_t result;
    ryz_app_execute(source,n,"@tool_i2c.lua",!strcmp(p.name,"repeat") ? 30000 : 14000,&platform,&result);
    if(!result.ok && strcmp(result.phase,"stopped")) fprintf(stderr,"Lua %s: %s\n",result.phase,result.error);
    assert(!result.ok && !strcmp(result.phase,"stopped") && p.cancel);
    assert(p.ui_closes==1 && p.cleans==1 && p.mounts<=16 && !p.handle);
    assert(result.peak_bytes<RYZ_LUA_HEAP_LIMIT);
    printf("I2C_SCRIPT_PASS %s (simulated bus) mounts=%u updates=%u probes=%u peak=%zu bytes=%zu\n",
        p.name,p.mounts,p.updates,p.probes,result.peak_bytes,n);
    return 0;
}
