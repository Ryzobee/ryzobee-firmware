/* The real Lua application/public UART+log APIs/LVGL. Only byte sources are
 * synthetic. There is no C-side Monitor state machine, history or pause. */
#include "monitor_demo.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_timer.h"
#include "ryz_font.h"
#include "ryz_lvgl.h"
#include "../pixels/monitor_stream_fixture.h"
#include "monitor_source.h"

#define INPUT_CAPACITY 16U
static struct {
    bool active, cancelled, pointer_pressed;
    void (*idle)(unsigned);
    void (*present)(void);
    ryz_ui_scene_t scene;
    ryz_app_pointer_t input[INPUT_CAPACITY];
    unsigned head, count, produced;
    uint64_t next_input;
    unsigned populated;
    monitor_stream_fixture_t stream;
} demo;

static uint64_t now_ms(void *context)
{ (void)context; return (uint64_t)esp_timer_get_time() / 1000U; }
static void checked(esp_err_t error) { assert(error == ESP_OK); (void)error; }
size_t ryz_sim_monitor_source_bytes(void) { return sizeof(monitor_source); }
const char *ryz_sim_monitor_version(void) { return monitor_version; }
bool ryz_sim_monitor_active(void) { return demo.active; }
const ryz_ui_scene_t *ryz_sim_monitor_scene(void) { return &demo.scene; }
void ryz_sim_monitor_cancel(void) { if (demo.active) demo.cancelled = true; }

void ryz_sim_monitor_pointer(const ryz_app_pointer_t *sample)
{
    if (!demo.active || (sample->phase == RYZ_APP_POINTER_NONE && !sample->interrupted)) return;
    if (sample->interrupted || demo.count == INPUT_CAPACITY) {
        /* Lost input invalidates the entire capture; never synthesize a click. */
        demo.head = 0; demo.count = 1; demo.pointer_pressed = false;
        demo.input[0] = (ryz_app_pointer_t){.interrupted = true,
            .sampled_ms = (uint32_t)now_ms(NULL)};
        return;
    }
    if (sample->phase == RYZ_APP_POINTER_MOVE && demo.count) {
        unsigned last = (demo.head + demo.count - 1U) % INPUT_CAPACITY;
        if (demo.input[last].phase == RYZ_APP_POINTER_MOVE) {
            demo.input[last] = *sample; return;
        }
    }
    demo.input[(demo.head + demo.count) % INPUT_CAPACITY] = *sample;
    ++demo.count;
}

static int pointer_read(void *context, ryz_app_pointer_t *out)
{
    (void)context;
    *out = (ryz_app_pointer_t){.pressed = demo.pointer_pressed,
        .sampled_ms = (uint32_t)now_ms(NULL)};
    if (demo.count) {
        *out = demo.input[demo.head];
        demo.head = (demo.head + 1U) % INPUT_CAPACITY; --demo.count;
        demo.pointer_pressed = out->pressed;
    }
    return ESP_OK;
}

static void append(const char *text)
{ monitor_fixture_feed(&demo.stream,text,strlen(text)); }
static void tick(void)
{
    uint64_t now=now_ms(NULL);
    if(!demo.stream.handle) return;
    if(demo.populated!=demo.stream.opened) {
        demo.populated=demo.stream.opened; demo.next_input=now+1000U;
        append("SIMULATED INPUT ONLY\nNO DEVICE CONNECTED\nPAUSE / CLEAR / CONFIG\n");
        for(unsigned i=1;i<=8;++i) {
            char sample[32]; snprintf(sample,sizeof(sample),"SIM SAMPLE / %02u\n",i); append(sample);
        }
    }
    if(now>=demo.next_input) {
        char line[48]; snprintf(line,sizeof(line),"SIM %s / %06u\n",
            demo.stream.kind==RYZ_PERIPHERAL_UART ? "UART1" : "SYSTEM",++demo.produced);
        append(line); demo.next_input=now+1000U;
    }
}

static void pause_ms(void *context, unsigned ms)
{ (void)context; demo.idle(ms); tick(); }
static bool cancelled(void *context) { (void)context; return demo.cancelled; }
static void *resize(void *ptr, size_t bytes)
{ if (!bytes) { free(ptr); return NULL; } return realloc(ptr, bytes); }
static int mount(void *context, const ryz_ui_scene_t *scene)
{
    (void)context; int error = ryz_lvgl_mount(scene);
    if (!error) {
        demo.scene = *scene;
        /* Input queued against the previous page cannot activate this one. */
        demo.head = demo.count = 0; demo.pointer_pressed = false;
        demo.present();
    }
    return error;
}
static int update(void *context, const ryz_ui_scene_t *scene)
{
    (void)context; int error = ryz_lvgl_update(scene);
    if (!error) { demo.scene = *scene; demo.present(); }
    return error;
}
static int pump(void *context)
{ (void)context; int error = ryz_lvgl_pump(); demo.present(); return error; }
static void close_ui(void *context)
{ (void)context; ryz_lvgl_release(); demo.scene = (ryz_ui_scene_t){0}; }
static void cleanup(void *context)
{
    (void)context;
    /* app_runtime closes generic handles after revoking the Lua job. */
    assert(!demo.stream.handle);
}
static ryz_peripheral_result_t peripheral_call(void *context,
    const ryz_peripheral_request_t *q,ryz_peripheral_reply_t *out)
{ (void)context; return monitor_fixture_call(&demo.stream,q,out); }

void ryz_sim_monitor_run(void (*idle)(unsigned), void (*present)(void), ryz_lua_result_t *result)
{
    assert(!demo.active && idle && present && result);
    memset(&demo, 0, sizeof(demo));
    demo.active = true; demo.idle = idle; demo.present = present;
    monitor_fixture_init(&demo.stream);
    checked(ryz_font_init());
    const ryz_app_platform_t platform = {
        .resize = resize, .now_ms = now_ms, .pause_ms = pause_ms, .cancelled = cancelled,
        .pointer_read = pointer_read, .ui_mount = mount, .ui_update = update,
        .ui_pump = pump, .ui_close = close_ui, .cleanup = cleanup, .peripheral_call = peripheral_call, .tool_call = NULL, .seed = 1,
    };
    ryz_app_execute(monitor_source, sizeof(monitor_source), "@tool_monitor.lua", 0, &platform, result);
    /* Even init/allocation failure must leave no live source or UI lease. */
    cleanup(NULL); close_ui(NULL); demo.active = false;
    printf("MONITOR DEMO: phase=%s source_bytes=%zu peak=%zu; simulated input, no device access\n",
           result->phase, sizeof(monitor_source), result->peak_bytes);
    if (!result->ok && strcmp(result->phase, "stopped")) fprintf(stderr, "Lua error: %s\n", result->error);
}
