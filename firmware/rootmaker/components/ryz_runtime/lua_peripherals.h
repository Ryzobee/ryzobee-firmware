#pragma once

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "ryz_peripheral.h"
#include "lua.h"
#include "lauxlib.h"

typedef struct {
    ryz_peripheral_call_fn call;
    void *context;
    void (*check)(lua_State *L);
    bool revoked;
    struct { uint32_t id; ryz_peripheral_kind_t kind; bool closing; } handles[RYZ_PERIPHERAL_HANDLE_MAX];
} ryz_lua_peripheral_binding_t;

typedef struct {
    ryz_lua_peripheral_binding_t *binding;
    uint32_t id;
    unsigned slot;
} ryz_lua_peripheral_handle_t;

static const char *const peripheral_log_levels[]={"error","warn","info","debug","verbose",NULL};

static int peripheral_error(lua_State *L, ryz_peripheral_result_t error)
{
    static const char *names[] = {"ok", "unavailable", "invalid", "busy", "closed",
        "timeout", "not_found", "no_memory", "unsupported", "failed"};
    lua_pushnil(L);
    lua_pushstring(L, (unsigned)error < sizeof(names)/sizeof(names[0]) ? names[error] : "failed");
    return 2;
}

static void peripheral_check(lua_State *L, ryz_lua_peripheral_binding_t *binding)
{
    if (binding->revoked) luaL_error(L, "application is closing");
    binding->check(L);
}

static int peripheral_int(lua_State *L, int table, const char *key, int fallback, int low, int high)
{
    lua_getfield(L, table, key);
    if (lua_isnil(L, -1)) { lua_pop(L, 1); return fallback; }
    if (!lua_isinteger(L, -1)) luaL_error(L, "%s must be an integer", key);
    lua_Integer value = lua_tointeger(L, -1);
    if (value < low || value > high) luaL_error(L, "%s out of range", key);
    lua_pop(L, 1);
    return (int)value;
}

static unsigned peripheral_enum(lua_State *L, int table, const char *key,
    unsigned fallback, const char *const *names)
{
    lua_getfield(L, table, key);
    if (lua_isnil(L, -1)) { lua_pop(L, 1); return fallback; }
    if (lua_type(L, -1) != LUA_TSTRING) luaL_error(L, "%s must be a string", key);
    size_t length; const char *text = lua_tolstring(L, -1, &length);
    for (unsigned i=0; names[i]; ++i) if (strlen(names[i])==length && !memcmp(text,names[i],length)) {
        lua_pop(L,1); return i;
    }
    return (unsigned)luaL_error(L, "unsupported %s", key);
}

static bool peripheral_bool(lua_State *L,int table,const char *key,bool fallback)
{
    lua_getfield(L,table,key);
    if(lua_isnil(L,-1)) { lua_pop(L,1); return fallback; }
    if(!lua_isboolean(L,-1)) luaL_error(L,"%s must be a boolean",key);
    bool value=lua_toboolean(L,-1); lua_pop(L,1); return value;
}

static void peripheral_counter(lua_State *L,const char *key,uint32_t value)
{
    char text[11]; snprintf(text,sizeof(text),"%lu",(unsigned long)value);
    lua_pushstring(L,text); lua_setfield(L,-2,key);
}

static void peripheral_fields(lua_State *L, const char *allowed)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    if (lua_gettop(L)!=1) luaL_error(L, "open takes one configuration table");
    if (lua_getmetatable(L, 1)) luaL_error(L, "configuration must be a plain table");
    lua_pushnil(L);
    while (lua_next(L, 1)) {
        size_t n; const char *key = lua_type(L,-2)==LUA_TSTRING ? lua_tolstring(L,-2,&n) : NULL;
        char token[40];
        if (!key || !n || n>32 || memchr(key,0,n) || memchr(key,'|',n)) luaL_error(L, "invalid configuration key");
        snprintf(token,sizeof(token),"|%s|",key);
        if (!strstr(allowed,token)) luaL_error(L, "unknown configuration key: %s",key);
        lua_pop(L,1);
    }
}

