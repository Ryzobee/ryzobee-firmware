#include "lua_runtime.h"
#include "ryz_tool_call.h"
#include "ryz_runtime_io.h"
#include "workbench_io.h"
#include "display.h"
#include "touch_demo.h"
#include "ryz_lvgl.h"
#include "runtime_owner_test_platform.h"
#include "../components/ryz_runtime/lua_hardware_esp.h"
#include "ryz_peripheral.h"
#include "ryz_fs.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        abort(); \
    } \
} while (0)

enum {
    MODE_NORMAL, MODE_SPIN_STOP, MODE_UI_STOP, MODE_NEURO_STOP,
    MODE_SHOW_TIMEOUT, MODE_NEURO_PENDING_STOP, MODE_NEURO_PENDING_TIMEOUT,
    MODE_NEURO_SAMPLE_STOP, MODE_NEURO_STRIPE_STOP, MODE_APP_DEADLINE_SKEW,
    MODE_TOUCH_FAILURE, MODE_TOUCH_INTERRUPTED, MODE_SHOW_FAILURE, MODE_PAINT_FAILURE,
    MODE_UPDATE_PENDING_CANCEL, MODE_UPDATE_POST_CANCEL, MODE_UPDATE_TIMEOUT,
    MODE_UPDATE_FAILURE,
    MODE_TOOLS, MODE_TOOLS_BAD_RESULT, MODE_TOOLS_POST_CANCEL,
};

typedef struct {
    pthread_t owner;
    ryz_workbench_io_t channel;
    atomic_bool cancel;
    atomic_bool worker_done;
    atomic_bool compute_started;
    _Atomic(const ryz_ui_scene_t *) mounted;
    atomic_bool cleanup_seen;
    const char *source;
    const char *source_name;
    unsigned timeout_ms;
    unsigned mode;
    ryz_lua_result_t result;
    unsigned owner_ticks;
    unsigned idle_worker_ticks;
    unsigned compute_ticks;
    unsigned calls[RYZ_IO_UI_UPDATE + 1];
    unsigned clear_calls, rect_calls, text_calls, read_pixel_calls, show_calls;
    unsigned step_calls, abort_calls, touch_reads, touch_infos, demo_calls;
    unsigned mounts, updates, pumps, closes;
    unsigned output_calls;
    size_t output_bytes;
    bool demo_enabled;
    bool painting;
    unsigned paint_stripes;
    uint16_t color;
    char cleanup_order[8];
    unsigned cleanup_steps;
    size_t live_bytes;
    unsigned frees_after_cleanup;
    bool queued_injection_done;
    bool allocation_delay_done;
    unsigned tool_calls;
    uint32_t accepted_tool;
} fixture_t;

static fixture_t fixture;

/* The device Adapter has a separate production-boundary test. This owner
 * fixture has no networking or IMU model. */
ryz_lua_hardware_result_t ryz_lua_hardware_esp_call(void *session,
    ryz_lua_hardware_op_t op, ryz_lua_hardware_value_t *out)
{
    (void)session; (void)op;
    memset(out, 0, sizeof(*out));
    return RYZ_LUA_HW_UNAVAILABLE;
}
void ryz_lua_hardware_esp_cleanup(ryz_lua_hardware_session_t *session)
{ CHECK(!session->imu_owned); }

/* Explicit single virtual GPIO peer at the public runtime boundary. This
 * host has no ESP drivers; it is not evidence of interrupt/core behaviour. */
static bool virtual_pin_enabled, virtual_pin_cancel;
static uint32_t virtual_pin_handle, virtual_pin_sequence;
static unsigned virtual_pin_level;
ryz_peripheral_result_t ryz_peripheral_esp_call(void *session,
    const ryz_peripheral_request_t *request, ryz_peripheral_reply_t *reply)
{
    (void)session; memset(reply,0,sizeof(*reply));
    if(!virtual_pin_enabled || request->kind!=RYZ_PERIPHERAL_GPIO) return RYZ_PERIPHERAL_UNAVAILABLE;
    if(request->op==RYZ_PERIPHERAL_OPEN) {
        if(virtual_pin_handle) return RYZ_PERIPHERAL_BUSY;
        virtual_pin_handle=++virtual_pin_sequence;
        virtual_pin_level=request->config.gpio.initial; reply->handle=virtual_pin_handle;
    } else {
        if(!virtual_pin_handle || request->handle!=virtual_pin_handle) return RYZ_PERIPHERAL_CLOSED;
        if(request->op==RYZ_PERIPHERAL_CLOSE) virtual_pin_handle=0;
        else if(request->op==RYZ_PERIPHERAL_READ) reply->value=virtual_pin_level;
        else if(request->op==RYZ_PERIPHERAL_WRITE) {
            virtual_pin_level=request->value;
            if(virtual_pin_cancel) atomic_store(&fixture.cancel,true);
        } else return RYZ_PERIPHERAL_UNSUPPORTED;
    }
    return RYZ_PERIPHERAL_OK;
}
void ryz_peripheral_esp_cleanup(ryz_peripheral_session_t *session) { (void)session; }

static void owner_only(void)
{
    CHECK(pthread_equal(pthread_self(), fixture.owner));
}

static void worker_only(void)
{
    CHECK(!pthread_equal(pthread_self(), fixture.owner));
}

/* A deliberately tiny in-memory backend at the real runtime/ESP seam. This
 * proves public registration, worker dispatch and cancellation, not on-disk
 * persistence, flash durability or the production filesystem adapter. */
static struct {
    bool enabled, cancel_after_write;
    unsigned calls;
    struct {
        bool present;
        char name[RYZ_FS_NAME_MAX + 1U];
        uint8_t data[RYZ_FS_MAX_FILE_BYTES];
        size_t size;
    } files[2];
} virtual_fs;

/* Explicit copied-event peer, not the real GPIO/debounce/queue producer. The
 * tests below prove both Lua dialects forward on the worker with the caller's
 * exact context and consume once across coroutines. */
static struct {
    bool enabled, cancel_before, cancel_after, timeout_after;
    unsigned calls, cursor, count;
    struct { ryz_boot_result_t result; ryz_boot_event_t event; } replies[32];
} virtual_boot;

static ryz_boot_result_t boot_poll(void *context, ryz_boot_event_t *event)
{
    CHECK(context == &fixture);
    worker_only();
    CHECK(event);
    ++virtual_boot.calls;
    if (virtual_boot.cancel_after) atomic_store(&fixture.cancel, true);
    if (virtual_boot.timeout_after) vTaskDelay(fixture.timeout_ms + 20U);
    if (virtual_boot.cursor == virtual_boot.count) {
        memset(event, 0xff, sizeof(*event)); /* EMPTY has no usable payload. */
        return RYZ_BOOT_EMPTY;
    }
    *event = virtual_boot.replies[virtual_boot.cursor].event;
    return virtual_boot.replies[virtual_boot.cursor++].result;
}

