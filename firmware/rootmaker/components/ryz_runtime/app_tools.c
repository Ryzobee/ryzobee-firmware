#include "app_tools.h"
#include "lauxlib.h"
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    ryz_tool_call_fn call;
    void *context;
    void (*check)(lua_State *);
    bool revoked;
} binding_t;

static const char binding_key;

static void number(lua_State *L, const char *key, lua_Integer value)
{ lua_pushinteger(L,value); lua_setfield(L,-2,key); }
static void boolean(lua_State *L, const char *key, bool value)
{ lua_pushboolean(L,value); lua_setfield(L,-2,key); }
static void text(lua_State *L, const char *key, const char *value)
{ lua_pushstring(L,value); lua_setfield(L,-2,key); }
static void decimal(lua_State *L, const char *key, uint64_t value)
{
    char buffer[24];
    int length = snprintf(buffer,sizeof(buffer),"%" PRIu64,value);
    if (length < 1 || (size_t)length >= sizeof(buffer)) luaL_error(L,"invalid tool counter");
    lua_pushlstring(L,buffer,(size_t)length); lua_setfield(L,-2,key);
}
static void timestamp(lua_State *L, const char *key, int64_t value)
{
    char buffer[24];
    int length = snprintf(buffer,sizeof(buffer),"%" PRId64,value);
    if (length < 1 || (size_t)length >= sizeof(buffer)) luaL_error(L,"invalid tool timestamp");
    lua_pushlstring(L,buffer,(size_t)length); lua_setfield(L,-2,key);
}
static bool equal(const char *value, size_t length, const char *expected)
{ return length == strlen(expected) && !memcmp(value,expected,length); }