static int peripheral_close(lua_State *L)
{
    ryz_lua_peripheral_handle_t *handle = luaL_checkudata(L,1,"ryz.peripheral");
    ryz_lua_peripheral_binding_t *binding=handle->binding;
    peripheral_check(L,binding);
    if(lua_gettop(L)!=1) return luaL_error(L,"close takes no arguments");
    if (handle->slot >= RYZ_PERIPHERAL_HANDLE_MAX || binding->handles[handle->slot].id != handle->id) {
        lua_pushboolean(L,1); return 1;
    }
    ryz_peripheral_request_t request={.op=RYZ_PERIPHERAL_CLOSE,.handle=handle->id,
        .kind=binding->handles[handle->slot].kind};
    /* A failed close may already have dismantled part of a driver. Only
     * close may be retried; a Host peer cannot revive ordinary operations. */
    binding->handles[handle->slot].closing=true;
    ryz_peripheral_reply_t reply={0};
    ryz_peripheral_result_t result=binding->call(binding->context,&request,&reply);
    if (result==RYZ_PERIPHERAL_OK) binding->handles[handle->slot].id=0;
    peripheral_check(L,binding);
    if (result!=RYZ_PERIPHERAL_OK) return peripheral_error(L,result);
    lua_pushboolean(L,1); return 1;
}

static int peripheral_integer_arg(lua_State *L,int index,int low,int high)
{
    if(!lua_isinteger(L,index)) luaL_error(L,"argument %d must be an integer",index);
    lua_Integer value=lua_tointeger(L,index);
    if(value<low || value>high) luaL_error(L,"argument %d out of range",index);
    return (int)value;
}

static const uint8_t *peripheral_bytes(lua_State *L,int index,size_t *length)
{
    if(lua_type(L,index)!=LUA_TSTRING) luaL_error(L,"argument %d must be a byte string",index);
    const uint8_t *data=(const uint8_t *)lua_tolstring(L,index,length);
    if(!*length || *length>RYZ_PERIPHERAL_BUFFER_MAX) luaL_error(L,"data must contain 1..256 bytes");
    return data;
}