ryz_fs_result_t ryz_app_fs_call(void *context, const char *app_id,
    const ryz_fs_request_t *request, ryz_fs_response_t *response)
{
    (void)context;
    worker_only();
    CHECK(app_id && !strcmp(app_id, "persist.lua"));
    CHECK(request && response);
    ++virtual_fs.calls;
    *response = (ryz_fs_response_t){0};
    if (!virtual_fs.enabled) return RYZ_FS_UNAVAILABLE;
    size_t used = 0, count = 0, found = 2, available = 2;
    for (size_t i = 0; i < 2; ++i) {
        if (!virtual_fs.files[i].present) { available = i; continue; }
        ++count;
        used += virtual_fs.files[i].size;
        if (request->name && !strcmp(request->name, virtual_fs.files[i].name)) found = i;
    }
    if (request->op == RYZ_FS_INFO) {
        response->info = (ryz_fs_info_t){.used_bytes = used, .file_count = count,
            .max_file_bytes = RYZ_FS_MAX_FILE_BYTES, .quota_bytes = RYZ_FS_QUOTA_BYTES,
            .max_files = RYZ_FS_MAX_FILES};
        return RYZ_FS_OK;
    }
    if (request->op == RYZ_FS_LIST) {
        CHECK(request->entries && request->entries_capacity >= count);
        for (size_t i = 0; i < 2; ++i) if (virtual_fs.files[i].present) {
            ryz_fs_entry_t *entry = &request->entries[response->count++];
            strcpy(entry->name, virtual_fs.files[i].name);
            entry->size = virtual_fs.files[i].size;
        }
        if (count == 2 && strcmp(request->entries[0].name, request->entries[1].name) > 0) {
            ryz_fs_entry_t tmp = request->entries[0];
            request->entries[0] = request->entries[1];
            request->entries[1] = tmp;
        }
        return RYZ_FS_OK;
    }
    CHECK(request->name && strlen(request->name) <= RYZ_FS_NAME_MAX);
    if (request->op == RYZ_FS_WRITE) {
        CHECK(request->data && request->size <= RYZ_FS_MAX_FILE_BYTES);
        if (found == 2) found = available;
        if (found == 2) return RYZ_FS_TOO_MANY_FILES;
        virtual_fs.files[found].present = true;
        strcpy(virtual_fs.files[found].name, request->name);
        memcpy(virtual_fs.files[found].data, request->data, request->size);
        virtual_fs.files[found].size = request->size;
        if (virtual_fs.cancel_after_write) atomic_store(&fixture.cancel, true);
        return RYZ_FS_OK;
    }
    if (found == 2) return RYZ_FS_NOT_FOUND;
    if (request->op == RYZ_FS_READ) {
        CHECK(request->read_buffer && request->read_capacity >= virtual_fs.files[found].size);
        response->size = virtual_fs.files[found].size;
        memcpy(request->read_buffer, virtual_fs.files[found].data, response->size);
        return RYZ_FS_OK;
    }
    CHECK(request->op == RYZ_FS_REMOVE);
    virtual_fs.files[found].present = false;
    return RYZ_FS_OK;
}

int64_t esp_timer_get_time(void)
{
    struct timespec now;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

void vTaskDelay(TickType_t ticks)
{
    struct timespec remaining = {
        .tv_sec = ticks / 1000,
        .tv_nsec = (long)(ticks % 1000) * 1000000,
    };
    while (nanosleep(&remaining, &remaining) && errno == EINTR) {}
}

void runtime_owner_test_yield(void) { sched_yield(); }
const char *esp_get_idf_version(void) { return "host-owned-runtime"; }
uint32_t esp_random(void) { return 42; }
size_t esp_psram_get_size(void) { return 8 * 1024 * 1024; }
size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return 1024 * 1024; }
void esp_chip_info(esp_chip_info_t *info) { info->revision = 1; }
esp_err_t esp_flash_get_size(void *chip, uint32_t *size)
{
    (void)chip;
    *size = 8 * 1024 * 1024;
    return ESP_OK;
}
const char *esp_err_to_name(esp_err_t error)
{
    switch (error) {
    case ESP_OK: return "ESP_OK";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    default: return "ESP_FAIL";
    }
}

typedef struct { max_align_t alignment; size_t size; } allocation_t;

void *heap_caps_realloc(void *ptr, size_t size, unsigned caps)
{
    (void)caps;
    worker_only();
    if (fixture.mode == MODE_APP_DEADLINE_SKEW && !fixture.allocation_delay_done) {
        fixture.allocation_delay_done = true;
        /* The native ctx already has its deadline; app_t's own deadline is
         * not initialized until its first allocation returns. */
        vTaskDelay(50);
    }
    allocation_t *old = ptr ? (allocation_t *)ptr - 1 : NULL;
    const size_t previous = old ? old->size : 0;
    const uintptr_t scene = (uintptr_t)atomic_load(&fixture.mounted);
    if (ptr && scene) {
        CHECK(scene < (uintptr_t)ptr || scene >= (uintptr_t)ptr + previous);
    }
    if (!size) {
        fixture.live_bytes -= previous;
        if (ptr && atomic_load(&fixture.cleanup_seen)) ++fixture.frees_after_cleanup;
        free(old);
        return NULL;
    }
    allocation_t *next = realloc(old, sizeof(*next) + size);
    if (!next) return NULL;
    next->size = size;
    fixture.live_bytes = fixture.live_bytes - previous + size;
    return next + 1;
}

void heap_caps_free(void *ptr) { (void)heap_caps_realloc(ptr, 0, 0); }

esp_err_t ryz_display_clear(uint16_t color)
{
    owner_only();
    CHECK(!fixture.painting);
    fixture.color = color;
    ++fixture.clear_calls;
    return ESP_OK;
}

esp_err_t ryz_display_rect(int x, int y, int width, int height, uint16_t color)
{
    owner_only();
    CHECK(!fixture.painting && x == 1 && y == 2 && width == 3 && height == 4);
    CHECK(color == 0xf800);
    ++fixture.rect_calls;
    return ESP_OK;
}

esp_err_t ryz_display_text(int x, int y, const char *text, size_t length,
                           uint16_t color, int scale)
{
    owner_only();
    CHECK(!fixture.painting && x == 5 && y == 6 && color == 0xffff && scale == 2);
    /* Borrowed Lua/app string must remain readable until the acknowledgement. */
    vTaskDelay(1);
    CHECK(length == 4 && !memcmp(text, "RYZO", length));
    ++fixture.text_calls;
    return ESP_OK;
}

esp_err_t ryz_display_read_pixel(int x, int y, uint16_t *color)
{
    owner_only();
    CHECK(x == 0 && y == 0 && color);
    *color = fixture.color;
    ++fixture.read_pixel_calls;
    return ESP_OK;
}

esp_err_t ryz_display_show_checked(bool (*cancelled)(void *), void *context)
{
    owner_only();
    ++fixture.show_calls;
    if (fixture.mode == MODE_SHOW_FAILURE) return ESP_ERR_TIMEOUT;
    if (fixture.mode == MODE_SHOW_TIMEOUT || fixture.mode == MODE_APP_DEADLINE_SKEW) {
        const int64_t deadline = esp_timer_get_time() + 2000000;
        while (!cancelled(context)) {
            CHECK(esp_timer_get_time() < deadline);
            vTaskDelay(1);
        }
        return ESP_ERR_TIMEOUT;
    }
    CHECK(!cancelled || !cancelled(context));
    return ESP_OK;
}

esp_err_t ryz_display_show_step(bool begin, bool *done)
{
    owner_only();
    ++fixture.step_calls;
    if (fixture.mode == MODE_PAINT_FAILURE) return ESP_FAIL;
    if (begin) {
        CHECK(!fixture.painting);
        fixture.painting = true;
        fixture.paint_stripes = 0;
    } else CHECK(fixture.painting);
    *done = ++fixture.paint_stripes == 3;
    if (*done) fixture.painting = false;
    return ESP_OK;
}

void ryz_display_show_abort(void)
{
    owner_only();
    fixture.painting = false;
    ++fixture.abort_calls;
    CHECK(!atomic_load(&fixture.mounted));
    fixture.cleanup_order[fixture.cleanup_steps++] = 'A';
    atomic_store(&fixture.cleanup_seen, true);
}

esp_err_t ryz_touch_read(ryz_touch_sample_t *out)
{
    owner_only();
    if (fixture.mode == MODE_TOUCH_FAILURE) {
        ++fixture.touch_reads;
        return ESP_ERR_TIMEOUT;
    }
    if (fixture.mode == MODE_TOUCH_INTERRUPTED) {
        *out=(ryz_touch_sample_t){.event=RYZ_TOUCH_NONE,.interrupted=true,
            .sampled_ms=++fixture.touch_reads};
        return ESP_OK;
    }
    const bool pressed = fixture.touch_reads++ % 2 == 0;
    *out = (ryz_touch_sample_t){
        .pressed = pressed, .has_position = pressed,
        .x = pressed ? 30 : 0, .y = pressed ? 80 : 0,
        .fingers = pressed, .event = pressed ? RYZ_TOUCH_DOWN : RYZ_TOUCH_UP,
        .sampled_ms = fixture.touch_reads,
    };
    return ESP_OK;
}

