/* ESP-Claw-inspired execution boundary: isolated VM, registered C module,
 * print capture and instruction hook. This is a bring-up runtime, not an
 * adversarial-code security sandbox or the complete ESP-Claw Agent. */
#include "lua_runtime.h"
#include "lua_sandbox.h"
#include "lua_coroutines.h"
#include "lua_hardware.h"
#include "lua_hardware_esp.h"
#include "lua_peripherals.h"
#include "lua_fs.h"
#include "lua_boot.h"
#include "app_runtime.h"
#include "display.h"
#include "touch.h"
#include "touch_demo.h"
#include "nervous_runtime.h"
#include "ryz_lvgl.h"
#include "ryz_runtime_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

typedef struct {
    ryz_lua_result_t *result;
    size_t allocated;
    size_t output_len;
    int64_t deadline_us;
    uint32_t timeout_ms;
    bool timing_started;
    bool closing;
    ryz_lua_hardware_session_t hardware_session;
    ryz_lua_hardware_binding_t hardware;
    ryz_peripheral_session_t peripheral_session;
    ryz_lua_peripheral_binding_t peripherals;
    ryz_lua_fs_binding_t filesystem;
    ryz_lua_boot_binding_t boot;
    int64_t next_yield_us;
    const char *source;
    size_t source_len;
    const char *name;
    const ryz_lua_options_t *options;
} run_context_t;

static run_context_t *context(lua_State *L)
{
    return *(run_context_t **)lua_getextraspace(L);
}

static bool display_cancelled(void *arg);

static void runtime_io(run_context_t *ctx, ryz_runtime_io_request_t *request)
{
    request->status = ESP_ERR_INVALID_STATE;
    request->cancelled = display_cancelled;
    request->cancel_context = ctx;
    if (ctx->options && ctx->options->io_call) {
        if (!ctx->options->io_call(ctx->options->context, request)) {
            request->status = ESP_ERR_INVALID_STATE;
        }
    } else {
        /* Only the serialized boot path may use direct execution. Workbench
         * always supplies its acknowledged owner channel for a worker job. */
        ryz_runtime_io_execute(request, NULL, NULL);
    }
}

static void *bounded_alloc(void *ud, void *ptr, size_t old_size, size_t new_size)
{
    run_context_t *ctx = ud;
    if (!ptr) old_size = 0; /* Lua passes a type tag for fresh allocations. */
    if (!new_size) {
        heap_caps_free(ptr);
        ctx->allocated -= old_size;
        return NULL;
    }
    size_t retained = ctx->allocated - old_size;
    if (new_size > RYZ_LUA_HEAP_LIMIT - retained) return NULL;
    void *next = heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (next) {
        ctx->allocated = retained + new_size;
        if (ctx->allocated > ctx->result->peak_bytes) {
            ctx->result->peak_bytes = ctx->allocated;
        }
    }
    return next;
}

static void check_deadline(lua_State *L)
{
    run_context_t *ctx = context(L);
    if (ctx->closing) luaL_error(L, "application is closing");
    if (!strcmp(ctx->result->phase, "stopped") ||
        (ctx->options && ctx->options->cancel && atomic_load(ctx->options->cancel))) {
        ctx->result->phase = "stopped";
        luaL_error(L, "stopped by user");
    }
    if (!strcmp(ctx->result->phase, "timeout") ||
        (ctx->deadline_us && esp_timer_get_time() >= ctx->deadline_us)) {
        ctx->result->phase = "timeout";
        luaL_error(L, "execution timed out");
    }
}

static int coroutine_check(lua_State *L) { check_deadline(L); return 0; }

static void instruction_hook(lua_State *L, lua_Debug *debug)
{
    (void)debug;
    check_deadline(L);
    run_context_t *ctx = context(L);
    if (esp_timer_get_time() >= ctx->next_yield_us) {
        vTaskDelay(1);
        ctx->next_yield_us = esp_timer_get_time() + 10000;
    }
}