static int peripheral_method(lua_State *L)
{
    ryz_lua_peripheral_handle_t *handle=luaL_checkudata(L,1,"ryz.peripheral");
    ryz_lua_peripheral_binding_t *binding=handle->binding;
    peripheral_check(L,binding);
    if(handle->slot>=RYZ_PERIPHERAL_HANDLE_MAX || !handle->id ||
        binding->handles[handle->slot].id!=handle->id ||
        binding->handles[handle->slot].closing) return peripheral_error(L,RYZ_PERIPHERAL_CLOSED);
    ryz_peripheral_request_t request={.kind=binding->handles[handle->slot].kind,
        .handle=handle->id,.op=(ryz_peripheral_op_t)lua_tointeger(L,lua_upvalueindex(1))};
    int count=lua_gettop(L);
    uint8_t rgb[3];
    if(request.kind==RYZ_PERIPHERAL_GPIO && request.op==RYZ_PERIPHERAL_READ) {
        if(count!=1) return luaL_error(L,"read takes no arguments");
    } else if(request.kind==RYZ_PERIPHERAL_GPIO && request.op==RYZ_PERIPHERAL_WRITE) {
        if(count!=2) return luaL_error(L,"write takes a level");
        request.value=peripheral_integer_arg(L,2,0,1);
    } else if((request.kind==RYZ_PERIPHERAL_UART || request.kind==RYZ_PERIPHERAL_LOG) && request.op==RYZ_PERIPHERAL_READ) {
        if(count>3) return luaL_error(L,"read takes length and optional timeout_ms");
        request.read_length=lua_isnoneornil(L,2) ? RYZ_PERIPHERAL_BUFFER_MAX :
            (unsigned)peripheral_integer_arg(L,2,1,RYZ_PERIPHERAL_BUFFER_MAX);
        request.timeout_ms=lua_isnoneornil(L,3) ? 0 :
            (unsigned)peripheral_integer_arg(L,3,0,RYZ_PERIPHERAL_WAIT_MAX_MS);
    } else if(request.kind==RYZ_PERIPHERAL_UART && request.op==RYZ_PERIPHERAL_WRITE) {
        if(count!=2 || lua_type(L,2)!=LUA_TSTRING) return luaL_error(L,"write takes a byte string");
        request.data=peripheral_bytes(L,2,&request.length);
    } else if(request.kind==RYZ_PERIPHERAL_TIMER && request.op==RYZ_PERIPHERAL_POLL) {
        if(count!=1) return luaL_error(L,"poll takes no arguments");
    } else if(request.kind==RYZ_PERIPHERAL_PWM && request.op==RYZ_PERIPHERAL_SET_DUTY) {
        if(count!=2) return luaL_error(L,"set_duty takes a duty value");
        request.value=peripheral_integer_arg(L,2,0,1000);
    } else if(request.kind==RYZ_PERIPHERAL_LED && request.op==RYZ_PERIPHERAL_WRITE) {
        if(count!=4) return luaL_error(L,"LED write takes red, green and blue");
        for(unsigned i=0;i<3;++i) rgb[i]=(uint8_t)peripheral_integer_arg(L,(int)i+2,0,255);
        request.data=rgb; request.length=3;
    } else if(request.kind==RYZ_PERIPHERAL_LED && request.op==RYZ_PERIPHERAL_STATUS) {
        if(count!=1) return luaL_error(L,"status takes no arguments");
    } else if(request.kind==RYZ_PERIPHERAL_ADC &&
        (request.op==RYZ_PERIPHERAL_READ || request.op==RYZ_PERIPHERAL_READ_MV)) {
        if(count!=1) return luaL_error(L,"ADC read takes no arguments");
    } else if(request.kind==RYZ_PERIPHERAL_SPI) {
        if(count<2 || count>3) return luaL_error(L,"SPI operation takes data/length and optional timeout_ms");
        if(request.op==RYZ_PERIPHERAL_READ)
            request.read_length=peripheral_integer_arg(L,2,1,RYZ_PERIPHERAL_BUFFER_MAX);
        else if(request.op==RYZ_PERIPHERAL_WRITE || request.op==RYZ_PERIPHERAL_TRANSFER) {
            request.data=peripheral_bytes(L,2,&request.length);
            if(request.op==RYZ_PERIPHERAL_TRANSFER) request.read_length=request.length;
        } else return luaL_error(L,"operation is not supported by SPI");
        request.timeout_ms=lua_isnoneornil(L,3) ? 10 :
            (unsigned)peripheral_integer_arg(L,3,0,RYZ_PERIPHERAL_WAIT_MAX_MS);
    } else if(request.kind==RYZ_PERIPHERAL_I2C) {
        request.address=peripheral_integer_arg(L,2,0x08,0x77);
        int timeout_index;
        if(request.op==RYZ_PERIPHERAL_PROBE) {
            if(count<2 || count>3) return luaL_error(L,"probe takes address and optional timeout_ms");
            timeout_index=3;
        } else if(request.op==RYZ_PERIPHERAL_READ) {
            if(count<3 || count>4) return luaL_error(L,"read takes address, length and optional timeout_ms");
            request.read_length=peripheral_integer_arg(L,3,1,RYZ_PERIPHERAL_BUFFER_MAX);
            timeout_index=4;
        } else if(request.op==RYZ_PERIPHERAL_WRITE || request.op==RYZ_PERIPHERAL_TRANSFER) {
            bool transfer=request.op==RYZ_PERIPHERAL_TRANSFER;
            if(count<(transfer ? 4 : 3) || count>(transfer ? 5 : 4))
                return luaL_error(L,"write/transfer requires address, bytes and optional timeout_ms");
            request.data=peripheral_bytes(L,3,&request.length);
            if(transfer) request.read_length=peripheral_integer_arg(L,4,1,RYZ_PERIPHERAL_BUFFER_MAX);
            timeout_index=transfer ? 5 : 4;
        } else return luaL_error(L,"operation is not supported by I2C");
        request.timeout_ms=lua_isnoneornil(L,timeout_index) ? 10 :
            (unsigned)peripheral_integer_arg(L,timeout_index,0,RYZ_PERIPHERAL_WAIT_MAX_MS);
    } else return luaL_error(L,"operation is not supported by this peripheral");
    ryz_peripheral_reply_t reply={0};
    ryz_peripheral_result_t result=binding->call(binding->context,&request,&reply);
    peripheral_check(L,binding);
    if(request.op==RYZ_PERIPHERAL_PROBE && result==RYZ_PERIPHERAL_NOT_FOUND) {
        lua_pushboolean(L,0); return 1;
    }
    if(result!=RYZ_PERIPHERAL_OK) return peripheral_error(L,result);
    if(request.kind==RYZ_PERIPHERAL_LED && request.op==RYZ_PERIPHERAL_STATUS) {
        static const char *const states[]={"idle","pending","ready","failed"};
        if((unsigned)reply.led.state>RYZ_PERIPHERAL_LED_FAILED) return peripheral_error(L,RYZ_PERIPHERAL_FAILED);
        lua_createtable(L,0,5);
        lua_pushstring(L,states[reply.led.state]); lua_setfield(L,-2,"state");
        lua_pushinteger(L,reply.led.red); lua_setfield(L,-2,"red");
        lua_pushinteger(L,reply.led.green); lua_setfield(L,-2,"green");
        lua_pushinteger(L,reply.led.blue); lua_setfield(L,-2,"blue");
        lua_pushboolean(L,reply.led.output_known); lua_setfield(L,-2,"output_known");
    } else if((request.kind==RYZ_PERIPHERAL_UART || request.kind==RYZ_PERIPHERAL_LOG) && request.op==RYZ_PERIPHERAL_READ) {
        if(reply.length>request.read_length) return peripheral_error(L,RYZ_PERIPHERAL_FAILED);
        lua_pushlstring(L,(const char *)reply.data,reply.length);
        lua_createtable(L,0,2);
        if(request.kind==RYZ_PERIPHERAL_UART) peripheral_counter(L,"error_events",reply.error_events);
        else peripheral_counter(L,"dropped_bytes",reply.dropped);
        lua_pushboolean(L,reply.loss_possible); lua_setfield(L,-2,"loss_possible");
        return 2;
    } else if((request.kind==RYZ_PERIPHERAL_I2C || request.kind==RYZ_PERIPHERAL_SPI) &&
        (request.op==RYZ_PERIPHERAL_READ || request.op==RYZ_PERIPHERAL_TRANSFER)) {
        if(reply.length>request.read_length) return peripheral_error(L,RYZ_PERIPHERAL_FAILED);
        lua_pushlstring(L,(const char *)reply.data,reply.length);
    } else if((request.kind==RYZ_PERIPHERAL_UART || request.kind==RYZ_PERIPHERAL_I2C ||
               request.kind==RYZ_PERIPHERAL_SPI) && request.op==RYZ_PERIPHERAL_WRITE) {
        if(reply.length>request.length) return peripheral_error(L,RYZ_PERIPHERAL_FAILED);
        lua_pushinteger(L,(lua_Integer)reply.length);
    } else if(request.op==RYZ_PERIPHERAL_READ || request.op==RYZ_PERIPHERAL_READ_MV || request.op==RYZ_PERIPHERAL_POLL) {
        if(reply.value>INT32_MAX) return peripheral_error(L,RYZ_PERIPHERAL_FAILED);
        lua_pushinteger(L,(lua_Integer)reply.value);
    }
    else lua_pushboolean(L,1);
    return 1;
}

