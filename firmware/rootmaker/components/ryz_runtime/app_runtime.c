/* Shared application-owned Lua facade. Never patches the third-party VM. */
#include "app_runtime.h"
#include "app_tools.h"
#include "lua_sandbox.h"
#include "lua_coroutines.h"
#include "lua_hardware.h"
#include "lua_peripherals.h"
#include "lua_fs.h"
#include "lua_boot.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

typedef struct {
    const ryz_app_platform_t *platform;
    ryz_lua_result_t *result;
    const char *source;
    const char *name;
    size_t source_length;
    size_t allocated;
    size_t output_length;
    uint64_t started;
    uint64_t deadline;
    unsigned hook_count;
    lua_State *L;
    bool closing;
    ryz_lua_hardware_binding_t hardware;
    ryz_lua_peripheral_binding_t peripherals;
    ryz_lua_fs_binding_t filesystem;
    ryz_lua_boot_binding_t boot;
    enum { SCREEN_OWNER_NONE, SCREEN_OWNER_DISPLAY, SCREEN_OWNER_UI } screen_owner;
    enum { INPUT_OWNER_NONE, INPUT_OWNER_TOUCH, INPUT_OWNER_UI } input_owner;
    bool touch_demo_enabled;
    bool ui_active;
    bool ui_has_position;
    uint16_t ui_x;
    uint16_t ui_y;
    uint16_t ui_scroll_y;
    bool ui_scroll_capture;
    bool ui_scroll_started;
    char ui_capture[RYZ_UI_ID_MAX + 1];
    char ui_scroll_id[RYZ_UI_ID_MAX + 1];
    ryz_ui_scene_t ui_scene;
    ryz_ui_scene_t ui_candidate;
} app_t;

static char finished_sentinel;

static app_t *app(lua_State *L) { return *(app_t **)lua_getextraspace(L); }
static uint64_t now(app_t *runtime) { return runtime->platform->now_ms(runtime->platform->context); }

bool ryz_app_is_source(const char *source, size_t length)
{
    static const char marker[] = "-- " RYZ_APP_ABI;
    if (!source || length < sizeof(marker) - 1 || memcmp(source, marker, sizeof(marker) - 1)) return false;
    if (length == sizeof(marker) - 1) return true;
    char next = source[sizeof(marker) - 1];
    return next == '\n' || next == '\r' || next == ' ' || next == '\t';
}

const char *ryz_app_pointer_phase_name(ryz_app_pointer_phase_t phase)
{
    switch (phase) {
    case RYZ_APP_POINTER_DOWN: return "down";
    case RYZ_APP_POINTER_MOVE: return "move";
    case RYZ_APP_POINTER_UP: return "up";
    default: return "none";
    }
}

static void *bounded_allocator(void *userdata, void *ptr, size_t old_size, size_t new_size)
{
    app_t *runtime = userdata;
    if (!ptr) old_size = 0;
    if (!new_size) {
        runtime->platform->resize(ptr, 0);
        runtime->allocated -= old_size;
        return NULL;
    }
    size_t retained = runtime->allocated - old_size;
    if (new_size > RYZ_LUA_HEAP_LIMIT - retained) return NULL;
    void *next = runtime->platform->resize(ptr, new_size);
    if (next) {
        runtime->allocated = retained + new_size;
        if (runtime->allocated > runtime->result->peak_bytes) runtime->result->peak_bytes = runtime->allocated;
    }
    return next;
}

static void check(lua_State *L)
{
    app_t *runtime = app(L);
    /* lua_close runs user finalizers after the platform lease is released.
     * Reject their native calls without overwriting the execution result. */
    if (runtime->closing) luaL_error(L, "application is closing");
    if (!strcmp(runtime->result->phase, "stopped") ||
        (runtime->platform->cancelled && runtime->platform->cancelled(runtime->platform->context))) {
        runtime->result->phase = "stopped";
        luaL_error(L, "stopped by user");
    }
    if (!strcmp(runtime->result->phase, "timeout") ||
        (runtime->deadline && now(runtime) >= runtime->deadline)) {
        runtime->result->phase = "timeout";
        luaL_error(L, "execution timed out");
    }
    if (!strcmp(runtime->result->phase, "done") ||
        (runtime->platform->finished && runtime->platform->finished(runtime->platform->context))) {
        runtime->result->phase = "done";
        lua_pushlightuserdata(L, &finished_sentinel);
        lua_error(L);
    }
}

static int coroutine_check(lua_State *L) { check(L); return 0; }

static void instruction_hook(lua_State *L, lua_Debug *debug)
{
    (void)debug;
    app_t *runtime = app(L);
    check(L);
    if (++runtime->hook_count >= 10) {
        runtime->hook_count = 0;
        runtime->platform->pause_ms(runtime->platform->context, 0);
        check(L);
    }
}

static void append_output(app_t *runtime, const char *text, size_t length)
{
    if (runtime->platform->output) runtime->platform->output(runtime->platform->context, text, length);
    size_t room = RYZ_LUA_OUTPUT_MAX - 1 - runtime->output_length;
    if (length > room) {
        length = room;
        runtime->result->output_truncated = true;
    }
    memcpy(runtime->result->output + runtime->output_length, text, length);
    runtime->output_length += length;
    runtime->result->output[runtime->output_length] = 0;
}

static int captured_print(lua_State *L)
{
    check(L);
    app_t *runtime = app(L);
    for (int i = 1; i <= lua_gettop(L); ++i) {
        size_t length;
        const char *text = luaL_tolstring(L, i, &length);
        if (i > 1) append_output(runtime, "\t", 1);
        append_output(runtime, text, length);
        lua_pop(L, 1);
    }
    append_output(runtime, "\n", 1);
    check(L);
    return 0;
}

static void integer_field(lua_State *L, const char *name, lua_Integer value)
{
    lua_pushinteger(L, value);
    lua_setfield(L, -2, name);
}

static void boolean_field(lua_State *L, const char *name, bool value)
{
    lua_pushboolean(L, value);
    lua_setfield(L, -2, name);
}

