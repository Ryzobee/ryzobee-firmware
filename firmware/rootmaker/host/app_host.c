#define _POSIX_C_SOURCE 200809L
#include "app_runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define HOST_POINTERS 128
#define HOST_CHECKS 128
#define HOST_OPS 128
#define HOST_MARKS 32
#define HOST_SCREEN_TEXTS 32

typedef struct { uint64_t at; ryz_app_pointer_t pointer; } scheduled_pointer_t;
typedef struct { char name[25], text[25]; ryz_app_value_kind_t kind; int32_t integer; bool boolean; } host_mark_t;
typedef struct {
    bool live, stopped, input_eof, input_failed, running, tool_attempted;
    uint64_t clock, duration, live_started, sequence;
    uint32_t seed, presented;
    clock_t cpu_started;
    scheduled_pointer_t pointers[HOST_POINTERS]; unsigned pointer_count, next_pointer;
    ryz_app_pointer_t pointer_queue[HOST_POINTERS]; unsigned queue_head, queue_count;
    ryz_app_pointer_t current; bool desired_pressed;
    uint64_t checks[HOST_CHECKS]; unsigned check_count, next_check;
    ryz_app_display_op_t ops[HOST_OPS]; unsigned op_count;
    char presented_texts[HOST_SCREEN_TEXTS][RYZ_APP_TEXT_MAX + 1];
    unsigned presented_text_count, presented_text_stored, presented_rect_count;
    host_mark_t marks[HOST_MARKS]; unsigned mark_count;
    bool marks_dirty;
    char input[256], input_error[96]; size_t input_length;
} host_t;

static void *host_resize(void *ptr, size_t size)
{
    if (!size) { free(ptr); return NULL; }
    return realloc(ptr, size);
}

static uint64_t monotonic_ms(void)
{
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000 + (uint64_t)value.tv_nsec / 1000000;
}

static uint64_t host_now(void *context)
{
    host_t *host = context;
    return host->live ? monotonic_ms() - host->live_started : host->clock;
}

static void quoted(const char *text, size_t length)
{
    putchar('"');
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c == '"' || c == '\\') { putchar('\\'); putchar(c); }
        else if (c == '\n') fputs("\\n", stdout);
        else if (c == '\r') fputs("\\r", stdout);
        else if (c == '\t') fputs("\\t", stdout);
        else if (c < 0x20) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

static void print_op(const ryz_app_display_op_t *op)
{
    if (op->kind == RYZ_APP_DISPLAY_CLEAR) {
        printf("{\"op\":\"clear\",\"color\":%u}", op->color);
    } else if (op->kind == RYZ_APP_DISPLAY_RECT) {
        printf("{\"op\":\"rect\",\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d,\"color\":%u}",
               op->x, op->y, op->width, op->height, op->color);
    } else {
        printf("{\"op\":\"text\",\"x\":%d,\"y\":%d,\"text\":", op->x, op->y);
        quoted(op->text, strlen(op->text));
        printf(",\"color\":%u,\"scale\":%d}", op->color, op->scale);
    }
}

static void print_mark(const host_mark_t *mark)
{
    /* Keep application-authored observables in their own namespace. The Lua
     * facade already restricts names to identifier bytes, so this is JSON-safe. */
    fputs("\"mark.", stdout); fputs(mark->name, stdout); fputs("\":", stdout);
    if (mark->kind == RYZ_APP_VALUE_INTEGER) printf("%ld", (long)mark->integer);
    else if (mark->kind == RYZ_APP_VALUE_BOOLEAN) fputs(mark->boolean ? "true" : "false", stdout);
    else quoted(mark->text, strlen(mark->text));
}

