/* Application-owned cooperative runtime. Never patches the Lua component. */
#include "nervous_runtime.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"

typedef struct { char id[25]; unsigned type; bool latest; uint32_t ttl; } port_t;
typedef struct { uint8_t bytes[8], length, port; uint64_t expires; } message_t;
typedef enum { INITIAL, SLEEPING, RECEIVING, PAINTING, ENDED } wait_t;
typedef struct {
    char id[25], kind[9]; lua_State *L;
    port_t inputs[RYZ_NEURO_PORTS], outputs[RYZ_NEURO_PORTS];
    unsigned input_count, output_count, count;
    message_t inbox[RYZ_NEURO_QUEUE];
    wait_t wait; uint64_t wake, waited; uint16_t color;
} node_t;
typedef struct { unsigned from, output, to, input; } wire_t;
typedef struct {
    const ryz_neuro_platform_t *platform; ryz_lua_result_t *result;
    const char *source, *name; size_t length, allocated;
    uint64_t start, deadline; lua_State *L;
    node_t nodes[RYZ_NEURO_NODES]; unsigned node_count;
    wire_t wires[32]; unsigned wire_count, budget, current;
    uint32_t delivered, expired, merged;
} runtime_t;

static unsigned payload_length(unsigned type)
{
    static const unsigned lengths[] = {99, 0, 1, 2, 4, 2, 1};
    return type < sizeof(lengths) / sizeof(lengths[0]) ? lengths[type] : 99;
}
bool ryz_neuro_decode(const uint8_t *b, size_t length, unsigned expected, int32_t *value)
{
    if (!b || !value || length < 4) return false;
    unsigned type = b[0] | ((unsigned)b[1] << 8), size = b[2] | ((unsigned)b[3] << 8);
    if (type != expected || size > 4 || size != payload_length(type) || length != size + 4) return false;
    uint32_t v = 0;
    for (unsigned i = 0; i < size; ++i) v |= (uint32_t)b[4 + i] << (8 * i);
    if (((type == 2 || type == 6) && v > 1) || v > INT32_MAX) return false;
    *value = (int32_t)v; return true;
}
static runtime_t *runtime(lua_State *L) { return *(runtime_t **)lua_getextraspace(L); }
static node_t *node(lua_State *L) { return lua_touserdata(L, lua_upvalueindex(1)); }
static uint64_t now(runtime_t *r) { return r->platform->now_ms(r->platform->context); }
static void check(lua_State *L)
{
    runtime_t *r = runtime(L);
    if (r->platform->cancelled && r->platform->cancelled(r->platform->context)) {
        r->result->phase = "stopped"; luaL_error(L, "stopped by user");
    }
    if (r->deadline && now(r) >= r->deadline) {
        r->result->phase = "timeout"; luaL_error(L, "execution timed out");
    }
}
static void hook(lua_State *L, lua_Debug *debug)
{
    (void)debug; check(L);
    runtime_t *r = runtime(L);
    if (++r->budget > 20) luaL_error(L, "node instruction budget exceeded");
}
static void *allocator(void *ud, void *ptr, size_t old, size_t size)
{
    runtime_t *r = ud;
    if (!ptr) old = 0;
    if (!size) { r->platform->resize(ptr, 0); r->allocated -= old; return NULL; }
    if (size > RYZ_LUA_HEAP_LIMIT - (r->allocated - old)) return NULL;
    void *next = r->platform->resize(ptr, size);
    if (next) { r->allocated = r->allocated - old + size; if (r->allocated > r->result->peak_bytes) r->result->peak_bytes = r->allocated; }
    return next;
}
static bool identifier(const char *s)
{
    if (!s || !*s || strlen(s) > 24 || *s < 'a' || *s > 'z') return false;
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_')) return false;
    return true;
}
static void string_field(lua_State *L, int index, const char *key, char *out, size_t size)
{
    lua_getfield(L, index, key); size_t len; const char *s = luaL_checklstring(L, -1, &len);
    if (len >= size || strlen(s) != len) luaL_error(L, "invalid %s", key);
    memcpy(out, s, len + 1); lua_pop(L, 1);
}
static unsigned find_port(lua_State *L, const char *name, port_t *ports, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) if (!strcmp(ports[i].id, name)) return i;
    return luaL_error(L, "undeclared port: %s", name);
}
static int emit(lua_State *L)
{
    check(L); runtime_t *r = runtime(L); node_t *n = node(L);
    unsigned p = find_port(L, luaL_checkstring(L, 1), n->outputs, n->output_count), type = n->outputs[p].type;
    int32_t value;
    if (type == 2 || type == 6) { luaL_checktype(L, 2, LUA_TBOOLEAN); value = lua_toboolean(L, 2); }
    else { value = luaL_checkinteger(L, 2); if (value < 0 || (type == 1 && value != 0) || ((type == 3 || type == 5) && value > 65535)) return luaL_error(L, "signal value out of range"); }
    message_t m = { .bytes = {(uint8_t)type, 0, (uint8_t)payload_length(type), 0}, .length = 4 + payload_length(type) };
    for (unsigned i = 0; i < payload_length(type); ++i) m.bytes[4 + i] = ((uint32_t)value >> (8 * i)) & 255;
    /* Preflight every recipient: reject overload without partial broadcast. */
    unsigned needed[RYZ_NEURO_NODES] = {0};
    for (unsigned i = 0; i < r->wire_count; ++i) {
        wire_t *w = &r->wires[i]; if (&r->nodes[w->from] != n || w->output != p) continue;
        node_t *to = &r->nodes[w->to]; bool replace = false;
        if (to->inputs[w->input].latest) for (unsigned j = 0; j < to->count; ++j) if (to->inbox[j].port == w->input) replace = true;
        if (!replace) needed[w->to]++;
        if (to->count + needed[w->to] > RYZ_NEURO_QUEUE) return luaL_error(L, "inbox full: %s", to->id);
    }
    for (unsigned i = 0; i < r->wire_count; ++i) {
        wire_t *w = &r->wires[i]; if (&r->nodes[w->from] != n || w->output != p) continue;
        node_t *to = &r->nodes[w->to]; unsigned at = to->count;
        if (to->inputs[w->input].latest) for (unsigned j = 0; j < to->count; ++j) if (to->inbox[j].port == w->input) { at = j; break; }
        m.port = w->input; m.expires = to->inputs[w->input].ttl ? now(r) + to->inputs[w->input].ttl : 0;
        to->inbox[at] = m;
        if (at == to->count) to->count++; else r->merged++;
        r->delivered++;
    }
    return 0;
}
static int resumed(lua_State *L, int status, lua_KContext ctx) { (void)status; (void)ctx; return lua_gettop(L); }
static int sleep_node(lua_State *L)
{
    node_t *n = node(L); int ms = luaL_checkinteger(L, 1);
    if (ms < 1 || ms > 300000) return luaL_error(L, "sleep requires 1..300000 ms");
    n->wait = SLEEPING; n->wake = now(runtime(L)) + ms;
    lua_settop(L, 0); return lua_yieldk(L, 0, 0, resumed);
}
static int receive(lua_State *L)
{
    node_t *n = node(L); int ms = luaL_checkinteger(L, 1);
    if (ms < -1 || ms > 300000) return luaL_error(L, "receive requires -1..300000 ms");
    n->wait = RECEIVING; n->waited = now(runtime(L)); n->wake = ms < 0 ? UINT64_MAX : n->waited + ms;
    lua_settop(L, 0); return lua_yieldk(L, 0, 0, resumed);
}
static int sample(lua_State *L)
{
    check(L); runtime_t *r = runtime(L);
    if (strcmp(node(L)->kind, "touch")) return luaL_error(L, "touch capability denied");
    int value = r->platform->sample(r->platform->context);
    check(L); /* A queued native call may have observed stop/timeout. */
    if (value < 0) return luaL_error(L, "touch read failed");
    lua_pushboolean(L, value); return 1;
}
static int paint(lua_State *L)
{
    check(L); runtime_t *r = runtime(L); node_t *n = node(L);
    if (strcmp(n->kind, "display")) return luaL_error(L, "display capability denied");
    int color = luaL_checkinteger(L, 1);
    if (color < 0 || color > 65535) return luaL_error(L, "invalid display color");
    n->color = color;
    int done = r->platform->paint(r->platform->context, n->color, true);
    check(L);
    if (done < 0) return luaL_error(L, "display operation failed");
    if (done) return 0;
    n->wait = PAINTING; lua_settop(L, 0); return lua_yieldk(L, 0, 0, resumed);
}
static int mark(lua_State *L)
{
    runtime_t *r = runtime(L); const char *state = ""; int32_t value = 0;
    if (lua_type(L, 1) == LUA_TSTRING) { state = luaL_checkstring(L, 1); if (!identifier(state)) return luaL_error(L, "invalid state identifier"); }
    else value = luaL_checkinteger(L, 1);
    if (r->platform->observe) r->platform->observe(r->platform->context, node(L)->id, state, value);
    return 0;
}
static void read_ports(lua_State *L, node_t *n, bool input)
{
    lua_getfield(L, -1, input ? "inputs" : "outputs"); luaL_checktype(L, -1, LUA_TTABLE);
    size_t count = lua_rawlen(L, -1); if (count > RYZ_NEURO_PORTS) luaL_error(L, "too many ports");
    port_t *ports = input ? n->inputs : n->outputs;
    if (input) n->input_count = count; else n->output_count = count;
    for (unsigned i = 0; i < count; ++i) {
        lua_rawgeti(L, -1, i + 1); luaL_checktype(L, -1, LUA_TTABLE);
        lua_rawgeti(L, -1, 1); const char *name = luaL_checkstring(L, -1);
        if (!identifier(name) || !strcmp(name, "timeout")) luaL_error(L, "invalid or reserved port identifier");
        strcpy(ports[i].id, name); lua_pop(L, 1);
        for (unsigned j = 0; j < i; ++j) if (!strcmp(ports[i].id, ports[j].id)) luaL_error(L, "duplicate port");
        lua_rawgeti(L, -1, 2); ports[i].type = luaL_checkinteger(L, -1); lua_pop(L, 1);
        if (payload_length(ports[i].type) > 4) luaL_error(L, "unknown signal type");
        if (input) {
            lua_rawgeti(L, -1, 3); const char *policy = luaL_checkstring(L, -1);
            if (strcmp(policy, "reject") && strcmp(policy, "latest")) luaL_error(L, "unknown queue policy");
            ports[i].latest = !strcmp(policy, "latest"); lua_pop(L, 1);
            lua_rawgeti(L, -1, 4); int ttl = luaL_checkinteger(L, -1); lua_pop(L, 1);
            if (ttl < 0 || ttl > 300000 || ((ports[i].type == 1 || ports[i].type == 2) && (ttl || ports[i].latest))) luaL_error(L, "invalid signal expiry policy");
            ports[i].ttl = ttl;
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}
static int setup(lua_State *L)
{
    runtime_t *r = runtime(L);
    if (luaL_loadbufferx(L, r->source, r->length, r->name, "t") != LUA_OK) return lua_error(L);
    /* Descriptor evaluation cannot access any standard library or native API. */
    lua_newtable(L); lua_setupvalue(L, -2, 1); lua_call(L, 0, 1); luaL_checktype(L, -1, LUA_TTABLE);
    char version[32]; string_field(L, -1, "runtime", version, sizeof(version));
    if (strcmp(version, RYZ_NEURO_ABI)) return luaL_error(L, "runtime ABI mismatch");
    string_field(L, -1, "catalog", version, sizeof(version));
    if (strcmp(version, RYZ_NEURO_CATALOG)) return luaL_error(L, "signal catalog mismatch");
    lua_getfield(L, -1, "nodes"); luaL_checktype(L, -1, LUA_TTABLE);
    r->node_count = lua_rawlen(L, -1); if (!r->node_count || r->node_count > RYZ_NEURO_NODES) return luaL_error(L, "invalid node count");
    unsigned touches = 0, displays = 0;
    for (unsigned i = 0; i < r->node_count; ++i) {
        node_t *n = &r->nodes[i]; lua_rawgeti(L, -1, i + 1); luaL_checktype(L, -1, LUA_TTABLE);
        string_field(L, -1, "id", n->id, sizeof(n->id)); string_field(L, -1, "kind", n->kind, sizeof(n->kind));
        if (!identifier(n->id)) return luaL_error(L, "invalid node id");
        for (unsigned j = 0; j < i; ++j) if (!strcmp(n->id, r->nodes[j].id)) return luaL_error(L, "duplicate node id");
        if (!strcmp(n->kind, "touch")) touches++; else if (!strcmp(n->kind, "display")) displays++;
        else if (strcmp(n->kind, "center") && strcmp(n->kind, "counter")) return luaL_error(L, "unknown node capability");
        read_ports(L, n, true); read_ports(L, n, false);
        lua_getfield(L, -1, "code"); size_t length; const char *code = luaL_checklstring(L, -1, &length);
        n->L = lua_newthread(L); *(runtime_t **)lua_getextraspace(n->L) = r;
        lua_sethook(n->L, hook, LUA_MASKCOUNT, 1000);
        char chunk[64]; snprintf(chunk, sizeof(chunk), "@node:%s", n->id);
        if (luaL_loadbufferx(L, code, length, chunk, "t") != LUA_OK) return lua_error(L);
        lua_newtable(L);
        const luaL_Reg functions[] = {{"emit", emit}, {"receive", receive}, {"sleep", sleep_node}, {"sample", sample}, {"paint", paint}, {"mark", mark}, {NULL, NULL}};
        for (const luaL_Reg *f = functions; f->name; ++f) { lua_pushlightuserdata(L, n); lua_pushcclosure(L, f->func, 1); lua_setfield(L, -2, f->name); }
        lua_setupvalue(L, -2, 1); lua_xmove(L, n->L, 1);
        /* Registry owns the coroutine; never expose its handle to another node. */
        lua_rawseti(L, LUA_REGISTRYINDEX, 100 + i);
        lua_pop(L, 2); /* code + node */
    }
    if (touches != 1 || displays != 1) return luaL_error(L, "exactly one touch and display owner required");
    lua_pop(L, 1); lua_getfield(L, -1, "wires"); luaL_checktype(L, -1, LUA_TTABLE);
    r->wire_count = lua_rawlen(L, -1); if (r->wire_count > 32) return luaL_error(L, "too many wires");
    for (unsigned i = 0; i < r->wire_count; ++i) {
        wire_t *w = &r->wires[i]; lua_rawgeti(L, -1, i + 1); luaL_checktype(L, -1, LUA_TTABLE);
        lua_rawgeti(L, -1, 1); w->from = luaL_checkinteger(L, -1) - 1; lua_pop(L, 1);
        lua_rawgeti(L, -1, 3); w->to = luaL_checkinteger(L, -1) - 1; lua_pop(L, 1);
        if (w->from >= r->node_count || w->to >= r->node_count) return luaL_error(L, "wire node missing");
        node_t *a = &r->nodes[w->from], *b = &r->nodes[w->to];
        lua_rawgeti(L, -1, 2); w->output = find_port(L, luaL_checkstring(L, -1), a->outputs, a->output_count); lua_pop(L, 1);
        lua_rawgeti(L, -1, 4); w->input = find_port(L, luaL_checkstring(L, -1), b->inputs, b->input_count); lua_pop(L, 1);
        unsigned type = a->outputs[w->output].type;
        if (type != b->inputs[w->input].type) return luaL_error(L, "wire signal mismatch");
        for (unsigned j = 0; j < i; ++j) {
            wire_t *v = &r->wires[j]; bool from = w->from == v->from && w->output == v->output, to = w->to == v->to && w->input == v->input;
            if ((from && to) || ((type == 2 || type == 3) && (from || to))) return luaL_error(L, "duplicate wire or conflicting command owner");
        }
        lua_pop(L, 1);
    }
    return 0;
}
static int execute(lua_State *L)
{
    runtime_t *r = runtime(L); r->result->phase = "runtime";
    while (!r->platform->finished || !r->platform->finished(r->platform->context)) {
        check(L);
        for (unsigned i = 0; i < r->node_count; ++i) {
            node_t *n = &r->nodes[i]; unsigned arguments = 0;
            if (n->wait == ENDED || (n->wait == SLEEPING && now(r) < n->wake)) continue;
            if (n->wait == RECEIVING) {
                while (n->count && n->inbox[0].expires && now(r) >= n->inbox[0].expires) {
                    memmove(n->inbox, n->inbox + 1, (--n->count) * sizeof(message_t)); r->expired++;
                }
                /* Expired local timer wins a tie; continuous input cannot starve it. */
                if (now(r) >= n->wake) { lua_pushliteral(n->L, "timeout"); lua_pushnil(n->L); }
                else if (n->count) {
                    message_t m = n->inbox[0]; memmove(n->inbox, n->inbox + 1, (--n->count) * sizeof(message_t));
                    int32_t value; unsigned type = n->inputs[m.port].type;
                    if (!ryz_neuro_decode(m.bytes, m.length, type, &value)) return luaL_error(L, "malformed TLV");
                    lua_pushstring(n->L, n->inputs[m.port].id);
                    if (type == 2 || type == 6) lua_pushboolean(n->L, value); else lua_pushinteger(n->L, value);
                } else continue;
                uint64_t elapsed = now(r) - n->waited;
                lua_pushinteger(n->L, elapsed > 300000 ? 300000 : (lua_Integer)elapsed); arguments = 3;
            }
            if (n->wait == PAINTING) {
                int done = r->platform->paint(r->platform->context, n->color, false);
                check(L);
                if (done < 0) return luaL_error(L, "display operation failed");
                if (!done) continue;
            }
            r->budget = 0; r->current = i;
            int results = 0, status = lua_resume(n->L, NULL, arguments, &results);
            if (status != LUA_OK && status != LUA_YIELD) {
                const char *error = lua_tostring(n->L, -1);
                if (status == LUA_ERRMEM) r->result->phase = "memory";
                return luaL_error(L, "node %s: %s", n->id, error ? error : "unknown error");
            }
            if (status == LUA_OK) n->wait = ENDED;
            lua_settop(n->L, 0);
        }
        bool painting = false;
        for (unsigned i = 0; i < r->node_count; ++i) if (r->nodes[i].wait == PAINTING) painting = true;
        r->platform->pause_ms(r->platform->context, painting ? 0 : 1);
    }
    return 0;
}
void ryz_neuro_execute(const char *source, size_t length, const char *name, uint32_t timeout_ms,
                      const ryz_neuro_platform_t *platform, ryz_lua_result_t *result)
{
    memset(result, 0, sizeof(*result)); result->phase = "init";
    runtime_t *r = platform->resize(NULL, sizeof(*r));
    if (!r) { snprintf(result->error, sizeof(result->error), "cannot allocate node runtime"); return; }
    memset(r, 0, sizeof(*r)); r->platform = platform; r->result = result; r->source = source; r->length = length; r->name = name;
    r->start = now(r); r->deadline = timeout_ms ? r->start + timeout_ms : 0;
    if (length > RYZ_LUA_SOURCE_MAX) { snprintf(result->error, sizeof(result->error), "source too large"); goto end; }
    r->L = lua_newstate(allocator, r, 0x52797a);
    if (!r->L) { snprintf(result->error, sizeof(result->error), "cannot create Lua VM"); goto end; }
    *(runtime_t **)lua_getextraspace(r->L) = r; lua_sethook(r->L, hook, LUA_MASKCOUNT, 1000);
    result->phase = "syntax"; lua_pushcfunction(r->L, setup);
    int status = lua_pcall(r->L, 0, 0, 0);
    if (status == LUA_OK) { lua_sethook(r->L, NULL, 0, 0); lua_pushcfunction(r->L, execute); status = lua_pcall(r->L, 0, 0, 0); }
    result->ok = status == LUA_OK;
    if (!result->ok) {
        const char *error = lua_tostring(r->L, -1);
        snprintf(result->error, sizeof(result->error), "%s", error ? error : "Lua runtime error");
        if (status == LUA_ERRMEM) result->phase = "memory";
    } else result->phase = "done";
    if (platform->cleanup) platform->cleanup(platform->context);
    lua_close(r->L);
end:
    result->elapsed_ms = now(r) - r->start;
    snprintf(result->output, sizeof(result->output), "neuro: nodes=%u delivered=%lu expired=%lu merged=%lu native_bytes=%u\n", r->node_count, (unsigned long)r->delivered, (unsigned long)r->expired, (unsigned long)r->merged, (unsigned)sizeof(*r));
    if (platform->output) platform->output(platform->context, result->output, strlen(result->output));
    platform->resize(r, 0);
}
