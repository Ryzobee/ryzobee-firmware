#pragma once

#include <stdbool.h>
#include <string.h>
#include "ryz_fs.h"
#include "lua.h"
#include "lauxlib.h"

/* Shared by the app and legacy facades. Lua never chooses its namespace or
 * receives native paths/handles. Scratch buffers belong to the bounded Lua
 * allocator, so cancellation, conversion OOM and coroutine errors cannot leak
 * a native allocation or leave an open file behind. */
typedef struct {
    ryz_fs_call_fn call;
    void *context;
    void (*check)(lua_State *L);
    char app_id[RYZ_FS_APP_ID_MAX + 1U];
} ryz_lua_fs_binding_t;

static bool ryz_lua_fs_alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9');
}

static bool ryz_lua_fs_name_valid(const char *name, size_t length, size_t maximum)
{
    if (!name || !length || length > maximum || !ryz_lua_fs_alnum(name[0])) return false;
    for (size_t i = 1; i < length; ++i) {
        char c = name[i];
        if (!ryz_lua_fs_alnum(c) && c != '_' && c != '-' && c != '.') return false;
        if (c == '.' && name[i - 1] == '.') return false;
    }
    return true;
}

static bool ryz_lua_fs_app_id_valid(const char *name, size_t length)
{
    /* Match the trusted Script Store launcher exactly; filename keys below
     * intentionally have a separate rule from installed script basenames. */
    if (length < 5 || length > RYZ_FS_APP_ID_MAX || memcmp(name + length - 4, ".lua", 4)) return false;
    for (size_t i = 0; i < length - 4; ++i)
        if (!ryz_lua_fs_alnum(name[i]) && name[i] != '_' && name[i] != '-') return false;
    return true;
}

static const char *ryz_lua_fs_reason(ryz_fs_result_t result)
{
    switch (result) {
    case RYZ_FS_UNAVAILABLE: return "unavailable";
    case RYZ_FS_INVALID_NAME: return "invalid_name";
    case RYZ_FS_INVALID_APP: return "invalid_app";
    case RYZ_FS_TOO_LARGE: return "too_large";
    case RYZ_FS_QUOTA: return "quota";
    case RYZ_FS_TOO_MANY_FILES: return "too_many_files";
    case RYZ_FS_BUSY: return "busy";
    case RYZ_FS_NOT_FOUND: return "not_found";
    case RYZ_FS_CORRUPT: return "corrupt";
    case RYZ_FS_NO_SPACE: return "no_space";
    case RYZ_FS_NO_MEMORY: return "no_memory";
    case RYZ_FS_COMMIT_UNKNOWN: return "commit_unknown";
    case RYZ_FS_RECOVERY_REQUIRED: return "recovery_required";
    default: return "io";
    }
}

static int ryz_lua_fs_failure(lua_State *L, ryz_fs_result_t result)
{
    lua_pushnil(L);
    lua_pushstring(L, ryz_lua_fs_reason(result));
    return 2;
}

static void ryz_lua_fs_integer(lua_State *L, const char *name, size_t value)
{
    lua_pushinteger(L, (lua_Integer)value);
    lua_setfield(L, -2, name);
}