static void snapshot(host_t *host)
{
    printf("{\"event\":\"frame\",\"runtime\":\"%s\",\"catalog\":\"%s\",\"seq\":%llu,\"at\":%llu,"
           "\"pressed\":%s,\"has_position\":%s,\"x\":%u,\"y\":%u,\"width\":%u,\"height\":%u,\"ops\":[",
           RYZ_APP_ABI, RYZ_APP_CATALOG, (unsigned long long)++host->sequence,
           (unsigned long long)host_now(host), host->current.pressed ? "true" : "false",
           host->current.has_position ? "true" : "false",
           host->current.x, host->current.y, RYZ_APP_WIDTH, RYZ_APP_HEIGHT);
    for (unsigned i = 0; i < host->op_count; ++i) { if (i) putchar(','); print_op(&host->ops[i]); }
    printf("],\"nodes\":{\"app.running\":%s,\"screen.presented\":%u,"
           "\"screen.text_count\":%u,\"screen.rect_count\":%u",
           host->running ? "true" : "false", host->presented,
           host->presented_text_count, host->presented_rect_count);
    for (unsigned i = 0; i < host->presented_text_stored; ++i) {
        printf(",\"screen.text.%u\":", i);
        quoted(host->presented_texts[i], strlen(host->presented_texts[i]));
    }
    for (unsigned i = 0; i < host->mark_count; ++i) { putchar(','); print_mark(&host->marks[i]); }
    fputs("}}\n", stdout); fflush(stdout);
    host->op_count = 0; host->marks_dirty = false;
}

static ryz_app_pointer_phase_t parse_phase(const char *text)
{
    if (!strcmp(text, "down")) return RYZ_APP_POINTER_DOWN;
    if (!strcmp(text, "move")) return RYZ_APP_POINTER_MOVE;
    if (!strcmp(text, "up")) return RYZ_APP_POINTER_UP;
    return RYZ_APP_POINTER_NONE;
}

static bool make_pointer(const char *phase_text, bool has_position, unsigned x, unsigned y,
                         uint32_t sampled_ms, ryz_app_pointer_t *pointer)
{
    ryz_app_pointer_phase_t phase = parse_phase(phase_text);
    if (phase == RYZ_APP_POINTER_NONE ||
        (phase == RYZ_APP_POINTER_UP ? has_position : !has_position) ||
        (has_position && (x >= RYZ_APP_WIDTH || y >= RYZ_APP_HEIGHT))) return false;
    *pointer = (ryz_app_pointer_t){
        .phase = phase, .pressed = phase != RYZ_APP_POINTER_UP,
        .has_position = has_position, .x = (uint16_t)x, .y = (uint16_t)y,
        .sampled_ms = sampled_ms,
    };
    return true;
}

static bool queue_pointer(host_t *host, ryz_app_pointer_t pointer)
{
    bool next_pressed = pointer.phase != RYZ_APP_POINTER_UP;
    if ((pointer.phase == RYZ_APP_POINTER_DOWN && host->desired_pressed) ||
        (pointer.phase == RYZ_APP_POINTER_UP && !host->desired_pressed) ||
        host->queue_count >= HOST_POINTERS) return false;
    unsigned tail = (host->queue_head + host->queue_count) % HOST_POINTERS;
    host->pointer_queue[tail] = pointer;
    host->queue_count++;
    host->desired_pressed = next_pressed;
    return true;
}

static void queue_due_pointers(host_t *host)
{
    while (host->next_pointer < host->pointer_count && host->pointers[host->next_pointer].at <= host->clock) {
        if (!queue_pointer(host, host->pointers[host->next_pointer].pointer)) {
            snprintf(host->input_error, sizeof(host->input_error), "pointer queue or phase invalid");
            host->input_failed = true; host->stopped = true; return;
        }
        host->next_pointer++;
    }
}

