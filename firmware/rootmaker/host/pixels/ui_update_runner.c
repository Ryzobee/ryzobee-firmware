/* Bounded diagnostic: the checked-in Lua app runs through the production
 * facade and actual LVGL/FreeType. Only clock, touch and panel are replaced. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_runtime.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "ryz_display_host.h"
#include "ryz_font.h"
#include "ryz_lvgl.h"

#ifdef NDEBUG
#error "Lua pixel diagnostic requires assertions"
#endif

typedef struct {
    const char *directory;
    uint32_t generation;
    unsigned mounts, updates, closes, deletions, pointer_events;
    int32_t marked_updates, marked_generation;
    bool exited, tool_attempted;
    lv_obj_t *root;
    lv_obj_t *objects[RYZ_UI_OBJECT_MAX];
    uint8_t object_count;
    clock_t started;
} probe_t;

static void *resize(void *pointer, size_t bytes)
{
    if (!bytes) { free(pointer); return NULL; }
    return realloc(pointer, bytes);
}

static uint64_t now_ms(void *context)
{ (void)context; return (uint64_t)esp_timer_get_time() / 1000; }

static void pause_ms(void *context, unsigned milliseconds)
{ (void)context; ryz_host_display_advance_ms(milliseconds ? milliseconds : 1); }

static bool cancelled(void *context)
{
    probe_t *probe = context;
    return (double)(clock() - probe->started) / CLOCKS_PER_SEC > 10;
}

static int pointer_read(void *context, ryz_app_pointer_t *out)
{
    probe_t *probe = context;
    uint64_t now = now_ms(context);
    *out = (ryz_app_pointer_t){.sampled_ms = (uint32_t)now};
    if (probe->pointer_events == 0 && now >= 100) {
        *out = (ryz_app_pointer_t){.phase = RYZ_APP_POINTER_DOWN,
            .pressed = true, .has_position = true, .x = 120, .y = 200, .sampled_ms = 100};
        ++probe->pointer_events;
    } else if (probe->pointer_events == 1 && now >= 650) {
        *out = (ryz_app_pointer_t){.phase = RYZ_APP_POINTER_UP, .sampled_ms = 650};
        ++probe->pointer_events;
    } else if (probe->pointer_events == 1) {
        out->pressed = out->has_position = true;
        out->x = 120; out->y = 200;
    }
    return 0;
}

static void deleted(lv_event_t *event)
{
    probe_t *probe = lv_event_get_user_data(event);
    ++probe->deletions;
}

static int mount(void *context, const ryz_ui_scene_t *scene)
{
    probe_t *probe = context;
    assert(!probe->mounts && !strcmp(scene->id, "live_update") && scene->generation);
    esp_err_t error = ryz_lvgl_mount(scene);
    if (error != ESP_OK) return error;
    ++probe->mounts;
    probe->generation = scene->generation;
    probe->root = lv_screen_active();
    probe->object_count = scene->object_count;
    assert(lv_obj_get_child_count(probe->root) == scene->object_count);
    for (unsigned i = 0; i < scene->object_count; ++i)
        probe->objects[i] = lv_obj_get_child(probe->root, i);
    assert(lv_obj_add_event_cb(probe->root, deleted, LV_EVENT_DELETE, probe));
    ryz_host_display_save(probe->directory, "lua-script-initial-240");
    return 0;
}

static int update(void *context, const ryz_ui_scene_t *scene)
{
    probe_t *probe = context;
    assert(probe->mounts == 1 && !probe->closes && !probe->deletions);
    assert(scene->generation == probe->generation && !strcmp(scene->id, "live_update"));
    assert(probe->object_count == scene->object_count && lv_screen_active() == probe->root);
    esp_err_t error = ryz_lvgl_update(scene);
    if (error != ESP_OK) return error;
    ++probe->updates;
    assert(probe->updates <= 3 && lv_screen_active() == probe->root);
    for (unsigned i = 0; i < scene->object_count; ++i)
        assert(lv_obj_get_child(probe->root, i) == probe->objects[i]);
    static const char *expected[] = {"UPDATES 1", "UPDATES 2", "UPDATES 3"};
    assert(!strcmp(lv_label_get_text(probe->objects[3]), expected[probe->updates - 1]));
    assert(probe->marked_updates == (int32_t)probe->updates);
    char name[40];
    snprintf(name, sizeof(name), "lua-script-update-%u-240", probe->updates);
    ryz_host_display_save(probe->directory, name);
    return 0;
}

static int pump(void *context) { (void)context; return ryz_lvgl_pump(); }

static void close_ui(void *context)
{
    probe_t *probe = context;
    assert(probe->closes == 0 && probe->mounts == 1);
    ++probe->closes;
    ryz_lvgl_release();
    assert(probe->deletions == 1);
}

static void mark(void *context, const char *name, const ryz_app_value_t *value)
{
    probe_t *probe = context;
    if (!strcmp(name, "updates")) {
        assert(value->kind == RYZ_APP_VALUE_INTEGER);
        probe->marked_updates = value->integer;
    } else if (!strcmp(name, "scene_generation")) {
        assert(value->kind == RYZ_APP_VALUE_INTEGER);
        probe->marked_generation = value->integer;
    } else if (!strcmp(name, "exited")) {
        assert(value->kind == RYZ_APP_VALUE_BOOLEAN);
        probe->exited = value->boolean;
    } else assert(!"unexpected diagnostic mark");
}

static ryz_tool_call_result_t unavailable(void *context,
    const ryz_tool_request_t *request, ryz_tool_reply_t *out)
{
    (void)request;
    probe_t *probe = context;
    probe->tool_attempted = true;
    memset(out, 0, sizeof(*out));
    return RYZ_TOOL_CALL_UNAVAILABLE;
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    FILE *file = fopen(argv[1], "rb");
    assert(file);
    char source[RYZ_LUA_SOURCE_MAX + 1];
    size_t bytes = fread(source, 1, sizeof(source), file);
    assert(!ferror(file) && fclose(file) == 0 && bytes && bytes <= RYZ_LUA_SOURCE_MAX);
    assert(ryz_font_init() == ESP_OK);
    ryz_host_display_reset();
    probe_t probe = {.directory = argv[2], .started = clock()};
    const ryz_app_platform_t platform = {
        .context = &probe, .resize = resize, .now_ms = now_ms, .pause_ms = pause_ms,
        .cancelled = cancelled, .pointer_read = pointer_read,
        .ui_mount = mount, .ui_update = update, .ui_pump = pump, .ui_close = close_ui,
        .mark = mark, .tool_call = unavailable, .seed = 1,
    };
    ryz_lua_result_t result;
    ryz_app_execute(source, bytes, "@ui_update_demo.lua", 2000, &platform, &result);
    if (!result.ok) fprintf(stderr, "Lua failed phase=%s error=%s\n", result.phase, result.error);
    assert(result.ok && !strcmp(result.phase, "done") && !probe.tool_attempted);
    assert(probe.mounts == 1 && probe.updates == 3 && probe.closes == 1 && probe.deletions == 1);
    assert(probe.exited && probe.marked_updates == 3 && probe.pointer_events == 2);
    assert(probe.marked_generation == (int32_t)probe.generation);
    assert(result.elapsed_ms >= 650 && result.elapsed_ms < 700);
    assert(ryz_lvgl_pump() == ESP_ERR_INVALID_STATE);
    printf("LUA_UPDATE_PIXELS_PASS mounts=%u updates=%u generation=%u exited=%d elapsed=%u closes=%u\n",
           probe.mounts, probe.updates, probe.generation, probe.exited, result.elapsed_ms, probe.closes);
    return 0;
}