static bool identifier(const char *text, size_t length)
{
    if (!text || !length || length > 24 || text[0] < 'a' || text[0] > 'z') return false;
    for (size_t i = 0; i < length; ++i) {
        char c = text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

static bool token_is(const char *value, size_t length, const char *expected)
{
    size_t expected_length = strlen(expected);
    return length == expected_length && !memcmp(value, expected, length);
}

static void claim_display(lua_State *L)
{
    app_t *runtime = app(L);
    if (runtime->screen_owner == SCREEN_OWNER_UI) {
        luaL_error(L, "display.canvas conflicts with ui.scene");
    }
    runtime->screen_owner = SCREEN_OWNER_DISPLAY;
}

static void claim_touch(lua_State *L)
{
    app_t *runtime = app(L);
    if (runtime->input_owner == INPUT_OWNER_UI) {
        luaL_error(L, "touch.read conflicts with ui.events");
    }
    runtime->input_owner = INPUT_OWNER_TOUCH;
}

static int board_info(lua_State *L)
{
    app_t *runtime = app(L);
    lua_newtable(L);
    lua_pushliteral(L, RYZ_APP_ABI); lua_setfield(L, -2, "runtime");
    lua_pushliteral(L, RYZ_APP_CATALOG); lua_setfield(L, -2, "catalog");
    integer_field(L, "width", RYZ_APP_WIDTH);
    integer_field(L, "height", RYZ_APP_HEIGHT);
    integer_field(L, "seed", runtime->platform->seed);
    return 1;
}

static int board_millis(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)now(app(L)));
    return 1;
}

static int board_seed(lua_State *L)
{
    lua_pushinteger(L, app(L)->platform->seed);
    return 1;
}

static int board_sleep(lua_State *L)
{
    app_t *runtime = app(L);
    lua_Integer milliseconds = luaL_checkinteger(L, 1);
    luaL_argcheck(L, milliseconds >= 0 && milliseconds <= 5000, 1, "expected 0..5000 milliseconds");
    uint64_t until = now(runtime) + (uint64_t)milliseconds;
    do {
        check(L);
        runtime->platform->pause_ms(runtime->platform->context, now(runtime) < until ? 1 : 0);
    } while (now(runtime) < until);
    check(L);
    return 0;
}

static int board_mark(lua_State *L)
{
    app_t *runtime = app(L);
    size_t name_length;
    const char *name = luaL_checklstring(L, 1, &name_length);
    luaL_argcheck(L, identifier(name, name_length), 1, "expected identifier up to 24 bytes");
    ryz_app_value_t value = {0};
    switch (lua_type(L, 2)) {
    case LUA_TBOOLEAN:
        value.kind = RYZ_APP_VALUE_BOOLEAN; value.boolean = lua_toboolean(L, 2); break;
    case LUA_TNUMBER: {
        lua_Integer integer = luaL_checkinteger(L, 2);
        luaL_argcheck(L, integer >= INT32_MIN && integer <= INT32_MAX, 2, "expected signed 32-bit integer");
        value.kind = RYZ_APP_VALUE_INTEGER; value.integer = (int32_t)integer; break;
    }
    case LUA_TSTRING:
        value.kind = RYZ_APP_VALUE_STRING; value.string = lua_tolstring(L, 2, &value.length);
        luaL_argcheck(L, identifier(value.string, value.length), 2, "expected identifier string up to 24 bytes");
        break;
    default:
        return luaL_argerror(L, 2, "expected boolean, integer or identifier string");
    }
    if (runtime->platform->mark) runtime->platform->mark(runtime->platform->context, name, &value);
    return 0;
}

static int open_board(lua_State *L)
{
    static const luaL_Reg functions[] = {
        {"info", board_info}, {"millis", board_millis}, {"seed", board_seed},
        {"sleep_ms", board_sleep}, {"mark", board_mark}, {NULL, NULL},
    };
    luaL_newlib(L, functions);
    return 1;
}

static int touch_info(lua_State *L)
{
    lua_newtable(L);
    boolean_field(L, "ready", app(L)->platform->pointer_read != NULL);
    integer_field(L, "width", RYZ_APP_WIDTH);
    integer_field(L, "height", RYZ_APP_HEIGHT);
    return 1;
}

static int touch_read(lua_State *L)
{
    app_t *runtime = app(L);
    check(L);
    claim_touch(L);
    if (!runtime->platform->pointer_read) return luaL_error(L, "touch unavailable");
    ryz_app_pointer_t pointer = {.sampled_ms = (uint32_t)now(runtime)};
    int status = runtime->platform->pointer_read(runtime->platform->context, &pointer);
    check(L);
    if (status) return luaL_error(L, "touch read failed");
    if (pointer.phase < RYZ_APP_POINTER_NONE || pointer.phase > RYZ_APP_POINTER_UP ||
        (pointer.interrupted &&
         (pointer.phase != RYZ_APP_POINTER_NONE || pointer.has_position)) ||
        (pointer.has_position && (pointer.x >= RYZ_APP_WIDTH || pointer.y >= RYZ_APP_HEIGHT))) {
        return luaL_error(L, "touch returned invalid sample");
    }
    lua_newtable(L);
    boolean_field(L, "pressed", pointer.pressed);
    boolean_field(L, "interrupted", pointer.interrupted);
    boolean_field(L, "has_position", pointer.has_position);
    lua_pushstring(L, ryz_app_pointer_phase_name(pointer.phase)); lua_setfield(L, -2, "event");
    integer_field(L, "sampled_ms", pointer.sampled_ms);
    if (pointer.has_position) {
        integer_field(L, "x", pointer.x);
        integer_field(L, "y", pointer.y);
    }
    return 1;
}

static int touch_demo(lua_State *L)
{
    app_t *runtime = app(L);
    luaL_checktype(L, 1, LUA_TBOOLEAN);
    if (runtime->ui_active) return luaL_error(L, "touch.demo conflicts with ui.scene");
    bool enabled = lua_toboolean(L, 1);
    check(L);
    int status = runtime->platform->touch_demo ?
        runtime->platform->touch_demo(runtime->platform->context, enabled) : 0;
    check(L);
    if (status) return luaL_error(L, "touch demo failed");
    runtime->touch_demo_enabled = enabled;
    lua_pushboolean(L, true);
    return 1;
}

static int open_touch(lua_State *L)
{
    static const luaL_Reg functions[] = {{"info", touch_info}, {"read", touch_read}, {"demo", touch_demo}, {NULL, NULL}};
    luaL_newlib(L, functions);
    return 1;
}

static uint16_t display_color(lua_State *L, int index)
{
    lua_Integer color = luaL_checkinteger(L, index);
    luaL_argcheck(L, color >= 0 && color <= UINT16_MAX, index, "expected RGB565 color 0..65535");
    return (uint16_t)color;
}

static void draw(lua_State *L, const ryz_app_display_op_t *op)
{
    app_t *runtime = app(L);
    check(L);
    claim_display(L);
    if (!runtime->platform->display_draw || runtime->platform->display_draw(runtime->platform->context, op)) {
        check(L);
        luaL_error(L, "display operation failed");
    }
    check(L);
}