static int host_pointer_read(void *context, ryz_app_pointer_t *pointer)
{
    host_t *host = context;
    if (!host->live) queue_due_pointers(host);
    if (host->queue_count) {
        *pointer = host->pointer_queue[host->queue_head];
        host->queue_head = (host->queue_head + 1) % HOST_POINTERS;
        host->queue_count--;
        /* ESP touch-up events carry no valid coordinate. Retain the previous
         * point only for frame rendering while the Lua sample exposes nil. */
        if (!pointer->has_position) { pointer->x = host->current.x; pointer->y = host->current.y; }
        host->current = *pointer;
        host->current.sampled_ms = (uint32_t)host_now(host);
        return 0;
    }
    *pointer = host->current;
    pointer->phase = RYZ_APP_POINTER_NONE;
    pointer->sampled_ms = (uint32_t)host_now(host);
    return 0;
}

static int host_display_draw(void *context, const ryz_app_display_op_t *op)
{
    host_t *host = context;
    if (host->op_count >= HOST_OPS) return -1;
    host->ops[host->op_count++] = *op;
    return 0;
}

static int host_display_show(void *context)
{
    host_t *host = context;
    for (unsigned i = 0; i < host->op_count; ++i) {
        const ryz_app_display_op_t *op = &host->ops[i];
        if (op->kind == RYZ_APP_DISPLAY_CLEAR) {
            host->presented_text_count = 0;
            host->presented_text_stored = 0;
            host->presented_rect_count = 0;
        } else if (op->kind == RYZ_APP_DISPLAY_RECT) {
            if (host->presented_rect_count < 2147483647u) host->presented_rect_count++;
        }
        else if (op->kind == RYZ_APP_DISPLAY_TEXT) {
            if (host->presented_text_stored < HOST_SCREEN_TEXTS) {
                snprintf(host->presented_texts[host->presented_text_stored],
                         sizeof(host->presented_texts[host->presented_text_stored]), "%s", op->text);
                host->presented_text_stored++;
            }
            if (host->presented_text_count < 2147483647u) host->presented_text_count++;
        }
    }
    host->presented++;
    snapshot(host);
    return 0;
}

static int host_ui_draw(host_t *host, const ryz_app_display_op_t *operation)
{
    return host_display_draw(host, operation);
}

static int host_ui_text(host_t *host, const ryz_ui_object_t *object)
{
    size_t length = strlen(object->text);
    /* This semantic Host uses coarse bitmap text, not production pixels.
     * Small Lua fonts must still fit their declared controls; pixel evidence
     * comes from the same-source LVGL runner in host/pixels. */
    int scale = object->font == RYZ_UI_FONT_TITLE_24 ? 3 :
        object->font == RYZ_UI_FONT_BODY_16 || object->font == RYZ_UI_FONT_DISPLAY_20 ? 2 : 1;
    int text_width = ((int)length * 6 - 1) * scale;
    int text_height = 7 * scale;
    if (text_width > object->width || text_height > object->height) {
        scale = 1;
        text_height = 7;
    }
    if (text_height > object->height) return 0;
    size_t fitting_length = ((size_t)object->width / (size_t)scale + 1) / 6;
    if (length > fitting_length) length = fitting_length;
    if (!length) return 0;
    text_width = ((int)length * 6 - 1) * scale;
    int x = object->x;
    if (object->align == RYZ_UI_ALIGN_CENTER) {
        x += ((int)object->width - text_width) / 2;
    } else if (object->align == RYZ_UI_ALIGN_RIGHT) {
        x += (int)object->width - text_width;
    }
    int y = object->y + ((int)object->height - text_height) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > RYZ_APP_WIDTH - text_width) x = RYZ_APP_WIDTH - text_width;
    if (y > RYZ_APP_HEIGHT - text_height) y = RYZ_APP_HEIGHT - text_height;
    ryz_app_display_op_t operation = {
        .kind = RYZ_APP_DISPLAY_TEXT,
        .x = x,
        .y = y,
        .width = text_width,
        .height = text_height,
        .scale = scale,
        .color = object->foreground,
    };
    memcpy(operation.text, object->text, length);
    operation.text[length] = 0;
    return host_ui_draw(host, &operation);
}