void ryz_touch_get_info(ryz_touch_info_t *out)
{
    owner_only();
    ++fixture.touch_infos;
    *out = (ryz_touch_info_t){.ready = true, .chip_id = 0xb5, .reads = fixture.touch_reads};
}

esp_err_t ryz_touch_demo_enable(bool enabled)
{
    owner_only();
    ++fixture.demo_calls;
    fixture.demo_enabled = enabled;
    return ESP_OK;
}

bool ryz_touch_demo_enabled(void)
{
    owner_only();
    return fixture.demo_enabled;
}

esp_err_t ryz_lvgl_mount_checked(const ryz_ui_scene_t *scene,
                                 bool (*cancelled)(void *), void *context)
{
    owner_only();
    CHECK(!cancelled || !cancelled(context));
    CHECK(scene && !strcmp(scene->id, "home") && scene->object_count == 1);
    CHECK(!strcmp(scene->objects[0].text, "APPS"));
    CHECK(scene->generation == 1);
    vTaskDelay(1);
    atomic_store(&fixture.mounted, scene);
    ++fixture.mounts;
    return ESP_OK;
}

esp_err_t ryz_lvgl_pump_checked(bool (*cancelled)(void *), void *context)
{
    owner_only();
    CHECK(!cancelled || !cancelled(context));
    const ryz_ui_scene_t *scene = atomic_load(&fixture.mounted);
    CHECK(scene && !strcmp(scene->objects[0].id, "apps"));
    ++fixture.pumps;
    return ESP_OK;
}

