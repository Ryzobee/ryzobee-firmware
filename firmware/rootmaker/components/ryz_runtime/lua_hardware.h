#pragma once

#include <string.h>
#include <inttypes.h>
#include <stdio.h>
#include "ryz_lua_hardware.h"
#include "lua.h"
#include "lauxlib.h"

typedef struct {
    ryz_lua_hardware_fn call;
    void *context;
    void (*check)(lua_State *L);
} ryz_lua_hardware_binding_t;

static void ryz_lua_hardware_counter(lua_State *L, const char *name, uint64_t value)
{
    /* The pinned VM has 32-bit integers AND floats. Preserve large monotonic
     * counters exactly instead of overflowing or silently rounding to float. */
    char token[21];
    snprintf(token, sizeof(token), "%" PRIu64, value);
    lua_pushstring(L, token);
    lua_setfield(L, -2, name);
}

static int ryz_lua_hardware_invoke(lua_State *L)
{
    ryz_lua_hardware_binding_t *binding = lua_touserdata(L, lua_upvalueindex(1));
    ryz_lua_hardware_op_t op = (ryz_lua_hardware_op_t)lua_tointeger(L, lua_upvalueindex(2));
    binding->check(L);
    if (op == RYZ_LUA_HID_PLAY_PAUSE) {
        static const struct {
            const char *name;
            ryz_lua_hardware_op_t op;
        } keys[] = {
            {"play_pause", RYZ_LUA_HID_PLAY_PAUSE},
            {"stop", RYZ_LUA_HID_STOP},
            {"next_track", RYZ_LUA_HID_NEXT_TRACK},
            {"previous_track", RYZ_LUA_HID_PREVIOUS_TRACK},
            {"mute", RYZ_LUA_HID_MUTE},
            {"volume_up", RYZ_LUA_HID_VOLUME_UP},
            {"volume_down", RYZ_LUA_HID_VOLUME_DOWN},
        };
        if (lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TSTRING)
            return luaL_error(L, "hid.tap expects one key string");
        size_t length;
        const char *key = lua_tolstring(L, 1, &length);
        bool found = false;
        for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
            if (length == strlen(keys[i].name) && !memcmp(key, keys[i].name, length)) {
                op = keys[i].op;
                found = true;
                break;
            }
        }
        if (!found) return luaL_error(L, "unknown HID key");
    } else if (lua_gettop(L)) return luaL_error(L, "hardware calls take no arguments");
    ryz_lua_hardware_value_t value = {0};
    ryz_lua_hardware_result_t result = binding->call ?
        binding->call(binding->context, op, &value) : RYZ_LUA_HW_UNAVAILABLE;
    binding->check(L);
    bool status = op == RYZ_LUA_WIFI_CONNECTED || op == RYZ_LUA_BLE_CONNECTED ||
        op == RYZ_LUA_HID_READY;
    if (result != RYZ_LUA_HW_OK) {
        const char *code = "failed";
        if (result == RYZ_LUA_HW_UNAVAILABLE) code = "unavailable";
        else if (result == RYZ_LUA_HW_NOT_INITIALIZED) code = "not_initialized";
        else if (result == RYZ_LUA_HW_NOT_READY) code = "not_ready";
        else if (result == RYZ_LUA_HW_BUSY) code = "busy";
        if (status) lua_pushboolean(L, 0); else lua_pushnil(L);
        lua_pushstring(L, code);
        return 2;
    }
    if (status) lua_pushboolean(L, value.connected);
    else if (op != RYZ_LUA_IMU_READ) lua_pushboolean(L, 1);
    else {
        lua_createtable(L, 0, 5);
        lua_pushnumber(L, value.x_mg); lua_setfield(L, -2, "x_mg");
        lua_pushnumber(L, value.y_mg); lua_setfield(L, -2, "y_mg");
        lua_pushnumber(L, value.z_mg); lua_setfield(L, -2, "z_mg");
        ryz_lua_hardware_counter(L, "timestamp_us", value.timestamp_us);
        ryz_lua_hardware_counter(L, "sequence", value.sequence);
    }
    return 1;
}

static void ryz_lua_hardware_install(lua_State *L, ryz_lua_hardware_binding_t *binding)
{
    static const struct {
        const char *module, *method;
        ryz_lua_hardware_op_t op;
    } methods[] = {
        {"wifi", "is_connected", RYZ_LUA_WIFI_CONNECTED},
        {"ble", "is_connected", RYZ_LUA_BLE_CONNECTED},
        {"imu", "init", RYZ_LUA_IMU_INIT},
        {"imu", "read", RYZ_LUA_IMU_READ},
        {"imu", "deinit", RYZ_LUA_IMU_DEINIT},
        {"hid", "is_ready", RYZ_LUA_HID_READY},
        {"hid", "tap", RYZ_LUA_HID_PLAY_PAUSE},
    };
    for (unsigned i = 0; i < sizeof(methods) / sizeof(methods[0]); ++i) {
        lua_getfield(L, LUA_REGISTRYINDEX, methods[i].module);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_setfield(L, LUA_REGISTRYINDEX, methods[i].module);
        }
        lua_pushlightuserdata(L, binding);
        lua_pushinteger(L, methods[i].op);
        lua_pushcclosure(L, ryz_lua_hardware_invoke, 2);
        lua_setfield(L, -2, methods[i].method);
        lua_pop(L, 1);
    }
}

static bool ryz_lua_hardware_require(lua_State *L, const char *name, size_t length)
{
    if ((length == 4 && !memcmp(name, "wifi", 4)) ||
        (length == 3 && (!memcmp(name, "ble", 3) || !memcmp(name, "imu", 3) ||
                         !memcmp(name, "hid", 3)))) {
        lua_getfield(L, LUA_REGISTRYINDEX, name);
        return true;
    }
    if (length == 9 && !memcmp(name, "coroutine", 9)) {
        luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
        lua_getfield(L, -1, "coroutine");
        lua_remove(L, -2);
        return true;
    }
    return false;
}
