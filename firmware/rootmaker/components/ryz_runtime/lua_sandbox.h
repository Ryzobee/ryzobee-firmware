#pragma once

#include "lua.h"
#include "lauxlib.h"

/* Private policy shared by the application and legacy facades. Lua disables
 * instruction hooks inside GC finalizers, so user __gc cannot participate in
 * the runtime's cancellation/deadline contract. Trusted VM/library finalizers
 * remain intact; native resources are reclaimed by the C job owner.
 *
 * In the frozen VM, tables enter the finalization list only when a metatable
 * with a non-nil raw __gc is installed. Later edits alone do not register them;
 * reinstalling goes through this guard again. Reject false and callable tables
 * as well as functions. __index on the metatable must never run during checking.
 * Ordinary metatables and hook-controlled __close retain base-library behavior.
 * This assumes the existing sandbox: no debug, io, package or dynamic loader,
 * and no native API exposing mutable user-accessible finalizable userdata. */
static int ryz_sandbox_setmetatable(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    if (lua_istable(L, 2)) {
        lua_pushliteral(L, "__gc");
        lua_rawget(L, 2);
        if (!lua_isnil(L, -1))
            return luaL_error(L, "__gc finalizers are not supported; use explicit cleanup");
        lua_pop(L, 1);
    }
    /* Delegate protection, nil removal, extra arguments and return identity
     * to the original base function; never expose that function to Lua. */
    int arguments = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, arguments, LUA_MULTRET);
    return lua_gettop(L);
}

/* Call once inside protected setup, after opening base and before user code. */
static void ryz_lua_sandbox_install(lua_State *L)
{
    lua_getglobal(L, "setmetatable");
    luaL_checktype(L, -1, LUA_TFUNCTION);
    lua_pushcclosure(L, ryz_sandbox_setmetatable, 1);
    lua_setglobal(L, "setmetatable");
}
