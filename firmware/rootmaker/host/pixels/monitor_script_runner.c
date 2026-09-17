/* Real public Lua entrypoint, actual factory script and LVGL. Only time,
 * pointer input and generic UART/log bytes come from an explicit fixture. */
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
#include "monitor_stream_fixture.h"

#ifdef NDEBUG
#error "Monitor fixture requires assertions"
#endif
typedef struct { unsigned at; const char *button, *shot; } step_t;
static const step_t happy[] = {
    {400,"pause","live"}, {700,"clear","paused"}, {1000,"pause","cleared"},
    {1300,"config","resumed"}, {1500,"source","config-system"}, {1700,"uart","source"},
    {1900,"use",NULL}, {2100,"port","config-uart"}, {2300,"p1","port-p1"},
    {2500,"swap",NULL}, {2700,"cancel","port-swapped"}, {2900,"port","cancel-port-draft"},
    {3100,"p4",NULL}, {3300,"swap",NULL}, {3500,"use","port-p4-swapped"},
    {3700,"baud",NULL}, {3900,"drag_up","baud"}, {4100,"b8","baud-scrolled"},
    {4300,"use",NULL}, {4500,"format",NULL}, {4700,"f2","format"},
    {4800,"f3",NULL}, {4900,"f4",NULL}, {5000,"f5",NULL}, {5100,"f6",NULL},
    {5300,"use","format-7o1"}, {5500,"apply","config-ready"},
    {5900,"config","uart-live"}, {6100,"source",NULL}, {6300,"system",NULL},
    {6500,"cancel",NULL}, {6700,"reset","cancel-source-draft"},
    {6900,"cancel","reset-effective"}, {7100,"cancel_fixture","config-return"},
};
static const step_t errors[] = {
    {200,"pause","open-error"}, {500,"config","retry-live"},
    {700,"source",NULL}, {900,"uart",NULL}, {1100,"use",NULL},
    {1300,"fail_close",NULL}, {1500,"apply",NULL},
    {1800,"apply","close-error"}, {2100,"config","close-retry-live"},
    {2300,"source",NULL}, {2500,"system",NULL}, {2700,"use",NULL},
    {2900,"fail_open",NULL}, {3100,"apply",NULL},
    {3400,"apply","apply-open-error"}, {3700,"config","open-retry-live"},
    {3900,"source",NULL}, {4100,"uart",NULL}, {4300,"use",NULL},
    {4500,"fail_close",NULL}, {4700,"apply",NULL},
    {5000,"cancel","close-error-cancel"}, {5300,"pause","close-cancel-runtime"},
    {5700,"cancel_fixture","close-runtime-retry-live"},
};
static const step_t unavailable[] = {{200,"blocked_clear","unavailable"},{400,"cancel_fixture",NULL}};
static const step_t gap_steps[] = {{600,"clear","gap"},{900,"cancel_fixture","gap-cleared"}};
static const step_t read_errors[] = {{400,"fail_read","live"},{540,"check","read-error"},{700,"cancel_fixture","read-recovered"}};
static const step_t stress_steps[] = {{15000,"cancel_fixture","sustained-input"}};
static const step_t scroll_steps[] = {
    {2000,"drag_down","scroll-live-tail"}, {2200,"pause",NULL},
    {2400,"drag_down","scroll-paused-tail"}, {2600,"check","scroll-history"},
    {8000,"check","scroll-frozen"}, {8200,"pause",NULL},
    {8400,"check","scroll-live-latest"}, {8600,"pause",NULL},
    {8800,"drag_down",NULL}, {9000,"clear",NULL}, {9300,"check","scroll-cleared"},
    {9600,"pause",NULL}, {10000,"cancel_fixture","scroll-auto-follow"},
};
typedef struct {
    const char *name, *directory;
    const step_t *steps;
    size_t count, step;
    bool down, moved, cancel, gap, stress, overload, scroll;
    unsigned mounts, updates, closes, cleans, producer, closing_reads;
    uint32_t anchor_pixels;
    ryz_ui_scene_t scene;
    monitor_stream_fixture_t stream;
} fixture_t;
static uint64_t now_ms(void *p) { (void)p; return esp_timer_get_time()/1000; }
static void *resize(void *p,size_t n) { if(!n){free(p);return NULL;} return realloc(p,n); }
static const ryz_ui_object_t *object(fixture_t *p,const char *id)
{
    for(unsigned i=0;i<p->scene.object_count;i++)
        if(!strcmp(p->scene.objects[i].id,id)) return &p->scene.objects[i];
    fprintf(stderr,"missing object %s on %s\n",id,p->scene.id); abort();
}
static bool contains(fixture_t *p,const char *needle)
{
    for(unsigned i=0;i<p->scene.object_count;i++)
        if(strstr(p->scene.objects[i].text,needle)) return true;
    return false;
}
static void shot(fixture_t *p,const char *name)
{
    char label[100]; snprintf(label,sizeof(label),"monitor-fixture-%s-%s-240",p->name,name);
    ryz_host_display_save(p->directory,label);
}
static void pause_ms(void *context,unsigned ms)
{
    fixture_t *p=context;
    ryz_host_display_advance_ms(ms ? ms : 1);
    unsigned now=(unsigned)now_ms(p);
    if(!p->stream.handle || p->cancel) return;
    if(p->scroll) {
        if(p->producer==now/100) return;
        p->producer=now/100;
        char bytes[32]; int n=snprintf(bytes,sizeof(bytes),"SIM LINE %03u\n",p->producer);
        monitor_fixture_feed(&p->stream,bytes,(size_t)n); return;
    }
    if(p->stress) {
        unsigned period=p->overload ? 20 : 100;
        if(p->producer==now/period) return;
        p->producer=now/period;
        char bytes[2048];
        for(unsigned i=0;i<sizeof(bytes);++i) bytes[i]=(char)(i+now);
        monitor_fixture_feed(&p->stream,bytes,sizeof(bytes)); return;
    }
    if(p->gap) {
        if(p->producer++) return;
        p->stream.dropped=UINT32_C(4000000000); p->stream.loss_possible=true;
        monitor_fixture_feed(&p->stream,"SIM CAPTURE GAP\n",16); return;
    }
    if(p->producer==0 && now>=80) {
        const char intro[]="I (1) sim: INJECTED BY FIXTURE\r";
        monitor_fixture_feed(&p->stream,intro,sizeof(intro)-1);
        const char binary[]="\nA\0\xff\tZ\n";
        monitor_fixture_feed(&p->stream,binary,sizeof(binary)-1);
        monitor_fixture_feed(&p->stream,"0123456789012345678901234567890123456789\n",41);
        monitor_fixture_feed(&p->stream,"W (2) sim: WARNING\n",19); p->producer++;
    } else if(p->producer==1 && now>=500) {
        monitor_fixture_feed(&p->stream,"PAUSED PRODUCER\n",16); p->producer++;
    } else if(p->producer==2 && now>=900) {
        monitor_fixture_feed(&p->stream,"AFTER CLEAR\n\n",13); p->producer++;
    }
}
static bool cancelled(void *context)
{ fixture_t *p=context; return p->cancel || (!p->steps && now_ms(p)>=700); }
static void check_step(fixture_t *p)
{
    unsigned at=p->steps[p->step].at;
    if(p->scroll) {
        if(at==2200) assert(!contains(p,"SIM LINE 001") && contains(p,"LIVE"));
        if(at==2600 || at==8000) {
            assert(contains(p,"PAUSED"));
            assert(contains(p,"SIM LINE 002"));
            assert(!contains(p,"SIM LINE 024"));
            uint32_t pixels=UINT32_C(2166136261);
            for(unsigned y=20;y<196;++y) for(unsigned x=8;x<234;++x)
                pixels=(pixels ^ ryz_host_display_pixel(x,y))*UINT32_C(16777619);
            if(at==2600) p->anchor_pixels=pixels;
            else assert(pixels==p->anchor_pixels);
        }
        if(at==8400) assert(contains(p,"LIVE") && !contains(p,"SIM LINE 022") && contains(p,"SIM LINE 083"));
        if(at==9300) assert(contains(p,"WAITING FOR DATA") && contains(p,"PAUSED"));
        if(at==10000) assert(contains(p,"LIVE") && contains(p,"SIM LINE 099") && p->mounts==1);
    } else if(p->stress) {
        if(p->overload) {
            assert(p->stream.dropped>0 && contains(p,"CAPTURE LOSS POSSIBLE"));
        } else {
            assert(!p->stream.dropped && contains(p,"HISTORY OVERWRITTEN"));
            assert(!contains(p,"CAPTURE LOSS"));
        }
    }
    else if(!strcmp(p->name,"happy")) {
        if(at==400) {
            assert(contains(p,"A[00][FF]    Z"));
            assert(contains(p,"012345678901234567890123456"));
            bool warning=false;
            for(unsigned i=0;i<p->scene.object_count;i++)
                if(strstr(p->scene.objects[i].text,"W (2) sim: WARNING"))
                    warning=p->scene.objects[i].foreground==0xfd84;
            assert(warning);
        }
        if(at==700) { assert(contains(p,"PAUSED")); assert(!contains(p,"PAUSED PRODUCER")); }
        if(at==1000) { assert(contains(p,"WAITING FOR DATA")); assert(!contains(p,"AFTER CLEAR")); }
        if(at==1300) assert(contains(p,"AFTER CLEAR"));
        if(at==1500) {
            assert(!object(p,"port")->enabled && !object(p,"baud")->enabled);
            assert(contains(p,"NOT USED"));
        }
        if(at==2100 || at==2900) assert(contains(p,"P1 15/16"));
        if(at==2300) assert(!object(p,"p2")->enabled);
        if(at==2700) assert(contains(p,"RX 16") && contains(p,"TX 15"));
        if(at==3500) assert(contains(p,"RX 18") && contains(p,"TX 17"));
        if(at==4100) assert(object(p,"presets")->scroll_y>=140);
        if(at==4700) assert(object(p,"f2")->enabled && object(p,"f6")->enabled);
        if(at==5300) assert(object(p,"f6")->border==0xfb40);
        if(at==5500) assert(contains(p,"P4 18/17") && contains(p,"921600 >") && contains(p,"7O1 >"));
        if(at==5900 || at==7100) {
            assert(contains(p,"P4 921600 / LIVE"));
            assert(p->stream.config.uart.rx==18 && p->stream.config.uart.tx==-1);
            assert(p->stream.config.uart.bits==7 && p->stream.config.uart.parity==2 && p->stream.config.uart.stop==1);
        }
        if(at==6700 || at==6900) assert(contains(p,"RSPORT RX >") && contains(p,"P4 18/17") && contains(p,"921600 >"));
    } else if(!strcmp(p->name,"errors")) {
        if(at==200) assert(contains(p,"OPEN: FAILED") && !p->stream.handle);
        if(at==500) assert(contains(p,"LIVE") && p->stream.kind==RYZ_PERIPHERAL_LOG);
        if(at==1800) assert(contains(p,"CLOSE: FAILED") && p->stream.handle && p->stream.kind==RYZ_PERIPHERAL_LOG);
        if(at==2100) assert(contains(p,"LIVE") && p->stream.kind==RYZ_PERIPHERAL_UART);
        if(at==3400) assert(contains(p,"OPEN: FAILED") && !p->stream.handle && !strcmp(p->scene.id,"config"));
        if(at==3700) assert(contains(p,"SYSTEM LOG / LIVE") && p->stream.kind==RYZ_PERIPHERAL_LOG);
        if(at==5000) {
            assert(contains(p,"CLOSE: FAILED") && p->stream.handle);
            p->closing_reads=p->stream.reads;
        }
        if(at==5300) {
            assert(!strcmp(p->scene.id,"monitor") && contains(p,"CLOSE: FAILED"));
            assert(contains(p,"RETRY") && !object(p,"clear")->enabled);
            assert(p->stream.reads==p->closing_reads && p->stream.handle);
        }
        if(at==5700) {
            assert(contains(p,"SYSTEM LOG / LIVE") && !contains(p,"CLOSE:"));
            assert(p->stream.kind==RYZ_PERIPHERAL_LOG && p->stream.reads>p->closing_reads);
        }
    } else if(p->gap) {
        if(at==600) assert(contains(p,"CAPTURE LOSS POSSIBLE"));
        if(at==900) assert(contains(p,"WAITING FOR DATA"));
    } else if(!strcmp(p->name,"read-errors")) {
        if(at==540) assert(contains(p,"READ: TIMEOUT") && p->stream.handle);
        if(at==700) assert(!contains(p,"READ:") && p->stream.handle);
    } else {
        assert(contains(p,"OPEN: UNAVAILABLE"));
        assert(!object(p,"clear")->enabled);
    }
}
static int pointer_read(void *context,ryz_app_pointer_t *out)
{
    fixture_t *p=context; unsigned now=(unsigned)now_ms(p);
    *out=(ryz_app_pointer_t){.sampled_ms=now};
    if(p->step>=p->count || now<p->steps[p->step].at) return 0;
    const char *action=p->steps[p->step].button;
    bool drag=!strncmp(action,"drag_",5);
    if(!p->down) {
        check_step(p);
        if(p->steps[p->step].shot) shot(p,p->steps[p->step].shot);
        if(!strcmp(action,"check")) { ++p->step; return 0; }
        if(!strcmp(action,"cancel_fixture")) { ++p->step; p->cancel=true; return 0; }
        if(!strncmp(action,"fail_",5)) {
            if(!strcmp(action,"fail_close")) p->stream.close_failures=1;
            else if(!strcmp(action,"fail_open")) p->stream.open_failures=1;
            else p->stream.read_failures=6;
            ++p->step; return 0;
        }
        if(drag) {
            *out=(ryz_app_pointer_t){.phase=RYZ_APP_POINTER_DOWN,.pressed=true,
                .has_position=true,.x=60,.y=!strcmp(action,"drag_up") ? 160 : 30,.sampled_ms=now};
            p->down=true; p->moved=false; return 0;
        }
        bool blocked=!strncmp(action,"blocked_",8);
        const ryz_ui_object_t *o=object(p,action+(blocked ? 8 : 0));
        assert(o->kind==RYZ_UI_BUTTON && o->enabled==!blocked && o->visible);
        *out=(ryz_app_pointer_t){.phase=RYZ_APP_POINTER_DOWN,.pressed=true,
            .has_position=true,.x=o->x+o->width/2,.y=o->y+o->height/2,.sampled_ms=now};
        if(o->parent[0]) {
            const ryz_ui_object_t *parent=object(p,o->parent);
            out->x+=parent->x; out->y+=parent->y-parent->scroll_y;
            assert(out->y>=parent->y && out->y<parent->y+parent->height);
        }
        p->down=true;
    } else if(now>=p->steps[p->step].at+40) {
        out->phase=RYZ_APP_POINTER_UP; p->down=false; ++p->step;
    } else if(drag && !p->moved && now>=p->steps[p->step].at+20) {
        *out=(ryz_app_pointer_t){.phase=RYZ_APP_POINTER_MOVE,.pressed=true,
            .has_position=true,.x=60,.y=!strcmp(action,"drag_up") ? 20 : 190,.sampled_ms=now};
        p->moved=true;
    } else out->pressed=true;
    return 0;
}
static void check_layout(const ryz_ui_scene_t *s)
{
    static bool ascii_logs;
    lv_obj_t *root=lv_screen_active();
    lv_obj_t *nodes[RYZ_UI_OBJECT_MAX];
    unsigned top=0, child_counts[RYZ_UI_OBJECT_MAX]={0};
    for(unsigned i=0;i<s->object_count;i++) {
        const ryz_ui_object_t *o=&s->objects[i];
        nodes[i]=NULL;
        if(o->parent[0]) {
            for(unsigned j=0;j<i;j++) if(!strcmp(s->objects[j].id,o->parent))
                nodes[i]=lv_obj_get_child(nodes[j],child_counts[j]++);
        } else nodes[i]=lv_obj_get_child(root,top++);
        assert(nodes[i]);
        if(o->kind!=RYZ_UI_LABEL && o->kind!=RYZ_UI_BUTTON) continue;
        lv_obj_t *label=nodes[i];
        if(o->kind==RYZ_UI_BUTTON) label=lv_obj_get_child(label,0);
        const lv_font_t *font=lv_obj_get_style_text_font(label,0);
        bool log_test=!ascii_logs && !strcmp(o->id,"log1");
        if(log_test) {
            unsigned columns=28, widest=0;
            for(unsigned c=32;c<=126;c++) {
                char text[29]; memset(text,(int)c,columns); text[columns]=0;
                lv_point_t measured; lv_text_get_size(&measured,text,font,0,0,LV_COORD_MAX,0);
                if((unsigned)measured.x>widest) widest=(unsigned)measured.x;
                if(measured.x>o->width) fprintf(stderr,"ASCII_BOUND %s char=%u width=%d box=%u\n",o->id,c,(int)measured.x,o->width);
                assert(measured.x<=o->width);
            }
            printf("MONITOR_ASCII_BOUND %s columns=%u widest=%u box=%u\n",o->id,columns,widest,o->width);
            ascii_logs=true;
        }
        lv_point_t size; lv_text_get_size(&size,o->text,font,0,0,LV_COORD_MAX,0);
        int width=o->width-(o->kind==RYZ_UI_BUTTON ? 8 : 0);
        if(size.x>width || size.y>o->height)
            fprintf(stderr,"CLIP %s text=%s actual=%dx%d box=%dx%d\n",o->id,o->text,
                    (int)size.x,(int)size.y,width,o->height);
        assert(size.x<=width && size.y<=o->height);
    }
    assert(lv_obj_get_child_count(root)==top);
}