static void plain_table(lua_State *L)
{
    luaL_checktype(L,1,LUA_TTABLE);
    if (lua_getmetatable(L,1)) luaL_error(L,"tool arguments must not have a metatable");
}
static void fields(lua_State *L, const char *const *allowed)
{
    lua_pushnil(L);
    while (lua_next(L,1)) {
        size_t length = 0;
        const char *key = lua_type(L,-2) == LUA_TSTRING ? lua_tolstring(L,-2,&length) : NULL;
        bool found = false;
        if (key) for (const char *const *name = allowed; *name; ++name)
            if (equal(key,length,*name)) { found = true; break; }
        if (!found) luaL_error(L,"tool arguments contain an unknown field");
        lua_pop(L,1);
    }
}
static void raw_field(lua_State *L, const char *name)
{ lua_pushstring(L,name); lua_rawget(L,1); }
static uint32_t token(lua_State *L, int index, bool zero)
{
    if (lua_type(L,index) != LUA_TSTRING) luaL_error(L,"tool token must be a decimal string");
    size_t length = 0;
    const char *value = lua_tolstring(L,index,&length);
    if (!length || length > 10 || (length > 1 && value[0] == '0'))
        luaL_error(L,"tool token must be canonical uint32 decimal");
    uint32_t result = 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned digit = (unsigned char)value[i] - (unsigned)'0';
        if (digit > 9 || result > (UINT32_MAX - digit) / 10)
            luaL_error(L,"tool token must be canonical uint32 decimal");
        result = result * 10 + digit;
    }
    if (!zero && !result) luaL_error(L,"tool token must be positive");
    return result;
}
static uint32_t token_field(lua_State *L, const char *name, bool zero)
{
    raw_field(L,name); uint32_t value = token(L,-1,zero); lua_pop(L,1); return value;
}
static lua_Integer integer_field(lua_State *L, const char *name, int minimum, int maximum)
{
    raw_field(L,name);
    if (!lua_isinteger(L,-1)) luaL_error(L,"%s must be an integer",name);
    lua_Integer value = lua_tointeger(L,-1);
    if (value < minimum || value > maximum) luaL_error(L,"%s is out of range",name);
    lua_pop(L,1); return value;
}
static bool boolean_field(lua_State *L, const char *name)
{
    raw_field(L,name); luaL_checktype(L,-1,LUA_TBOOLEAN);
    bool value = lua_toboolean(L,-1); lua_pop(L,1); return value;
}
static bool baud_valid(uint32_t baud)
{
    const uint32_t values[] = {1200,2400,4800,9600,19200,38400,57600,115200,230400,460800,921600};
    for (unsigned i = 0; i < sizeof(values)/sizeof(values[0]); ++i) if (baud == values[i]) return true;
    return false;
}
static void parse(lua_State *L, ryz_tool_request_t *request)
{
    ryz_tool_action_t action = request->action;
    bool status = action == RYZ_TOOL_I2C_STATUS || action == RYZ_TOOL_RGB_STATUS || action == RYZ_TOOL_MONITOR_STATUS;
    if (lua_gettop(L) != (status ? 0 : 1)) luaL_error(L,"wrong number of tool arguments");
    if (status) return;
    if (action == RYZ_TOOL_I2C_START || action == RYZ_TOOL_MONITOR_START) {
        request->expected_revision = token(L,1,false); return;
    }
    if (action == RYZ_TOOL_I2C_CANCEL || action == RYZ_TOOL_MONITOR_STOP) {
        request->operation_id = token(L,1,false); return;
    }
    plain_table(L);
    if (action == RYZ_TOOL_I2C_CONFIGURE) {
        const char *const allowed[] = {"sda","scl","hz","expected_revision",NULL}; fields(L,allowed);
        request->config.i2c.sda = (int)integer_field(L,"sda",0,48);
        request->config.i2c.scl = (int)integer_field(L,"scl",0,48);
        request->config.i2c.hz = (uint32_t)integer_field(L,"hz",100000,400000);
        if (request->config.i2c.hz != 100000 && request->config.i2c.hz != 400000)
            luaL_error(L,"I2C hz must be 100000 or 400000");
        request->expected_revision = token_field(L,"expected_revision",false);
    } else if (action == RYZ_TOOL_RGB_SET) {
        const char *const allowed[] = {"red","green","blue",NULL}; fields(L,allowed);
        request->config.rgb.red = (uint8_t)integer_field(L,"red",0,255);
        request->config.rgb.green = (uint8_t)integer_field(L,"green",0,255);
        request->config.rgb.blue = (uint8_t)integer_field(L,"blue",0,255);
    } else if (action == RYZ_TOOL_MONITOR_CONFIGURE) {
        raw_field(L,"source");
        if (lua_type(L,-1) != LUA_TSTRING) luaL_error(L,"Monitor source must be system or uart");
        size_t length;
        const char *source = lua_tolstring(L,-1,&length);
        bool system = equal(source,length,"system"), uart = equal(source,length,"uart");
        lua_pop(L,1);
        if (!system && !uart) luaL_error(L,"Monitor source must be system or uart");
        const char *const system_fields[] = {"source","expected_revision",NULL};
        const char *const uart_fields[] = {"source","expected_revision","rx","tx","baud",NULL};
        fields(L,system ? system_fields : uart_fields);
        request->config.monitor = RYZ_MONITOR_DEFAULT_CONFIG;
        request->expected_revision = token_field(L,"expected_revision",false);
        if (uart) {
            request->config.monitor.source = RYZ_MONITOR_UART;
            request->config.monitor.rx = (int)integer_field(L,"rx",0,48);
            request->config.monitor.tx = (int)integer_field(L,"tx",0,48);
            request->config.monitor.baud = (uint32_t)integer_field(L,"baud",1200,921600);
            if (!baud_valid(request->config.monitor.baud)) luaL_error(L,"unsupported Monitor baud");
        }
    } else {
        const char *const pause_fields[] = {"operation_id","view_generation","paused",NULL};
        const char *const clear_fields[] = {"operation_id","view_generation",NULL};
        const char *const read_fields[] = {"operation_id","view_generation","after_sequence",NULL};
        fields(L,action == RYZ_TOOL_MONITOR_PAUSE ? pause_fields :
                  action == RYZ_TOOL_MONITOR_CLEAR ? clear_fields : read_fields);
        request->operation_id = token_field(L,"operation_id",false);
        request->view_generation = token_field(L,"view_generation",false);
        if (action == RYZ_TOOL_MONITOR_PAUSE) request->paused = boolean_field(L,"paused");
        if (action == RYZ_TOOL_MONITOR_READ) request->after_sequence = token_field(L,"after_sequence",true);
    }
}

