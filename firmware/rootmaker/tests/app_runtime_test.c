#include "app_runtime.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { max_align_t align; size_t size; } allocation_t;
static size_t live_bytes;
static bool poison_allocations;

static void *tracked_resize(void *ptr, size_t size)
{
    allocation_t *old = ptr ? (allocation_t *)ptr - 1 : NULL;
    size_t previous = old ? old->size : 0;
    if (!size) {
        live_bytes -= previous;
        free(old);
        return NULL;
    }
    if (poison_allocations) return NULL;
    allocation_t *next = realloc(old, sizeof(*next) + size);
    if (!next) return NULL;
    next->size = size;
    live_bytes = live_bytes - previous + size;
    return next + 1;
}

typedef struct {
    uint64_t now;
    ryz_app_display_op_t ops[8];
    unsigned op_count;
    unsigned shows;
    ryz_ui_scene_t scene;
    unsigned ui_mounts;
    unsigned ui_updates;
    int ui_update_error;
    uint64_t ui_update_delay_ms;
    bool ui_update_cancel;
    bool ui_update_poison;
    bool cancelled;
    unsigned ui_pumps;
    unsigned ui_closes;
    unsigned touch_demo_calls;
    bool touch_demo_enabled;
    uint64_t ui_mount_delay_ms;
    uint64_t ui_pump_delay_ms;
    uint64_t pointer_delay_ms;
    ryz_app_pointer_t ui_pointers[8];
    unsigned ui_pointer_count;
    unsigned ui_pointer_index;
    char phase[25];
    int32_t seed_mark;
} fixture_t;

static uint64_t fixture_now(void *context) { return ((fixture_t *)context)->now; }
static void fixture_pause(void *context, unsigned ms) { ((fixture_t *)context)->now += ms ? ms : 1; }
static bool fixture_false(void *context) { (void)context; return false; }
static int fixture_pointer(void *context, ryz_app_pointer_t *pointer)
{
    fixture_t *fixture = context;
    fixture->now += fixture->pointer_delay_ms;
    if (fixture->ui_pointer_index < fixture->ui_pointer_count) {
        *pointer = fixture->ui_pointers[fixture->ui_pointer_index++];
        return 0;
    }
    *pointer = (ryz_app_pointer_t){
        .phase = RYZ_APP_POINTER_DOWN,
        .pressed = true,
        .has_position = true,
        .x = 12,
        .y = 34,
        .sampled_ms = 7,
    };
    return 0;
}
static int fixture_ui_mount(void *context, const ryz_ui_scene_t *scene)
{
    fixture_t *fixture = context;
    fixture->scene = *scene;
    fixture->ui_mounts++;
    fixture->now += fixture->ui_mount_delay_ms;
    return 0;
}
static void fixture_ui_close(void *context)
{
    ((fixture_t *)context)->ui_closes++;
}
static int fixture_ui_update(void *context, const ryz_ui_scene_t *scene)
{
    fixture_t *fixture = context;
    assert(scene && scene->generation == fixture->scene.generation);
    fixture->scene = *scene;
    ++fixture->ui_updates;
    fixture->now += fixture->ui_update_delay_ms;
    if (fixture->ui_update_cancel) fixture->cancelled = true;
    if (fixture->ui_update_poison) poison_allocations = true;
    return fixture->ui_update_error;
}
static int fixture_ui_pump(void *context)
{
    fixture_t *fixture = context;
    fixture->ui_pumps++;
    fixture->now += fixture->ui_pump_delay_ms;
    return 0;
}
static int fixture_touch_demo(void *context, bool enabled)
{
    fixture_t *fixture = context;
    fixture->touch_demo_calls++;
    fixture->touch_demo_enabled = enabled;
    return 0;
}
static int fixture_draw(void *context, const ryz_app_display_op_t *op)
{
    fixture_t *fixture = context;
    assert(fixture->op_count < sizeof(fixture->ops) / sizeof(fixture->ops[0]));
    fixture->ops[fixture->op_count++] = *op;
    return 0;
}
static int fixture_show(void *context)
{
    ((fixture_t *)context)->shows++;
    return 0;
}
static void fixture_mark(void *context, const char *name, const ryz_app_value_t *value)
{
    fixture_t *fixture = context;
    if (!strcmp(name, "seed")) {
        assert(value->kind == RYZ_APP_VALUE_INTEGER);
        fixture->seed_mark = value->integer;
    } else if (!strcmp(name, "phase")) {
        assert(value->kind == RYZ_APP_VALUE_STRING && value->length < sizeof(fixture->phase));
        memcpy(fixture->phase, value->string, value->length);
        fixture->phase[value->length] = 0;
    } else assert(!"unexpected mark");
}

static const char source[] =
    "-- ryz-app/1\n"
    "local board=require('ryzobee')\n"
    "local touch=require('touch')\n"
    "local display=require('display')\n"
    "math.randomseed(board.seed())\n"
    "local point=touch.read()\n"
    "board.mark('seed',board.seed())\n"
    "board.mark('phase',point.event)\n"
    "display.clear(0x0841)\n"
    "display.rect(1,2,3,4,0xf800)\n"
    "display.text(5,6,'2048',0xffff,2)\n"
    "assert(display.show())\n"
    "print('APP_PASS',point.x,point.y)\n";

static const char sandbox_source[] =
    "-- ryz-app/1\n"
    "assert(io == nil and os == nil and debug == nil and package == nil)\n"
    "assert(type(coroutine)=='table' and require('coroutine')==coroutine)\n"
    "assert(load == nil and loadfile == nil and dofile == nil)\n"
    "assert(pcall == nil and xpcall == nil)\n";

static const char forbidden_source[] =
    "-- ryz-app/1\n"
    "require('socket')\n";

static const char embedded_nul_module_source[] =
    "-- ryz-app/1\n"
    "require('ui\\0ignored')\n";

static const char embedded_nul_scene_field_source[] =
    "-- ryz-app/1\n"
    "local ui=require('ui')\n"
    "ui.mount({id='home',background=0,objects={},['id\\0extra']='ignored'})\n";

static const char ui_source[] =
    "-- ryz-app/1\n"
    "local ui=require('ui')\n"
    "local generation=ui.mount({\n"
    " id='home', background=0x0000, objects={\n"
    "  {id='frame',kind='box',x=8,y=8,width=224,height=224,background=0x0000,border=0xfb40,border_width=1,radius=4},\n"
    "  {id='title',kind='label',x=16,y=20,width=208,height=28,text='RYZOBEE',foreground=0xfb40,font='title_24',align='center'},\n"
    "  {id='apps',kind='button',x=20,y=72,width=200,height=44,text='APPS',foreground=0xfb40,background=0x0000,border=0xfb40,border_width=2,radius=4,font='body_16',align='center'}\n"
    " }\n"
    "})\n"
    "assert(generation==1)\n"
    "assert(ui.poll()==nil)\n"
    "local event=ui.poll()\n"
    "assert(event.kind=='activate' and event.id=='apps' and event.scene=='home' and event.generation==1)\n"
    "print('UI_PASS',event.id)\n";