static int display_clear(lua_State *L)
{
    ryz_app_display_op_t op = {.kind = RYZ_APP_DISPLAY_CLEAR, .color = display_color(L, 1)};
    draw(L, &op);
    return 0;
}

static int display_rect(lua_State *L)
{
    ryz_app_display_op_t op = {
        .kind = RYZ_APP_DISPLAY_RECT,
        .x = (int)luaL_checkinteger(L, 1), .y = (int)luaL_checkinteger(L, 2),
        .width = (int)luaL_checkinteger(L, 3), .height = (int)luaL_checkinteger(L, 4),
        .color = display_color(L, 5),
    };
    luaL_argcheck(L, op.width > 0 && op.height > 0 && op.x >= 0 && op.y >= 0 &&
                  op.x <= RYZ_APP_WIDTH - op.width && op.y <= RYZ_APP_HEIGHT - op.height, 1,
                  "rectangle must fit 240x240 canvas");
    draw(L, &op);
    return 0;
}

static int display_text(lua_State *L)
{
    ryz_app_display_op_t op = {.kind = RYZ_APP_DISPLAY_TEXT};
    op.x = (int)luaL_checkinteger(L, 1); op.y = (int)luaL_checkinteger(L, 2);
    size_t length; const char *text = luaL_checklstring(L, 3, &length);
    op.color = display_color(L, 4); op.scale = (int)luaL_optinteger(L, 5, 1);
    luaL_argcheck(L, length > 0 && length <= RYZ_APP_TEXT_MAX, 3, "expected 1..40 bytes");
    luaL_argcheck(L, op.scale >= 1 && op.scale <= 6, 5, "expected scale 1..6");
    op.width = ((int)length * 6 - 1) * op.scale; op.height = 7 * op.scale;
    luaL_argcheck(L, op.x >= 0 && op.y >= 0 && op.x <= RYZ_APP_WIDTH - op.width && op.y <= RYZ_APP_HEIGHT - op.height, 1, "text must fit 240x240 canvas");
    for (size_t i = 0; i < length; ++i) luaL_argcheck(L, (unsigned char)text[i] >= 0x20 && (unsigned char)text[i] <= 0x7e, 3, "expected printable ASCII");
    memcpy(op.text, text, length); op.text[length] = 0;
    draw(L, &op);
    return 0;
}

static int display_show(lua_State *L)
{
    app_t *runtime = app(L);
    check(L);
    claim_display(L);
    if (!runtime->platform->display_show || runtime->platform->display_show(runtime->platform->context)) {
        check(L);
        return luaL_error(L, "display show failed");
    }
    check(L);
    lua_pushboolean(L, true);
    return 1;
}

static int open_display(lua_State *L)
{
    static const luaL_Reg functions[] = {
        {"clear", display_clear}, {"rect", display_rect}, {"text", display_text}, {"show", display_show}, {NULL, NULL},
    };
    luaL_newlib(L, functions);
    return 1;
}

static bool allowed_field(const char *name, size_t name_length,
                          const char *const *allowed)
{
    for (; *allowed; ++allowed) {
        if (token_is(name, name_length, *allowed)) return true;
    }
    return false;
}

static void check_fields(lua_State *L, int index, const char *const *allowed,
                         const char *description)
{
    index = lua_absindex(L, index);
    lua_pushnil(L);
    while (lua_next(L, index)) {
        size_t name_length = 0;
        const char *name = lua_type(L, -2) == LUA_TSTRING ?
            lua_tolstring(L, -2, &name_length) : NULL;
        if (!name || !allowed_field(name, name_length, allowed)) {
            luaL_error(L, "%s contains an unknown field", description);
        }
        lua_pop(L, 1);
    }
}

static void required_id_field(lua_State *L, int index, const char *name,
                              char destination[RYZ_UI_ID_MAX + 1])
{
    lua_getfield(L, index, name);
    size_t length = 0;
    const char *value = luaL_checklstring(L, -1, &length);
    if (!identifier(value, length)) {
        luaL_error(L,
                   "scene and object ids must be lowercase identifiers up to 24 bytes");
    }
    memcpy(destination, value, length);
    destination[length] = 0;
    lua_pop(L, 1);
}

static lua_Integer integer_field_value(lua_State *L, int index,
                                       const char *name, lua_Integer fallback,
                                       lua_Integer minimum, lua_Integer maximum,
                                       bool required)
{
    lua_getfield(L, index, name);
    if (lua_isnil(L, -1) && !required) {
        lua_pop(L, 1);
        return fallback;
    }
    lua_Integer value = luaL_checkinteger(L, -1);
    if (value < minimum || value > maximum) {
        luaL_error(L, "%s must be in range %lld..%lld", name,
                   (long long)minimum, (long long)maximum);
    }
    lua_pop(L, 1);
    return value;
}

static bool boolean_field_value(lua_State *L, int index, const char *name,
                                bool fallback)
{
    lua_getfield(L, index, name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return fallback;
    }
    luaL_checktype(L, -1, LUA_TBOOLEAN);
    bool value = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return value;
}

static const char *optional_text_field(lua_State *L, int index,
                                       const char *name, size_t *length)
{
    lua_getfield(L, index, name);
    if (lua_isnil(L, -1)) {
        *length = 0;
        return "";
    }
    return luaL_checklstring(L, -1, length);
}