esp_err_t ryz_lvgl_update_checked(const ryz_ui_scene_t *scene,
                                  bool (*cancelled)(void *), void *context)
{
    owner_only();
    CHECK(scene && atomic_load(&fixture.mounted));
    CHECK(!strcmp(scene->id,"home") && scene->generation==1 && scene->object_count==1);
    CHECK(!strcmp(scene->objects[0].id,"apps") && !strcmp(scene->objects[0].text,"NEXT"));
    CHECK(scene->objects[0].x==20 && scene->objects[0].width==200);
    CHECK(cancelled && !cancelled(context));
    ++fixture.updates;
    atomic_store(&fixture.mounted,scene);
    if (fixture.mode==MODE_UPDATE_FAILURE) return ESP_ERR_TIMEOUT;
    if (fixture.mode==MODE_UPDATE_POST_CANCEL) {
        atomic_store(&fixture.cancel,true);
        return ESP_OK; /* Check must still run after an accepted owner call. */
    }
    if (fixture.mode==MODE_UPDATE_TIMEOUT) {
        const int64_t deadline=esp_timer_get_time()+2000000;
        while (!cancelled(context)) {
            CHECK(esp_timer_get_time()<deadline);
            vTaskDelay(1);
        }
        /* Borrowed candidate is still live even after the caller's deadline. */
        CHECK(!strcmp(scene->objects[0].text,"NEXT"));
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void ryz_lvgl_release(void)
{
    owner_only();
    const ryz_ui_scene_t *scene = atomic_load(&fixture.mounted);
    CHECK(scene && !strcmp(scene->id, "home"));
    atomic_store(&fixture.mounted, NULL);
    ++fixture.closes;
    fixture.cleanup_order[fixture.cleanup_steps++] = 'C';
}

static esp_err_t owner_input(void *context, ryz_touch_sample_t *sample)
{
    CHECK(context == &fixture);
    owner_only();
    return ryz_touch_read(sample);
}

static void execute_request(void *opaque)
{
    ryz_runtime_io_request_t *request = opaque;
    owner_only();
    CHECK(request->kind <= RYZ_IO_UI_UPDATE);
    ++fixture.calls[request->kind];
    const bool paint_injection = request->kind == RYZ_IO_NEURO_PAINT &&
        ((request->begin && (fixture.mode == MODE_NEURO_PENDING_STOP ||
                             fixture.mode == MODE_NEURO_PENDING_TIMEOUT)) ||
         (!request->begin && fixture.mode == MODE_NEURO_STRIPE_STOP));
    const bool sample_injection = request->kind == RYZ_IO_TOUCH_READ &&
        fixture.mode == MODE_NEURO_SAMPLE_STOP;
    const bool update_injection = request->kind == RYZ_IO_UI_UPDATE &&
        fixture.mode == MODE_UPDATE_PENDING_CANCEL;
    if ((paint_injection || sample_injection || update_injection) && !fixture.queued_injection_done) {
        fixture.queued_injection_done = true;
        if (fixture.mode != MODE_NEURO_PENDING_TIMEOUT) atomic_store(&fixture.cancel, true);
        if (fixture.mode == MODE_NEURO_PENDING_TIMEOUT) vTaskDelay(250);
    }
    ryz_runtime_io_execute(request, owner_input, &fixture);
}

static bool io_call(void *context, struct ryz_runtime_io_request *request)
{
    worker_only();
    CHECK(context == &fixture);
    const uint32_t ticket = ryz_workbench_io_submit(&fixture.channel, execute_request, request);
    CHECK(ticket != 0);
    const int64_t deadline = esp_timer_get_time() + 5000000;
    while (!ryz_workbench_io_completed(&fixture.channel, ticket)) {
        CHECK(esp_timer_get_time() < deadline);
        vTaskDelay(1);
    }
    CHECK(ryz_workbench_io_acknowledge(&fixture.channel, ticket));
    return true;
}

static void output(void *context, const char *text, size_t length)
{
    worker_only();
    CHECK(context == &fixture && text);
    ++fixture.output_calls;
    fixture.output_bytes += length;
    static const char marker[] = "COMPUTE_STARTED";
    if (length == sizeof(marker) - 1 && !memcmp(text, marker, length)) {
        atomic_store(&fixture.compute_started, true);
    }
}

static int tool_admission(void *context, const struct ryz_tool_request *request,
                           struct ryz_tool_reply *reply)
{
    worker_only();
    CHECK(context == &fixture && request->action == RYZ_TOOL_I2C_START);
    CHECK(request->expected_revision == UINT32_MAX);
    ++fixture.tool_calls;
    reply->operation_id = fixture.accepted_tool = UINT32_MAX;
    if (fixture.mode == MODE_TOOLS_BAD_RESULT) return -42;
    if (fixture.mode == MODE_TOOLS_POST_CANCEL) atomic_store(&fixture.cancel, true);
    return RYZ_TOOL_CALL_OK;
}

static void *run_worker(void *unused)
{
    (void)unused;
    if (virtual_boot.cancel_before) atomic_store(&fixture.cancel, true);
    const ryz_lua_options_t options = {
        .cancel = &fixture.cancel, .output = output, .context = &fixture,
        .io_call = io_call,
        .tool_call = fixture.mode >= MODE_TOOLS ? tool_admission : NULL,
        .boot_poll = virtual_boot.enabled ? boot_poll : NULL,
    };
    ryz_lua_execute_with_options(fixture.source, strlen(fixture.source),
                                 fixture.source_name ? fixture.source_name : "=owner-test",
                                 fixture.timeout_ms, &options,
                                 &fixture.result);
    CHECK(!atomic_load(&fixture.mounted));
    CHECK(ryz_workbench_io_idle(&fixture.channel));
    atomic_store(&fixture.worker_done, true);
    return NULL;
}

static void run_named(const char *source, const char *name, unsigned timeout, unsigned mode)
{
    memset(&fixture, 0, sizeof(fixture));
    fixture.owner = pthread_self();
    fixture.source = source;
    fixture.source_name = name;
    fixture.timeout_ms = timeout;
    fixture.mode = mode;
    atomic_init(&fixture.cancel, false);
    atomic_init(&fixture.worker_done, false);
    atomic_init(&fixture.compute_started, false);
    atomic_init(&fixture.mounted, NULL);
    atomic_init(&fixture.cleanup_seen, false);
    ryz_workbench_io_init(&fixture.channel);
    pthread_t worker;
    CHECK(pthread_create(&worker, NULL, run_worker, NULL) == 0);
    const int64_t deadline = esp_timer_get_time() + 5000000;
    while (!atomic_load(&fixture.worker_done)) {
        CHECK(esp_timer_get_time() < deadline);
        ++fixture.owner_ticks;
        const bool dispatched = ryz_workbench_io_dispatch(&fixture.channel);
        if (!dispatched) ++fixture.idle_worker_ticks;
        if (atomic_load(&fixture.compute_started)) ++fixture.compute_ticks;
        if ((mode == MODE_SPIN_STOP && fixture.compute_ticks >= 20) ||
            (mode == MODE_UI_STOP && fixture.mounts && fixture.idle_worker_ticks >= 20) ||
            (mode == MODE_NEURO_STOP && fixture.step_calls >= 3 && fixture.touch_reads)) {
            atomic_store(&fixture.cancel, true);
        }
        vTaskDelay(1);
    }
    CHECK(pthread_join(worker, NULL) == 0);
    CHECK(fixture.live_bytes == 0 && fixture.frees_after_cleanup > 0);
    CHECK(fixture.abort_calls == 1 && ryz_workbench_io_idle(&fixture.channel));
    CHECK(fixture.calls[RYZ_IO_ABORT] == 1);
}

static void run(const char *source, unsigned timeout, unsigned mode)
{ run_named(source, "=owner-test", timeout, mode); }

static void phase(const char *expected)
{
    if (strcmp(fixture.result.phase, expected)) {
        fprintf(stderr, "expected phase=%s; got phase=%s ok=%d error=%s\n",
                expected, fixture.result.phase, fixture.result.ok, fixture.result.error);
        abort();
    }
    CHECK(fixture.result.ok == !strcmp(expected, "done"));
}

#define RAW_IO \
    "local d=require('display'); local t=require('touch'); " \
    "d.clear(0x1234); d.rect(1,2,3,4,0xf800); d.text(5,6,'RYZO',0xffff,2); " \
    "assert(d.show()); local p=t.read(); assert(p.pressed and p.x==30 and p.y==80); "

#define UI_SCENE \
    "local ui=require('ui'); ui.mount({id='home',background=0,objects={" \
    "{id='apps',kind='button',x=20,y=72,width=200,height=44,text='APPS'}}}); "

static const char neuro_source[] =
    "-- ryz-neuro/1\n"
    "return {runtime='ryz-neuro/1', catalog='ryz-signals/1', nodes={"
    "{id='finger',kind='touch',inputs={},outputs={},code='while true do sample(); sleep(2) end'},"
    "{id='screen',kind='display',inputs={},outputs={},code='paint(0x1234); sleep(20)'}"
    "},wires={}}";

static void run_lua_body(bool app, const char *body, unsigned timeout, unsigned mode)
{
    char source[4096];
    int length = snprintf(source, sizeof(source), "%s%s", app ? "-- ryz-app/1\n" : "", body);
    CHECK(length > 0 && (size_t)length < sizeof(source));
    run(source, timeout, mode);
}

static void finalizer_case(bool app, const char *name)
{
    if (!strcmp(name, "registration")) {
        static const char *const bodies[] = {
            "local mt={__gc=false}; keeper=setmetatable({},mt); "
            "mt.__gc=function() while true do end end",
            "local callable=setmetatable({}, {__call=function() while true do end end}); "
            "keeper=setmetatable({}, {__gc=callable})",
            "local mt={}; keeper=setmetatable({},mt); "
            "rawset(mt,'__gc',function() while true do end end); other=setmetatable({},mt)",
            "local mt={}; keeper=setmetatable({},mt); "
            "mt.__gc=function() while true do end end; setmetatable(keeper,mt)",
            "keeper=setmetatable({}, {__gc=0})",
        };
        for (size_t i = 0; i < sizeof(bodies) / sizeof(bodies[0]); ++i) {
            char body[1024];
            CHECK(snprintf(body, sizeof(body), "%s; print('REGISTRATION_ACCEPTED')", bodies[i]) > 0);
            run_lua_body(app, body, 100, MODE_NORMAL);
            phase("runtime");
            CHECK(strstr(fixture.result.error, "__gc"));
            CHECK(!strstr(fixture.result.output, "REGISTRATION_ACCEPTED"));
        }
    } else if (!strcmp(name, "delayed")) {
        run_lua_body(app,
            "local mt={}; local value=setmetatable({},mt); "
            "mt.__gc=function() while true do end end; value=nil; "
            "collectgarbage('collect'); collectgarbage('collect'); "
            "local inherited=setmetatable({}, {__index=function() error('META_LOOKUP') end}); "
            "local plain=setmetatable({}, inherited); assert(getmetatable(plain)==inherited); "
            "print('UNREGISTERED_PASS')", 100, MODE_NORMAL);
        phase("done");
        CHECK(strstr(fixture.result.output, "UNREGISTERED_PASS"));
    } else if (!strcmp(name, "metatables")) {
        run_lua_body(app,
            "local mt={__index=function() return 7 end, __add=function() return 11 end}; "
            "local value=setmetatable({},mt); assert(getmetatable(value)==mt); "
            "assert(value.x==7 and value+value==11); mt.__index=function() return 9 end; "
            "assert(value.x==9); assert(setmetatable(value,nil)==value); "
            "assert(getmetatable(value)==nil and value.x==nil); "
            "local locked=setmetatable({}, {__metatable='LOCKED'}); assert(getmetatable(locked)=='LOCKED'); "
            "local weak=setmetatable({}, {__mode='v'}); weak[1]={}; collectgarbage('collect'); assert(weak[1]==nil); "
            "assert(debug==nil and io==nil and package==nil and type(coroutine)=='table' and load==nil); "
            "assert(#string.rep('x',8192)==8192); local n=0; for _ in string.gmatch('abc','.') do n=n+1 end; "
            "assert(n==3); math.randomseed(1); assert(type(math.random())=='number'); "
            "print('METATABLE_PASS')", 500, MODE_NORMAL);
        phase("done");
        CHECK(strstr(fixture.result.output, "METATABLE_PASS"));
        static const char *const invalid[] = {
            "local t=setmetatable({}, {__metatable='LOCKED'}); setmetatable(t,nil)",
            "setmetatable('',{})", "setmetatable({},false)",
        };
        for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
            run_lua_body(app, invalid[i], 100, MODE_NORMAL);
            phase("runtime");
        }
    } else if (!strcmp(name, "close_normal")) {
        run_lua_body(app,
            "local closed={}; local mt={__close=function(self,err) assert(err==nil); "
            "closed[#closed+1]=self.id end}; "
            "local function f() local a <close> = setmetatable({id='A'},mt); "
            "local b <close> = setmetatable({id='B'},mt); return 42 end; "
            "assert(f()==42 and table.concat(closed)=='BA'); "
            "getmetatable('').__close=function(value,err) assert(value=='str' and err==nil); "
            "closed[#closed+1]='S' end; do local value <close> = 'str' end; "
            "assert(table.concat(closed)=='BAS'); print('CLOSE_PASS')", 100, MODE_NORMAL);
        phase("done");
        CHECK(strstr(fixture.result.output, "CLOSE_PASS"));
        run_lua_body(app,
            "local mt={__close=function(self,err) assert(string.find(err,'USER_ERROR',1,true)); "
            "print(self.id) end}; local a <close> = setmetatable({id='CLOSE_A'},mt); "
            "local b <close> = setmetatable({id='CLOSE_B'},mt); error('USER_ERROR')", 100, MODE_NORMAL);
        phase("runtime");
        CHECK(strstr(fixture.result.error, "USER_ERROR"));
        CHECK(strstr(fixture.result.output, "CLOSE_B\nCLOSE_A\n"));
    } else if (!strcmp(name, "close_timeout")) {
        static const char *const bodies[] = {
            "local value <close> = setmetatable({}, {__close=function() while true do end end})",
            "local value <close> = setmetatable({}, {__close=function() while true do end end}); error('USER_ERROR')",
            "local value <close> = setmetatable({}, {__close=function() while true do end end}); while true do end",
            "getmetatable('').__close=function() while true do end end; local value <close> = ''; error('USER_ERROR')",
            "local value <close> = setmetatable({}, {__close=function() "
            "local inner <close> = setmetatable({}, {__close=function() while true do end end}) end}); error('USER_ERROR')",
            "local value <close> = setmetatable({}, {__close=function() while true do end end}); "
            "string.rep('x',4*1024*1024)",
        };
        for (size_t i = 0; i < sizeof(bodies) / sizeof(bodies[0]); ++i) {
            run_lua_body(app, bodies[i], 40, MODE_NORMAL);
            /* This must reach the hook inside __close, not merely report the
             * earlier USER_ERROR / allocation error while leaving teardown stuck. */
            phase("timeout");
            CHECK(strstr(fixture.result.error, "timed out"));
        }
    } else if (!strcmp(name, "close_cancel")) {
        static const char *const bodies[] = {
            "local value <close> = setmetatable({}, {__close=function() "
            "print('COMPUTE_STARTED'); while true do end end})",
            "local value <close> = setmetatable({}, {__close=function() while true do end end}); "
            "print('COMPUTE_STARTED'); while true do end",
            "getmetatable('').__close=function() print('COMPUTE_STARTED'); while true do end end; "
            "local value <close> = ''; error('USER_ERROR')",
        };
        for (size_t i = 0; i < sizeof(bodies) / sizeof(bodies[0]); ++i) {
            run_lua_body(app, bodies[i], 0, MODE_SPIN_STOP);
            phase("stopped");
            CHECK(fixture.compute_ticks >= 20 && atomic_load(&fixture.cancel));
        }
    } else CHECK(false);
}

static void filesystem_case(const char *name)
{
    for (unsigned dialect = 0; dialect < 2; ++dialect) {
        memset(&virtual_fs, 0, sizeof(virtual_fs));
        const char *body = NULL;
        const char *source_name = dialect ? "@persist.lua" : "persist.lua";
        if (!strcmp(name, "fs_public_both")) {
            virtual_fs.enabled = true;
            body = "local fs=require('fs'); "
                "for _,n in ipairs({'read','write','remove','list','info'}) do assert(type(fs[n])=='function') end; "
                "assert(io==nil and package==nil and loadfile==nil); "
                "local i=assert(fs.info()); assert(i.used_bytes==0 and i.file_count==0 and i.max_file_bytes==8192 "
                "and i.quota_bytes==32768 and i.max_files==16); assert(#assert(fs.list())==0); "
                "local v,e=fs.read('missing'); assert(v==nil and e=='not_found'); "
                "assert(fs.write('zeta.dat','A\\0B')); assert(fs.read('zeta.dat')=='A\\0B'); "
                "assert(fs.write('alpha.dat','')); assert(fs.read('alpha.dat')==''); "
                "local files=assert(fs.list()); assert(#files==2 and files[1].name=='alpha.dat' "
                "and files[1].size==0 and files[2].name=='zeta.dat' and files[2].size==3); "
                "local bytes=string.rep('x',8192); assert(fs.write('zeta.dat',bytes)); "
                "assert(fs.read('zeta.dat')==bytes); i=assert(fs.info()); assert(i.file_count==2 and i.used_bytes==8192); "
                "assert(fs.remove('alpha.dat')); v,e=fs.remove('alpha.dat'); assert(v==nil and e=='not_found'); "
                "assert(fs.remove('zeta.dat')); i=assert(fs.info()); assert(i.file_count==0 and i.used_bytes==0); "
                "assert(#assert(fs.list())==0); print('FS_PUBLIC_PASS')";
        } else if (!strcmp(name, "fs_unavailable_both") || !strcmp(name, "fs_anonymous_both")) {
            if (!strcmp(name, "fs_anonymous_both")) { source_name = "=serial"; virtual_fs.enabled = true; }
            body = "local fs=require('fs'); local v,e=fs.read('a'); assert(v==nil and e=='unavailable'); "
                "v,e=fs.write('a','b'); assert(v==nil and e=='unavailable'); "
                "v,e=fs.remove('a'); assert(v==nil and e=='unavailable'); "
                "v,e=fs.list(); assert(v==nil and e=='unavailable'); "
                "v,e=fs.info(); assert(v==nil and e=='unavailable')";
        } else {
            CHECK(!strcmp(name, "fs_post_cancel_both"));
            virtual_fs.enabled = true;
            virtual_fs.cancel_after_write = true;
            body = "local fs=require('fs'); coroutine.resume(coroutine.create(function() "
                "assert(fs.write('cancel.dat','SAVED')); print('AFTER_FS_CALLBACK') end)); "
                "error('FS_CANCEL_SWALLOWED')";
        }
        char source[4096];
        int length = snprintf(source, sizeof(source), "%s%s", dialect ? "-- ryz-app/1\n" : "", body);
        CHECK(length > 0 && (size_t)length < sizeof(source));
        run_named(source, source_name, 2000, MODE_NORMAL);
        if (!strcmp(name, "fs_post_cancel_both")) {
            phase("stopped");
            CHECK(virtual_fs.calls == 1 && atomic_load(&fixture.cancel));
            CHECK(!strstr(fixture.result.output, "AFTER_FS_CALLBACK"));
            CHECK(!strstr(fixture.result.error, "FS_CANCEL_SWALLOWED"));
            /* Cancellation after a successful callback does not roll back the
             * write. The fake remains explicit in-memory evidence only. */
            CHECK(virtual_fs.files[1].present && virtual_fs.files[1].size == 5);
            CHECK(!memcmp(virtual_fs.files[1].data, "SAVED", 5));
        } else {
            phase("done");
            if (!strcmp(name, "fs_anonymous_both")) CHECK(!virtual_fs.calls);
            else if (!strcmp(name, "fs_unavailable_both")) CHECK(virtual_fs.calls == 5);
            else CHECK(virtual_fs.calls == 16 && strstr(fixture.result.output, "FS_PUBLIC_PASS"));
        }
        CHECK(fixture.calls[RYZ_IO_ABORT] == 1 && !fixture.mounts && !fixture.updates);
    }
}

static void boot_case(const char *name)
{
    unsigned cases = 0;
    for (unsigned dialect = 0; dialect < 2; ++dialect) {
        memset(&virtual_boot, 0, sizeof(virtual_boot));
        virtual_boot.enabled = true;
        if (!strcmp(name, "boot_public_both")) {
            virtual_boot.count = 3;
            virtual_boot.replies[0].event = (ryz_boot_event_t){RYZ_BOOT_CLICK, 0, 0};
            virtual_boot.replies[1].event = (ryz_boot_event_t){RYZ_BOOT_DOUBLE_CLICK, UINT32_MAX, 2999};
            virtual_boot.replies[2].event = (ryz_boot_event_t){RYZ_BOOT_LONG_PRESS, UINT32_C(2147483648), 3000};
            run_lua_body(dialect, "local b=require('boot'); assert(b==require('boot')); "
                "assert(math.maxinteger==2147483647 and b.configure==nil and b.on==nil and b.pin==nil); "
                "local a=table.pack(b.poll()); assert(a.n==1); local e=a[1]; "
                "assert(e.type=='click' and e.held_ms==0 and e.timestamp_ms=='0'); "
                "local n=0; for _ in pairs(e) do n=n+1 end; assert(n==3); "
                "e=assert(b.poll()); assert(e.type=='double_click' and e.held_ms==2999 "
                "and math.type(e.held_ms)=='integer' and e.timestamp_ms=='4294967295'); "
                "e=assert(b.poll()); assert(e.type=='long_press' and e.held_ms==3000 "
                "and e.timestamp_ms=='2147483648'); a=table.pack(b.poll()); "
                "assert(a.n==1 and a[1]==nil); print('BOOT_PUBLIC_PASS')", 2000, MODE_NORMAL);
            phase("done"); CHECK(virtual_boot.calls == 4 && virtual_boot.cursor == 3); ++cases;
        } else if (!strcmp(name, "boot_no_backend_both")) {
            virtual_boot.enabled = false;
            run_lua_body(dialect, "local a=table.pack(require('boot').poll()); "
                "assert(a.n==2 and a[1]==nil and a[2]=='unavailable')", 2000, MODE_NORMAL);
            phase("done"); CHECK(!virtual_boot.calls); ++cases;
        } else if (!strcmp(name, "boot_errors_both")) {
            static const ryz_boot_result_t codes[] = {RYZ_BOOT_UNAVAILABLE, RYZ_BOOT_BUSY,
                RYZ_BOOT_OVERFLOW, RYZ_BOOT_FAILED, (ryz_boot_result_t)-1, (ryz_boot_result_t)123};
            static const char *reasons[] = {"unavailable", "busy", "overflow", "failed", "failed", "failed"};
            for (unsigned i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) {
                virtual_boot.cursor = virtual_boot.calls = 0; virtual_boot.count = 1;
                virtual_boot.replies[0].result = codes[i];
                memset(&virtual_boot.replies[0].event, 0xff, sizeof(ryz_boot_event_t));
                char body[256];
                snprintf(body, sizeof(body), "local a=table.pack(require('boot').poll()); "
                    "assert(a.n==2 and a[1]==nil and a[2]=='%s')", reasons[i]);
                run_lua_body(dialect, body, 2000, MODE_NORMAL);
                phase("done"); CHECK(virtual_boot.calls == 1); ++cases;
            }
        } else if (!strcmp(name, "boot_invalid_payload_both")) {
            static const ryz_boot_event_t events[] = {
                {(ryz_boot_event_kind_t)-1, 1, 20}, {(ryz_boot_event_kind_t)99, 1, 20},
                {RYZ_BOOT_CLICK, 1, 3000}, {RYZ_BOOT_CLICK, 1, UINT32_MAX},
                {RYZ_BOOT_DOUBLE_CLICK, 1, 3000}, {RYZ_BOOT_DOUBLE_CLICK, 1, UINT32_MAX},
                {RYZ_BOOT_LONG_PRESS, 1, 0}, {RYZ_BOOT_LONG_PRESS, 1, 2999},
                {RYZ_BOOT_LONG_PRESS, 1, 3001}, {RYZ_BOOT_LONG_PRESS, 1, UINT32_MAX},
            };
            for (unsigned i = 0; i < sizeof(events) / sizeof(events[0]); ++i) {
                virtual_boot.cursor = virtual_boot.calls = 0; virtual_boot.count = 1;
                virtual_boot.replies[0].event = events[i];
                run_lua_body(dialect, "local a=table.pack(require('boot').poll()); "
                    "assert(a.n==2 and a[1]==nil and a[2]=='failed')", 2000, MODE_NORMAL);
                phase("done"); CHECK(virtual_boot.calls == 1); ++cases;
            }
        } else if (!strcmp(name, "boot_arguments_both")) {
            static const char *bodies[] = {"require('boot').poll(nil)", "require('boot').poll(1)",
                "require('boot').poll(true)", "require('boot').poll('click')", "require('boot').poll({})",
                "require('boot').poll(function() end)", "require('boot'):poll()", "require('boot\\0other')"};
            for (unsigned i = 0; i < sizeof(bodies) / sizeof(bodies[0]); ++i) {
                run_lua_body(dialect, bodies[i], 2000, MODE_NORMAL);
                phase("runtime"); CHECK(!virtual_boot.calls); ++cases;
            }
        } else if (!strcmp(name, "boot_coroutine_both")) {
            virtual_boot.count = 3;
            for (unsigned i = 0; i < 3; ++i)
                virtual_boot.replies[i].event = (ryz_boot_event_t){RYZ_BOOT_CLICK, i + 10U, 100};
            run_lua_body(dialect, "local b=require('boot'); local co=coroutine.create(function() "
                "assert(b.poll().timestamp_ms=='10'); coroutine.yield(); "
                "assert(b.poll().timestamp_ms=='12') end); assert(coroutine.resume(co)); "
                "assert(b.poll().timestamp_ms=='11'); assert(coroutine.resume(co)); "
                "assert(b.poll()==nil)", 2000, MODE_NORMAL);
            phase("done"); CHECK(virtual_boot.calls == 4 && virtual_boot.cursor == 3); ++cases;
        } else if (!strcmp(name, "boot_cancel_both")) {
            virtual_boot.cancel_before = true;
            run_lua_body(dialect, "require('boot').poll(); error('ESCAPED')", 2000, MODE_NORMAL);
            phase("stopped"); CHECK(!virtual_boot.calls); ++cases;
            virtual_boot.cancel_before = false; virtual_boot.cancel_after = true;
            for (unsigned co = 0; co < 2; ++co) {
                virtual_boot.calls = 0;
                run_lua_body(dialect, co ?
                    "coroutine.resume(coroutine.create(function() require('boot').poll() end)); error('SWALLOWED')" :
                    "require('boot').poll(); error('ESCAPED')", 2000, MODE_NORMAL);
                phase("stopped"); CHECK(virtual_boot.calls == 1);
                CHECK(!strstr(fixture.result.error, "ESCAPED") && !strstr(fixture.result.error, "SWALLOWED")); ++cases;
            }
        } else {
            CHECK(!strcmp(name, "boot_timeout_both"));
            virtual_boot.timeout_after = true;
            for (unsigned co = 0; co < 2; ++co) {
                virtual_boot.calls = 0;
                run_lua_body(dialect, co ?
                    "coroutine.resume(coroutine.create(function() require('boot').poll() end)); error('SWALLOWED')" :
                    "require('boot').poll(); error('ESCAPED')", 50, MODE_NORMAL);
                phase("timeout"); CHECK(virtual_boot.calls == 1);
                CHECK(!strstr(fixture.result.error, "ESCAPED") && !strstr(fixture.result.error, "SWALLOWED")); ++cases;
            }
        }
        CHECK(!fixture.mounts && !fixture.updates);
    }
    printf("BOOT_LUA_CASES %u\n", cases);
    memset(&virtual_boot, 0, sizeof(virtual_boot));
}

static void boot_demo_case(const char *path)
{
    FILE *file = fopen(path, "rb"); CHECK(file);
    char *source = malloc(RYZ_LUA_SOURCE_MAX + 1U); CHECK(source);
    size_t size = fread(source, 1, RYZ_LUA_SOURCE_MAX + 1U, file);
    CHECK(!ferror(file) && size > 0 && size <= RYZ_LUA_SOURCE_MAX && fclose(file) == 0);
    source[size] = 0;
    CHECK(strncmp(source, "-- ryz-app/1\n", 13U) == 0);
    for (unsigned mode = 0; mode < 3; ++mode) {
        memset(&virtual_boot, 0, sizeof(virtual_boot)); virtual_boot.enabled = true;
        if (mode == 0) {
            virtual_boot.count = 26;
            virtual_boot.replies[0].result = RYZ_BOOT_EMPTY;
            virtual_boot.replies[1].result = RYZ_BOOT_BUSY;
            virtual_boot.replies[2].result = RYZ_BOOT_OVERFLOW;
            for (unsigned i = 3; i < 26; ++i) {
                ryz_boot_event_kind_t kind = (ryz_boot_event_kind_t)((i - 3U) % 3U);
                virtual_boot.replies[i].event = (ryz_boot_event_t){kind, UINT32_MAX,
                    kind == RYZ_BOOT_LONG_PRESS ? 3000U : 100U};
            }
        } else {
            virtual_boot.count = 1;
            virtual_boot.replies[0].result = mode == 1 ? RYZ_BOOT_UNAVAILABLE : RYZ_BOOT_FAILED;
        }
        run_named(source, "@boot_events_demo.lua", 2000, MODE_NORMAL); phase("done");
        if (mode == 0) {
            CHECK(virtual_boot.calls == 26);
            CHECK(strstr(fixture.result.output, "CLICK HELD=100MS AT=4294967295"));
            CHECK(strstr(fixture.result.output, "DOUBLE_CLICK HELD=100MS AT=4294967295"));
            CHECK(strstr(fixture.result.output, "LONG_PRESS HELD=3000MS AT=4294967295"));
            CHECK(strstr(fixture.result.output, "BOOT OVERFLOW:"));
            CHECK(strstr(fixture.result.output, "BOOT EVENT DEMO COMPLETE"));
            CHECK(!strstr(fixture.result.output, "BOOT ERROR:"));
        } else {
            CHECK(virtual_boot.calls == 1);
            CHECK(strstr(fixture.result.output, mode == 1 ? "BOOT ERROR: unavailable" : "BOOT ERROR: failed"));
            CHECK(!strstr(fixture.result.output, "BOOT EVENT DEMO COMPLETE"));
        }
        CHECK(!fixture.result.output_truncated);
    }
    free(source);
    memset(&virtual_boot, 0, sizeof(virtual_boot));
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "boot_demo")) {
        boot_demo_case(argv[2]);
        puts("RUNTIME_OWNER_PASS: actual boot_events_demo.lua, 3 host scenarios");
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "finalizer")) {
        CHECK(!strcmp(argv[2], "app") || !strcmp(argv[2], "legacy"));
        finalizer_case(!strcmp(argv[2], "app"), argv[3]);
        printf("RUNTIME_OWNER_PASS: finalizer %s %s\n", argv[2], argv[3]);
        return 0;
    }
    CHECK(argc == 2);
    const char *name = argv[1];
    if (!strncmp(name, "boot_", 5)) {
        boot_case(name);
    } else if (!strncmp(name, "fs_", 3)) {
        filesystem_case(name);
    } else if(!strcmp(name,"peripheral_public_both")) {
        const char *open="local p=assert(require('gpio').open{pin=13,mode='output',initial=1}); ";
        for(unsigned dialect=0;dialect<2;++dialect) {
            char source[512]; const char *prefix=dialect ? "-- ryz-app/1\n" : "";
            snprintf(source,sizeof(source),"%sfor _,n in ipairs({'gpio','timer','pwm','i2c','spi','uart','adc','log','led'}) do "
                "assert(type(require(n).open)=='function') end; "
                "local p,e=require('gpio').open{pin=13}; assert(p==nil and e=='unavailable')",prefix);
            virtual_pin_enabled=false; run(source,2000,MODE_NORMAL); phase("done");
            virtual_pin_enabled=true;
            snprintf(source,sizeof(source),"%s%sassert(p:read()==1); assert(p:write(0)); assert(p:read()==0)",prefix,open);
            run(source,2000,MODE_NORMAL); phase("done");
            snprintf(source,sizeof(source),"%s%serror('PUBLIC_JOB_ERROR')",prefix,open);
            run(source,2000,MODE_NORMAL); phase("runtime"); CHECK(strstr(fixture.result.error,"PUBLIC_JOB_ERROR"));
            virtual_pin_cancel=true;
            snprintf(source,sizeof(source),"%s%scoroutine.resume(coroutine.create(function() p:write(1) end)); "
                "error('CANCEL_SWALLOWED')",prefix,open);
            run(source,2000,MODE_NORMAL); phase("stopped");
            CHECK(!strstr(fixture.result.error,"CANCEL_SWALLOWED")); virtual_pin_cancel=false;
            snprintf(source,sizeof(source),"%s%swhile true do end",prefix,open);
            run(source,30,MODE_NORMAL); phase("timeout");
            snprintf(source,sizeof(source),"%s%sassert(p:close()); assert(p:close())",prefix,open);
            run(source,2000,MODE_NORMAL); phase("done");
        }
    } else if (!strcmp(name, "finalizer_gc_app") || !strcmp(name, "finalizer_gc_legacy")) {
        const bool app = !strcmp(name, "finalizer_gc_app");
        const char *body = "keeper=setmetatable({}, {__gc=function() while true do end end}); "
                           "print('REGISTRATION_ACCEPTED')";
        char source[256];
        CHECK(snprintf(source, sizeof(source), "%s%s", app ? "-- ryz-app/1\n" : "", body) > 0);
        run(source, 50, MODE_NORMAL);
        phase("runtime");
        CHECK(strstr(fixture.result.error, "__gc"));
        CHECK(!strstr(fixture.result.output, "REGISTRATION_ACCEPTED"));
    } else if (!strcmp(name, "legacy")) {
        run(RAW_IO "assert(d.read_pixel(0,0)==0x1234); assert(t.demo(true)); "
            "local i=t.info(); assert(i.ready and i.controller=='CST816T' and i.demo_enabled); "
            "assert(t.demo(false)); print('LEGACY_OWNER');", 2000, MODE_NORMAL);
        phase("done");
        CHECK(fixture.read_pixel_calls == 1 && fixture.touch_infos == 1);
        CHECK(fixture.clear_calls == 1 && fixture.rect_calls == 1 && fixture.text_calls == 1);
        CHECK(fixture.show_calls == 1 && fixture.touch_reads == 1 && fixture.demo_calls >= 6);
        CHECK(fixture.output_calls && fixture.output_bytes);
    } else if (!strcmp(name, "app_raw")) {
        run("-- ryz-app/1\n" RAW_IO "print('APP_OWNER')", 2000, MODE_NORMAL);
        phase("done");
        CHECK(fixture.clear_calls == 1 && fixture.rect_calls == 1 && fixture.text_calls == 1);
        CHECK(fixture.show_calls == 1 && fixture.touch_reads == 1);
        CHECK(fixture.output_calls && fixture.output_bytes);
    } else if (!strcmp(name,"app_touch_interrupted") || !strcmp(name,"legacy_touch_interrupted")) {
        const char *body="local p=require('touch').read(); "
            "assert(p.interrupted==true and not p.pressed and p.event=='none' and p.x==nil and p.y==nil)";
        char source[256];
        CHECK(snprintf(source,sizeof(source),"%s%s",name[0]=='a'?"-- ryz-app/1\n":"",body)>0);
        run(source,2000,MODE_TOUCH_INTERRUPTED);
        phase("done");
        CHECK(fixture.touch_reads==1);
    } else if (!strcmp(name, "app_ui")) {
        run("-- ryz-app/1\n" UI_SCENE
            "assert(ui.poll()==nil); local e=ui.poll(); "
            "assert(e.kind=='activate' and e.id=='apps'); print('SCENE_OWNER')", 2000, MODE_NORMAL);
        phase("done");
        CHECK(fixture.mounts == 1 && fixture.pumps == 2 && fixture.closes == 1);
        CHECK(fixture.touch_reads == 2 && !strcmp(fixture.cleanup_order, "CA"));
    } else if (!strcmp(name, "neuro")) {
        run(neuro_source, 2000, MODE_NEURO_STOP);
        phase("stopped");
        CHECK(fixture.touch_reads >= 1 && fixture.step_calls == 3 && fixture.clear_calls == 1);
    } else if (!strcmp(name, "legacy_spin") || !strcmp(name, "app_spin")) {
        const bool app = !strcmp(name, "app_spin");
        run(app ? "-- ryz-app/1\nprint('COMPUTE_STARTED'); while true do end" :
                  "print('COMPUTE_STARTED'); while true do end", 0, MODE_SPIN_STOP);
        phase("stopped");
        CHECK(fixture.compute_ticks >= 20 && fixture.clear_calls == 0);
        for (unsigned i = 0; i <= RYZ_IO_UI_UPDATE; ++i) {
            if (i != RYZ_IO_ABORT) CHECK(fixture.calls[i] == 0);
        }
    } else if (!strcmp(name, "ui_stop") || !strcmp(name, "ui_timeout")) {
        const bool timeout = !strcmp(name, "ui_timeout");
        run("-- ryz-app/1\n" UI_SCENE "while true do end", timeout ? 100 : 0,
            timeout ? MODE_NORMAL : MODE_UI_STOP);
        phase(timeout ? "timeout" : "stopped");
        CHECK(fixture.mounts == 1 && fixture.closes == 1);
        CHECK(!strcmp(fixture.cleanup_order, "CA") && fixture.idle_worker_ticks >= 10);
    } else if (!strcmp(name, "show_timeout")) {
        run("local d=require('display'); d.clear(0); d.show()", 60, MODE_SHOW_TIMEOUT);
        phase("timeout");
        CHECK(fixture.show_calls == 1 && fixture.calls[RYZ_IO_SHOW] == 1);
    } else if (!strcmp(name, "neuro_pending_stop")) {
        run(neuro_source, 2000, MODE_NEURO_PENDING_STOP);
        phase("stopped");
        CHECK(fixture.calls[RYZ_IO_NEURO_PAINT] == 1 && fixture.step_calls == 0);
    } else if (!strcmp(name, "neuro_pending_timeout")) {
        run(neuro_source, 100, MODE_NEURO_PENDING_TIMEOUT);
        phase("timeout");
        CHECK(fixture.calls[RYZ_IO_NEURO_PAINT] == 1 && fixture.step_calls == 0);
    } else if (!strcmp(name, "neuro_sample_stop")) {
        run(neuro_source, 2000, MODE_NEURO_SAMPLE_STOP);
        phase("stopped");
        CHECK(fixture.calls[RYZ_IO_TOUCH_READ] == 1 && fixture.touch_reads == 0);
    } else if (!strcmp(name, "neuro_stripe_stop")) {
        run(neuro_source, 2000, MODE_NEURO_STRIPE_STOP);
        phase("stopped");
        CHECK(fixture.calls[RYZ_IO_NEURO_PAINT] == 2 && fixture.step_calls == 1);
        CHECK(!fixture.painting);
    } else if (!strcmp(name, "app_deadline_skew")) {
        run("-- ryz-app/1\nlocal d=require('display'); d.show()", 120, MODE_APP_DEADLINE_SKEW);
        phase("timeout");
        CHECK(fixture.show_calls == 1);
    } else if (!strcmp(name, "legacy_touch_error")) {
        run("require('touch').read()", 2000, MODE_TOUCH_FAILURE);
        phase("runtime");
        CHECK(!atomic_load(&fixture.cancel) && fixture.touch_reads == 1);
        CHECK(strstr(fixture.result.error, "ESP_ERR_TIMEOUT"));
    } else if (!strcmp(name, "app_show_error")) {
        run("-- ryz-app/1\nrequire('display').show()", 2000, MODE_SHOW_FAILURE);
        phase("runtime");
        CHECK(!atomic_load(&fixture.cancel) && fixture.show_calls == 1);
        CHECK(strstr(fixture.result.error, "display show failed"));
    } else if (!strcmp(name, "neuro_paint_error")) {
        run(neuro_source, 2000, MODE_PAINT_FAILURE);
        phase("runtime");
        CHECK(!atomic_load(&fixture.cancel) && fixture.step_calls == 1);
        CHECK(strstr(fixture.result.error, "display operation failed"));
    } else if (!strcmp(name,"app_update")) {
        run("-- ryz-app/1\n" UI_SCENE
            "assert(ui.poll()==nil); assert(ui.update(1,{})==1); "
            "assert(ui.update(1,{{id='apps',text='NEXT'}})==1); assert(ui.poll()==nil); "
            "assert(ui.update(1,{{id='apps',text='NEXT'}})==1)",2000,MODE_NORMAL);
        phase("done");
        CHECK(fixture.mounts==1 && fixture.updates==1 && fixture.closes==1 && fixture.pumps==2);
        CHECK(fixture.calls[RYZ_IO_UI_UPDATE]==1 && !strcmp(fixture.cleanup_order,"CA"));
    } else if (!strcmp(name,"app_update_invalid")) {
        run("-- ryz-app/1\n" UI_SCENE
            "ui.update(1,{{id='apps',text='NEXT'},{id='missing',text='X'}})",2000,MODE_NORMAL);
        phase("runtime");
        CHECK(fixture.mounts==1 && !fixture.updates && !fixture.calls[RYZ_IO_UI_UPDATE]);
        CHECK(fixture.closes==1 && !strcmp(fixture.cleanup_order,"CA"));
    } else if (!strcmp(name,"app_update_pending_cancel") || !strcmp(name,"app_update_post_cancel") ||
               !strcmp(name,"app_update_timeout") || !strcmp(name,"app_update_driver_error")) {
        const unsigned mode=!strcmp(name,"app_update_pending_cancel") ? MODE_UPDATE_PENDING_CANCEL :
            !strcmp(name,"app_update_post_cancel") ? MODE_UPDATE_POST_CANCEL :
            !strcmp(name,"app_update_timeout") ? MODE_UPDATE_TIMEOUT : MODE_UPDATE_FAILURE;
        run("-- ryz-app/1\n" UI_SCENE
            "ui.update(1,{{id='apps',text='NEXT'}}); error('AFTER_UPDATE')",
            mode==MODE_UPDATE_TIMEOUT ? 100 : 2000,mode);
        phase(mode==MODE_UPDATE_FAILURE ? "runtime" : mode==MODE_UPDATE_TIMEOUT ? "timeout" : "stopped");
        CHECK(fixture.mounts==1 && fixture.closes==1 && fixture.calls[RYZ_IO_UI_UPDATE]==1);
        CHECK(fixture.updates==(mode==MODE_UPDATE_PENDING_CANCEL ? 0U : 1U));
        CHECK(!strcmp(fixture.cleanup_order,"CA") && !strstr(fixture.result.error,"AFTER_UPDATE"));
        if (mode==MODE_UPDATE_FAILURE) CHECK(!atomic_load(&fixture.cancel));
    } else if (!strcmp(name, "legacy_coroutine_timeout")) {
        run("local co=coroutine.create(function() while true do end end); "
            "coroutine.resume(co); error('ESCAPED')", 40, MODE_NORMAL);
        phase("timeout"); CHECK(!strstr(fixture.result.error,"ESCAPED"));
    } else if (!strcmp(name, "legacy_coroutine_cancel")) {
        run("print('COMPUTE_STARTED'); local co=coroutine.create(function() while true do end end); "
            "coroutine.resume(co); error('ESCAPED')", 2000, MODE_SPIN_STOP);
        phase("stopped"); CHECK(!strstr(fixture.result.error,"ESCAPED"));
    } else if (!strcmp(name, "app_tools")) {
        run("-- ryz-app/1\nlocal t=require('tools'); local r=t.i2c.start('4294967295'); "
            "assert(r.ok and r.accepted and r.operation_id=='4294967295')", 2000, MODE_TOOLS);
        phase("done"); CHECK(fixture.tool_calls == 1 && fixture.accepted_tool == UINT32_MAX);
    } else if (!strcmp(name, "app_tools_unavailable")) {
        run("-- ryz-app/1\nlocal r=require('tools').rgb.status(); "
            "assert(not r.ok and r.code=='unavailable')", 2000, MODE_NORMAL);
        phase("done"); CHECK(!fixture.tool_calls);
    } else if (!strcmp(name, "app_tools_bad_result")) {
        run("-- ryz-app/1\nlocal r=require('tools').i2c.start('4294967295'); "
            "assert(not r.ok and r.code=='failed' and r.operation_id==nil)", 2000, MODE_TOOLS_BAD_RESULT);
        phase("done"); CHECK(fixture.tool_calls == 1);
    } else if (!strcmp(name, "app_tools_post_cancel")) {
        run("-- ryz-app/1\nrequire('tools').i2c.start('4294967295'); error('unreachable')",
            2000, MODE_TOOLS_POST_CANCEL);
        phase("stopped"); CHECK(fixture.tool_calls == 1 && fixture.accepted_tool == UINT32_MAX);
    } else CHECK(false);
    printf("RUNTIME_OWNER_PASS: %s owner_ticks=%u idle_ticks=%u\n", name,
           fixture.owner_ticks, fixture.idle_worker_ticks);
    return 0;
}