static const char mixed_owner_source[] =
    "-- ryz-app/1\n"
    "local ui=require('ui')\n"
    "local display=require('display')\n"
    "ui.mount({id='home',background=0,objects={}})\n"
    "display.clear(0)\n";

static const char display_then_ui_source[] =
    "-- ryz-app/1\n"
    "local display=require('display')\n"
    "local ui=require('ui')\n"
    "display.clear(0)\n"
    "ui.mount({id='home',background=0,objects={}})\n";

static const char touch_then_ui_poll_source[] =
    "-- ryz-app/1\n"
    "local touch=require('touch')\n"
    "local ui=require('ui')\n"
    "touch.read()\n"
    "ui.mount({id='home',background=0,objects={}})\n"
    "ui.poll()\n";

static const char ui_poll_then_touch_source[] =
    "-- ryz-app/1\n"
    "local touch=require('touch')\n"
    "local ui=require('ui')\n"
    "ui.mount({id='home',background=0,objects={}})\n"
    "assert(ui.poll()==nil)\n"
    "touch.read()\n";

static const char disabled_touch_demo_then_ui_source[] =
    "-- ryz-app/1\n"
    "local touch=require('touch')\n"
    "local ui=require('ui')\n"
    "assert(touch.demo(false))\n"
    "assert(ui.mount({id='home',background=0,objects={}})==1)\n";

static const char too_many_objects_source[] =
    "-- ryz-app/1\n"
    "local ui=require('ui')\n"
    "local objects={}\n"
    "for i=1,33 do\n"
    " objects[i]={id='item_'..i,kind='box',x=0,y=0,width=1,height=1}\n"
    "end\n"
    "ui.mount({id='home',background=0,objects=objects})\n";

static const char invalid_object_id_source[] =
    "-- ryz-app/1\n"
    "local ui=require('ui')\n"
    "ui.mount({id='home',background=0,objects={\n"
    " {id='BAD',kind='box',x=0,y=0,width=1,height=1}\n"
    "}})\n";

static const char embedded_nul_token_source[] =
    "-- ryz-app/1\n"
    "local ui=require('ui')\n"
    "ui.mount({id='home',background=0,objects={\n"
    " {id='box',kind='box\\0ignored',x=0,y=0,width=1,height=1}\n"
    "}})\n";

static const char ui_mount_only_source[] =
    "-- ryz-app/1\n"
    "local ui=require('ui')\n"
    "ui.mount({id='home',background=0,objects={}})\n";

static const char ui_poll_once_source[] =
    "-- ryz-app/1\n"
    "local ui=require('ui')\n"
    "ui.mount({id='home',background=0,objects={}})\n"
    "ui.poll()\n";

static const char interrupted_touch_source[] =
    "-- ryz-app/1\n"
    "local point=require('touch').read()\n"
    "assert(point.event=='none' and point.pressed and "
    "not point.has_position and point.interrupted==true)\n";

static const char interrupted_ui_source[] =
    "-- ryz-app/1\n"
    "local ui=require('ui')\n"
    "ui.mount({id='home',background=0,objects={{id='apps',kind='button',"
    "x=20,y=72,width=200,height=44,text='APPS'}}})\n"
    "assert(ui.poll()==nil)\n"
    "assert(ui.poll()==nil)\n"
    "assert(ui.poll()==nil)\n";

#define UPDATE_SETUP \
    "-- ryz-app/1\nlocal ui=require('ui'); local scene={id='home',background=0,objects={" \
    "{id='frame',kind='box',x=0,y=0,width=240,height=240}," \
    "{id='title',kind='label',x=10,y=10,width=220,height=30,text='READY'}," \
    "{id='apps',kind='button',x=20,y=72,width=200,height=44,text='APPS'}}}; " \
    "local generation=ui.mount(scene); "

static bool fixture_cancelled(void *context) { return ((fixture_t *)context)->cancelled; }

static fixture_t update_fixture(void)
{
    fixture_t fixture = {.ui_pointers = {
        {.phase=RYZ_APP_POINTER_DOWN,.pressed=true,.has_position=true,.x=40,.y=90},
        {.phase=RYZ_APP_POINTER_UP,.has_position=false}},
        .ui_pointer_count = 2};
    return fixture;
}

static ryz_lua_result_t update_run(fixture_t *fixture, const char *body,
                                  unsigned timeout, bool callback)
{
    assert(!live_bytes);
    char source[8192];
    int length = snprintf(source, sizeof(source), "%s%s", UPDATE_SETUP, body);
    assert(length > 0 && (size_t)length < sizeof(source));
    ryz_app_platform_t platform = {.context=fixture,.resize=tracked_resize,
        .now_ms=fixture_now,.pause_ms=fixture_pause,.cancelled=fixture_cancelled,
        .pointer_read=fixture_pointer,.ui_mount=fixture_ui_mount,.ui_pump=fixture_ui_pump,
        .ui_close=fixture_ui_close,.ui_update=callback ? fixture_ui_update : NULL};
    ryz_lua_result_t result;
    ryz_app_execute(source, (size_t)length, "@update-case.lua", timeout, &platform, &result);
    poison_allocations = false;
    assert(!live_bytes && fixture->ui_closes == 1);
    return result;
}

static void update_ok(ryz_lua_result_t result)
{
    if (!result.ok) fprintf(stderr, "update %s: %s\n", result.phase, result.error);
    assert(result.ok && !strcmp(result.phase, "done"));
}

static void update_behavior(void)
{
    fixture_t fixture = update_fixture();
    update_ok(update_run(&fixture,
        "assert(ui.poll()==nil); assert(ui.update(generation,{"
        "{id='title',text='RUNNING',foreground=65535,background=0,border=1,visible=false,enabled=false},"
        "{id='frame',background=321,border=0}})==generation); "
        "local event=ui.poll(); assert(event.id=='apps' and event.generation==generation); "
        "assert(ui.update(generation,{})==generation); "
        "assert(ui.update(generation,{{id='apps',text='APPS'},{id='title',text='RUNNING'}})==generation)",0,true));
    assert(fixture.ui_mounts == 1 && fixture.ui_updates == 1 && fixture.ui_closes == 1);
    assert(fixture.scene.generation == 1 && fixture.scene.object_count == 3);
    assert(!strcmp(fixture.scene.objects[1].text,"RUNNING"));
    assert(!fixture.scene.objects[1].visible && !fixture.scene.objects[1].enabled);
    assert(fixture.scene.objects[1].foreground == 65535 && fixture.scene.objects[1].border == 1);
    assert(fixture.scene.objects[0].background == 321 && fixture.scene.objects[0].width == 240);
    const char *changes[] = {"text='NEXT'","foreground=17","background=17","border=17",
                             "visible=false","enabled=false"};
    for (unsigned i = 0; i < sizeof(changes)/sizeof(*changes); ++i) {
        fixture = update_fixture(); char body[512];
        snprintf(body,sizeof(body),"assert(ui.poll()==nil); "
            "assert(ui.update(generation,{{id='apps',%s}})==generation); assert(ui.poll()==nil)",changes[i]);
        update_ok(update_run(&fixture,body,0,true));
        assert(fixture.ui_updates == 1 && fixture.scene.generation == 1);
    }
    fixture = update_fixture();
    update_ok(update_run(&fixture,"assert(ui.poll()==nil); "
        "assert(ui.update(generation,{{id='apps'},{id='frame'}})==generation); "
        "assert(ui.poll().id=='apps')",0,true));
    assert(fixture.ui_updates == 0);
    fixture = update_fixture();
    update_ok(update_run(&fixture,"assert(ui.poll()==nil); local next=ui.mount(scene); "
        "assert(next==generation+1); assert(ui.poll()==nil); "
        "assert(ui.update(next,{{id='title',text=string.rep('A',40)}})==next)",0,true));
    assert(fixture.ui_mounts == 2 && fixture.ui_updates == 1 && strlen(fixture.scene.objects[1].text)==40);
    puts("APP_UI_UPDATE_PASS: same-generation label/box updates preserve capture; button changes and mount revoke");
}