static int peripheral_open(lua_State *L)
{
    ryz_lua_peripheral_binding_t *binding=lua_touserdata(L,lua_upvalueindex(1));
    peripheral_check(L,binding);
    ryz_peripheral_request_t request={.kind=(ryz_peripheral_kind_t)lua_tointeger(L,lua_upvalueindex(2)),
        .op=RYZ_PERIPHERAL_OPEN};
    if(request.kind==RYZ_PERIPHERAL_GPIO) {
    peripheral_fields(L,"|pin|mode|pull|initial|");
    request.config.gpio.pin=peripheral_int(L,1,"pin",-1,0,48);
    if(request.config.gpio.pin<0) return luaL_error(L,"pin is required");
    static const char *const modes[]={"input","output","open_drain",NULL};
    static const char *const pulls[]={"none","up","down","both",NULL};
    request.config.gpio.mode=peripheral_enum(L,1,"mode",0,modes);
    request.config.gpio.pull=peripheral_enum(L,1,"pull",0,pulls);
    request.config.gpio.initial=peripheral_int(L,1,"initial",0,0,1);
    } else if(request.kind==RYZ_PERIPHERAL_UART) {
        peripheral_fields(L,"|port|tx|rx|baud|bits|parity|stop|");
        request.config.uart.port=peripheral_int(L,1,"port",1,1,2);
        request.config.uart.tx=peripheral_int(L,1,"tx",-1,-1,48);
        request.config.uart.rx=peripheral_int(L,1,"rx",-1,-1,48);
        if(request.config.uart.tx<0 && request.config.uart.rx<0) return luaL_error(L,"UART requires tx or rx");
        if(request.config.uart.tx==request.config.uart.rx) return luaL_error(L,"UART tx and rx must differ");
        request.config.uart.baud=peripheral_int(L,1,"baud",115200,300,5000000);
        request.config.uart.bits=peripheral_int(L,1,"bits",8,5,8);
        request.config.uart.stop=peripheral_int(L,1,"stop",1,1,2);
        static const char *const parity[]={"none","even","odd",NULL};
        request.config.uart.parity=peripheral_enum(L,1,"parity",0,parity);
    } else if(request.kind==RYZ_PERIPHERAL_TIMER) {
        peripheral_fields(L,"|period_ms|periodic|");
        request.config.timer.period_ms=peripheral_int(L,1,"period_ms",0,1,3600000);
        if(!request.config.timer.period_ms) return luaL_error(L,"period_ms is required");
        request.config.timer.periodic=peripheral_bool(L,1,"periodic",true);
    } else if(request.kind==RYZ_PERIPHERAL_PWM) {
        peripheral_fields(L,"|pin|frequency_hz|duty|");
        request.config.pwm.pin=peripheral_int(L,1,"pin",-1,0,48);
        if(request.config.pwm.pin<0) return luaL_error(L,"pin is required");
        request.config.pwm.frequency_hz=peripheral_int(L,1,"frequency_hz",1000,1,1000000);
        request.config.pwm.duty=peripheral_int(L,1,"duty",0,0,1000);
    } else if(request.kind==RYZ_PERIPHERAL_I2C) {
        peripheral_fields(L,"|board|sda|scl|frequency_hz|");
        request.config.i2c.board=peripheral_bool(L,1,"board",false);
        request.config.i2c.sda=peripheral_int(L,1,"sda",-1,0,48);
        request.config.i2c.scl=peripheral_int(L,1,"scl",-1,0,48);
        request.config.i2c.frequency_hz=peripheral_int(L,1,"frequency_hz",100000,100000,400000);
        if(request.config.i2c.frequency_hz!=100000 && request.config.i2c.frequency_hz!=400000)
            return luaL_error(L,"frequency_hz must be 100000 or 400000");
        if(request.config.i2c.board) {
            if(request.config.i2c.sda>=0 || request.config.i2c.scl>=0 || request.config.i2c.frequency_hz!=100000)
                return luaL_error(L,"board I2C cannot change pins or frequency");
        } else {
            if(request.config.i2c.sda<0 || request.config.i2c.scl<0) return luaL_error(L,"sda and scl are required");
            if(request.config.i2c.sda==request.config.i2c.scl) return luaL_error(L,"sda and scl must differ");
        }
    } else if(request.kind==RYZ_PERIPHERAL_SPI) {
        peripheral_fields(L,"|sclk|mosi|miso|cs|frequency_hz|mode|");
        request.config.spi.sclk=peripheral_int(L,1,"sclk",-1,0,48);
        request.config.spi.cs=peripheral_int(L,1,"cs",-1,0,48);
        request.config.spi.mosi=peripheral_int(L,1,"mosi",-1,-1,48);
        request.config.spi.miso=peripheral_int(L,1,"miso",-1,-1,48);
        if(request.config.spi.sclk<0) return luaL_error(L,"sclk is required");
        if(request.config.spi.cs<0) return luaL_error(L,"cs is required");
        if(request.config.spi.mosi<0 && request.config.spi.miso<0) return luaL_error(L,"SPI requires mosi or miso");
        const int pins[]={request.config.spi.sclk,request.config.spi.mosi,request.config.spi.miso,request.config.spi.cs};
        for(unsigned i=0;i<4;++i) for(unsigned j=0;j<i;++j)
            if(pins[i]>=0 && pins[i]==pins[j]) return luaL_error(L,"SPI pins must differ");
        request.config.spi.frequency_hz=peripheral_int(L,1,"frequency_hz",1000000,100000,20000000);
        request.config.spi.mode=peripheral_int(L,1,"mode",0,0,3);
    } else if(request.kind==RYZ_PERIPHERAL_ADC) {
        peripheral_fields(L,"|pin|attenuation_db|");
        request.config.adc.pin=peripheral_int(L,1,"pin",-1,0,48);
        if(request.config.adc.pin<0) return luaL_error(L,"pin is required");
        unsigned attenuation=peripheral_int(L,1,"attenuation_db",12,0,12);
        if(attenuation!=0 && attenuation!=2 && attenuation!=6 && attenuation!=12)
            return luaL_error(L,"attenuation_db must be 0, 2, 6 or 12");
        request.config.adc.attenuation_db=attenuation;
    } else if(request.kind==RYZ_PERIPHERAL_LOG) {
        peripheral_fields(L,"|level|");
        request.config.log.level=1+peripheral_enum(L,1,"level",2,peripheral_log_levels);
    } else if(request.kind==RYZ_PERIPHERAL_LED) {
        peripheral_fields(L,"|board|");
        request.config.led.board=peripheral_bool(L,1,"board",true);
        if(!request.config.led.board) return luaL_error(L,"only the board LED is supported");
    } else return peripheral_error(L,RYZ_PERIPHERAL_UNSUPPORTED);
    unsigned slot=0;
    while(slot<RYZ_PERIPHERAL_HANDLE_MAX && binding->handles[slot].id) ++slot;
    if(slot==RYZ_PERIPHERAL_HANDLE_MAX) return peripheral_error(L,RYZ_PERIPHERAL_NO_MEMORY);
    /* Allocate before opening hardware: Lua OOM cannot lose a native handle. */
    ryz_lua_peripheral_handle_t *handle=lua_newuserdatauv(L,sizeof(*handle),0);
    *handle=(ryz_lua_peripheral_handle_t){binding,0,slot};
    luaL_setmetatable(L,"ryz.peripheral");
    ryz_peripheral_reply_t reply={0};
    ryz_peripheral_result_t result=binding->call ? binding->call(binding->context,&request,&reply) : RYZ_PERIPHERAL_UNAVAILABLE;
    if(result==RYZ_PERIPHERAL_OK && reply.handle) {
        handle->id=reply.handle;
        binding->handles[slot].id=reply.handle;
        binding->handles[slot].kind=request.kind;
        binding->handles[slot].closing=false;
    } else if(result==RYZ_PERIPHERAL_OK) result=RYZ_PERIPHERAL_FAILED;
    peripheral_check(L,binding);
    if(result!=RYZ_PERIPHERAL_OK) return peripheral_error(L,result);
    return 1;
}