static int host_ui_mount(void *context, const ryz_ui_scene_t *scene)
{
    host_t *host = context;
    ryz_app_display_op_t clear = {
        .kind = RYZ_APP_DISPLAY_CLEAR,
        .color = scene->background,
    };
    if (host_ui_draw(host, &clear)) return -1;
    for (size_t i = 0; i < scene->object_count; ++i) {
        const ryz_ui_object_t *object = &scene->objects[i];
        if (!object->visible) continue;
        if (object->kind != RYZ_UI_LABEL) {
            ryz_app_display_op_t outer = {
                .kind = RYZ_APP_DISPLAY_RECT,
                .x = object->x,
                .y = object->y,
                .width = object->width,
                .height = object->height,
                .color = object->border_width ? object->border : object->background,
            };
            if (host_ui_draw(host, &outer)) return -1;
            int inset = object->border_width;
            if (inset && object->width > 2 * inset && object->height > 2 * inset) {
                ryz_app_display_op_t inner = {
                    .kind = RYZ_APP_DISPLAY_RECT,
                    .x = object->x + inset,
                    .y = object->y + inset,
                    .width = object->width - 2 * inset,
                    .height = object->height - 2 * inset,
                    .color = object->background,
                };
                if (host_ui_draw(host, &inner)) return -1;
            }
        }
        if (object->kind != RYZ_UI_BOX && host_ui_text(host, object)) return -1;
    }
    return host_display_show(host);
}

static int host_ui_pump(void *context) { (void)context; return 0; }
/* This is the existing semantic preview, not the device LVGL pixel Adapter.
 * Core validation/capture/generation are shared; preview repaints its frame. */
static int host_ui_update(void *context, const ryz_ui_scene_t *scene)
{ return host_ui_mount(context, scene); }
static void host_ui_close(void *context) { (void)context; }

static int host_touch_demo(void *context, bool enabled) { (void)context; (void)enabled; return 0; }

static void host_mark(void *context, const char *name, const ryz_app_value_t *value)
{
    host_t *host = context; unsigned index = 0;
    while (index < host->mark_count && strcmp(host->marks[index].name, name)) index++;
    if (index == host->mark_count) {
        if (host->mark_count >= HOST_MARKS) return;
        strncpy(host->marks[index].name, name, sizeof(host->marks[index].name) - 1);
        host->mark_count++;
    }
    host_mark_t *mark = &host->marks[index]; mark->kind = value->kind;
    if (value->kind == RYZ_APP_VALUE_INTEGER) mark->integer = value->integer;
    else if (value->kind == RYZ_APP_VALUE_BOOLEAN) mark->boolean = value->boolean;
    else { memcpy(mark->text, value->string, value->length); mark->text[value->length] = 0; }
    host->marks_dirty = true;
}

static bool host_cancelled(void *context)
{
    host_t *host = context;
    return !host->live && (double)(clock() - host->cpu_started) / CLOCKS_PER_SEC > 5;
}

static bool host_finished(void *context)
{
    host_t *host = context;
    return host->live ? host->stopped || host->input_eof || host_now(host) > 600000 : host->clock >= host->duration;
}

static void fail_input(host_t *host, const char *error)
{
    snprintf(host->input_error, sizeof(host->input_error), "%s", error);
    host->input_failed = true; host->stopped = true;
}

static void live_line(host_t *host, char *line)
{
    char phase_text[8], extra; unsigned x = 0, y = 0;
    int positioned = sscanf(line, "pointer %7s %u %u %c", phase_text, &x, &y, &extra);
    int unpositioned = sscanf(line, "pointer %7s %c", phase_text, &extra);
    if (positioned == 3 || unpositioned == 1) {
        ryz_app_pointer_t pointer;
        if (!make_pointer(phase_text, positioned == 3, x, y, (uint32_t)host_now(host), &pointer)) {
            fail_input(host, "invalid pointer command"); return;
        }
        if (!queue_pointer(host, pointer)) fail_input(host, "pointer queue or phase invalid");
    } else if (!strcmp(line, "stop")) host->stopped = true;
    else fail_input(host, "invalid live app-host command");
}