static void parse_ui_object(lua_State *L, int index, ryz_ui_object_t *object)
{
    static const char *const fields[] = {
        "id", "kind", "x", "y", "width", "height", "text",
        "foreground", "background", "border", "border_width", "radius",
        "font", "align", "visible", "enabled", "parent", "content_height",
        "scroll_y", "line_height", "asset", NULL,
    };
    index = lua_absindex(L, index);
    luaL_checktype(L, index, LUA_TTABLE);
    check_fields(L, index, fields, "ui object");
    memset(object, 0, sizeof(*object));
    required_id_field(L, index, "id", object->id);

    lua_getfield(L, index, "kind");
    size_t kind_length = 0;
    const char *kind = luaL_checklstring(L, -1, &kind_length);
    if (token_is(kind, kind_length, "box")) object->kind = RYZ_UI_BOX;
    else if (token_is(kind, kind_length, "label")) object->kind = RYZ_UI_LABEL;
    else if (token_is(kind, kind_length, "button")) object->kind = RYZ_UI_BUTTON;
    else if (token_is(kind, kind_length, "viewport")) object->kind = RYZ_UI_VIEWPORT;
    else if (token_is(kind, kind_length, "image")) object->kind = RYZ_UI_IMAGE;
    else luaL_error(L, "unsupported ui object kind");
    lua_pop(L, 1);

    lua_getfield(L, index, "parent");
    if (!lua_isnil(L, -1)) required_id_field(L, index, "parent", object->parent);
    lua_pop(L, 1);
    if (object->kind == RYZ_UI_VIEWPORT && object->parent[0])
        luaL_error(L, "nested viewports are not supported");

    object->x = (int16_t)integer_field_value(L, index, "x", 0, 0, RYZ_APP_WIDTH - 1, true);
    object->y = (int16_t)integer_field_value(L, index, "y", 0, 0,
        object->parent[0] ? 1023 : RYZ_APP_HEIGHT - 1, true);
    object->width = (uint16_t)integer_field_value(L, index, "width", 0, 1, RYZ_APP_WIDTH, true);
    object->height = (uint16_t)integer_field_value(L, index, "height", 0, 1, RYZ_APP_HEIGHT, true);
    if (object->x > RYZ_APP_WIDTH - object->width ||
        (!object->parent[0] && object->y > RYZ_APP_HEIGHT - object->height)) {
        luaL_error(L, "ui object must fit 240x240 canvas");
    }
    object->content_height = (uint16_t)integer_field_value(L, index, "content_height", 0,
        object->kind == RYZ_UI_VIEWPORT ? object->height : 0,
        object->kind == RYZ_UI_VIEWPORT ? 1024 : 0, object->kind == RYZ_UI_VIEWPORT);
    object->scroll_y = (uint16_t)integer_field_value(L, index, "scroll_y", 0, 0,
        object->kind == RYZ_UI_VIEWPORT ? object->content_height - object->height : 0, false);
    object->line_height = (uint8_t)integer_field_value(L, index, "line_height", 0, 0, 64, false);
    if (object->line_height && object->line_height < 12)
        luaL_error(L, "ui line height must be zero or at least 12");
    lua_getfield(L, index, "asset");
    if (!lua_isnil(L, -1)) required_id_field(L, index, "asset", object->asset);
    lua_pop(L, 1);
    bool image = object->kind == RYZ_UI_IMAGE;
    if (image ? (strcmp(object->asset, "rootmaker_a_face") && strcmp(object->asset, "monitor_divider")) : object->asset[0] != 0)
        luaL_error(L, "unsupported ui asset");
    if (image && (object->width != (!strcmp(object->asset,"rootmaker_a_face") ? 88 : 232) ||
        object->height != (!strcmp(object->asset,"rootmaker_a_face") ? 88 : 2)))
        luaL_error(L, "ui asset requires native dimensions");
    object->foreground = (uint16_t)integer_field_value(L, index, "foreground", 0xffff, 0, UINT16_MAX, false);
    object->background = (uint16_t)integer_field_value(L, index, "background", 0, 0, UINT16_MAX, false);
    object->border = (uint16_t)integer_field_value(L, index, "border", object->foreground, 0, UINT16_MAX, false);
    object->border_width = (uint8_t)integer_field_value(L, index, "border_width", 0, 0, 2, false);
    object->radius = (uint8_t)integer_field_value(L, index, "radius", 0, 0, 32, false);
    object->visible = boolean_field_value(L, index, "visible", true);
    object->enabled = boolean_field_value(L, index, "enabled", true);

    size_t text_length = 0;
    const char *text = optional_text_field(L, index, "text", &text_length);
    bool text_object = object->kind == RYZ_UI_LABEL || object->kind == RYZ_UI_BUTTON;
    if (text_object && (text_length < 1 || text_length > RYZ_UI_TEXT_MAX)) {
        luaL_error(L, "label and button text must contain 1..40 bytes");
    }
    if (!text_object && text_length != 0) {
        luaL_error(L, "box does not accept text");
    }
    for (size_t i = 0; i < text_length; ++i) {
        if (text[i] != '\n' && ((unsigned char)text[i] < 0x20 || (unsigned char)text[i] > 0x7e)) {
            luaL_error(L, "ui text must be printable ASCII");
        }
    }
    memcpy(object->text, text, text_length);
    object->text[text_length] = 0;
    lua_pop(L, 1);

    lua_getfield(L, index, "font");
    size_t font_length = 7;
    const char *font = lua_isnil(L, -1) ?
        "body_16" : luaL_checklstring(L, -1, &font_length);
    if (token_is(font, font_length, "body_16")) object->font = RYZ_UI_FONT_BODY_16;
    else if (token_is(font, font_length, "title_24")) object->font = RYZ_UI_FONT_TITLE_24;
    else if (token_is(font, font_length, "mono_12")) object->font = RYZ_UI_FONT_MONO_12;
    else if (token_is(font, font_length, "mono_14")) object->font = RYZ_UI_FONT_MONO_14;
    else if (token_is(font, font_length, "mono_semibold_12")) object->font = RYZ_UI_FONT_MONO_SEMIBOLD_12;
    else if (token_is(font, font_length, "mono_semibold_14")) object->font = RYZ_UI_FONT_MONO_SEMIBOLD_14;
    else if (token_is(font, font_length, "button_14")) object->font = RYZ_UI_FONT_BUTTON_14;
    else if (token_is(font, font_length, "display_20")) object->font = RYZ_UI_FONT_DISPLAY_20;
    else if (token_is(font, font_length, "mono_medium_12")) object->font = RYZ_UI_FONT_MONO_MEDIUM_12;
    else if (token_is(font, font_length, "body_12")) object->font = RYZ_UI_FONT_BODY_12;
    else if (token_is(font, font_length, "body_14")) object->font = RYZ_UI_FONT_BODY_14;
    else if (token_is(font, font_length, "button_12")) object->font = RYZ_UI_FONT_BUTTON_12;
    else if (token_is(font, font_length, "medium_14")) object->font = RYZ_UI_FONT_MEDIUM_14;
    else if (token_is(font, font_length, "button_16")) object->font = RYZ_UI_FONT_BUTTON_16;
    else if (token_is(font, font_length, "medium_12")) object->font = RYZ_UI_FONT_MEDIUM_12;
    else if (token_is(font, font_length, "mono_medium_13")) object->font = RYZ_UI_FONT_MONO_MEDIUM_13;
    else luaL_error(L, "unsupported ui font");
    lua_pop(L, 1);

    lua_getfield(L, index, "align");
    size_t align_length = object->kind == RYZ_UI_LABEL ? 4 : 6;
    const char *align = lua_isnil(L, -1) ?
        (object->kind == RYZ_UI_LABEL ? "left" : "center") :
        luaL_checklstring(L, -1, &align_length);
    if (token_is(align, align_length, "left")) object->align = RYZ_UI_ALIGN_LEFT;
    else if (token_is(align, align_length, "center")) object->align = RYZ_UI_ALIGN_CENTER;
    else if (token_is(align, align_length, "right")) object->align = RYZ_UI_ALIGN_RIGHT;
    else luaL_error(L, "align must be left, center or right");
    lua_pop(L, 1);
}