static int peripheral_log_write(lua_State *L)
{
    ryz_lua_peripheral_binding_t *binding=lua_touserdata(L,lua_upvalueindex(1));
    peripheral_check(L,binding);
    if(lua_gettop(L)!=2) return luaL_error(L,"log.write takes level and message");
    if(lua_type(L,1)!=LUA_TSTRING) return luaL_error(L,"level must be a string");
    size_t length; const char *level=lua_tolstring(L,1,&length);
    unsigned selected=0;
    for(unsigned i=0;peripheral_log_levels[i];++i)
        if(strlen(peripheral_log_levels[i])==length && !memcmp(level,peripheral_log_levels[i],length)) selected=i+1;
    if(!selected) return luaL_error(L,"unsupported level");
    ryz_peripheral_request_t request={.kind=RYZ_PERIPHERAL_LOG,.op=RYZ_PERIPHERAL_WRITE};
    request.config.log.level=selected;
    request.data=peripheral_bytes(L,2,&request.length);
    ryz_peripheral_reply_t reply={0};
    ryz_peripheral_result_t result=binding->call ? binding->call(binding->context,&request,&reply) : RYZ_PERIPHERAL_UNAVAILABLE;
    peripheral_check(L,binding);
    if(result!=RYZ_PERIPHERAL_OK) return peripheral_error(L,result);
    lua_pushboolean(L,1); return 1;
}

