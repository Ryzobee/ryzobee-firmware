#pragma once
#include "lua.h"
#include "ryz_tool_call.h"

/* Private Lua facade; owner/session authority stays in the trusted context.
 * Creates one tools table. Captures only C callback/context/check in userdata.
 * Result construction can fail after admission; the Adapter must already have
 * recorded native ownership, and job cleanup must survive that Lua error. */
int ryz_app_tools_open(lua_State *L, ryz_tool_call_fn callback, void *context,
                       void (*check)(lua_State *L));

/* Revoke before platform cleanup/lua_close: Lua finalizers must not admit
 * new native work after the execution phase. No allocation or Lua callback. */
void ryz_app_tools_revoke(lua_State *L);