static void parse_ui_scene(lua_State *L, int index, ryz_ui_scene_t *scene)
{
    static const char *const fields[] = {"id", "background", "objects", NULL};
    index = lua_absindex(L, index);
    luaL_checktype(L, index, LUA_TTABLE);
    check_fields(L, index, fields, "ui scene");
    memset(scene, 0, sizeof(*scene));
    required_id_field(L, index, "id", scene->id);
    scene->background = (uint16_t)integer_field_value(
        L, index, "background", 0, 0, UINT16_MAX, true);
    lua_getfield(L, index, "objects");
    luaL_checktype(L, -1, LUA_TTABLE);
    size_t count = lua_rawlen(L, -1);
    if (count > RYZ_UI_OBJECT_MAX) luaL_error(L, "ui scene exceeds 32 objects");
    int objects_index = lua_absindex(L, -1);
    lua_pushnil(L);
    while (lua_next(L, objects_index)) {
        if (!lua_isinteger(L, -2)) luaL_error(L, "ui objects must be a dense array");
        lua_Integer key = lua_tointeger(L, -2);
        if (key < 1 || (size_t)key > count) luaL_error(L, "ui objects must be a dense array");
        lua_pop(L, 1);
    }
    for (size_t i = 0; i < count; ++i) {
        lua_geti(L, objects_index, (lua_Integer)i + 1);
        parse_ui_object(L, -1, &scene->objects[i]);
        lua_pop(L, 1);
        for (size_t prior = 0; prior < i; ++prior) {
            if (!strcmp(scene->objects[prior].id, scene->objects[i].id)) {
                luaL_error(L, "ui object ids must be unique");
            }
        }
        ryz_ui_object_t *o = &scene->objects[i];
        if (o->parent[0]) {
            const ryz_ui_object_t *parent = NULL;
            for (size_t prior = 0; prior < i; ++prior)
                if (!strcmp(scene->objects[prior].id, o->parent)) parent = &scene->objects[prior];
            if (!parent || parent->kind != RYZ_UI_VIEWPORT ||
                o->x + o->width > parent->width || o->y + o->height > parent->content_height)
                luaL_error(L, "child must fit an earlier viewport content");
        }
    }
    scene->object_count = (uint8_t)count;
    lua_pop(L, 1);
}

static int ui_mount(lua_State *L)
{
    app_t *runtime = app(L);
    check(L);
    if (!runtime->platform->ui_mount || !runtime->platform->ui_close) {
        return luaL_error(L, "ui unavailable");
    }
    if (runtime->screen_owner == SCREEN_OWNER_DISPLAY) {
        return luaL_error(L, "ui.scene conflicts with display.canvas");
    }
    if (runtime->touch_demo_enabled) {
        return luaL_error(L, "ui.scene conflicts with touch.demo");
    }
    parse_ui_scene(L, 1, &runtime->ui_candidate);
    if (runtime->ui_scene.generation >= INT32_MAX) {
        return luaL_error(L, "ui scene generation exhausted");
    }
    runtime->ui_candidate.generation = runtime->ui_scene.generation + 1;
    int status = runtime->platform->ui_mount(runtime->platform->context,
                                             &runtime->ui_candidate);
    if (!status) runtime->ui_active = true;
    check(L);
    if (status) {
        return luaL_error(L, "ui mount failed");
    }
    runtime->ui_scene = runtime->ui_candidate;
    runtime->screen_owner = SCREEN_OWNER_UI;
    runtime->ui_active = true;
    runtime->ui_has_position = false;
    runtime->ui_capture[0] = 0;
    runtime->ui_scroll_capture = false;
    runtime->ui_scroll_started = false;
    lua_pushinteger(L, runtime->ui_scene.generation);
    return 1;
}

static void plain_ui_table(lua_State *L, int index)
{
    luaL_checktype(L, index, LUA_TTABLE);
    if (lua_getmetatable(L, index)) luaL_error(L, "ui update requires plain tables");
}

static uint16_t ui_patch_color(lua_State *L, int index, const char *name,
                                uint16_t original)
{
    lua_getfield(L, index, name);
    if (lua_isnil(L, -1)) { lua_pop(L, 1); return original; }
    if (!lua_isinteger(L, -1)) luaL_error(L, "ui update colors must be integers");
    lua_Integer value = lua_tointeger(L, -1);
    if (value < 0 || value > UINT16_MAX) luaL_error(L, "ui update color out of range");
    lua_pop(L, 1);
    return (uint16_t)value;
}