static void append_output(run_context_t *ctx, const char *text, size_t len)
{
    if (ctx->options && ctx->options->output) {
        ctx->options->output(ctx->options->context, text, len);
    }
    size_t room = RYZ_LUA_OUTPUT_MAX - 1 - ctx->output_len;
    if (len > room) {
        len = room;
        ctx->result->output_truncated = true;
    }
    memcpy(ctx->result->output + ctx->output_len, text, len);
    ctx->output_len += len;
    ctx->result->output[ctx->output_len] = '\0';
}

static int captured_print(lua_State *L)
{
    check_deadline(L);
    run_context_t *ctx = context(L);
    int count = lua_gettop(L);
    for (int i = 1; i <= count; ++i) {
        size_t len;
        const char *s = luaL_tolstring(L, i, &len);
        if (i > 1) append_output(ctx, "\t", 1);
        append_output(ctx, s, len);
        lua_pop(L, 1);
    }
    append_output(ctx, "\n", 1);
    check_deadline(L);
    return 0;
}

static void number_field(lua_State *L, const char *key, lua_Integer value)
{
    lua_pushinteger(L, value);
    lua_setfield(L, -2, key);
}

static int board_info(lua_State *L)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    ESP_ERROR_CHECK(esp_flash_get_size(NULL, &flash_size));
    lua_newtable(L);
    lua_pushliteral(L, "ESP32-S3");
    lua_setfield(L, -2, "chip");
    lua_pushstring(L, esp_get_idf_version());
    lua_setfield(L, -2, "idf");
    number_field(L, "revision", chip.revision);
    number_field(L, "flash_bytes", flash_size);
    number_field(L, "psram_bytes", esp_psram_get_size());
    number_field(L, "free_internal_bytes", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    number_field(L, "free_psram_bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return 1;
}

static int board_millis(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)(esp_timer_get_time() / 1000));
    return 1;
}