static void live_input(host_t *host)
{
    char bytes[128]; ssize_t length;
    while ((length = read(STDIN_FILENO, bytes, sizeof(bytes))) > 0) {
        for (ssize_t i = 0; i < length; ++i) {
            if (bytes[i] == '\n') {
                host->input[host->input_length] = 0;
                if (host->input_length && host->input[host->input_length - 1] == '\r') host->input[--host->input_length] = 0;
                if (host->input_length) live_line(host, host->input);
                host->input_length = 0;
            } else if (host->input_length + 1 < sizeof(host->input)) host->input[host->input_length++] = bytes[i];
            else { fail_input(host, "live app-host command too long"); return; }
        }
    }
    if (length == 0) host->input_eof = true;
    else if (length < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) fail_input(host, "live app-host input failed");
}

static void host_pause(void *context, unsigned ms)
{
    host_t *host = context;
    if (host->live) {
        live_input(host);
        if (!host->stopped && !host->input_eof) {
            struct timespec delay = {.tv_sec = 0, .tv_nsec = (long)(ms ? ms : 1) * 1000000L};
            nanosleep(&delay, NULL);
        }
        return;
    }
    host->clock += ms ? ms : 1;
    queue_due_pointers(host);
    while (host->next_check < host->check_count && host->checks[host->next_check] <= host->clock) {
        snapshot(host); host->next_check++;
    }
}

static void host_output(void *context, const char *text, size_t length)
{
    (void)context;
    fwrite(text, 1, length, stderr);
}

/* This Host has no hardware tool model. Keep attempts visible even if Lua
 * handles the structured unavailable result and finishes successfully. */
static ryz_tool_call_result_t host_tool_call(void *context,
    const ryz_tool_request_t *request, ryz_tool_reply_t *out)
{
    host_t *host = context;
    (void)request;
    host->tool_attempted = true;
    memset(out, 0, sizeof(*out));
    return RYZ_TOOL_CALL_UNAVAILABLE;
}

static ryz_lua_hardware_result_t host_hardware_call(void *context,
    ryz_lua_hardware_op_t op, ryz_lua_hardware_value_t *out)
{
    host_t *host = context;
    (void)op;
    host->tool_attempted = true;
    memset(out, 0, sizeof(*out));
    return RYZ_LUA_HW_UNAVAILABLE;
}

static ryz_peripheral_result_t host_peripheral_call(void *context,
    const ryz_peripheral_request_t *request,ryz_peripheral_reply_t *out)
{
    host_t *host=context;
    (void)request;
    host->tool_attempted=true;
    memset(out,0,sizeof(*out));
    return RYZ_PERIPHERAL_UNAVAILABLE;
}

static bool parse_u32(const char *text, uint32_t *value)
{
    if (!text || !*text || *text == '-') return false;
    char *end; errno = 0; unsigned long long parsed = strtoull(text, &end, 10);
    if (errno || *end || parsed > UINT32_MAX) return false;
    *value = (uint32_t)parsed; return true;
}

static int usage(void)
{
    fputs("usage: app-host [--live] [--seed N] SOURCE\n", stderr);
    return 2;
}