static void update_rejected(const char *body)
{
    fixture_t fixture = update_fixture();
    ryz_lua_result_t result = update_run(&fixture,body,0,true);
    assert(!result.ok && !strcmp(result.phase,"runtime"));
    assert(!strstr(result.error,"META_EXECUTED") && !strstr(result.output,"META_EXECUTED"));
    assert(fixture.ui_mounts == 1 && fixture.ui_updates == 0);
    assert(!strcmp(fixture.scene.objects[1].text,"READY"));
}

static void update_validation(void)
{
    const char *generations[] = {"0","-1","1.5","'1'","true","nil","2","math.huge","0/0"};
    char body[1024];
    for (unsigned i = 0; i < sizeof(generations)/sizeof(*generations); ++i) {
        snprintf(body,sizeof(body),"ui.update(%s,{{id='title',text='CHANGED'}})",generations[i]);
        update_rejected(body);
    }
    const char *patches[] = {
        "nil", "1", "'x'", "true", "{[0]={id='title'}}", "{[2]={id='title'}}",
        "{unexpected={id='title'}}", "{{id='title'},false}", "{{}}", "{{id='unknown'}}",
        "{{id='title'},{id='title'}}", "{{id='frame',text='X'}}",
        "{{id='title',text=''}}", "{{id='title',text=string.rep('X',41)}}",
        "{{id='title',text='bad\\t'}}", "{{id='title',text='bad\\0'}}",
        "{{id='title',text=string.char(127)}}", "{{id='title',text=string.char(195,169)}}",
        "{{id='title',text=12}}", "{{id='title',text=true}}",
        "{{id='title',visible=1}}", "{{id='title',enabled='false'}}",
        "{{id='title',x=1}}", "{{id='title',y=1}}", "{{id='title',width=1}}",
        "{{id='title',height=1}}", "{{id='title',font='title_24'}}", "{{id='title',align='left'}}",
        "{{id='title',radius=1}}", "{{id='title',border_width=1}}", "{{id='title',kind='label'}}",
        "{{id='title',['text\\0extra']='X'}}", "{{id='title',[1]='X'}}",
        "setmetatable({}, {__index=function() error('META_EXECUTED') end})",
        "{setmetatable({id='title'}, {__index=function() error('META_EXECUTED') end})}",
    };
    for (unsigned i = 0; i < sizeof(patches)/sizeof(*patches); ++i) {
        snprintf(body,sizeof(body),"ui.update(generation,%s)",patches[i]); update_rejected(body);
    }
    const char *colors[] = {"-1","65536","1.5","'1'","true","{}","math.huge","0/0"};
    const char *fields[] = {"foreground","background","border"};
    for (unsigned field = 0; field < 3; ++field) {
        for (unsigned i = 0; i < sizeof(colors)/sizeof(*colors); ++i) {
            snprintf(body,sizeof(body),"ui.update(generation,{{id='title',%s=%s}})",fields[field],colors[i]);
            update_rejected(body);
        }
    }
    update_rejected("ui.update()"); update_rejected("ui.update(generation)");
    update_rejected("ui.update(generation,{},'extra')");
    update_rejected("local patches={}; for i=1,33 do patches[i]={id='title'} end; ui.update(generation,patches)");
    /* A valid first patch must never reach the external renderer if a later
     * patch fails. No pcall or private runtime state is needed to observe it. */
    update_rejected("ui.update(generation,{{id='title',text='CHANGED'},{id='unknown',text='X'}})");
    fixture_t fixture = update_fixture();
    ryz_lua_result_t result = update_run(&fixture,
        "ui.mount(scene); ui.update(generation,{{id='title',text='OLD'}})",0,true);
    assert(!result.ok && !strcmp(result.phase,"runtime") && !fixture.ui_updates && fixture.ui_mounts==2);
    fixture = update_fixture();
    result = update_run(&fixture,"ui.update(generation,{})",0,false);
    assert(!result.ok && strstr(result.error,"unavailable") && !fixture.ui_updates);
    ryz_app_platform_t platform = {.context=&fixture,.resize=tracked_resize,
        .now_ms=fixture_now,.pause_ms=fixture_pause,.ui_update=fixture_ui_update};
    const char *unmounted="-- ryz-app/1\nrequire('ui').update(1,{})";
    ryz_app_execute(unmounted,strlen(unmounted),"@unmounted.lua",0,&platform,&result);
    assert(!result.ok && strstr(result.error,"not mounted") && !fixture.ui_updates && !live_bytes);
    /* The accepted maximum is also executable, not merely a count constant. */
    fixture = update_fixture();
    update_ok(update_run(&fixture,
        "local objects={}; local patches={}; for i=1,32 do "
        "objects[i]={id='box_'..i,kind='box',x=0,y=0,width=1,height=1}; "
        "patches[i]={id='box_'..i,background=i} end; "
        "local g=ui.mount({id='many',background=0,objects=objects}); assert(ui.update(g,patches)==g)",0,true));
    assert(fixture.ui_updates==1 && fixture.scene.object_count==32 && fixture.scene.objects[31].background==32);
    puts("APP_UI_UPDATE_PASS: strict prevalidation, stale/no-owner rejection and 32-patch boundary");
}

