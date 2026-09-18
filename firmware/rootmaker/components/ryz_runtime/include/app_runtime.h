#pragma once

#include "lua_runtime.h"
#include "ryz_ui.h"
#include "ryz_tool_call.h"
#include "ryz_lua_hardware.h"
#include "ryz_peripheral.h"
#include "ryz_fs.h"

#define RYZ_APP_ABI "ryz-app/1"
#define RYZ_APP_CATALOG "ryz-capabilities/1"
#define RYZ_APP_WIDTH 240
#define RYZ_APP_HEIGHT 240
#define RYZ_APP_TEXT_MAX 40

#ifndef RYZ_APP_CORE_SHA
#define RYZ_APP_CORE_SHA "unhashed"
#endif

typedef enum {
    RYZ_APP_POINTER_NONE,
    RYZ_APP_POINTER_DOWN,
    RYZ_APP_POINTER_MOVE,
    RYZ_APP_POINTER_UP,
} ryz_app_pointer_phase_t;

typedef struct {
    ryz_app_pointer_phase_t phase;
    bool pressed;
    /* Internal adapter signal: samples were lost during the current gesture.
     * phase stays NONE and no position is supplied. Managed UI cancels its
     * capture; raw v1 keeps the last pressed state until a real sample returns. */
    bool interrupted;
    bool has_position;
    uint16_t x;
    uint16_t y;
    uint32_t sampled_ms;
} ryz_app_pointer_t;

typedef enum {
    RYZ_APP_DISPLAY_CLEAR,
    RYZ_APP_DISPLAY_RECT,
    RYZ_APP_DISPLAY_TEXT,
} ryz_app_display_kind_t;

typedef struct {
    ryz_app_display_kind_t kind;
    int x;
    int y;
    int width;
    int height;
    int scale;
    uint16_t color;
    char text[RYZ_APP_TEXT_MAX + 1];
} ryz_app_display_op_t;

typedef enum {
    RYZ_APP_VALUE_INTEGER,
    RYZ_APP_VALUE_BOOLEAN,
    RYZ_APP_VALUE_STRING,
} ryz_app_value_kind_t;

typedef struct {
    ryz_app_value_kind_t kind;
    int32_t integer;
    bool boolean;
    const char *string;
    size_t length;
} ryz_app_value_t;

/* Environment-dependent seam. app_runtime.c owns the Lua VM, safe libraries,
 * validation, cancellation, memory quota and public Lua modules. Adapters own
 * only time, pointer input, display execution and observable output. */
typedef struct {
    void *context;
    void *(*resize)(void *ptr, size_t size);
    uint64_t (*now_ms)(void *context);
    void (*pause_ms)(void *context, unsigned ms);
    bool (*cancelled)(void *context);
    bool (*finished)(void *context);
    uint32_t seed;
    int (*pointer_read)(void *context, ryz_app_pointer_t *pointer);
    int (*display_draw)(void *context, const ryz_app_display_op_t *op);
    int (*display_show)(void *context);
    int (*ui_mount)(void *context, const ryz_ui_scene_t *scene);
    /* Optional in-place data update. Same scene identity, generation, order
     * and geometry as the last mount; only text/colors/visibility/enabled vary.
     * On failure execution ends and ui_close follows; no rollback is assumed. */
    int (*ui_update)(void *context, const ryz_ui_scene_t *scene);
    int (*ui_pump)(void *context);
    void (*ui_close)(void *context);
    int (*touch_demo)(void *context, bool enabled);
    void (*mark)(void *context, const char *name, const ryz_app_value_t *value);
    void (*cleanup)(void *context);
    void (*output)(void *context, const char *text, size_t length);
    ryz_tool_call_fn tool_call; /* Optional; unavailable without an Adapter. */
    ryz_lua_hardware_fn hardware_call; /* Optional; never simulate connectivity. */
    ryz_peripheral_call_fn peripheral_call; /* Optional hardware environment. */
    ryz_fs_call_fn fs_call; /* Optional app-data backend; absent means unavailable. */
} ryz_app_platform_t;

bool ryz_app_is_source(const char *source, size_t length);
const char *ryz_app_pointer_phase_name(ryz_app_pointer_phase_t phase);
void ryz_app_execute(const char *source, size_t length, const char *name,
                     uint32_t timeout_ms, const ryz_app_platform_t *platform,
                     ryz_lua_result_t *result);