int main(int argc, char **argv)
{
    host_t host = {.seed = 1, .cpu_started = clock(), .live_started = monotonic_ms(), .running = true};
    const char *source_path = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--live")) {
            if (host.live) return usage(); host.live = true;
        } else if (!strcmp(argv[i], "--seed")) {
            if (++i >= argc || !parse_u32(argv[i], &host.seed)) return usage();
        } else if (argv[i][0] == '-' || source_path) return usage();
        else source_path = argv[i];
    }
    if (!source_path) return usage();
    FILE *file = fopen(source_path, "rb"); if (!file) return 2;
    char source[RYZ_LUA_SOURCE_MAX + 1]; size_t length = fread(source, 1, sizeof(source), file); fclose(file);
    if (!length || length > RYZ_LUA_SOURCE_MAX) return 2;

    if (host.live) {
        int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) return 2;
    } else {
        char line[128], command[16], phase_text[8], extra; unsigned long long at; unsigned x = 0, y = 0;
        while (fgets(line, sizeof(line), stdin)) {
            if (sscanf(line, "%15s", command) != 1) continue;
            if (!strcmp(command, "end") && sscanf(line, "end %llu %c", &at, &extra) == 1 && at > 0 && at <= 600000) host.duration = at;
            else if (!strcmp(command, "check") && sscanf(line, "check %llu %c", &at, &extra) == 1 && at <= 600000 && host.check_count < HOST_CHECKS && (!host.check_count || at > host.checks[host.check_count - 1])) host.checks[host.check_count++] = at;
            else if (!strcmp(command, "pointer")) {
                int positioned = sscanf(line, "pointer %llu %7s %u %u %c", &at, phase_text, &x, &y, &extra);
                int unpositioned = sscanf(line, "pointer %llu %7s %c", &at, phase_text, &extra);
                ryz_app_pointer_t pointer;
                if ((positioned != 4 && unpositioned != 2) || at > 600000 ||
                    host.pointer_count >= HOST_POINTERS ||
                    (host.pointer_count && at < host.pointers[host.pointer_count - 1].at) ||
                    !make_pointer(phase_text, positioned == 4, x, y, (uint32_t)at, &pointer)) return 2;
                host.pointers[host.pointer_count++] = (scheduled_pointer_t){.at = at, .pointer = pointer};
            } else return 2;
        }
        if (!host.duration) return 2;
        queue_due_pointers(&host);
        if (host.input_failed) return 2;
    }

    ryz_app_platform_t platform = {
        .context = &host, .resize = host_resize, .now_ms = host_now, .pause_ms = host_pause,
        .cancelled = host_cancelled, .finished = host_finished, .seed = host.seed,
        .pointer_read = host_pointer_read, .display_draw = host_display_draw,
        .display_show = host_display_show, .touch_demo = host_touch_demo,
        .ui_mount = host_ui_mount, .ui_update = host_ui_update, .ui_pump = host_ui_pump,
        .ui_close = host_ui_close,
        .mark = host_mark, .output = host_output, .tool_call = host_tool_call,
        .hardware_call = host_hardware_call,
        .peripheral_call = host_peripheral_call,
    };
    ryz_lua_result_t result;
    ryz_app_execute(source, length, "@app.lua", 0, &platform, &result);
    if (host.input_failed) {
        result.ok = false; result.phase = "input";
        snprintf(result.error, sizeof(result.error), "%s", host.input_error);
    }
    if (host.tool_attempted) {
        result.ok = false; result.phase = "unsupported";
        snprintf(result.error, sizeof(result.error),
                 "Hardware tools are unavailable in this Host; no hardware simulation or passed receipt is possible.");
    }
    host.running = false;
    if (host.presented) snapshot(&host);
    printf("{\"summary\":true,\"ok\":%s,\"runtime\":\"%s\",\"catalog\":\"%s\",\"seed\":%u,\"peakBytes\":%u,\"phase\":",
           result.ok ? "true" : "false", RYZ_APP_ABI, RYZ_APP_CATALOG, host.seed, (unsigned)result.peak_bytes);
    quoted(result.phase, strlen(result.phase)); fputs(",\"error\":", stdout); quoted(result.error, strlen(result.error));
    printf(",\"resources\":\"app: frames=%llu presented=%u marks=%u core=%s\"}\n",
           (unsigned long long)host.sequence, host.presented, host.mark_count, RYZ_APP_CORE_SHA);
    fflush(stdout);
    return result.ok ? 0 : 1;
}