static void update_lifecycle(void)
{
    const char *change="ui.update(generation,{{id='title',text='CHANGED'}}); print('AFTER_UPDATE')";
    for (unsigned mode=0; mode<3; ++mode) {
        fixture_t fixture=update_fixture();
        if (mode==0) fixture.ui_update_error=-17;
        if (mode==1) fixture.ui_update_cancel=true;
        if (mode==2) fixture.ui_update_delay_ms=5;
        ryz_lua_result_t result=update_run(&fixture,change,mode==2 ? 5 : 0,true);
        assert(!result.ok && !strcmp(result.phase,mode==0 ? "runtime" : mode==1 ? "stopped" : "timeout"));
        assert(fixture.ui_updates==1 && fixture.ui_closes==1 && !strstr(result.output,"AFTER_UPDATE"));
        /* The renderer may have applied the candidate before failing. Never
         * assert a rollback that the API does not promise. */
        assert(!strcmp(fixture.scene.objects[1].text,"CHANGED"));
    }
    fixture_t fixture=update_fixture(); fixture.ui_update_poison=true;
    ryz_lua_result_t result=update_run(&fixture,
        "ui.update(generation,{{id='title',text='CHANGED'}}); print(string.rep('x',4096))",0,true);
    assert(!result.ok && !strcmp(result.phase,"memory") && fixture.ui_updates==1 && fixture.ui_closes==1);
    const char *late_calls[]={"ui.update(generation,{{id='title',text='LATE'}})",
                              "ui.mount(scene)","ui.poll()"};
    for (unsigned action=0; action<3; ++action) {
        for (unsigned error=0; error<2; ++error) {
            fixture=update_fixture(); char body[256];
            snprintf(body,sizeof(body),"keeper=setmetatable({}, {__gc=function() %s end}); %s",
                late_calls[action],error ? "error('USER_ERROR')" : "return");
            result=update_run(&fixture,body,0,true);
            assert(!result.ok && !strcmp(result.phase,"runtime") && strstr(result.error,"__gc"));
            assert(!strstr(result.error,"USER_ERROR"));
            /* The sandbox rejects registration before either the requested
             * finalizer or following user code runs. This is not evidence of
             * an executing lua_close finalizer being stopped by closing. */
            assert(fixture.ui_updates==0 && fixture.ui_mounts==1 && fixture.ui_pumps==0);
            assert(fixture.ui_closes==1 && !live_bytes);
        }
    }
    puts("APP_UI_UPDATE_PASS: renderer failure, cancellation, deadline and post-update OOM close the scene");
}

static fixture_t scroll_fixture(void)
{
    fixture_t fixture = {.ui_pointers = {
        {.phase=RYZ_APP_POINTER_DOWN,.pressed=true,.has_position=true,.x=10,.y=140},
        {.phase=RYZ_APP_POINTER_MOVE,.pressed=true,.has_position=true,.x=10,.y=180,.sampled_ms=40},
        {.phase=RYZ_APP_POINTER_UP,.has_position=false,.sampled_ms=60}},
        .ui_pointer_count=3};
    return fixture;
}

static void scroll_behavior(void)
{
    fixture_t fixture = scroll_fixture();
    update_ok(update_run(&fixture,
        "assert(ui.poll('frame')==nil); ui.update(generation,{{id='title',text='NEW LOG'}}); "
        "local e=ui.poll('frame'); assert(e.kind=='scroll' and e.id=='frame' and e.dy==40); "
        "assert(e.scene=='home' and e.generation==generation and e.at_ms==40); "
        "assert(ui.poll('frame')==nil)",0,true));
    assert(fixture.ui_mounts==1 && fixture.ui_updates==1);
    fixture = scroll_fixture();
    fixture.ui_pointers[1].y=144; /* 4 px jitter: accumulate until >= 6. */
    fixture.ui_pointers[2]=(ryz_app_pointer_t){.phase=RYZ_APP_POINTER_MOVE,
        .pressed=true,.has_position=true,.x=10,.y=146};
    fixture.ui_pointers[3]=fixture.ui_pointers[2]; fixture.ui_pointers[3].y=149;
    fixture.ui_pointers[4]=fixture.ui_pointers[3]; /* Stationary: no duplicate. */
    fixture.ui_pointers[5]=(ryz_app_pointer_t){.phase=RYZ_APP_POINTER_UP,
        .has_position=true,.x=40,.y=90}; /* Release over a button: scroll, never activate. */
    fixture.ui_pointers[6]=fixture.ui_pointers[2];
    fixture.ui_pointer_count=7;
    update_ok(update_run(&fixture,
        "assert(ui.poll('frame')==nil); assert(ui.poll('frame')==nil); "
        "assert(ui.poll('frame').dy==6); assert(ui.poll('frame').dy==3); "
        "assert(ui.poll('frame')==nil); local e=ui.poll('frame'); "
        "assert(e.kind=='scroll' and e.dy==-59); assert(ui.poll('frame')==nil)",0,true));
    /* DOWN/UP alone can scroll; no MOVE packet is required. A tap cannot. */
    for (unsigned tap=0; tap<2; ++tap) {
        fixture=scroll_fixture(); fixture.ui_pointer_count=2;
        fixture.ui_pointers[1].phase=RYZ_APP_POINTER_UP;
        if(tap) fixture.ui_pointers[1].y=140;
        update_ok(update_run(&fixture,tap ?
            "assert(ui.poll('frame')==nil); assert(ui.poll('frame')==nil)" :
            "assert(ui.poll('frame')==nil); assert(ui.poll('frame').dy==40)",0,true));
    }
    /* Button priority and legacy ui.poll() behavior remain unchanged. */
    fixture=update_fixture();
    update_ok(update_run(&fixture,"assert(ui.poll('frame')==nil); "
        "local e=ui.poll('frame'); assert(e.kind=='activate' and e.id=='apps' and e.dy==nil)",0,true));
    fixture=scroll_fixture();
    update_ok(update_run(&fixture,
        "for i=1,3 do assert(ui.poll()==nil) end",0,true));
    const char *cancel[] = {
        "ui.mount(scene)",
        "ui.update(generation,{{id='frame',visible=false}})",
        "ui.update(generation,{{id='frame',enabled=false}})",
        "ui.update(generation,{{id='apps',text='CHANGED'}})",
        ("ui.update(generation,{{id='frame',visible=false}}); "
          "ui.update(generation,{{id='frame',visible=true}})"),
    };
    for(unsigned i=0;i<sizeof(cancel)/sizeof(*cancel);++i) {
        fixture=scroll_fixture(); char body[1024];
        snprintf(body,sizeof(body),"assert(ui.poll('frame')==nil); %s; "
            "assert(ui.poll('frame')==nil); assert(ui.poll('frame')==nil)",cancel[i]);
        update_ok(update_run(&fixture,body,0,true));
    }
    fixture=scroll_fixture();
    update_ok(update_run(&fixture,"assert(ui.poll('frame')==nil); "
        "assert(ui.poll(nil)==nil); assert(ui.poll('frame')==nil)",0,true));
    fixture=scroll_fixture();
    fixture.ui_pointers[1]=(ryz_app_pointer_t){.interrupted=true};
    fixture.ui_pointers[2].has_position=true; fixture.ui_pointers[2].x=40; fixture.ui_pointers[2].y=90;
    update_ok(update_run(&fixture,"for i=1,3 do assert(ui.poll('frame')==nil) end",0,true));
    const char *invalid[]={"'missing'","'title'","'apps'","false","12","{}","''",
        "'frame\\0x'","string.rep('x',25)","'frame','extra'"};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);++i) {
        char body[256]; snprintf(body,sizeof(body),"ui.poll(%s)",invalid[i]);
        update_rejected(body);
    }
    puts("APP_UI_SCROLL_PASS: bounded opt-in region, deltas/jitter, button priority, capture cancellation and strict args");
}