static int board_sleep(lua_State *L)
{
    lua_Integer ms = luaL_checkinteger(L, 1);
    luaL_argcheck(L, ms >= 0 && ms <= 5000, 1, "expected 0..5000 milliseconds");
    int64_t until = esp_timer_get_time() + (int64_t)ms * 1000;
    while (esp_timer_get_time() < until) {
        check_deadline(L);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    check_deadline(L);
    return 0;
}

static int open_board(lua_State *L)
{
    static const luaL_Reg functions[] = {
        {"info", board_info}, {"millis", board_millis},
        {"sleep_ms", board_sleep}, {NULL, NULL}
    };
    luaL_newlib(L, functions);
    return 1;
}

static int require_native(lua_State *L)
{
    size_t length;
    const char *name = luaL_checklstring(L, 1, &length);
    if (strlen(name) != length) return luaL_error(L, "invalid module name");
    if (!strcmp(name, "ryzobee")) lua_getfield(L, LUA_REGISTRYINDEX, "ryzobee.native");
    else if (!strcmp(name, "display")) lua_getfield(L, LUA_REGISTRYINDEX, "ryzobee.display");
    else if (!strcmp(name, "touch")) lua_getfield(L, LUA_REGISTRYINDEX, "ryzobee.touch");
    else if (ryz_lua_hardware_require(L, name, length)) return 1;
    else if (ryz_lua_peripherals_require(L, name, length)) return 1;
    else if (ryz_lua_fs_require(L, name, length)) return 1;
    else if (ryz_lua_boot_require(L, name, length)) return 1;
    else return luaL_error(L, "module not allowed: %s", name);
    return 1;
}

static void display_result(lua_State *L, esp_err_t err)
{
    check_deadline(L);
    if (err != ESP_OK) luaL_error(L, "display: %s", esp_err_to_name(err));
}

static void display_draw_result(lua_State *L, esp_err_t err)
{
    /* Demo ownership is changed by the same UI-owner operation as drawing. */
    display_result(L, err);
}

static uint16_t display_color(lua_State *L, int index)
{
    lua_Integer value = luaL_checkinteger(L, index);
    luaL_argcheck(L, value >= 0 && value <= 0xffff, index, "expected RGB565 color 0..65535");
    return (uint16_t)value;
}

static int display_clear(lua_State *L)
{
    check_deadline(L);
    ryz_runtime_io_request_t request = {
        .kind = RYZ_IO_CLEAR, .color = display_color(L, 1),
    };
    runtime_io(context(L), &request);
    display_draw_result(L, request.status);
    return 0;
}

static int display_read_pixel(lua_State *L)
{
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    ryz_runtime_io_request_t request = {.kind = RYZ_IO_READ_PIXEL, .x = x, .y = y};
    runtime_io(context(L), &request);
    display_result(L, request.status);
    lua_pushinteger(L, request.color);
    return 1;
}

static int display_rect(lua_State *L)
{
    check_deadline(L);
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    int width = luaL_checkinteger(L, 3);
    int height = luaL_checkinteger(L, 4);
    ryz_runtime_io_request_t request = {
        .kind = RYZ_IO_RECT, .x = x, .y = y, .width = width, .height = height,
        .color = display_color(L, 5),
    };
    runtime_io(context(L), &request);
    display_draw_result(L, request.status);
    return 0;
}

static bool display_cancelled(void *arg)
{
    run_context_t *ctx = arg;
    return (ctx->options && ctx->options->cancel && atomic_load(ctx->options->cancel)) ||
           (ctx->deadline_us && esp_timer_get_time() >= ctx->deadline_us);
}

static int display_show(lua_State *L)
{
    check_deadline(L);
    ryz_runtime_io_request_t request = {.kind = RYZ_IO_SHOW};
    runtime_io(context(L), &request);
    display_draw_result(L, request.status);
    lua_pushboolean(L, true);
    return 1;
}

static int display_text(lua_State *L)
{
    check_deadline(L);
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    size_t length;
    const char *text = luaL_checklstring(L, 3, &length);
    uint16_t color = display_color(L, 4);
    int scale = luaL_optinteger(L, 5, 1);
    ryz_runtime_io_request_t request = {
        .kind = RYZ_IO_TEXT, .x = x, .y = y, .text = text,
        .text_length = length, .color = color, .scale = scale,
    };
    runtime_io(context(L), &request);
    esp_err_t err = request.status;
    if (err == ESP_ERR_NOT_SUPPORTED) return luaL_error(L, "display.text: unsupported character (uppercase ASCII font)");
    display_draw_result(L, err);
    return 0;
}

static int open_display(lua_State *L)
{
    static const luaL_Reg functions[] = {
        {"clear", display_clear}, {"read_pixel", display_read_pixel},
        {"rect", display_rect}, {"text", display_text},
        {"show", display_show}, {NULL, NULL}
    };
    luaL_newlib(L, functions);
    return 1;
}

static void boolean_field(lua_State *L, const char *key, bool value)
{
    lua_pushboolean(L, value);
    lua_setfield(L, -2, key);
}

static int touch_info(lua_State *L)
{
    check_deadline(L);
    ryz_runtime_io_request_t request = {.kind = RYZ_IO_TOUCH_INFO};
    runtime_io(context(L), &request);
    check_deadline(L);
    if (request.status != ESP_OK) return luaL_error(L, "touch: %s", esp_err_to_name(request.status));
    ryz_touch_info_t info = request.touch_info;
    lua_newtable(L);
    const char *controller = ryz_touch_controller_name(info.chip_id);
    lua_pushstring(L, controller ? controller : "unknown");
    lua_setfield(L, -2, "controller");
    boolean_field(L, "ready", info.ready);
    boolean_field(L, "demo_enabled", request.demo_enabled);
    number_field(L, "chip_id", info.chip_id);
    number_field(L, "project_id", info.project_id);
    number_field(L, "firmware_version", info.firmware_version);
    number_field(L, "factory_id", info.factory_id);
    number_field(L, "disable_auto_sleep", info.disable_auto_sleep);
    number_field(L, "width", RYZ_TOUCH_WIDTH);
    number_field(L, "height", RYZ_TOUCH_HEIGHT);
    number_field(L, "address", 0x15);
    number_field(L, "reads", info.reads);
    number_field(L, "errors", info.errors);
    number_field(L, "presses", info.presses);
    number_field(L, "releases", info.releases);
    return 1;
}

static int touch_read(lua_State *L)
{
    check_deadline(L);
    ryz_runtime_io_request_t request = {.kind = RYZ_IO_TOUCH_READ};
    runtime_io(context(L), &request);
    esp_err_t err = request.status;
    check_deadline(L);
    if (err != ESP_OK) return luaL_error(L, "touch: %s", esp_err_to_name(err));
    ryz_touch_sample_t sample = request.touch;
    lua_newtable(L);
    boolean_field(L, "pressed", sample.pressed);
    boolean_field(L, "interrupted", sample.interrupted);
    if (sample.has_position) {
        number_field(L, "x", sample.x);
        number_field(L, "y", sample.y);
    }
    number_field(L, "fingers", sample.fingers);
    number_field(L, "gesture", sample.gesture);
    number_field(L, "raw_event", sample.raw_event);
    number_field(L, "sampled_ms", sample.sampled_ms);
    lua_pushstring(L, ryz_touch_event_name(sample.event));
    lua_setfield(L, -2, "event");
    return 1;
}

static int touch_demo(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TBOOLEAN);
    check_deadline(L);
    ryz_runtime_io_request_t request = {
        .kind = RYZ_IO_TOUCH_DEMO, .enabled = lua_toboolean(L, 1),
    };
    runtime_io(context(L), &request);
    esp_err_t err = request.status;
    check_deadline(L);
    if (err != ESP_OK) return luaL_error(L, "touch.demo: %s", esp_err_to_name(err));
    lua_pushboolean(L, true);
    return 1;
}