static void ryz_lua_peripherals_install(lua_State *L, ryz_lua_peripheral_binding_t *binding)
{
    luaL_newmetatable(L,"ryz.peripheral");
    lua_pushliteral(L,"peripheral handle"); lua_setfield(L,-2,"__metatable");
    lua_newtable(L);
    lua_pushcfunction(L,peripheral_close); lua_setfield(L,-2,"close");
    lua_pushinteger(L,RYZ_PERIPHERAL_READ); lua_pushcclosure(L,peripheral_method,1); lua_setfield(L,-2,"read");
    lua_pushinteger(L,RYZ_PERIPHERAL_WRITE); lua_pushcclosure(L,peripheral_method,1); lua_setfield(L,-2,"write");
    lua_pushinteger(L,RYZ_PERIPHERAL_POLL); lua_pushcclosure(L,peripheral_method,1); lua_setfield(L,-2,"poll");
    lua_pushinteger(L,RYZ_PERIPHERAL_SET_DUTY); lua_pushcclosure(L,peripheral_method,1); lua_setfield(L,-2,"set_duty");
    lua_pushinteger(L,RYZ_PERIPHERAL_PROBE); lua_pushcclosure(L,peripheral_method,1); lua_setfield(L,-2,"probe");
    lua_pushinteger(L,RYZ_PERIPHERAL_TRANSFER); lua_pushcclosure(L,peripheral_method,1); lua_setfield(L,-2,"transfer");
    lua_pushinteger(L,RYZ_PERIPHERAL_READ_MV); lua_pushcclosure(L,peripheral_method,1); lua_setfield(L,-2,"read_mv");
    lua_pushinteger(L,RYZ_PERIPHERAL_STATUS); lua_pushcclosure(L,peripheral_method,1); lua_setfield(L,-2,"status");
    lua_setfield(L,-2,"__index"); lua_pop(L,1);
    static const struct { const char *name; ryz_peripheral_kind_t kind; } modules[]={
        {"gpio",RYZ_PERIPHERAL_GPIO},{"uart",RYZ_PERIPHERAL_UART},
        {"timer",RYZ_PERIPHERAL_TIMER},{"pwm",RYZ_PERIPHERAL_PWM},{"i2c",RYZ_PERIPHERAL_I2C},
        {"spi",RYZ_PERIPHERAL_SPI},{"adc",RYZ_PERIPHERAL_ADC},{"log",RYZ_PERIPHERAL_LOG},
        {"led",RYZ_PERIPHERAL_LED}};
    for(unsigned i=0;i<sizeof(modules)/sizeof(modules[0]);++i) {
        lua_newtable(L);
        lua_pushlightuserdata(L,binding); lua_pushinteger(L,modules[i].kind);
        lua_pushcclosure(L,peripheral_open,2); lua_setfield(L,-2,"open");
        if(modules[i].kind==RYZ_PERIPHERAL_LOG) {
            lua_pushlightuserdata(L,binding); lua_pushcclosure(L,peripheral_log_write,1); lua_setfield(L,-2,"write");
        }
        char key[40]; snprintf(key,sizeof(key),"ryz.peripheral.%s",modules[i].name);
        lua_setfield(L,LUA_REGISTRYINDEX,key);
    }
}