#define VIEWPORT_MODEL \
    "scene={id='scroll',background=0,objects={" \
    "{id='list',kind='viewport',x=8,y=30,width=224,height=138,content_height=284}," \
    "{id='a',parent='list',kind='button',x=0,y=0,width=108,height=44,text='A'}," \
    "{id='b',parent='list',kind='button',x=116,y=144,width=108,height=44,text='B'}," \
    "{id='last',parent='list',kind='label',x=0,y=240,width=108,height=16,text='END',font='body_12',line_height=16}," \
    "{id='thumb',kind='box',x=234,y=30,width=2,height=66}," \
    "{id='fixed',kind='button',x=8,y=172,width=224,height=44,text='CANCEL'}}}; "
#define VIEWPORT_SETUP VIEWPORT_MODEL "generation=ui.mount(scene); "

static void viewport_behavior(void)
{
    fixture_t fixture=update_fixture();
    fixture.ui_pointers[0].y=50;
    fixture.ui_pointers[1]=(ryz_app_pointer_t){.phase=RYZ_APP_POINTER_UP,
        .has_position=true,.x=40,.y=54};
    update_ok(update_run(&fixture,VIEWPORT_SETUP
        "assert(ui.poll('list')==nil); local e=ui.poll('list'); "
        "assert(e.kind=='activate' and e.id=='a')",0,true));
    fixture=(fixture_t){.ui_pointer_count=8,.ui_pointers={
        {.phase=RYZ_APP_POINTER_DOWN,.pressed=true,.has_position=true,.x=40,.y=50},
        {.phase=RYZ_APP_POINTER_MOVE,.pressed=true,.has_position=true,.x=40,.y=20},
        {.phase=RYZ_APP_POINTER_MOVE,.pressed=true,.has_position=true,.x=40,.y=10},
        {.phase=RYZ_APP_POINTER_UP},
        {.phase=RYZ_APP_POINTER_MOVE,.has_position=true,.x=145,.y=50},
        {.phase=RYZ_APP_POINTER_DOWN,.pressed=true,.has_position=true,.x=145,.y=50},
        {.phase=RYZ_APP_POINTER_UP},
        {.phase=RYZ_APP_POINTER_UP}}};
    update_ok(update_run(&fixture,VIEWPORT_SETUP
        "assert(ui.poll('list')==nil); local e=ui.poll('list'); "
        "assert(e.kind=='scroll' and e.id=='list' and e.dy==-30); "
        "ui.update(generation,{{id='list',scroll_y=146},{id='a',foreground=123},{id='thumb',y=102,height=66}}); "
        "assert(ui.poll('list').dy==-10); assert(ui.poll('list')==nil); "
        "assert(ui.poll('list')==nil); assert(ui.poll('list')==nil); "
        "e=ui.poll('list'); assert(e.kind=='activate' and e.id=='b'); assert(ui.poll('list')==nil)",0,true));
    assert(fixture.scene.objects[0].scroll_y==146 && fixture.scene.objects[4].y==102);
    const char *revoke[]={
        "ui.update(generation,{{id='list',visible=false}})",
        "ui.update(generation,{{id='list',enabled=false}})",
        "ui.update(generation,{{id='list',scroll_y=1}})",
        "ui.mount(scene)",
    };
    for(unsigned i=0;i<sizeof(revoke)/sizeof(*revoke);++i) {
        fixture=scroll_fixture(); fixture.ui_pointers[0].x=40; fixture.ui_pointers[0].y=50;
        char body[3000];
        snprintf(body,sizeof(body),"%s assert(ui.poll('list')==nil); %s; "
            "assert(ui.poll('list')==nil); assert(ui.poll('list')==nil)",VIEWPORT_SETUP,revoke[i]);
        update_ok(update_run(&fixture,body,0,true));
    }
    fixture=update_fixture(); fixture.ui_pointers[0].y=20;
    update_ok(update_run(&fixture,VIEWPORT_SETUP
        "assert(ui.poll('list')==nil); assert(ui.poll('list')==nil)",0,true));
    fixture=update_fixture(); fixture.ui_pointers[0].y=190;
    update_ok(update_run(&fixture,VIEWPORT_SETUP
        "ui.update(generation,{{id='list',scroll_y=146}}); "
        "assert(ui.poll('list')==nil); assert(ui.poll('list').id=='fixed')",0,true));
    const char *invalid[]={
        "scene.objects[1].content_height=1025",
        "scene.objects[1].scroll_y=147",
        "scene.objects[2].parent='missing'",
        "scene.objects[1].parent='list'",
        "scene.objects[2].parent='fixed'",
        "scene.objects[2].width=225",
        "scene.objects[4].y=280",
        "scene.objects[4].line_height=11",
        "scene.objects[4].text='bad\\t'",
    };
    for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);++i) {
        fixture=update_fixture(); char body[3000];
        snprintf(body,sizeof(body),"%s %s; ui.mount(scene)",VIEWPORT_MODEL,invalid[i]);
        ryz_lua_result_t result=update_run(&fixture,body,0,true);
        assert(!result.ok && !strcmp(result.phase,"runtime") && fixture.ui_mounts==1);
    }
    const char *invalid_patch[]={"{id='list',scroll_y=147}","{id='thumb',y=230,height=11}",
        "{id='a',y=1}","{id='a',scroll_y=1}"};
    for(unsigned i=0;i<sizeof(invalid_patch)/sizeof(*invalid_patch);++i) {
        fixture=update_fixture(); char body[3000];
        snprintf(body,sizeof(body),"%s ui.update(generation,{{id='thumb',y=40},%s})",
                 VIEWPORT_SETUP,invalid_patch[i]);
        ryz_lua_result_t result=update_run(&fixture,body,0,true);
        assert(!result.ok && fixture.ui_updates==0 && fixture.scene.objects[4].y==30);
    }
    fixture=update_fixture();
    update_ok(update_run(&fixture,VIEWPORT_MODEL
        "scene.objects[4].text='TWO\\nLINES'; scene.objects[4].height=32; "
        "scene.objects[7]={id='board',kind='image',x=76,y=42,width=88,height=88,asset='rootmaker_a_face'}; "
        "scene.objects[8]={id='divider',kind='image',x=4,y=18,width=232,height=2,asset='monitor_divider'}; "
        "generation=ui.mount(scene)",0,true));
    const char *bad_asset[]={"asset='unknown',width=88,height=88", "asset='rootmaker_a_face',width=87,height=88"};
    for(unsigned i=0;i<2;++i) {
        fixture=update_fixture(); char body[3000];
        snprintf(body,sizeof(body),"%s scene.objects[7]={id='image',kind='image',x=0,y=0,%s}; ui.mount(scene)",VIEWPORT_MODEL,bad_asset[i]);
        ryz_lua_result_t result=update_run(&fixture,body,0,true);
        assert(!result.ok && fixture.ui_mounts==1);
    }
    puts("APP_UI_VIEWPORT_PASS: clipped child hits, tap/drag arbitration, fixed controls, atomic bounded patches and builtin assets");
}