static int open_touch(lua_State *L)
{
    static const luaL_Reg functions[] = {
        {"info", touch_info}, {"read", touch_read}, {"demo", touch_demo}, {NULL, NULL}
    };
    luaL_newlib(L, functions);
    return 1;
}

/* Run library setup and source loading inside pcall as those can also OOM. */
static int protected_run(lua_State *L)
{
    run_context_t *ctx = context(L);
    static const luaL_Reg libraries[] = {
        {LUA_GNAME, luaopen_base}, {LUA_TABLIBNAME, luaopen_table},
        {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8}, {NULL, NULL}
    };
    for (const luaL_Reg *lib = libraries; lib->name; ++lib) {
        luaL_requiref(L, lib->name, lib->func, 1);
        lua_pop(L, 1);
    }
    ryz_lua_sandbox_install(L);
    ryz_lua_coroutines_install(L, coroutine_check);
    ctx->hardware = (ryz_lua_hardware_binding_t){
        ryz_lua_hardware_esp_call, &ctx->hardware_session, check_deadline};
    ryz_lua_hardware_install(L, &ctx->hardware);
    ctx->peripherals=(ryz_lua_peripheral_binding_t){.call=ryz_peripheral_esp_call,
        .context=&ctx->peripheral_session,.check=check_deadline};
    ryz_lua_peripherals_install(L,&ctx->peripherals);
    ctx->filesystem = (ryz_lua_fs_binding_t){.call = ryz_app_fs_call,
        .check = check_deadline};
    ryz_lua_fs_install(L, &ctx->filesystem, ctx->name);
    ctx->boot = (ryz_lua_boot_binding_t){
        .poll = ctx->options ? ctx->options->boot_poll : NULL,
        .context = ctx->options ? ctx->options->context : NULL, .check = check_deadline};
    ryz_lua_boot_install(L, &ctx->boot);
    /* No arbitrary file/network configuration/debug access or catches outside the
     * guarded coroutine boundary that could swallow a timeout. */
    const char *removed[] = {"dofile", "loadfile", "load", "pcall", "xpcall", NULL};
    for (const char **name = removed; *name; ++name) {
        lua_pushnil(L);
        lua_setglobal(L, *name);
    }
    lua_pushcfunction(L, captured_print);
    lua_setglobal(L, "print");
    open_board(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "ryzobee.native");
    open_display(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "ryzobee.display");
    open_touch(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "ryzobee.touch");
    lua_pushcfunction(L, require_native);
    lua_setglobal(L, "require");
    ctx->result->phase = "syntax";
    int status = luaL_loadbufferx(L, ctx->source, ctx->source_len, ctx->name, "t");
    if (status != LUA_OK) return lua_error(L);
    ctx->result->phase = "runtime";
    lua_call(L, 0, 0);
    return 0;
}