static const char *source_name(ryz_monitor_source_t source)
{ return source == RYZ_MONITOR_SYSTEM ? "system" : source == RYZ_MONITOR_UART ? "uart" : NULL; }
static void i2c_config(lua_State *L, const char *name, const ryz_i2c_scan_config_t *config)
{
    lua_createtable(L,0,3);
    number(L,"sda",config->sda); number(L,"scl",config->scl); number(L,"hz",config->hz);
    lua_setfield(L,-2,name);
}
static void monitor_config(lua_State *L, const char *name, const ryz_monitor_config_t *config)
{
    lua_createtable(L,0,4);
    text(L,"source",source_name(config->source)); number(L,"rx",config->rx);
    number(L,"tx",config->tx); number(L,"baud",config->baud); lua_setfield(L,-2,name);
}
static void color(lua_State *L, const char *name, ryz_rgb_color_t value)
{
    lua_createtable(L,0,3);
    number(L,"red",value.red); number(L,"green",value.green); number(L,"blue",value.blue);
    lua_setfield(L,-2,name);
}
static void stream(lua_State *L, const ryz_monitor_stream_info_t *value)
{
    lua_createtable(L,0,17);
    text(L,"source",source_name(value->source)); decimal(L,"session_id",value->session_id);
    decimal(L,"config_revision",value->config_revision); decimal(L,"view_generation",value->view_generation);
    boolean(L,"accepting",value->accepting); boolean(L,"paused",value->paused);
    boolean(L,"sequence_exhausted",value->sequence_exhausted);
    boolean(L,"capture_gap_since_boot",value->capture_gap_since_boot);
    decimal(L,"first_sequence",value->first_sequence); decimal(L,"last_sequence",value->last_sequence);
    number(L,"retained_records",value->retained_records); decimal(L,"chunks",value->chunks);
    decimal(L,"bytes",value->bytes); decimal(L,"overwritten_chunks",value->overwritten_chunks);
    decimal(L,"overwritten_bytes",value->overwritten_bytes); decimal(L,"truncated_bytes",value->truncated_bytes);
    decimal(L,"filtered_logs",value->filtered_logs); lua_setfield(L,-2,"stream");
}
static const char *const i2c_phases[] = {"idle","queued","running","cancelling","completed","cancelled","failed","releasing"};
static const char *const rgb_phases[] = {"idle","queued","sending","completed","failed","cleaning","cleaned"};
static const char *const monitor_phases[] = {"idle","starting","running","reconfiguring","stopping","stopped","failed"};
static const char *const monitor_operations[] = {"none","start","configure","stop"};