static int ui_update(lua_State *L)
{
    app_t *runtime = app(L);
    check(L);
    if (lua_gettop(L) != 2) return luaL_error(L, "ui.update expects generation and patches");
    if (!runtime->ui_active || runtime->screen_owner != SCREEN_OWNER_UI)
        return luaL_error(L, "ui scene is not mounted");
    if (!runtime->platform->ui_update) return luaL_error(L, "ui update unavailable");
    if (!lua_isinteger(L, 1) || lua_tointeger(L, 1) <= 0 ||
        lua_tointeger(L, 1) > INT32_MAX ||
        (uint32_t)lua_tointeger(L, 1) != runtime->ui_scene.generation)
        return luaL_error(L, "ui update requires current generation");
    plain_ui_table(L, 2);
    size_t count = lua_rawlen(L, 2);
    if (count > RYZ_UI_OBJECT_MAX) return luaL_error(L, "ui update exceeds 32 patches");
    lua_pushnil(L);
    while (lua_next(L, 2)) {
        if (!lua_isinteger(L, -2) || lua_tointeger(L, -2) < 1 ||
            (size_t)lua_tointeger(L, -2) > count)
            return luaL_error(L, "ui patches must be a dense array");
        lua_pop(L, 1);
    }
    runtime->ui_candidate = runtime->ui_scene;
    bool seen[RYZ_UI_OBJECT_MAX] = {false};
    bool changed = false, cancel_capture = false, cancel_scroll = false;
    static const char *const fields[] = {
        "id", "text", "foreground", "background", "border", "visible", "enabled", "scroll_y", "y", "height", "width", NULL,
    };
    for (size_t i = 0; i < count; ++i) {
        lua_rawgeti(L, 2, (lua_Integer)i + 1);
        int patch = lua_absindex(L, -1);
        plain_ui_table(L, patch);
        check_fields(L, patch, fields, "ui patch");
        char id[RYZ_UI_ID_MAX + 1];
        required_id_field(L, patch, "id", id);
        size_t target = 0;
        while (target < runtime->ui_scene.object_count &&
               strcmp(id, runtime->ui_scene.objects[target].id)) ++target;
        if (target == runtime->ui_scene.object_count)
            return luaL_error(L, "ui update has unknown object id");
        if (seen[target]) return luaL_error(L, "ui update has duplicate object id");
        seen[target] = true;
        ryz_ui_object_t *object = &runtime->ui_candidate.objects[target];
        lua_getfield(L, patch, "text");
        if (!lua_isnil(L, -1)) {
            if (object->kind != RYZ_UI_LABEL && object->kind != RYZ_UI_BUTTON)
                return luaL_error(L, "box does not accept text");
            luaL_checktype(L, -1, LUA_TSTRING);
            size_t length = 0;
            const char *text = lua_tolstring(L, -1, &length);
            if (length < 1 || length > RYZ_UI_TEXT_MAX)
                return luaL_error(L, "ui update text must contain 1..40 bytes");
            for (size_t c = 0; c < length; ++c)
                if (text[c] != '\n' && ((unsigned char)text[c] < 0x20 || (unsigned char)text[c] > 0x7e))
                    return luaL_error(L, "ui text must be printable ASCII");
            memset(object->text, 0, sizeof(object->text));
            memcpy(object->text, text, length);
        }
        lua_pop(L, 1);
        object->foreground = ui_patch_color(L, patch, "foreground", object->foreground);
        object->background = ui_patch_color(L, patch, "background", object->background);
        object->border = ui_patch_color(L, patch, "border", object->border);
        object->visible = boolean_field_value(L, patch, "visible", object->visible);
        object->enabled = boolean_field_value(L, patch, "enabled", object->enabled);
        object->scroll_y = (uint16_t)integer_field_value(L, patch, "scroll_y", object->scroll_y, 0,
            object->kind == RYZ_UI_VIEWPORT ? object->content_height - object->height : 0, false);
        int max_height = RYZ_APP_HEIGHT, max_width = RYZ_APP_WIDTH;
        if (object->parent[0]) for (size_t j = 0; j < target; ++j)
            if (!strcmp(runtime->ui_candidate.objects[j].id,object->parent)) {
                max_height = runtime->ui_candidate.objects[j].content_height;
                max_width = runtime->ui_candidate.objects[j].width;
            }
        int y = (int)integer_field_value(L,patch,"y",object->y,0,max_height-1,false);
        int height = (int)integer_field_value(L,patch,"height",object->height,1,RYZ_APP_HEIGHT,false);
        lua_getfield(L,patch,"width");
        if(!lua_isnil(L,-1) && !lua_isinteger(L,-1))
            return luaL_error(L,"ui width must be an integer");
        lua_pop(L,1);
        int width = (int)integer_field_value(L,patch,"width",object->width,1,RYZ_APP_WIDTH,false);
        if (y + height > max_height || object->x + width > max_width ||
            (object->kind != RYZ_UI_BOX && (y != object->y || height != object->height || width != object->width)))
            return luaL_error(L,"only box y/height/width may change within its canvas");
        bool geometry_changed = y != object->y || height != object->height || width != object->width;
        object->y = y; object->height = height; object->width = width;
        bool differs = memcmp(object, &runtime->ui_scene.objects[target], sizeof(*object)) != 0;
        changed |= differs;
        cancel_capture |= differs && (object->kind == RYZ_UI_BUTTON ||
            (!strcmp(object->id, runtime->ui_capture) && (!object->visible || !object->enabled)) ||
            (object->kind == RYZ_UI_VIEWPORT && !runtime->ui_scroll_started));
        if (geometry_changed && !strcmp(object->id,runtime->ui_scroll_id)) {
            cancel_capture = true;
            cancel_scroll = true;
        }
        cancel_scroll |= !strcmp(object->id,runtime->ui_scroll_id) && (!object->visible || !object->enabled);
        lua_pop(L, 1);
    }
    if (changed) {
        /* Full validation precedes the single owner call. A renderer can have
         * changed pixels before reporting failure; cleanup, not rollback,
         * closes that execution. Label updates preserve the pressed button. */
        int status = runtime->platform->ui_update(runtime->platform->context,
                                                   &runtime->ui_candidate);
        check(L);
        if (status) return luaL_error(L, "ui update failed");
        runtime->ui_scene = runtime->ui_candidate;
        if (cancel_capture) {
            runtime->ui_capture[0] = 0;
            if (!runtime->ui_scroll_started) cancel_scroll = true;
        }
        if (cancel_scroll) runtime->ui_scroll_capture = false;
    }
    lua_pushinteger(L, runtime->ui_scene.generation);
    return 1;
}

static const ryz_ui_object_t *ui_hit(const app_t *runtime, uint16_t x, uint16_t y)
{
    for (size_t i = runtime->ui_scene.object_count; i > 0; --i) {
        const ryz_ui_object_t *object = &runtime->ui_scene.objects[i - 1];
        int ox = object->x, oy = object->y;
        if (object->parent[0]) {
            const ryz_ui_object_t *parent = NULL;
            for (size_t j = 0; j < i - 1; ++j)
                if (!strcmp(runtime->ui_scene.objects[j].id, object->parent)) parent = &runtime->ui_scene.objects[j];
            if (!parent || !parent->visible || !parent->enabled || x < parent->x || y < parent->y ||
                x >= parent->x + parent->width || y >= parent->y + parent->height) continue;
            ox += parent->x; oy += parent->y - parent->scroll_y;
        }
        if (object->kind == RYZ_UI_BUTTON && object->visible && object->enabled &&
            x >= ox && y >= oy && x < ox + object->width && y < oy + object->height) return object;
    }
    return NULL;
}