void ryz_lua_execute(const char *source, size_t length, const char *name,
                     uint32_t timeout_ms, ryz_lua_result_t *result)
{
    ryz_lua_execute_with_options(source, length, name, timeout_ms, NULL, result);
}

static void *neuro_resize(void *ptr, size_t size)
{
    if (!size) { heap_caps_free(ptr); return NULL; }
    return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static uint64_t neuro_now(void *arg)
{
    run_context_t *ctx = arg;
    uint64_t milliseconds = esp_timer_get_time() / 1000;
    if (!ctx->timing_started) {
        /* app/neuro set their deadline from this first clock read after the
         * native runtime allocation. Use the same millisecond boundary so a
         * queued I/O timeout cannot precede the core's timeout classification.
         * Frozen before the first owner call; later reads do not mutate it. */
        ctx->deadline_us = ctx->timeout_ms
            ? (int64_t)(milliseconds + ctx->timeout_ms) * 1000 : 0;
        ctx->timing_started = true;
    }
    return milliseconds;
}
static void neuro_pause(void *ctx, unsigned ms) { (void)ctx; if (ms) vTaskDelay(pdMS_TO_TICKS(ms)); else taskYIELD(); }
static bool neuro_cancel(void *arg) { run_context_t *ctx = arg; return ctx->options && ctx->options->cancel && atomic_load(ctx->options->cancel); }
static int neuro_sample(void *ctx)
{
    ryz_runtime_io_request_t request = {.kind = RYZ_IO_TOUCH_READ};
    runtime_io(ctx, &request);
    return request.status == ESP_OK ? request.touch.pressed : -1;
}
static int neuro_paint(void *ctx, uint16_t color, bool begin)
{
    ryz_runtime_io_request_t request = {
        .kind = RYZ_IO_NEURO_PAINT, .color = color, .begin = begin,
    };
    runtime_io(ctx, &request);
    return request.status == ESP_OK ? request.done : -1;
}
static void neuro_cleanup(void *ctx)
{
    ryz_runtime_io_request_t request = {.kind = RYZ_IO_ABORT};
    runtime_io(ctx, &request);
}
static void neuro_output(void *arg, const char *text, size_t length)
{
    run_context_t *ctx = arg;
    if (ctx->options && ctx->options->output) ctx->options->output(ctx->options->context, text, length);
}

static int app_pointer_read(void *arg, ryz_app_pointer_t *pointer)
{
    ryz_runtime_io_request_t request = {.kind = RYZ_IO_TOUCH_READ};
    runtime_io(arg, &request);
    if (request.status != ESP_OK) return -1;
    ryz_touch_sample_t sample = request.touch;
    *pointer = (ryz_app_pointer_t){
        .pressed = sample.pressed,
        .interrupted = sample.interrupted,
        .has_position = sample.has_position,
        .x = sample.x,
        .y = sample.y,
        .sampled_ms = sample.sampled_ms,
    };
    switch (sample.event) {
    case RYZ_TOUCH_DOWN: pointer->phase = RYZ_APP_POINTER_DOWN; break;
    case RYZ_TOUCH_MOVE: pointer->phase = RYZ_APP_POINTER_MOVE; break;
    case RYZ_TOUCH_UP: pointer->phase = RYZ_APP_POINTER_UP; break;
    default: pointer->phase = RYZ_APP_POINTER_NONE; break;
    }
    return 0;
}

static int app_display_draw(void *arg, const ryz_app_display_op_t *op)
{
    ryz_runtime_io_request_t request = {
        .x = op->x, .y = op->y, .width = op->width, .height = op->height,
        .scale = op->scale, .color = op->color,
    };
    switch (op->kind) {
    case RYZ_APP_DISPLAY_CLEAR:
        request.kind = RYZ_IO_CLEAR;
        break;
    case RYZ_APP_DISPLAY_RECT:
        request.kind = RYZ_IO_RECT;
        break;
    case RYZ_APP_DISPLAY_TEXT:
        request.kind = RYZ_IO_TEXT;
        request.text = op->text;
        request.text_length = strlen(op->text);
        break;
    default:
        return -1;
    }
    runtime_io(arg, &request);
    return request.status == ESP_OK ? 0 : -1;
}

static int app_display_show(void *arg)
{
    ryz_runtime_io_request_t request = {.kind = RYZ_IO_SHOW};
    runtime_io(arg, &request);
    return request.status == ESP_OK ? 0 : -1;
}

static int app_ui_mount(void *arg, const ryz_ui_scene_t *scene)
{
    ryz_runtime_io_request_t request = {.kind = RYZ_IO_UI_MOUNT, .scene = scene};
    runtime_io(arg, &request);
    return request.status == ESP_OK ? 0 : -1;
}

static int app_ui_pump(void *arg)
{
    ryz_runtime_io_request_t request = {.kind = RYZ_IO_UI_PUMP};
    runtime_io(arg, &request);
    return request.status == ESP_OK ? 0 : -1;
}

static int app_ui_update(void *arg, const ryz_ui_scene_t *scene)
{
    ryz_runtime_io_request_t request = {.kind = RYZ_IO_UI_UPDATE, .scene = scene};
    runtime_io(arg, &request);
    return request.status == ESP_OK ? 0 : -1;
}

static void app_ui_close(void *arg)
{
    ryz_runtime_io_request_t request = {.kind = RYZ_IO_UI_CLOSE};
    runtime_io(arg, &request);
}

static int app_touch_demo(void *arg, bool enabled)
{
    ryz_runtime_io_request_t request = {.kind = RYZ_IO_TOUCH_DEMO, .enabled = enabled};
    runtime_io(arg, &request);
    return request.status == ESP_OK ? 0 : -1;
}

static void app_cleanup(void *arg)
{
    run_context_t *ctx = arg;
    ryz_lua_hardware_esp_cleanup(&ctx->hardware_session);
    ryz_peripheral_esp_cleanup(&ctx->peripheral_session);
    neuro_cleanup(arg);
}

static ryz_lua_hardware_result_t app_hardware_call(void *arg,
    ryz_lua_hardware_op_t op, ryz_lua_hardware_value_t *out)
{
    run_context_t *ctx = arg;
    return ryz_lua_hardware_esp_call(&ctx->hardware_session, op, out);
}

static ryz_peripheral_result_t app_peripheral_call(void *arg,
    const ryz_peripheral_request_t *request,ryz_peripheral_reply_t *reply)
{
    run_context_t *ctx=arg;
    return ryz_peripheral_esp_call(&ctx->peripheral_session,request,reply);
}

static ryz_boot_result_t app_boot_poll(void *arg, ryz_boot_event_t *event)
{
    run_context_t *ctx = arg;
    if (!ctx->options || !ctx->options->boot_poll) return RYZ_BOOT_UNAVAILABLE;
    return ctx->options->boot_poll(ctx->options->context, event);
}

static ryz_tool_call_result_t app_tool_call(void *arg, const ryz_tool_request_t *request,
                                           ryz_tool_reply_t *reply)
{
    run_context_t *context = arg;
    memset(reply, 0, sizeof(*reply));
    /* Only the Workbench/job controller may supply a tool session. Bootstrap
     * and callers without an Adapter remain unavailable, never direct GPIO. */
    if (!context->options || !context->options->tool_call) return RYZ_TOOL_CALL_UNAVAILABLE;
    int result = context->options->tool_call(context->options->context, request, reply);
    if (result < RYZ_TOOL_CALL_OK || result > RYZ_TOOL_CALL_FAILED) {
        memset(reply, 0, sizeof(*reply));
        return RYZ_TOOL_CALL_FAILED;
    }
    return (ryz_tool_call_result_t)result;
}

static void app_mark(void *arg, const char *name, const ryz_app_value_t *value)
{
    char line[80];
    int length;
    if (value->kind == RYZ_APP_VALUE_INTEGER) {
        length = snprintf(line, sizeof(line), "app:mark %s=%ld\n", name, (long)value->integer);
    } else if (value->kind == RYZ_APP_VALUE_BOOLEAN) {
        length = snprintf(line, sizeof(line), "app:mark %s=%s\n", name, value->boolean ? "true" : "false");
    } else {
        length = snprintf(line, sizeof(line), "app:mark %s=%.*s\n", name, (int)value->length, value->string);
    }
    if (length > 0) neuro_output(arg, line, (size_t)length);
}

void ryz_lua_execute_with_options(const char *source, size_t length, const char *name,
                                 uint32_t timeout_ms, const ryz_lua_options_t *options,
                                 ryz_lua_result_t *result)
{
    if (ryz_app_is_source(source, length)) {
        run_context_t ctx = {
            .result = result, .options = options,
            .timeout_ms = timeout_ms,
        };
        ryz_app_platform_t platform = {
            .context = &ctx, .resize = neuro_resize, .now_ms = neuro_now,
            .pause_ms = neuro_pause, .cancelled = neuro_cancel,
            .seed = esp_random(), .pointer_read = app_pointer_read,
            .display_draw = app_display_draw, .display_show = app_display_show,
            .ui_mount = app_ui_mount, .ui_update = app_ui_update, .ui_pump = app_ui_pump,
            .ui_close = app_ui_close,
            .touch_demo = app_touch_demo, .mark = app_mark,
            .cleanup = app_cleanup, .output = neuro_output,
            .tool_call = app_tool_call,
            .hardware_call = app_hardware_call,
            .peripheral_call = app_peripheral_call,
            .fs_call = ryz_app_fs_call,
            .boot_poll = app_boot_poll,
        };
        ryz_app_execute(source, length, name, timeout_ms, &platform, result);
        return;
    }
    if (length >= 13 && !memcmp(source, "-- ryz-neuro/", 13)) {
        run_context_t ctx = {
            .result = result, .options = options,
            .timeout_ms = timeout_ms,
        };
        ryz_neuro_platform_t platform = { .context = &ctx, .resize = neuro_resize, .now_ms = neuro_now, .pause_ms = neuro_pause,
            .cancelled = neuro_cancel, .sample = neuro_sample, .paint = neuro_paint, .cleanup = neuro_cleanup, .output = neuro_output };
        ryz_neuro_execute(source, length, name, timeout_ms, &platform, result); return;
    }
    memset(result, 0, sizeof(*result));
    result->phase = "init";
    int64_t start = esp_timer_get_time();
    run_context_t ctx = {
        .result = result, .source = source, .source_len = length, .name = name,
        .deadline_us = timeout_ms ? start + (int64_t)timeout_ms * 1000 : 0,
        .next_yield_us = start + 10000,
        .options = options,
    };
    /* The ESP-Claw-pinned Lua 5.5 snapshot requires an explicit hash seed. */
    lua_State *L = lua_newstate(bounded_alloc, &ctx, esp_random());
    if (!L) {
        snprintf(result->error, sizeof(result->error), "cannot create Lua VM");
        return;
    }
    *(run_context_t **)lua_getextraspace(L) = &ctx;
    lua_sethook(L, instruction_hook, LUA_MASKCOUNT, 1000);
    lua_pushcfunction(L, protected_run);
    int status = lua_pcall(L, 0, 0, 0);
    ctx.closing = true;
    ryz_lua_peripherals_cleanup(&ctx.peripherals);
    result->ok = status == LUA_OK;
    if (!result->ok) {
        const char *error = lua_tostring(L, -1);
        snprintf(result->error, sizeof(result->error), "%s", error ? error : "non-string Lua error");
        if (status == LUA_ERRMEM) result->phase = "memory";
    } else {
        result->phase = "done";
    }
    app_cleanup(&ctx);
    lua_close(L);
    result->elapsed_ms = (uint32_t)((esp_timer_get_time() - start) / 1000);
}