static bool valid_reply(ryz_tool_action_t action, const ryz_tool_reply_t *reply)
{
    if (action == RYZ_TOOL_I2C_STATUS && reply->started) {
        const ryz_i2c_scan_snapshot_t *value = &reply->state.i2c;
        if ((unsigned)value->phase >= sizeof(i2c_phases)/sizeof(i2c_phases[0])) return false;
        for (unsigned address = RYZ_I2C_SCAN_FIRST; address <= RYZ_I2C_SCAN_LAST; ++address)
            if (value->results[address] > RYZ_I2C_SCAN_ERROR) return false;
    } else if (action == RYZ_TOOL_RGB_STATUS && reply->started) {
        if ((unsigned)reply->state.rgb.phase >= sizeof(rgb_phases)/sizeof(rgb_phases[0])) return false;
    } else if (action == RYZ_TOOL_MONITOR_STATUS && reply->started) {
        const ryz_monitor_snapshot_t *value = &reply->state.monitor;
        if ((unsigned)value->phase >= sizeof(monitor_phases)/sizeof(monitor_phases[0]) ||
            (unsigned)value->operation >= sizeof(monitor_operations)/sizeof(monitor_operations[0]) ||
            !source_name(value->config.source) || (value->session_id && !source_name(value->capture_config.source)) ||
            (value->operation && !source_name(value->requested_config.source)) ||
            (value->stream.session_id && (!source_name(value->stream.source) || value->stream.retained_records > RYZ_MONITOR_CAPACITY))) return false;
    } else if (action == RYZ_TOOL_MONITOR_READ) {
        const ryz_monitor_page_t *page = &reply->state.page;
        if (page->count > RYZ_MONITOR_PAGE_RECORDS || !page->stream.session_id || !page->stream.view_generation ||
            !source_name(page->stream.source) || page->stream.retained_records > RYZ_MONITOR_CAPACITY) return false;
        for (unsigned i = 0; i < page->count; ++i) if (page->records[i].length > RYZ_MONITOR_RECORD_BYTES) return false;
    }
    return true;
}
static void i2c_status(lua_State *L, const ryz_tool_reply_t *reply)
{
    ryz_i2c_scan_snapshot_t initial = {.config = RYZ_I2C_SCAN_DEFAULT_CONFIG,.config_revision = 1};
    const ryz_i2c_scan_snapshot_t *value = reply->started ? &reply->state.i2c : &initial;
    text(L,"phase",reply->started ? i2c_phases[value->phase] : "unstarted");
    decimal(L,"scan_id",value->scan_id); i2c_config(L,"config",&value->config);
    decimal(L,"config_revision",value->config_revision); decimal(L,"scan_config_revision",value->scan_config_revision);
    if (value->scan_config_revision) i2c_config(L,"scan_config",&value->scan_config);
    boolean(L,"resources_held",value->resources_held); number(L,"cleanup_error",value->cleanup_error); number(L,"error",value->error);
    timestamp(L,"queued_ms",value->queued_ms); timestamp(L,"started_ms",value->started_ms); timestamp(L,"finished_ms",value->finished_ms);
    number(L,"completed_addresses",value->completed_addresses); number(L,"probe_calls",value->probe_calls);
    number(L,"busy_responses",value->busy_responses); number(L,"current_address",value->current_address);
    number(L,"ack_count",value->ack_count); number(L,"nack_count",value->nack_count); number(L,"busy_count",value->busy_count);
    number(L,"timeout_count",value->timeout_count); number(L,"error_count",value->error_count);
    unsigned unscanned = 0, nacks = 0;
    lua_createtable(L,0,6);
    const char *const kinds[] = {"unscanned","ack","nack","busy","timeout","error"};
    for (unsigned kind = 0; kind < 6; ++kind) {
        lua_createtable(L,0,0); unsigned count = 0;
        for (unsigned address = RYZ_I2C_SCAN_FIRST; address <= RYZ_I2C_SCAN_LAST; ++address) {
            if (value->results[address] != kind) continue;
            if (kind == RYZ_I2C_SCAN_ERROR) {
                lua_createtable(L,0,2); number(L,"address",address); number(L,"code",value->errors[address]);
            } else lua_pushinteger(L,address);
            lua_rawseti(L,-2,++count);
        }
        if (kind == RYZ_I2C_SCAN_UNSCANNED) unscanned = count;
        if (kind == RYZ_I2C_SCAN_NACK) nacks = count;
        lua_setfield(L,-2,kinds[kind]);
    }
    lua_setfield(L,-2,"results"); number(L,"unscanned_count",unscanned);
    boolean(L,"empty",reply->started && value->phase == RYZ_I2C_SCAN_COMPLETED && !value->error &&
        value->completed_addresses == RYZ_I2C_SCAN_ADDRESSES && nacks == RYZ_I2C_SCAN_ADDRESSES &&
        value->nack_count == RYZ_I2C_SCAN_ADDRESSES && !value->ack_count && !value->busy_count && !value->timeout_count &&
        !value->error_count && !unscanned && !value->resources_held && !value->cleanup_error);
}
static void rgb_status(lua_State *L, const ryz_tool_reply_t *reply)
{
    ryz_rgb_snapshot_t initial = {0};
    const ryz_rgb_snapshot_t *value = reply->started ? &reply->state.rgb : &initial;
    text(L,"phase",reply->started ? rgb_phases[value->phase] : "unstarted");
    decimal(L,"request_id",value->request_id); decimal(L,"last_completed_id",value->last_completed_id);
    if (value->request_id) color(L,"requested",value->requested);
    if (value->last_completed_id) color(L,"color",value->last_completed_color);
    number(L,"error",value->error); number(L,"cleanup_error",value->cleanup_error);
    boolean(L,"resources_held",value->resources_held); boolean(L,"output_known",value->output_known);
    timestamp(L,"queued_ms",value->queued_ms); timestamp(L,"finished_ms",value->finished_ms);
}
static void monitor_status(lua_State *L, const ryz_tool_reply_t *reply)
{
    ryz_monitor_snapshot_t initial = {.config = RYZ_MONITOR_DEFAULT_CONFIG,.config_revision = 1};
    const ryz_monitor_snapshot_t *value = reply->started ? &reply->state.monitor : &initial;
    text(L,"phase",reply->started ? monitor_phases[value->phase] : "unstarted");
    monitor_config(L,"config",&value->config); decimal(L,"config_revision",value->config_revision);
    if (value->session_id) monitor_config(L,"capture_config",&value->capture_config);
    if (value->operation) monitor_config(L,"requested_config",&value->requested_config);
    decimal(L,"session_id",value->session_id); decimal(L,"operation_id",value->operation_id);
    text(L,"operation",monitor_operations[value->operation]); boolean(L,"operation_pending",value->operation_pending);
    boolean(L,"source_active",value->source_active); boolean(L,"resources_held",value->resources_held);
    number(L,"operation_error",value->operation_error); number(L,"cleanup_error",value->cleanup_error); number(L,"restore_error",value->restore_error);
    timestamp(L,"started_ms",value->started_ms); timestamp(L,"operation_finished_ms",value->operation_finished_ms);
    boolean(L,"capture_gap_since_boot",value->stream.capture_gap_since_boot);
    if (value->session_id && value->stream.session_id == value->session_id) stream(L,&value->stream);
    const ryz_monitor_uart_events_t *events = &value->uart_events;
    lua_createtable(L,0,6);
    decimal(L,"fifo_overflow",events->fifo_overflow); decimal(L,"buffer_full",events->buffer_full);
    decimal(L,"frame_error",events->frame_error); decimal(L,"parity_error",events->parity_error); decimal(L,"break_events",events->break_events);
    boolean(L,"events_may_be_lost",events->events_may_be_lost); lua_setfield(L,-2,"uart_events");
}
static void monitor_page(lua_State *L, const ryz_monitor_page_t *page)
{
    stream(L,&page->stream); decimal(L,"next_sequence",page->next_sequence);
    boolean(L,"gap",page->gap); boolean(L,"more",page->more); number(L,"count",page->count);
    lua_createtable(L,page->count,0);
    for (unsigned i = 0; i < page->count; ++i) {
        const ryz_monitor_record_t *record = &page->records[i];
        lua_createtable(L,0,5); decimal(L,"sequence",record->sequence); timestamp(L,"captured_ms",record->captured_ms);
        number(L,"length",record->length); boolean(L,"truncated",record->truncated);
        lua_pushlstring(L,(const char *)record->bytes,record->length); lua_setfield(L,-2,"bytes");
        lua_rawseti(L,-2,i+1);
    }
    lua_setfield(L,-2,"records");
}
static int call_tool(lua_State *L)
{
    binding_t *binding = lua_touserdata(L,lua_upvalueindex(1));
    if (binding->revoked) return luaL_error(L,"tool binding is closed");
    binding->check(L);
    ryz_tool_request_t request = {.action = (ryz_tool_action_t)lua_tointeger(L,lua_upvalueindex(2))};
    parse(L,&request);
    binding->check(L);
    ryz_tool_reply_t reply = {0};
    ryz_tool_call_result_t result = binding->call ? binding->call(binding->context,&request,&reply) : RYZ_TOOL_CALL_UNAVAILABLE;
    binding->check(L);
    if ((unsigned)result > RYZ_TOOL_CALL_FAILED || (result == RYZ_TOOL_CALL_OK && !valid_reply(request.action,&reply)))
        result = RYZ_TOOL_CALL_FAILED;
    const char *const codes[] = {"ok","busy","invalid","state","unavailable","no_memory","failed"};
    /* Deliberately after callback: Lua allocation may fail here. The trusted
     * callback recorded accepted ownership already; the job must clean it up. */
    lua_createtable(L,0,32);
    boolean(L,"ok",result == RYZ_TOOL_CALL_OK); text(L,"code",codes[result]); number(L,"error_code",reply.error_code);
    if (result != RYZ_TOOL_CALL_OK) return 1;
    bool status = request.action == RYZ_TOOL_I2C_STATUS || request.action == RYZ_TOOL_RGB_STATUS || request.action == RYZ_TOOL_MONITOR_STATUS;
    if (status) {
        boolean(L,"started",reply.started);
        if (request.action == RYZ_TOOL_I2C_STATUS) i2c_status(L,&reply);
        else if (request.action == RYZ_TOOL_RGB_STATUS) rgb_status(L,&reply);
        else monitor_status(L,&reply);
    } else if (request.action == RYZ_TOOL_MONITOR_READ) monitor_page(L,&reply.state.page);
    else {
        boolean(L,"accepted",true); decimal(L,"operation_id",reply.operation_id);
        decimal(L,"revision",reply.revision); decimal(L,"view_generation",reply.view_generation);
    }
    return 1;
}
int ryz_app_tools_open(lua_State *L, ryz_tool_call_fn callback, void *context, void (*check)(lua_State *))
{
    binding_t *binding = lua_newuserdatauv(L,sizeof(*binding),0);
    *binding = (binding_t){.call=callback,.context=context,.check=check};
    int binding_index = lua_absindex(L,-1);
    lua_pushvalue(L,binding_index);
    lua_rawsetp(L,LUA_REGISTRYINDEX,&binding_key);
    lua_createtable(L,0,3);
    const struct { const char *name; ryz_tool_action_t first, last; } groups[] = {
        {"i2c",RYZ_TOOL_I2C_CONFIGURE,RYZ_TOOL_I2C_CANCEL},
        {"rgb",RYZ_TOOL_RGB_SET,RYZ_TOOL_RGB_STATUS},
        {"monitor",RYZ_TOOL_MONITOR_CONFIGURE,RYZ_TOOL_MONITOR_READ},
    };
    const char *const names[] = {"configure","start","status","cancel","set","status",
        "configure","start","status","stop","pause","clear","read"};
    for (unsigned group = 0; group < sizeof(groups)/sizeof(groups[0]); ++group) {
        lua_createtable(L,0,groups[group].last-groups[group].first+1);
        for (unsigned action = groups[group].first; action <= (unsigned)groups[group].last; ++action) {
            lua_pushvalue(L,binding_index); lua_pushinteger(L,action); lua_pushcclosure(L,call_tool,2);
            lua_setfield(L,-2,names[action]);
        }
        lua_setfield(L,-2,groups[group].name);
    }
    lua_remove(L,binding_index);
    return 1;
}

void ryz_app_tools_revoke(lua_State *L)
{
    lua_rawgetp(L,LUA_REGISTRYINDEX,&binding_key);
    binding_t *binding = lua_touserdata(L,-1);
    if (binding) binding->revoked = true;
    lua_pop(L,1);
}