static int mount(void *context,const ryz_ui_scene_t *s)
{
    fixture_t *p=context;
    int error=ryz_lvgl_mount(s); if(error) return error;
    p->scene=*s; p->mounts++; check_layout(s); return 0;
}
static int update(void *context,const ryz_ui_scene_t *s)
{
    fixture_t *p=context;
    assert(p->scene.generation==s->generation && !strcmp(p->scene.id,s->id));
    lv_obj_t *root=lv_screen_active();
    int error=ryz_lvgl_update(s); if(error) return error;
    assert(root==lv_screen_active());
    p->scene=*s; p->updates++; check_layout(s); return 0;
}
static int pump(void *p) { (void)p; return ryz_lvgl_pump(); }
static void close_ui(void *context)
{ fixture_t *p=context; p->closes++; assert(p->closes==1); ryz_lvgl_release(); }
static void cleanup(void *context)
{
    fixture_t *p=context; p->cleans++;
    /* Production binding must close every public handle before job cleanup. */
    assert(!p->stream.handle);
}
static ryz_peripheral_result_t peripheral(void *context,const ryz_peripheral_request_t *q,
    ryz_peripheral_reply_t *out)
{ fixture_t *p=context; return monitor_fixture_call(&p->stream,q,out); }

int main(int argc,char **argv)
{
    assert(argc==4);
    FILE *f=fopen(argv[1],"rb"); assert(f);
    char source[RYZ_LUA_SOURCE_MAX+1]; size_t n=fread(source,1,sizeof(source),f);
    assert(!ferror(f) && fclose(f)==0 && n && n<=RYZ_LUA_SOURCE_MAX);
    fixture_t p={.name=argv[2],.directory=argv[3]};
    monitor_fixture_init(&p.stream);
    if(!strcmp(p.name,"happy")) { p.steps=happy; p.count=sizeof(happy)/sizeof(*happy); }
    else if(!strcmp(p.name,"errors")) { p.stream.open_failures=1; p.steps=errors; p.count=sizeof(errors)/sizeof(*errors); }
    else if(!strcmp(p.name,"unavailable")) { p.stream.unavailable=true; p.steps=unavailable; p.count=2; }
    else if(!strcmp(p.name,"gap")) { p.gap=true; p.steps=gap_steps; p.count=2; }
    else if(!strcmp(p.name,"read-errors")) { p.steps=read_errors; p.count=sizeof(read_errors)/sizeof(*read_errors); }
    else if(!strcmp(p.name,"stress") || !strcmp(p.name,"overload")) {
        p.stress=true; p.overload=!strcmp(p.name,"overload"); p.steps=stress_steps; p.count=1;
    }
    else if(!strcmp(p.name,"scroll")) { p.scroll=true; p.steps=scroll_steps; p.count=sizeof(scroll_steps)/sizeof(*scroll_steps); }
    else assert(!strcmp(p.name,"cancel"));
    assert(ryz_font_init()==ESP_OK); ryz_host_display_reset();
    const ryz_app_platform_t platform={.context=&p,.resize=resize,.now_ms=now_ms,
        .pause_ms=pause_ms,.cancelled=cancelled,.pointer_read=pointer_read,
        .ui_mount=mount,.ui_update=update,.ui_pump=pump,.ui_close=close_ui,
        .cleanup=cleanup,.peripheral_call=peripheral,.tool_call=NULL,.seed=1};
    ryz_lua_result_t result;
    ryz_app_execute(source,n,"@tool_monitor.lua",p.stress ? 16000 : p.scroll ? 12000 : 8000,&platform,&result);
    if(!result.ok && strcmp(result.phase,"stopped")) fprintf(stderr,"Lua %s: %s\n",result.phase,result.error);
    assert(p.closes==1 && p.cleans==1 && p.step==p.count);
    assert(!result.ok && !strcmp(result.phase,"stopped") && !p.stream.handle);
    assert(p.mounts<=28 && result.peak_bytes<RYZ_LUA_HEAP_LIMIT);
    printf("MONITOR_SCRIPT_PASS %s mounts=%u updates=%u reads=%u peak=%zu bytes=%zu elapsed=%u\n",
        p.name,p.mounts,p.updates,p.stream.reads,result.peak_bytes,n,result.elapsed_ms);
    return 0;
}