static int ui_poll(lua_State *L)
{
    app_t *runtime = app(L);
    check(L);
    if (!runtime->ui_active) return luaL_error(L, "ui scene is not mounted");
    if (lua_gettop(L) > 1) return luaL_error(L, "ui.poll expects an optional scroll box id");
    const ryz_ui_object_t *scroll = NULL;
    if (!lua_isnoneornil(L, 1)) {
        luaL_checktype(L, 1, LUA_TSTRING);
        size_t length;
        const char *id = lua_tolstring(L, 1, &length);
        if (!identifier(id, length)) return luaL_error(L, "invalid scroll box id");
        for (size_t i = 0; i < runtime->ui_scene.object_count; ++i)
            if (!strcmp(runtime->ui_scene.objects[i].id, id)) scroll = &runtime->ui_scene.objects[i];
        if (!scroll || (scroll->kind != RYZ_UI_BOX && scroll->kind != RYZ_UI_VIEWPORT))
            return luaL_error(L, "scroll target must be a mounted box");
    }
    /* Opt-in is per poll: changing/removing the region revokes an old drag. */
    if (runtime->ui_scroll_capture && (!scroll || !scroll->visible || !scroll->enabled ||
        strcmp(scroll->id, runtime->ui_scroll_id))) {
        runtime->ui_capture[0] = 0;
        runtime->ui_scroll_capture = false;
    }
    if (runtime->input_owner == INPUT_OWNER_TOUCH) {
        return luaL_error(L, "ui.events conflicts with touch.read");
    }
    if (!runtime->platform->pointer_read) return luaL_error(L, "touch unavailable");
    runtime->input_owner = INPUT_OWNER_UI;
    if (runtime->platform->ui_pump) {
        int status = runtime->platform->ui_pump(runtime->platform->context);
        check(L);
        if (status) return luaL_error(L, "ui refresh failed");
    }
    ryz_app_pointer_t pointer = {.sampled_ms = (uint32_t)now(runtime)};
    int status = runtime->platform->pointer_read(runtime->platform->context,
                                                 &pointer);
    check(L);
    if (status) return luaL_error(L, "touch read failed");
    if (pointer.phase < RYZ_APP_POINTER_NONE || pointer.phase > RYZ_APP_POINTER_UP ||
        (pointer.interrupted &&
         (pointer.phase != RYZ_APP_POINTER_NONE || pointer.has_position)) ||
        (pointer.has_position && (pointer.x >= RYZ_APP_WIDTH || pointer.y >= RYZ_APP_HEIGHT))) {
        return luaL_error(L, "touch returned invalid sample");
    }
    if (pointer.interrupted) {
        runtime->ui_capture[0] = 0;
        runtime->ui_has_position = false;
        runtime->ui_scroll_capture = false;
        runtime->ui_scroll_started = false;
        lua_pushnil(L);
        return 1;
    }
    if (pointer.has_position) {
        runtime->ui_x = pointer.x;
        runtime->ui_y = pointer.y;
        runtime->ui_has_position = true;
    }
    if (pointer.phase == RYZ_APP_POINTER_DOWN) {
        const ryz_ui_object_t *target = pointer.has_position ?
            ui_hit(runtime, pointer.x, pointer.y) : NULL;
        runtime->ui_scroll_capture = false;
        runtime->ui_scroll_started = false;
        if (scroll && scroll->visible && scroll->enabled && pointer.has_position &&
            (!target || (scroll->kind == RYZ_UI_VIEWPORT && !strcmp(target->parent,scroll->id))) &&
            pointer.x >= scroll->x && pointer.y >= scroll->y &&
            pointer.x < scroll->x + scroll->width && pointer.y < scroll->y + scroll->height) {
            if (!target) target = scroll;
            runtime->ui_scroll_capture = true;
            runtime->ui_scroll_y = pointer.y;
            snprintf(runtime->ui_scroll_id, sizeof(runtime->ui_scroll_id), "%s", scroll->id);
        }
        snprintf(runtime->ui_capture, sizeof(runtime->ui_capture), "%s",
                 target ? target->id : "");
    }
    if (runtime->ui_scroll_capture) {
        bool released = pointer.phase == RYZ_APP_POINTER_UP;
        int dy = pointer.has_position ? (int)pointer.y - runtime->ui_scroll_y : 0;
        bool moved = pointer.phase == RYZ_APP_POINTER_MOVE || released;
        bool emit = moved && dy && (runtime->ui_scroll_started || abs(dy) >= 6);
        if (released) runtime->ui_scroll_capture = false;
        if (emit) {
            runtime->ui_capture[0] = 0; /* A drag can never become a click. */
            runtime->ui_scroll_started = true;
            runtime->ui_scroll_y = pointer.y;
            lua_newtable(L);
            lua_pushliteral(L, "scroll"); lua_setfield(L, -2, "kind");
            lua_pushstring(L, runtime->ui_scene.id); lua_setfield(L, -2, "scene");
            lua_pushinteger(L, runtime->ui_scene.generation); lua_setfield(L, -2, "generation");
            lua_pushstring(L, scroll->id); lua_setfield(L, -2, "id");
            lua_pushinteger(L, pointer.sampled_ms); lua_setfield(L, -2, "at_ms");
            lua_pushinteger(L, dy); lua_setfield(L, -2, "dy");
            return 1;
        }
        if (runtime->ui_scroll_started) { lua_pushnil(L); return 1; }
    }
    if (pointer.phase != RYZ_APP_POINTER_UP || !runtime->ui_capture[0]) {
        lua_pushnil(L);
        return 1;
    }
    const ryz_ui_object_t *target = runtime->ui_has_position ?
        ui_hit(runtime, runtime->ui_x, runtime->ui_y) : NULL;
    char captured[sizeof(runtime->ui_capture)];
    snprintf(captured, sizeof(captured), "%s", runtime->ui_capture);
    runtime->ui_capture[0] = 0;
    if (!target || strcmp(target->id, captured)) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    lua_pushliteral(L, "activate"); lua_setfield(L, -2, "kind");
    lua_pushstring(L, runtime->ui_scene.id); lua_setfield(L, -2, "scene");
    lua_pushinteger(L, runtime->ui_scene.generation); lua_setfield(L, -2, "generation");
    lua_pushstring(L, target->id); lua_setfield(L, -2, "id");
    lua_pushinteger(L, pointer.sampled_ms); lua_setfield(L, -2, "at_ms");
    return 1;
}

static int open_ui(lua_State *L)
{
    static const luaL_Reg functions[] = {
        {"mount", ui_mount}, {"update", ui_update}, {"poll", ui_poll}, {NULL, NULL},
    };
    luaL_newlib(L, functions);
    return 1;
}