static void source_capacity(void)
{
    char bytes[RYZ_LUA_SOURCE_MAX+1];
    memset(bytes,' ',sizeof(bytes)); memcpy(bytes,"-- ryz-app/1\nreturn\n",19);
    fixture_t fixture={0};
    const ryz_app_platform_t platform={.context=&fixture,.resize=tracked_resize,
        .now_ms=fixture_now,.pause_ms=fixture_pause};
    ryz_lua_result_t result;
    ryz_app_execute(bytes,RYZ_LUA_SOURCE_MAX,"@exact-limit.lua",0,&platform,&result);
    assert(result.ok && !live_bytes && result.peak_bytes<RYZ_LUA_HEAP_LIMIT);
    ryz_app_execute(bytes,sizeof(bytes),"@over-limit.lua",0,&platform,&result);
    assert(!result.ok && !strcmp(result.phase,"init") && strstr(result.error,"source too large") && !live_bytes);
    puts("APP_SOURCE_CAPACITY_PASS: exact 16384 bytes accepted, 16385 rejected, unchanged bounded heap");
}

static bool cancel_at_20(void *context) { return fixture_now(context) >= 20; }

static void coroutine_contract(void)
{
    fixture_t fixture = {0};
    ryz_app_platform_t platform = {.context=&fixture,.resize=tracked_resize,
        .now_ms=fixture_now,.pause_ms=fixture_pause};
    ryz_lua_result_t result;
    const char *good = "-- ryz-app/1\n"
        "local b=require('ryzobee'); local c=require('coroutine'); "
        "local main,is_main=c.running(); assert(is_main and not c.isyieldable()); "
        "local co=c.create(function(x) assert(c.isyieldable()); b.sleep_ms(1); "
        "local a,z=c.yield(x,nil,3); return a,z end); "
        "assert(c.status(co)=='suspended'); local ok,a,b,d=c.resume(co,7); "
        "assert(ok and a==7 and b==nil and d==3); ok,a,b=c.resume(co,9,10); "
        "assert(ok and a==9 and b==10 and c.status(co)=='dead'); assert(not c.resume(co)); "
        "local f=c.wrap(function(a) c.yield(a,nil,5); return 8 end); "
        "local a,b,d=f(2); assert(a==2 and b==nil and d==5 and f()==8); "
        "local co=c.create(function() error('normal error') end); "
        "local ok,err=c.resume(co); assert(not ok and string.find(err,'normal error')); "
        "local closed=0; co=c.create(function() local x <close> = setmetatable({}, "
        "{__close=function() closed=closed+1 end}); c.yield() end); "
        "assert(c.resume(co)); assert(c.close(co)); assert(closed==1); "
        "assert(c.status(co)=='dead'); print('COROUTINE_OK')";
    ryz_app_execute(good,strlen(good),"@coroutine.lua",100,&platform,&result);
    if (!result.ok) fprintf(stderr,"coroutine semantics: %s\n",result.error);
    assert(result.ok && strstr(result.output,"COROUTINE_OK") && !live_bytes);
    const char *loops[] = {
        "local co=coroutine.create(function() while true do end end); coroutine.resume(co)",
        "coroutine.wrap(function() while true do end end)()",
        "local co=coroutine.create(function() local inner=coroutine.create(function() while true do end end); coroutine.resume(inner); error('caught') end); coroutine.resume(co)",
        "local co=coroutine.create(function() local x <close> = setmetatable({}, {__close=function() while true do end end}); coroutine.yield() end); coroutine.resume(co); coroutine.close(co)",
        "local co=coroutine.create(function() while true do coroutine.yield() end end); while true do coroutine.resume(co) end",
        "while true do coroutine.wrap(function() return 1 end)() end",
    };
    for (unsigned mode=0;mode<3;++mode) for (unsigned i=0;i<sizeof(loops)/sizeof(*loops);++i) {
        char source[1024]; snprintf(source,sizeof(source),"-- ryz-app/1\n%s; print('ESCAPED')",loops[i]);
        fixture.now=0;
        platform.cancelled=mode==1 ? cancel_at_20 : NULL;
        platform.finished=mode==2 ? cancel_at_20 : NULL;
        ryz_app_execute(source,strlen(source),"@coroutine-stop.lua",40,&platform,&result);
        assert(!strcmp(result.phase,mode==0 ? "timeout" : mode==1 ? "stopped" : "done"));
        assert(result.ok==(mode==2) && !strstr(result.output,"ESCAPED") && !live_bytes);
        assert(result.peak_bytes <= RYZ_LUA_HEAP_LIMIT && result.elapsed_ms <= 41);
    }
    puts("APP_COROUTINE_PASS: upstream values/status/close; nested resume/wrap/close cannot swallow stop, timeout or completion");
}

static void hardware_unavailable_contract(void)
{
    fixture_t fixture={0};
    ryz_app_platform_t platform={.context=&fixture,.resize=tracked_resize,
        .now_ms=fixture_now,.pause_ms=fixture_pause};
    ryz_lua_result_t result;
    const char *source="-- ryz-app/1\n"
        "for _,name in ipairs({'wifi','ble'}) do local n=require(name); "
        "assert(n.configure==nil and n.connect==nil and n.disconnect==nil and n.scan==nil); "
        "local value,code=n.is_connected(); assert(value==false and code=='unavailable'); "
        "local count=0; for k in pairs(n) do count=count+1; assert(k=='is_connected') end; assert(count==1) end; "
        "local imu=require('imu'); for _,fn in ipairs({imu.init,imu.read,imu.deinit}) do "
        "local value,code=fn(); assert(value==nil and code=='unavailable') end";
    ryz_app_execute(source,strlen(source),"@hardware.lua",100,&platform,&result);
    assert(result.ok && !live_bytes);
    puts("APP_HARDWARE_PASS: no adapter is unavailable, no network mutations or credentials exposed");
}

static void tool_font_contract(void)
{
    fixture_t fixture = {0};
    ryz_app_platform_t platform = {.context=&fixture,.resize=tracked_resize,
        .now_ms=fixture_now,.pause_ms=fixture_pause,.cancelled=fixture_false,
        .ui_mount=fixture_ui_mount,.ui_close=fixture_ui_close};
    const char *script = "-- ryz-app/1\nlocal ui=require('ui'); "
        "for i,font in ipairs({'button_16','medium_12','mono_medium_13'}) do "
        "assert(ui.mount{id='font',background=0,objects={{id='text',kind='label',"
        "x=8,y=8,width=224,height=24,text='SCAN PINS',foreground=65535,font=font}}}) end";
    ryz_lua_result_t result;
    ryz_app_execute(script,strlen(script),"@tool-fonts.lua",0,&platform,&result);
    if(!result.ok) fprintf(stderr,"tool fonts: %s\n",result.error);
    assert(result.ok && fixture.ui_mounts==3 && live_bytes==0);
}

