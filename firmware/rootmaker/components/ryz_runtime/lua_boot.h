#pragma once

#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "ryz_lua_boot.h"
#include "lua.h"
#include "lauxlib.h"

/* The producer, queue and GPIO remain owned by the trusted platform. This
 * binding only consumes a copy, shared by all coroutines in the same job. */
typedef struct {
    ryz_boot_poll_fn poll;
    void *context;
    void (*check)(lua_State *L);
} ryz_lua_boot_binding_t;

static int ryz_lua_boot_poll(lua_State *L)
{
    ryz_lua_boot_binding_t *binding = lua_touserdata(L, lua_upvalueindex(1));
    binding->check(L);
    if (lua_gettop(L)) return luaL_error(L, "boot.poll expects no arguments");
    ryz_boot_event_t event = {0};
    ryz_boot_result_t result = binding->poll ?
        binding->poll(binding->context, &event) : RYZ_BOOT_UNAVAILABLE;
    binding->check(L);
    if (result == RYZ_BOOT_EMPTY) { lua_pushnil(L); return 1; }
    const char *type = NULL;
    if (result == RYZ_BOOT_OK) {
        if (event.kind == RYZ_BOOT_CLICK && event.held_ms < 3000U) type = "click";
        else if (event.kind == RYZ_BOOT_DOUBLE_CLICK && event.held_ms < 3000U) type = "double_click";
        else if (event.kind == RYZ_BOOT_LONG_PRESS && event.held_ms == 3000U) type = "long_press";
    }
    if (!type) {
        const char *reason = "failed";
        if (result == RYZ_BOOT_UNAVAILABLE) reason = "unavailable";
        else if (result == RYZ_BOOT_BUSY) reason = "busy";
        else if (result == RYZ_BOOT_OVERFLOW) reason = "overflow";
        lua_pushnil(L); lua_pushstring(L, reason);
        return 2;
    }
    lua_createtable(L, 0, 3);
    lua_pushstring(L, type); lua_setfield(L, -2, "type");
    lua_pushinteger(L, (lua_Integer)event.held_ms); lua_setfield(L, -2, "held_ms");
    /* The pinned Lua has 32-bit integers AND floats. Preserve the full uint32
     * monotonic timestamp exactly rather than making it negative or rounded. */
    char timestamp[11];
    snprintf(timestamp, sizeof(timestamp), "%" PRIu32, event.timestamp_ms);
    lua_pushstring(L, timestamp); lua_setfield(L, -2, "timestamp_ms");
    return 1;
}

static void ryz_lua_boot_install(lua_State *L, ryz_lua_boot_binding_t *binding)
{
    lua_createtable(L, 0, 1);
    lua_pushlightuserdata(L, binding);
    lua_pushcclosure(L, ryz_lua_boot_poll, 1);
    lua_setfield(L, -2, "poll");
    lua_setfield(L, LUA_REGISTRYINDEX, "ryz-app.boot");
}

static bool ryz_lua_boot_require(lua_State *L, const char *name, size_t length)
{
    if (length != 4 || memcmp(name, "boot", 4)) return false;
    lua_getfield(L, LUA_REGISTRYINDEX, "ryz-app.boot");
    return true;
}