static int require_native(lua_State *L)
{
    size_t name_length = 0;
    const char *name = luaL_checklstring(L, 1, &name_length);
    if (token_is(name, name_length, "ryzobee")) lua_getfield(L, LUA_REGISTRYINDEX, "ryz-app.board");
    else if (token_is(name, name_length, "touch")) lua_getfield(L, LUA_REGISTRYINDEX, "ryz-app.touch");
    else if (token_is(name, name_length, "display")) lua_getfield(L, LUA_REGISTRYINDEX, "ryz-app.display");
    else if (token_is(name, name_length, "ui")) lua_getfield(L, LUA_REGISTRYINDEX, "ryz-app.ui");
    else if (token_is(name, name_length, "tools")) lua_getfield(L, LUA_REGISTRYINDEX, "ryz-app.tools");
    else if (ryz_lua_hardware_require(L, name, name_length)) return 1;
    else if (ryz_lua_peripherals_require(L, name, name_length)) return 1;
    else if (ryz_lua_fs_require(L, name, name_length)) return 1;
    else if (ryz_lua_boot_require(L, name, name_length)) return 1;
    else return luaL_error(L, "module not allowed: %s", name);
    return 1;
}

static int protected_run(lua_State *L)
{
    app_t *runtime = app(L);
    static const luaL_Reg libraries[] = {
        {LUA_GNAME, luaopen_base}, {LUA_TABLIBNAME, luaopen_table},
        {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8}, {NULL, NULL},
    };
    for (const luaL_Reg *library = libraries; library->name; ++library) {
        luaL_requiref(L, library->name, library->func, 1);
        lua_pop(L, 1);
    }
    ryz_lua_sandbox_install(L);
    ryz_lua_coroutines_install(L, coroutine_check);
    runtime->hardware = (ryz_lua_hardware_binding_t){
        runtime->platform->hardware_call, runtime->platform->context, check};
    ryz_lua_hardware_install(L, &runtime->hardware);
    runtime->peripherals=(ryz_lua_peripheral_binding_t){.call=runtime->platform->peripheral_call,
        .context=runtime->platform->context,.check=check};
    ryz_lua_peripherals_install(L,&runtime->peripherals);
    runtime->filesystem = (ryz_lua_fs_binding_t){.call = runtime->platform->fs_call,
        .context = runtime->platform->context, .check = check};
    ryz_lua_fs_install(L, &runtime->filesystem, runtime->name);
    runtime->boot = (ryz_lua_boot_binding_t){.poll = runtime->platform->boot_poll,
        .context = runtime->platform->context, .check = check};
    ryz_lua_boot_install(L, &runtime->boot);
    const char *removed[] = {"dofile", "loadfile", "load", "pcall", "xpcall", NULL};
    for (const char **name = removed; *name; ++name) { lua_pushnil(L); lua_setglobal(L, *name); }
    lua_pushcfunction(L, captured_print); lua_setglobal(L, "print");
    open_board(L); lua_setfield(L, LUA_REGISTRYINDEX, "ryz-app.board");
    open_touch(L); lua_setfield(L, LUA_REGISTRYINDEX, "ryz-app.touch");
    open_display(L); lua_setfield(L, LUA_REGISTRYINDEX, "ryz-app.display");
    open_ui(L); lua_setfield(L, LUA_REGISTRYINDEX, "ryz-app.ui");
    ryz_app_tools_open(L, runtime->platform->tool_call, runtime->platform->context, check);
    lua_setfield(L, LUA_REGISTRYINDEX, "ryz-app.tools");
    lua_pushcfunction(L, require_native); lua_setglobal(L, "require");
    runtime->result->phase = "syntax";
    if (luaL_loadbufferx(L, runtime->source, runtime->source_length, runtime->name, "t") != LUA_OK) return lua_error(L);
    runtime->result->phase = "runtime";
    lua_call(L, 0, 0);
    return 0;
}

void ryz_app_execute(const char *source, size_t length, const char *name,
                     uint32_t timeout_ms, const ryz_app_platform_t *platform,
                     ryz_lua_result_t *result)
{
    memset(result, 0, sizeof(*result)); result->phase = "init";
    if (!platform || !platform->resize || !platform->now_ms || !platform->pause_ms) {
        snprintf(result->error, sizeof(result->error), "invalid app platform"); return;
    }
    if (!ryz_app_is_source(source, length)) {
        snprintf(result->error, sizeof(result->error), "missing %s marker", RYZ_APP_ABI); return;
    }
    if (length > RYZ_LUA_SOURCE_MAX) {
        snprintf(result->error, sizeof(result->error), "source too large"); return;
    }
    app_t *runtime = platform->resize(NULL, sizeof(*runtime));
    if (!runtime) { snprintf(result->error, sizeof(result->error), "cannot allocate app runtime"); return; }
    memset(runtime, 0, sizeof(*runtime));
    runtime->platform = platform; runtime->result = result; runtime->source = source;
    runtime->source_length = length; runtime->name = name; runtime->started = now(runtime);
    runtime->deadline = timeout_ms ? runtime->started + timeout_ms : 0;
    runtime->L = lua_newstate(bounded_allocator, runtime, platform->seed);
    if (!runtime->L) { snprintf(result->error, sizeof(result->error), "cannot create Lua VM"); goto done; }
    *(app_t **)lua_getextraspace(runtime->L) = runtime;
    lua_sethook(runtime->L, instruction_hook, LUA_MASKCOUNT, 1000);
    lua_pushcfunction(runtime->L, protected_run);
    int status = lua_pcall(runtime->L, 0, 0, 0);
    /* Revoke native calls before cleanup or result processing can run Lua.
     * ui_close still needs ui_active to release an acquired scene exactly once. */
    runtime->closing = true;
    ryz_lua_peripherals_cleanup(&runtime->peripherals);
    ryz_app_tools_revoke(runtime->L);
    bool completed_by_platform = status != LUA_OK && lua_touserdata(runtime->L, -1) == &finished_sentinel;
    result->ok = status == LUA_OK || completed_by_platform;
    if (!result->ok) {
        const char *error = lua_tostring(runtime->L, -1);
        snprintf(result->error, sizeof(result->error), "%s", error ? error : "non-string Lua error");
        if (status == LUA_ERRMEM) result->phase = "memory";
    } else result->phase = "done";
    if (runtime->ui_active && platform->ui_close) platform->ui_close(platform->context);
    if (platform->cleanup) platform->cleanup(platform->context);
    lua_close(runtime->L);
done:
    result->elapsed_ms = (uint32_t)(now(runtime) - runtime->started);
    platform->resize(runtime, 0);
}
