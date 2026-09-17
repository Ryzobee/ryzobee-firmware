#pragma once

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

/* The pinned VM copies the main extraspace and the creating thread's hook to
 * each new thread. Keep upstream coroutine semantics, but put the job guard
 * on both sides of every protected resume/close: a child timeout, cancellation
 * or Host completion must escape, never become a catchable false,error pair.
 * No VM patch, independent heap, scheduler, or native resource owner is added.
 * Upvalues are trusted functions, inaccessible without the disabled debug lib. */
static int ryz_coroutine_guarded_call(lua_State *L)
{
    int count = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(2));
    lua_call(L, 0, 0);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, count, LUA_MULTRET);
    lua_pushvalue(L, lua_upvalueindex(2));
    lua_call(L, 0, 0);
    return lua_gettop(L);
}

static int ryz_coroutine_guarded_wrap(lua_State *L)
{
    ryz_coroutine_guarded_call(L); /* Upstream wrap creates the hidden thread. */
    lua_pushvalue(L, lua_upvalueindex(2));
    lua_pushcclosure(L, ryz_coroutine_guarded_call, 2);
    return 1;
}

static void ryz_lua_coroutines_install(lua_State *L, lua_CFunction guard)
{
    luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
    const char *protected_calls[] = {"resume", "close", "wrap", NULL};
    for (unsigned i = 0; protected_calls[i]; ++i) {
        lua_getfield(L, -1, protected_calls[i]);
        lua_pushcfunction(L, guard);
        lua_pushcclosure(L, i == 2 ? ryz_coroutine_guarded_wrap :
                          ryz_coroutine_guarded_call, 2);
        lua_setfield(L, -2, protected_calls[i]);
    }
    lua_pop(L, 1);
}
