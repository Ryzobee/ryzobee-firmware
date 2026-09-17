#include "app_runtime.h"
#include "lua.h"
#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Real Lua VM/facade; only the trusted typed call and platform are adapted.
 * Cleanup below proves a recorded token survives Lua OOM, not that a device
 * worker or real Workbench asynchronous close has completed. */
typedef union { max_align_t alignment; size_t bytes; } allocation_t;
typedef struct {
    uint64_t now;
    unsigned calls, cleanups, cleaned_owned, checks, cancel_on_check, delay;
    bool callback_available, started, owned, poison_after_submit, poison;
    uint32_t recorded;
    ryz_tool_call_result_t code;
    int error;
    ryz_tool_request_t requests[32];
    ryz_i2c_scan_snapshot_t i2c;
    ryz_rgb_snapshot_t rgb;
    ryz_monitor_snapshot_t monitor;
    ryz_monitor_page_t page;
} fixture_t;
static fixture_t fixture;
static size_t live_bytes, allocations, fail_at;

static void *resize(void *pointer, size_t size)
{
    allocation_t *old = pointer ? (allocation_t *)pointer - 1 : NULL;
    size_t previous = old ? old->bytes : 0;
    if (!size) { live_bytes -= previous; free(old); return NULL; }
    ++allocations;
    if (size > previous && (fixture.poison || (fail_at && allocations >= fail_at))) return NULL;
    allocation_t *next = realloc(old,sizeof(*next)+size);
    if (!next) return NULL;
    next->bytes = size; live_bytes = live_bytes-previous+size;
    return next+1;
}
static uint64_t now(void *context) { return ((fixture_t *)context)->now; }
static void pause_ms(void *context, unsigned ms) { ((fixture_t *)context)->now += ms ? ms : 1; }
static bool cancelled(void *context)
{
    fixture_t *value = context; ++value->checks;
    return value->cancel_on_check && value->checks >= value->cancel_on_check;
}
static void cleanup(void *context)
{
    fixture_t *value = context; ++value->cleanups;
    if (value->owned) { assert(value->recorded == UINT32_MAX); ++value->cleaned_owned; value->owned = false; }
}
static ryz_tool_call_result_t callback(void *context, const ryz_tool_request_t *request, ryz_tool_reply_t *reply)
{
    fixture_t *value = context;
    assert(value->calls < 32); value->requests[value->calls++] = *request;
    memset(reply,0,sizeof(*reply));
    reply->error_code = value->error;
    reply->operation_id = reply->revision = reply->view_generation = UINT32_MAX;
    reply->started = value->started;
    if (request->action == RYZ_TOOL_I2C_STATUS) reply->state.i2c = value->i2c;
    else if (request->action == RYZ_TOOL_RGB_STATUS) reply->state.rgb = value->rgb;
    else if (request->action == RYZ_TOOL_MONITOR_STATUS) reply->state.monitor = value->monitor;
    else if (request->action == RYZ_TOOL_MONITOR_READ) reply->state.page = value->page;
    else if (value->code == RYZ_TOOL_CALL_OK) {
        value->owned = true; value->recorded = reply->operation_id;
        if (value->poison_after_submit) value->poison = true;
    }
    value->now += value->delay;
    return value->code;
}
static void reset(void)
{
    assert(!live_bytes);
    memset(&fixture,0,sizeof(fixture)); fixture.callback_available = true;
    fixture.i2c.config = RYZ_I2C_SCAN_DEFAULT_CONFIG; fixture.i2c.config_revision = 1;
    fixture.monitor.config = fixture.monitor.capture_config = fixture.monitor.requested_config = RYZ_MONITOR_DEFAULT_CONFIG;
    fixture.monitor.config_revision = 1;
    fixture.page.stream.session_id = fixture.page.stream.view_generation = UINT32_MAX;
    allocations = fail_at = 0;
}
static ryz_lua_result_t run(const char *body, uint32_t timeout)
{
    char source[8192];
    int length = snprintf(source,sizeof(source),"-- ryz-app/1\n%s",body);
    assert(length > 0 && (size_t)length < sizeof(source));
    ryz_app_platform_t platform = {.context=&fixture,.resize=resize,.now_ms=now,.pause_ms=pause_ms,
        .cancelled=cancelled,.cleanup=cleanup,.tool_call=fixture.callback_available ? callback : NULL};
    ryz_lua_result_t result;
    ryz_app_execute(source,(size_t)length,"@tools.lua",timeout,&platform,&result);
    assert(!live_bytes);
    return result;
}
static void expect_ok(ryz_lua_result_t result)
{
    if (!result.ok) fprintf(stderr,"unexpected %s: %s\n",result.phase,result.error);
    assert(result.ok && !strcmp(result.phase,"done"));
}
static void invalid(const char *source)
{
    reset(); ryz_lua_result_t result = run(source,0);
    assert(!result.ok && !strcmp(result.phase,"runtime") && !fixture.calls && !fixture.owned);
}
static void module_case(void)
{
    reset(); fixture.callback_available = false;
    expect_ok(run("local t=require('tools'); assert(t==require('tools')); "
        "assert(t.open==nil and t.owner_session==nil and t.rgb.cleanup==nil and t.rgb.off==nil); "
        "assert(t.monitor.write==nil and t.i2c.read==nil); "
        "assert(pcall==nil and xpcall==nil and io==nil and os==nil and package==nil and type(coroutine)=='table'); "
        "local r=t.i2c.status(); assert(not r.ok and r.code=='unavailable' and r.error_code==0 and r.config==nil)",0));
    assert(!fixture.calls && fixture.cleanups == 1);
    invalid("require('tools\\0ignored')"); invalid("require('unknown_peripheral')");
    reset();
    expect_ok(run("local p,e=require('uart').open{rx=17}; assert(p==nil and e=='unavailable'); "
        "p,e=require('gpio').open{pin=13}; assert(p==nil and e=='unavailable')",0));
    assert(!fixture.calls && !fixture.owned); /* Never routed to tool-specific C. */
}
static void actions(void)
{
    reset();
    expect_ok(run("local t=require('tools'); local function ack(r) "
        "assert(r.ok and r.code=='ok' and r.accepted and r.error_code==0); "
        "assert(r.operation_id=='4294967295' and r.revision=='4294967295' and r.view_generation=='4294967295') end; "
        "ack(t.i2c.configure{sda=4,scl=5,hz=400000,expected_revision='4294967295'}); "
        "ack(t.i2c.start('4294967295')); assert(t.i2c.status().phase=='unstarted'); ack(t.i2c.cancel('4294967295')); "
        "ack(t.rgb.set{red=255,green=97,blue=0}); assert(t.rgb.status().phase=='unstarted'); "
        "ack(t.monitor.configure{source='uart',rx=2,tx=3,baud=115200,expected_revision='4294967295'}); "
        "ack(t.monitor.start('4294967295')); assert(t.monitor.status().phase=='unstarted'); "
        "ack(t.monitor.stop('4294967295')); "
        "ack(t.monitor.pause{operation_id='4294967295',view_generation='4294967295',paused=true}); "
        "ack(t.monitor.clear{operation_id='4294967295',view_generation='4294967295'}); "
        "local r=t.monitor.read{operation_id='4294967295',view_generation='4294967295',after_sequence='0'}; "
        "assert(r.ok and r.count==0 and #r.records==0)",0));
    assert(fixture.calls == 13 && fixture.cleaned_owned == 1);
    for (unsigned i=0;i<13;++i) assert(fixture.requests[i].action == (ryz_tool_action_t)i);
    assert(fixture.requests[0].expected_revision == UINT32_MAX && fixture.requests[0].config.i2c.hz == 400000);
    assert(fixture.requests[1].expected_revision == UINT32_MAX && fixture.requests[3].operation_id == UINT32_MAX);
    assert(fixture.requests[4].config.rgb.red == 255 && fixture.requests[4].config.rgb.green == 97 && !fixture.requests[4].config.rgb.blue);
    assert(fixture.requests[6].config.monitor.source == RYZ_MONITOR_UART && fixture.requests[6].config.monitor.rx == 2 && fixture.requests[6].config.monitor.tx == 3);
    assert(fixture.requests[6].config.monitor.baud == 115200 && fixture.requests[6].expected_revision == UINT32_MAX);
    assert(fixture.requests[9].operation_id == UINT32_MAX && fixture.requests[10].view_generation == UINT32_MAX && fixture.requests[10].paused);
    assert(fixture.requests[12].operation_id == UINT32_MAX && !fixture.requests[12].after_sequence);
    reset(); expect_ok(run("assert(require('tools').monitor.configure{source='system',expected_revision='1'}.ok)",0));
    assert(fixture.requests[0].config.monitor.source == RYZ_MONITOR_SYSTEM && fixture.requests[0].config.monitor.rx == -1 &&
           fixture.requests[0].config.monitor.tx == -1 && !fixture.requests[0].config.monitor.baud);
}
static void validation(void)
{
    const char *tokens[] = {"0","01","-1","+1"," 1","1 ","1.0","1e2","4294967296","99999999999","","1\\0x"};
    for (unsigned i=0;i<sizeof(tokens)/sizeof(tokens[0]);++i) {
        char source[192]; snprintf(source,sizeof(source),"require('tools').i2c.start('%s')",tokens[i]); invalid(source);
    }
    const char *bad[] = {
        "require('tools').i2c.start(1)","require('tools').i2c.start(true)","require('tools').i2c.start(nil)",
        "require('tools').i2c.status({})","require('tools').rgb.status('extra')",
        "require('tools').rgb.set{red='255',green=0,blue=0}","require('tools').rgb.set{red=1.5,green=0,blue=0}",
        "require('tools').rgb.set{red=256,green=0,blue=0}","require('tools').rgb.set{red=-1,green=0,blue=0}",
        "require('tools').rgb.set{red=true,green=0,blue=0}","require('tools').rgb.set{red=0,green=0}",
        "require('tools').rgb.set{red=0,green=0,blue=0,gpio=45}",
        "require('tools').rgb.set{red=0,green=0,blue=0,['red\\0x']=1}",
        "require('tools').rgb.set{red=0,green=0,blue=0,[1]=1}",
        "require('tools').rgb.set(setmetatable({red=0,green=0,blue=0},{__index=function() error('METAMETHOD_RAN') end}))",
        "require('tools').i2c.configure{sda=4,scl=5,hz=123456,expected_revision='1'}",
        "require('tools').i2c.configure{sda=49,scl=5,hz=100000,expected_revision='1'}",
        "require('tools').i2c.configure{sda=4,scl=5,hz=100000,expected_revision=1}",
        "require('tools').monitor.configure{source='system',rx=-1,expected_revision='1'}",
        "require('tools').monitor.configure{source='system\\0uart',expected_revision='1'}",
        "require('tools').monitor.configure{source='uart',rx=2,tx=3,baud=12345,expected_revision='1'}",
        "require('tools').monitor.configure{source='uart',rx=2,tx=3,baud=115200,expected_revision='1',owner_session='1'}",
        "require('tools').monitor.pause{operation_id='1',view_generation='1',paused=1}",
        "require('tools').monitor.clear{operation_id='1',view_generation='0'}",
        "require('tools').monitor.read{operation_id='1',view_generation='1',after_sequence=0}",
        "require('tools').monitor.read{operation_id='1',view_generation='1',after_sequence='0',limit=9}",
    };
    for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) invalid(bad[i]);
    reset(); ryz_lua_result_t result=run(bad[14],0);
    assert(strstr(result.error,"metatable") && !strstr(result.error,"METAMETHOD_RAN") && !fixture.calls);
}
static void errors(void)
{
    const char *codes[]={"ok","busy","invalid","state","unavailable","no_memory","failed"};
    for (unsigned i=1;i<=6;++i) {
        reset(); fixture.code=(ryz_tool_call_result_t)i; fixture.error=-77;
        char source[256]; snprintf(source,sizeof(source),
            "local r=require('tools').rgb.set{red=1,green=2,blue=3}; assert(not r.ok and r.code=='%s' and r.error_code==-77 and r.accepted==nil and r.operation_id==nil)",codes[i]);
        expect_ok(run(source,0)); assert(fixture.calls==1 && !fixture.cleaned_owned);
    }
    reset(); fixture.code=(ryz_tool_call_result_t)99;
    expect_ok(run("assert(require('tools').rgb.status().code=='failed')",0));
}
static void populate(void)
{
    fixture.started=true;
    fixture.i2c.scan_id=UINT32_MAX; fixture.i2c.config_revision=3;
    fixture.i2c.scan_config=(ryz_i2c_scan_config_t){4,5,400000}; fixture.i2c.scan_config_revision=2;
    fixture.i2c.phase=RYZ_I2C_SCAN_COMPLETED; fixture.i2c.nack_count=112; fixture.i2c.completed_addresses=112;
    fixture.i2c.queued_ms=INT64_MAX;
    for (unsigned address=8;address<=119;++address) fixture.i2c.results[address]=RYZ_I2C_SCAN_NACK;
    fixture.rgb.request_id=UINT32_MAX; fixture.rgb.last_completed_id=UINT32_MAX-1;
    fixture.rgb.phase=RYZ_RGB_CLEANING; fixture.rgb.resources_held=true; fixture.rgb.cleanup_error=-1;
    fixture.rgb.last_completed_color=(ryz_rgb_color_t){255,97,0};
    fixture.monitor.config=(ryz_monitor_config_t){RYZ_MONITOR_UART,4,5,115200};
    fixture.monitor.capture_config=RYZ_MONITOR_DEFAULT_CONFIG;
    fixture.monitor.requested_config=(ryz_monitor_config_t){RYZ_MONITOR_UART,2,3,9600};
    fixture.monitor.config_revision=3; fixture.monitor.session_id=UINT32_MAX;
    fixture.monitor.operation_id=UINT32_MAX; fixture.monitor.operation=RYZ_MONITOR_OP_CONFIGURE;
    fixture.monitor.phase=RYZ_MONITOR_RECONFIGURING; fixture.monitor.operation_pending=true;
    fixture.monitor.cleanup_error=-1; fixture.monitor.restore_error=-2;
    fixture.monitor.started_ms=INT64_MAX;
    fixture.monitor.stream=(ryz_monitor_stream_info_t){.session_id=UINT32_MAX,.config_revision=2,.view_generation=UINT32_MAX,
        .source=RYZ_MONITOR_SYSTEM,.paused=true,.capture_gap_since_boot=true,.chunks=UINT64_MAX,.bytes=9007199254740993ULL};
    fixture.monitor.uart_events.fifo_overflow=UINT64_MAX; fixture.monitor.uart_events.events_may_be_lost=true;
}
static void status_case(void)
{
    reset(); populate();
    expect_ok(run("local t=require('tools'); local i=t.i2c.status(); "
        "assert(i.ok and i.started and i.scan_id=='4294967295' and i.empty and #i.results.nack==112); "
        "assert(i.config_revision=='3' and i.scan_config_revision=='2' and i.config.sda==41 and i.scan_config.sda==4); "
        "assert(i.queued_ms=='9223372036854775807'); "
        "local r=t.rgb.status(); assert(r.phase=='cleaning' and r.request_id=='4294967295' and r.last_completed_id=='4294967294'); "
        "assert(r.resources_held and r.cleanup_error==-1 and not r.output_known and r.color.red==255); "
        "local m=t.monitor.status(); assert(m.phase=='reconfiguring' and m.operation_pending); "
        "assert(m.config.source=='uart' and m.capture_config.source=='system' and m.requested_config.rx==2); "
        "assert(m.config_revision=='3' and m.stream.config_revision=='2' and m.stream.session_id=='4294967295'); "
        "assert(m.stream.chunks=='18446744073709551615' and m.stream.bytes=='9007199254740993'); "
        "assert(m.capture_gap_since_boot and m.stream.capture_gap_since_boot and m.stream.paused); "
        "assert(m.started_ms=='9223372036854775807' and m.cleanup_error==-1 and m.restore_error==-2); "
        "assert(m.uart_events.fifo_overflow=='18446744073709551615' and m.uart_events.events_may_be_lost)",0));
    reset(); populate(); fixture.i2c.results[8]=RYZ_I2C_SCAN_ERROR; fixture.i2c.errors[8]=-37;
    expect_ok(run("local r=require('tools').i2c.status(); assert(not r.empty and r.results.error[1].address==8 and r.results.error[1].code==-37)",0));
    reset(); populate(); fixture.monitor.stream.session_id=0;
    expect_ok(run("local r=require('tools').monitor.status(); assert(r.ok and r.stream==nil and r.capture_gap_since_boot)",0));
    reset(); populate(); fixture.started=false;
    expect_ok(run("local t=require('tools'); local i=t.i2c.status(); assert(not i.started and i.config_revision=='1' and not i.empty and i.scan_config==nil); "
        "local r=t.rgb.status(); assert(not r.started and r.color==nil and not r.resources_held); "
        "local m=t.monitor.status(); assert(m.config_revision=='1' and m.stream==nil and m.capture_config==nil and m.requested_config==nil and not m.uart_events.events_may_be_lost)",0));
    reset(); populate(); fixture.i2c.results[8]=99;
    expect_ok(run("local r=require('tools').i2c.status(); assert(not r.ok and r.code=='failed' and r.results==nil)",0));
}
static void read_case(void)
{
    reset(); fixture.page.count=8; fixture.page.more=fixture.page.gap=true;
    fixture.page.next_sequence=UINT32_MAX;
    for (unsigned i=0;i<8;++i) {
        ryz_monitor_record_t *record=&fixture.page.records[i]; record->sequence=UINT32_MAX-i;
        record->length=96; record->captured_ms=INT64_MAX; record->truncated=i==7;
        for (unsigned j=0;j<96;++j) record->bytes[j]=(uint8_t)j;
    }
    const char *read_source="local r=require('tools').monitor.read{operation_id='4294967295',view_generation='4294967295',after_sequence='0'}; "
        "assert(r.ok and r.count==8 and #r.records==8 and r.more and r.gap and r.next_sequence=='4294967295'); "
        "for i=1,8 do local s=r.records[i].bytes; assert(#s==96 and string.byte(s,1)==0 and string.byte(s,96)==95) end; "
        "assert(r.records[1].captured_ms=='9223372036854775807' and r.records[8].truncated)";
    expect_ok(run(read_source,0)); assert(fixture.calls==1);
    for (unsigned mode=0;mode<4;++mode) {
        reset(); fixture.page.count=mode==0 ? 9 : 1;
        if (mode==1) fixture.page.records[0].length=97;
        if (mode==2) fixture.page.stream.source=(ryz_monitor_source_t)99;
        if (mode==3) fixture.page.stream.session_id=0;
        expect_ok(run("local r=require('tools').monitor.read{operation_id='4294967295',view_generation='4294967295',after_sequence='0'}; assert(not r.ok and r.code=='failed' and r.records==nil)",0));
    }
}
static const char *mutation="require('tools').rgb.set{red=255,green=97,blue=0}";
static void expect_finalizer_rejected(ryz_lua_result_t result)
{
    assert(!result.ok && !strcmp(result.phase,"runtime"));
    assert(strstr(result.error,"__gc finalizers are not supported; use explicit cleanup"));
}
static void finalizer_case(void)
{
    const char *values[]={
        "function() set{red=1,green=2,blue=3} end", "false", "1",
        "setmetatable({}, {__call=function() set{red=1,green=2,blue=3} end})",
    };
    for (unsigned i=0;i<sizeof(values)/sizeof(values[0]);++i) {
        reset();
        char source[512];
        snprintf(source,sizeof(source),
            "local set=require('tools').rgb.set; keeper=setmetatable({}, {__gc=%s}); "
            "collectgarbage('collect'); error('REGISTRATION_WAS_ACCEPTED')",values[i]);
        expect_finalizer_rejected(run(source,0));
        assert(fixture.cleanups==1 && !fixture.calls && !fixture.owned);
    }
    /* Rejection must not lose a C-owned token from an earlier accepted call. */
    reset();
    expect_finalizer_rejected(run("local set=require('tools').rgb.set; set{red=255,green=97,blue=0}; "
        "keeper=setmetatable({}, {__gc=function() set{red=1,green=2,blue=3} end})",0));
    assert(fixture.calls==1 && fixture.cleanups==1 && fixture.cleaned_owned==1 && !fixture.owned);
    assert(fixture.requests[0].action==RYZ_TOOL_RGB_SET && fixture.requests[0].config.rgb.red==255);
    /* Raw writes alone do not register a finalizer; reinstallation still must reject it. */
    reset();
    expect_finalizer_rejected(run("local set=require('tools').rgb.set; local mt={}; "
        "local v=setmetatable({},mt); rawset(getmetatable(v),'__gc',function() set{red=1,green=2,blue=3} end); "
        "collectgarbage('collect'); setmetatable(v,mt)",0));
    assert(fixture.cleanups==1 && !fixture.calls && !fixture.owned);
}
static void metatable_case(void)
{
    reset();
    expect_ok(run("local set=require('tools').rgb.set; "
        "local mt={__index={submit=set},__call=function(_,color) return set(color) end}; "
        "local value={}; assert(setmetatable(value,mt)==value and getmetatable(value)==mt); "
        "assert(value.submit{red=1,green=2,blue=3}.ok); "
        "assert(value{red=4,green=5,blue=6}.ok); "
        "assert(setmetatable(value,nil)==value and getmetatable(value)==nil); "
        "local raw_mt=setmetatable({}, {__index=function() error('MUST_NOT_LOOK_UP_GC') end}); "
        "assert(setmetatable({},raw_mt)); "
        "local late_mt={}; local late=setmetatable({},late_mt); "
        "rawset(late_mt,'__gc',function() set{red=9,green=9,blue=9} end); "
        "late=nil; collectgarbage('collect')",0));
    assert(fixture.calls==2 && fixture.cleanups==1 && fixture.cleaned_owned==1 && !fixture.owned);
    assert(fixture.requests[0].config.rgb.red==1 && fixture.requests[1].config.rgb.red==4);
    /* __close is an ordinary, hook-controlled scope exit, not a GC finalizer. */
    reset();
    expect_ok(run("local set=require('tools').rgb.set; do "
        "local value <close> = setmetatable({}, {__close=function(_,err) "
        "assert(err==nil); assert(set{red=7,green=8,blue=9}.ok) end}) end",0));
    assert(fixture.calls==1 && fixture.requests[0].config.rgb.red==7);
    assert(fixture.cleanups==1 && fixture.cleaned_owned==1 && !fixture.owned);
}
static void explicit_cleanup_case(void)
{
    const char *ending[]={"", "error('EXPECTED_AFTER_EXPLICIT_STOP')"};
    for (unsigned i=0;i<sizeof(ending)/sizeof(ending[0]);++i) {
        reset();
        char source[768];
        snprintf(source,sizeof(source),
            "local t=require('tools'); local scan=t.i2c.start('1'); assert(scan.ok); "
            "assert(t.i2c.cancel(scan.operation_id).ok); "
            "local monitor=t.monitor.start('1'); assert(monitor.ok); "
            "assert(t.monitor.stop(monitor.operation_id).ok); %s",ending[i]);
        ryz_lua_result_t result=run(source,0);
        if (!i) expect_ok(result);
        else assert(!result.ok && !strcmp(result.phase,"runtime") &&
                    strstr(result.error,"EXPECTED_AFTER_EXPLICIT_STOP"));
        assert(fixture.calls==4 && fixture.cleanups==1 && fixture.cleaned_owned==1 && !fixture.owned);
        assert(fixture.requests[0].action==RYZ_TOOL_I2C_START && fixture.requests[0].expected_revision==1);
        assert(fixture.requests[1].action==RYZ_TOOL_I2C_CANCEL && fixture.requests[1].operation_id==UINT32_MAX);
        assert(fixture.requests[2].action==RYZ_TOOL_MONITOR_START && fixture.requests[2].expected_revision==1);
        assert(fixture.requests[3].action==RYZ_TOOL_MONITOR_STOP && fixture.requests[3].operation_id==UINT32_MAX);
    }
}
static void cancel_case(void)
{
    for (unsigned check=1;check<=3;++check) {
        reset(); fixture.cancel_on_check=check;
        ryz_lua_result_t result=run(mutation,0);
        assert(!result.ok && !strcmp(result.phase,"stopped"));
        assert(fixture.calls==(check==3 ? 1U : 0U) && fixture.cleaned_owned==(check==3 ? 1U : 0U));
    }
    reset(); fixture.delay=5; ryz_lua_result_t result=run(mutation,5);
    assert(!result.ok && !strcmp(result.phase,"timeout") && fixture.calls==1 && fixture.cleaned_owned==1);
}
static void oom(void)
{
    reset(); fixture.poison_after_submit=true;
    ryz_lua_result_t result=run(mutation,0);
    assert(!result.ok && !strcmp(result.phase,"memory") && fixture.calls==1);
    assert(fixture.recorded==UINT32_MAX && fixture.cleanups==1 && fixture.cleaned_owned==1 && !fixture.owned);
    reset(); expect_ok(run(mutation,0)); size_t total=allocations;
    unsigned before=0, after=0;
    for (size_t point=1;point<=total;++point) {
        reset(); fail_at=point; result=run(mutation,0);
        assert(!fixture.owned && fixture.calls<=1);
        if (fixture.calls) { assert(fixture.recorded==UINT32_MAX && fixture.cleaned_owned==1); ++after; }
        else { assert(!fixture.cleaned_owned); ++before; }
    }
    assert(before && after); printf("APP_TOOLS_OOM %zu points before=%u after=%u\n",total,before,after);
}
int main(int argc,char **argv)
{
    assert(argc==2 && sizeof(lua_Integer)==4 && sizeof(lua_Number)==4);
    reset();
    if (!strcmp(argv[1],"module")) module_case();
    else if (!strcmp(argv[1],"actions")) actions();
    else if (!strcmp(argv[1],"validation")) validation();
    else if (!strcmp(argv[1],"errors")) errors();
    else if (!strcmp(argv[1],"status")) status_case();
    else if (!strcmp(argv[1],"read")) read_case();
    else if (!strcmp(argv[1],"finalizer")) finalizer_case();
    else if (!strcmp(argv[1],"metatable")) metatable_case();
    else if (!strcmp(argv[1],"explicit_cleanup")) explicit_cleanup_case();
    else if (!strcmp(argv[1],"cancel")) cancel_case();
    else { assert(!strcmp(argv[1],"oom")); oom(); }
    assert(!live_bytes); printf("APP_TOOLS_PASS %s\n",argv[1]); return 0;
}