static bool ryz_lua_peripherals_require(lua_State *L,const char *name,size_t length)
{
    if(!((length==4 && (!memcmp(name,"gpio",4) || !memcmp(name,"uart",4))) ||
        (length==5 && !memcmp(name,"timer",5)) ||
        (length==3 && (!memcmp(name,"pwm",3) || !memcmp(name,"i2c",3) ||
                      !memcmp(name,"spi",3) || !memcmp(name,"adc",3) || !memcmp(name,"log",3) ||
                      !memcmp(name,"led",3))))) return false;
    char key[40]; snprintf(key,sizeof(key),"ryz.peripheral.%s",name);
    lua_getfield(L,LUA_REGISTRYINDEX,key); return true;
}

static void ryz_lua_peripherals_cleanup(ryz_lua_peripheral_binding_t *binding)
{
    binding->revoked=true;
    for(unsigned i=0;i<RYZ_PERIPHERAL_HANDLE_MAX;++i) if(binding->handles[i].id && binding->call) {
        ryz_peripheral_request_t request={.kind=binding->handles[i].kind,
            .op=RYZ_PERIPHERAL_CLOSE,.handle=binding->handles[i].id};
        ryz_peripheral_reply_t reply={0};
        (void)binding->call(binding->context,&request,&reply);
        binding->handles[i].id=0;
    }
    /* Target session cleanup also retries failed closes. Handle storage never
     * owns driver memory; it remains unavailable to Lua after revocation. */
}