static void box_width_contract(void)
{
    const char *model = "local g=ui.mount{id='meter',background=0,objects={"
        "{id='bar',kind='box',x=8,y=8,width=1,height=4},"
        "{id='view',kind='viewport',x=8,y=30,width=100,height=60,content_height=100},"
        "{id='inner',kind='box',parent='view',x=5,y=10,width=1,height=4},"
        "{id='tag',kind='label',x=8,y=110,width=100,height=20,text='READY'}}}; ";
    char source[2048];
    fixture_t fixture = update_fixture();
    snprintf(source,sizeof(source),"%s assert(ui.update(g,{{id='bar',width=232},"
        "{id='inner',width=95}})==g)",model);
    update_ok(update_run(&fixture,source,0,true));
    assert(fixture.scene.objects[0].width==232 && fixture.scene.objects[2].width==95);
    const char *invalid[] = {"{id='bar',width=233}","{id='bar',width=0}",
        "{id='bar',width=1.5}","{id='bar',width='2'}","{id='inner',width=96}",
        "{id='view',width=99}"};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);++i) {
        fixture=update_fixture();
        snprintf(source,sizeof(source),"%s ui.update(g,{{id='tag',text='CHANGED'},%s})",model,invalid[i]);
        ryz_lua_result_t result=update_run(&fixture,source,0,true);
        if(result.ok || fixture.ui_updates || fixture.scene.objects[0].width!=1)
            fprintf(stderr,"box width rejection %s: ok=%d updates=%u width=%u error=%s\n",
                invalid[i],result.ok,fixture.ui_updates,fixture.scene.objects[0].width,result.error);
        assert(!result.ok && fixture.ui_updates==0 && fixture.scene.objects[0].width==1);
        assert(!strcmp(fixture.scene.objects[3].text,"READY"));
    }
    puts("APP_UI_BOX_WIDTH_PASS: same-generation meters, screen/viewport bounds and atomic rejection");
}