static int ryz_lua_fs_invoke(lua_State *L)
{
    ryz_lua_fs_binding_t *binding = lua_touserdata(L, lua_upvalueindex(1));
    ryz_fs_op_t op = (ryz_fs_op_t)lua_tointeger(L, lua_upvalueindex(2));
    binding->check(L);
    int expected = op == RYZ_FS_WRITE ? 2 : op == RYZ_FS_LIST || op == RYZ_FS_INFO ? 0 : 1;
    if (lua_gettop(L) != expected) return luaL_error(L, "wrong number of fs arguments");
    ryz_fs_request_t request = {.op = op};
    if (expected) {
        if (lua_type(L, 1) != LUA_TSTRING) return luaL_error(L, "fs name must be a string");
        size_t length = 0;
        request.name = lua_tolstring(L, 1, &length);
        if (!ryz_lua_fs_name_valid(request.name, length, RYZ_FS_NAME_MAX))
            return luaL_error(L, "invalid fs name: expected a flat ASCII name, 1..24 bytes");
    }
    if (op == RYZ_FS_WRITE) {
        if (lua_type(L, 2) != LUA_TSTRING) return luaL_error(L, "fs data must be a string");
        request.data = lua_tolstring(L, 2, &request.size);
        if (request.size > RYZ_FS_MAX_FILE_BYTES)
            return luaL_error(L, "fs data exceeds 8192 bytes");
    }
    if (!binding->call || !binding->app_id[0])
        return ryz_lua_fs_failure(L, RYZ_FS_UNAVAILABLE);
    if (op == RYZ_FS_READ) {
        request.read_capacity = RYZ_FS_MAX_FILE_BYTES;
        request.read_buffer = lua_newuserdatauv(L, request.read_capacity, 0);
    } else if (op == RYZ_FS_LIST) {
        request.entries_capacity = RYZ_FS_MAX_FILES;
        request.entries = lua_newuserdatauv(L, sizeof(*request.entries) * request.entries_capacity, 0);
        memset(request.entries, 0, sizeof(*request.entries) * request.entries_capacity);
    }
    ryz_fs_response_t response = {0};
    binding->check(L);
    ryz_fs_result_t result = binding->call(binding->context, binding->app_id, &request, &response);
    binding->check(L);
    if (result != RYZ_FS_OK) return ryz_lua_fs_failure(L, result);
    if (op == RYZ_FS_READ) {
        if (response.size > request.read_capacity) return ryz_lua_fs_failure(L, RYZ_FS_IO);
        lua_pushlstring(L, request.read_buffer, response.size);
    } else if (op == RYZ_FS_LIST) {
        if (response.count > request.entries_capacity) return ryz_lua_fs_failure(L, RYZ_FS_IO);
        /* Do not trust callback counts, unterminated names or dirty success
         * payloads. The backend contract requires unique sorted entries. */
        for (size_t i = 0; i < response.count; ++i) {
            const ryz_fs_entry_t *entry = &request.entries[i];
            const char *end = memchr(entry->name, 0, sizeof(entry->name));
            if (!end || !ryz_lua_fs_name_valid(entry->name, (size_t)(end - entry->name), RYZ_FS_NAME_MAX) ||
                entry->size > RYZ_FS_MAX_FILE_BYTES ||
                (i && strcmp(request.entries[i - 1].name, entry->name) >= 0))
                return ryz_lua_fs_failure(L, RYZ_FS_IO);
        }
        lua_createtable(L, (int)response.count, 0);
        for (size_t i = 0; i < response.count; ++i) {
            lua_createtable(L, 0, 2);
            lua_pushstring(L, request.entries[i].name); lua_setfield(L, -2, "name");
            ryz_lua_fs_integer(L, "size", request.entries[i].size);
            lua_rawseti(L, -2, (lua_Integer)i + 1);
        }
    } else if (op == RYZ_FS_INFO) {
        const ryz_fs_info_t *info = &response.info;
        if (info->max_file_bytes > RYZ_FS_MAX_FILE_BYTES || info->quota_bytes > RYZ_FS_QUOTA_BYTES ||
            info->max_files > RYZ_FS_MAX_FILES || info->used_bytes > info->quota_bytes ||
            info->file_count > info->max_files) return ryz_lua_fs_failure(L, RYZ_FS_IO);
        lua_createtable(L, 0, 5);
        ryz_lua_fs_integer(L, "used_bytes", info->used_bytes);
        ryz_lua_fs_integer(L, "file_count", info->file_count);
        ryz_lua_fs_integer(L, "max_file_bytes", info->max_file_bytes);
        ryz_lua_fs_integer(L, "quota_bytes", info->quota_bytes);
        ryz_lua_fs_integer(L, "max_files", info->max_files);
    } else lua_pushboolean(L, 1);
    return 1;
}

static void ryz_lua_fs_install(lua_State *L, ryz_lua_fs_binding_t *binding, const char *source_name)
{
    /* Source names are supplied by C, not extracted from editable Lua metadata.
     * Reject anonymous eval chunks and paths rather than inventing a shared
     * namespace. Keeping the .lua extension also keeps the identity explicit. */
    binding->app_id[0] = 0;
    if (source_name) {
        if (*source_name == '@') ++source_name;
        size_t length = strlen(source_name);
        if (ryz_lua_fs_app_id_valid(source_name, length))
            memcpy(binding->app_id, source_name, length + 1);
    }
    static const struct { const char *name; ryz_fs_op_t op; } methods[] = {
        {"read", RYZ_FS_READ}, {"write", RYZ_FS_WRITE}, {"remove", RYZ_FS_REMOVE},
        {"list", RYZ_FS_LIST}, {"info", RYZ_FS_INFO},
    };
    lua_createtable(L, 0, 5);
    for (unsigned i = 0; i < sizeof(methods) / sizeof(methods[0]); ++i) {
        lua_pushlightuserdata(L, binding);
        lua_pushinteger(L, methods[i].op);
        lua_pushcclosure(L, ryz_lua_fs_invoke, 2);
        lua_setfield(L, -2, methods[i].name);
    }
    lua_setfield(L, LUA_REGISTRYINDEX, "ryz-app.fs");
}

static bool ryz_lua_fs_require(lua_State *L, const char *name, size_t length)
{
    if (length != 2 || memcmp(name, "fs", 2)) return false;
    lua_getfield(L, LUA_REGISTRYINDEX, "ryz-app.fs");
    return true;
}