int main(void)
{
    box_width_contract();
    tool_font_contract();
    coroutine_contract();
    hardware_unavailable_contract();
    fixture_t fixture = {
        .ui_pointers = {
            {.phase = RYZ_APP_POINTER_DOWN, .pressed = true,
             .has_position = true, .x = 40, .y = 90, .sampled_ms = 10},
            {.phase = RYZ_APP_POINTER_UP, .pressed = false,
             .has_position = false, .sampled_ms = 20},
        },
        .ui_pointer_count = 2,
        .ui_pointer_index = 2,
    };
    ryz_app_platform_t platform = {
        .context = &fixture,
        .resize = tracked_resize,
        .now_ms = fixture_now,
        .pause_ms = fixture_pause,
        .cancelled = fixture_false,
        .finished = fixture_false,
        .seed = 0x12345678,
        .pointer_read = fixture_pointer,
        .display_draw = fixture_draw,
        .display_show = fixture_show,
        .ui_mount = fixture_ui_mount,
        .ui_update = fixture_ui_update,
        .ui_pump = fixture_ui_pump,
        .ui_close = fixture_ui_close,
        .touch_demo = fixture_touch_demo,
        .mark = fixture_mark,
    };
    ryz_lua_result_t result;
    assert(ryz_app_is_source(source, strlen(source)));
    ryz_app_execute(source, strlen(source), "@app.lua", 0, &platform, &result);

    assert(result.ok && !strcmp(result.phase, "done") && result.error[0] == 0);
    assert(strstr(result.output, "APP_PASS\t12\t34\n"));
    assert(result.peak_bytes > 0 && result.peak_bytes <= RYZ_LUA_HEAP_LIMIT);
    assert(live_bytes == 0);
    assert(fixture.seed_mark == 0x12345678);
    assert(!strcmp(fixture.phase, "down"));
    assert(fixture.op_count == 3 && fixture.shows == 1);
    assert(fixture.ops[0].kind == RYZ_APP_DISPLAY_CLEAR && fixture.ops[0].color == 0x0841);
    assert(fixture.ops[1].kind == RYZ_APP_DISPLAY_RECT && fixture.ops[1].x == 1 && fixture.ops[1].y == 2 && fixture.ops[1].width == 3 && fixture.ops[1].height == 4 && fixture.ops[1].color == 0xf800);
    assert(fixture.ops[2].kind == RYZ_APP_DISPLAY_TEXT && fixture.ops[2].x == 5 && fixture.ops[2].y == 6 && fixture.ops[2].scale == 2 && fixture.ops[2].color == 0xffff && !strcmp(fixture.ops[2].text, "2048"));

    ryz_app_execute(sandbox_source, strlen(sandbox_source), "@sandbox.lua", 0, &platform, &result);
    assert(result.ok && !strcmp(result.phase, "done") && live_bytes == 0);
    ryz_app_execute(forbidden_source, strlen(forbidden_source), "@forbidden.lua", 0, &platform, &result);
    assert(!result.ok && !strcmp(result.phase, "runtime"));
    assert(strstr(result.error, "module not allowed: socket") && live_bytes == 0);

    ryz_app_execute(embedded_nul_module_source,
                    strlen(embedded_nul_module_source),
                    "@embedded-nul-module.lua", 0, &platform, &result);
    assert(!result.ok && !strcmp(result.phase, "runtime"));
    assert(strstr(result.error, "module not allowed") && live_bytes == 0);
    assert(fixture.ui_mounts == 0);

    ryz_app_execute(embedded_nul_scene_field_source,
                    strlen(embedded_nul_scene_field_source),
                    "@embedded-nul-scene-field.lua", 0, &platform, &result);
    assert(!result.ok && !strcmp(result.phase, "runtime"));
    assert(strstr(result.error, "ui scene contains an unknown field") &&
           live_bytes == 0);
    assert(fixture.ui_mounts == 0);

    fixture.ui_pointer_index = 0;
    ryz_app_execute(ui_source, strlen(ui_source), "@ui.lua", 0, &platform, &result);
    assert(result.ok && !strcmp(result.phase, "done") && live_bytes == 0);
    assert(strstr(result.output, "UI_PASS\tapps\n"));
    assert(fixture.ui_mounts == 1 && fixture.ui_pumps == 2 &&
           fixture.ui_closes == 1);
    assert(!strcmp(fixture.scene.id, "home") && fixture.scene.generation == 1);
    assert(fixture.scene.background == 0x0000 && fixture.scene.object_count == 3);
    assert(fixture.scene.objects[0].kind == RYZ_UI_BOX);
    assert(fixture.scene.objects[1].kind == RYZ_UI_LABEL &&
           fixture.scene.objects[1].font == RYZ_UI_FONT_TITLE_24);
    assert(fixture.scene.objects[2].kind == RYZ_UI_BUTTON &&
           fixture.scene.objects[2].border_width == 2 &&
           !strcmp(fixture.scene.objects[2].id, "apps"));

    ryz_app_execute(mixed_owner_source, strlen(mixed_owner_source),
                    "@mixed.lua", 0, &platform, &result);
    assert(!result.ok && !strcmp(result.phase, "runtime"));
    assert(strstr(result.error, "display.canvas conflicts with ui.scene"));
    assert(fixture.ui_mounts == 2 && fixture.ui_closes == 2 && live_bytes == 0);

    ryz_app_execute(display_then_ui_source, strlen(display_then_ui_source),
                    "@display-then-ui.lua", 0, &platform, &result);
    assert(!result.ok && !strcmp(result.phase, "runtime"));
    assert(strstr(result.error, "ui.scene conflicts with display.canvas"));
    assert(fixture.ui_mounts == 2 && fixture.ui_closes == 2 && live_bytes == 0);

    ryz_app_execute(touch_then_ui_poll_source,
                    strlen(touch_then_ui_poll_source),
                    "@touch-then-ui.lua", 0, &platform, &result);
    assert(!result.ok && !strcmp(result.phase, "runtime"));
    assert(strstr(result.error, "ui.events conflicts with touch.read"));
    assert(fixture.ui_mounts == 3 && fixture.ui_pumps == 2 &&
           fixture.ui_closes == 3 && live_bytes == 0);

    ryz_app_execute(ui_poll_then_touch_source,
                    strlen(ui_poll_then_touch_source),
                    "@ui-then-touch.lua", 0, &platform, &result);
    assert(!result.ok && !strcmp(result.phase, "runtime"));
    assert(strstr(result.error, "touch.read conflicts with ui.events"));
    assert(fixture.ui_mounts == 4 && fixture.ui_pumps == 3 &&
           fixture.ui_closes == 4 && live_bytes == 0);

    ryz_app_execute(disabled_touch_demo_then_ui_source,
                    strlen(disabled_touch_demo_then_ui_source),
                    "@touch-demo-disabled.lua", 0, &platform, &result);
    assert(result.ok && !strcmp(result.phase, "done"));
    assert(fixture.touch_demo_calls == 1 && !fixture.touch_demo_enabled);
    assert(fixture.ui_mounts == 5 && fixture.ui_closes == 5 && live_bytes == 0);

    ryz_app_execute(too_many_objects_source, strlen(too_many_objects_source),
                    "@too-many-objects.lua", 0, &platform, &result);
    assert(!result.ok && !strcmp(result.phase, "runtime"));
    assert(strstr(result.error, "ui scene exceeds 32 objects"));
    assert(fixture.ui_mounts == 5 && fixture.ui_closes == 5 && live_bytes == 0);

    ryz_app_execute(invalid_object_id_source, strlen(invalid_object_id_source),
                    "@invalid-object-id.lua", 0, &platform, &result);
    assert(!result.ok && !strcmp(result.phase, "runtime"));
    assert(strstr(result.error,
                  "scene and object ids must be lowercase identifiers up to 24 bytes"));
    assert(!strstr(result.error, "bad argument #"));
    assert(fixture.ui_mounts == 5 && fixture.ui_closes == 5 && live_bytes == 0);

    ryz_app_execute(embedded_nul_token_source,
                    strlen(embedded_nul_token_source),
                    "@embedded-nul-token.lua", 0, &platform, &result);
    assert(!result.ok && !strcmp(result.phase, "runtime"));
    assert(strstr(result.error, "unsupported ui object kind"));
    assert(fixture.ui_mounts == 5 && fixture.ui_closes == 5 && live_bytes == 0);

    fixture.now = 0;
    fixture.ui_mount_delay_ms = 5;
    ryz_app_execute(ui_mount_only_source, strlen(ui_mount_only_source),
                    "@slow-ui-mount.lua", 5, &platform, &result);
    assert(!result.ok && !strcmp(result.phase, "timeout"));
    assert(strstr(result.error, "execution timed out"));
    assert(fixture.ui_mounts == 6 && fixture.ui_closes == 6 && live_bytes == 0);

    fixture.now = 0;
    fixture.ui_mount_delay_ms = 0;
    fixture.ui_pump_delay_ms = 5;
    fixture.ui_pointer_index = 2;
    ryz_app_execute(ui_poll_once_source, strlen(ui_poll_once_source),
                    "@slow-ui-pump.lua", 5, &platform, &result);
    assert(!result.ok && !strcmp(result.phase, "timeout"));
    assert(strstr(result.error, "execution timed out"));
    assert(fixture.ui_mounts == 7 && fixture.ui_pumps == 4 &&
           fixture.ui_closes == 7 && live_bytes == 0);

    fixture.now = 0;
    fixture.ui_pump_delay_ms = 0;
    fixture.pointer_delay_ms = 5;
    ryz_app_execute(ui_poll_once_source, strlen(ui_poll_once_source),
                    "@slow-ui-pointer.lua", 5, &platform, &result);
    assert(!result.ok && !strcmp(result.phase, "timeout"));
    assert(strstr(result.error, "execution timed out"));
    assert(fixture.ui_mounts == 8 && fixture.ui_pumps == 5 &&
           fixture.ui_closes == 8 && live_bytes == 0);
    /* Public API tracer: an empty update retains the mounted generation. */
    const char update_source[] =
        "-- ryz-app/1\nlocal ui=require('ui'); "
        "local generation=ui.mount({id='home',background=0,objects={}}); "
        "assert(ui.update(generation,{})==generation)";
    fixture.pointer_delay_ms = 0;
    ryz_app_execute(update_source, strlen(update_source), "@ui-update.lua", 0, &platform, &result);
    if (!result.ok) fprintf(stderr, "ui.update tracer %s: %s\n", result.phase, result.error);
    assert(result.ok && !strcmp(result.phase, "done") && live_bytes == 0);
    assert(fixture.ui_updates == 0);

    fixture.ui_pointers[0] = (ryz_app_pointer_t){
        .phase = RYZ_APP_POINTER_NONE, .pressed = true,
        .interrupted = true, .sampled_ms = 30,
    };
    fixture.ui_pointer_count = 1;
    fixture.ui_pointer_index = 0;
    ryz_app_execute(interrupted_touch_source, strlen(interrupted_touch_source),
                    "@interrupted-touch.lua", 0, &platform, &result);
    assert(result.ok && !strcmp(result.phase, "done") && live_bytes == 0);

    fixture.ui_pointers[0] = (ryz_app_pointer_t){
        .phase = RYZ_APP_POINTER_DOWN, .pressed = true, .has_position = true,
        .x = 40, .y = 90, .sampled_ms = 40,
    };
    fixture.ui_pointers[1] = (ryz_app_pointer_t){
        .phase = RYZ_APP_POINTER_NONE, .pressed = true,
        .interrupted = true, .sampled_ms = 50,
    };
    fixture.ui_pointers[2] = (ryz_app_pointer_t){
        .phase = RYZ_APP_POINTER_UP, .sampled_ms = 60,
    };
    fixture.ui_pointer_count = 3;
    fixture.ui_pointer_index = 0;
    ryz_app_execute(interrupted_ui_source, strlen(interrupted_ui_source),
                    "@interrupted-ui.lua", 0, &platform, &result);
    assert(result.ok && !strcmp(result.phase, "done") && live_bytes == 0);
    update_behavior();
    update_validation();
    update_lifecycle();
    scroll_behavior();
    viewport_behavior();
    source_capacity();
    puts("APP_RUNTIME_PASS: bounded VM, raw display/touch and managed UI ownership");
    return 0;
}
