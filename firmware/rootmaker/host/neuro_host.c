#define _POSIX_C_SOURCE 200809L
#include "nervous_runtime.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LIVE_TOUCH_QUEUE 128
#define LIVE_TOUCH_DWELL_MS 32

typedef struct { uint64_t at; int pressed; } touch_t;
typedef struct { char id[25], state[25]; int32_t value; } observed_t;
typedef struct {
    uint64_t clock, duration; bool pressed; uint16_t color;
    touch_t touches[128]; unsigned touch_count, next_touch;
    uint64_t checks[128]; unsigned check_count, next_check;
    observed_t nodes[RYZ_NEURO_NODES]; unsigned node_count;
    clock_t started;
    bool live, stopped, input_eof, dirty, painted;
    uint64_t live_started, sequence;
    bool live_touches[LIVE_TOUCH_QUEUE], desired_pressed, input_failed;
    unsigned live_touch_head, live_touch_count;
    uint64_t live_touch_ready_at;
    char input_error[80];
    char input[256]; size_t input_length;
} host_t;
static void *resize(void *ptr, size_t size) { if (!size) { free(ptr); return NULL; } return realloc(ptr, size); }
static uint64_t monotonic_ms(void)
{
    struct timespec value; clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000 + (uint64_t)value.tv_nsec / 1000000;
}
static uint64_t now(void *ctx) { host_t *h = ctx; return h->live ? monotonic_ms() - h->live_started : h->clock; }
static int sample(void *ctx) { return ((host_t *)ctx)->pressed; }
static int paint(void *ctx, uint16_t color, bool begin)
{
    (void)begin; host_t *h = ctx;
    if (!h->painted || h->color != color) { h->color = color; h->dirty = true; }
    h->painted = true;
    return 1;
}
static bool cancelled(void *ctx)
{
    host_t *h = ctx;
    return !h->live && (double)(clock() - h->started) / CLOCKS_PER_SEC > 5;
}
static bool finished(void *ctx)
{
    host_t *h = ctx;
    return h->live ? h->stopped || h->input_eof || now(h) > 600000 : h->clock > h->duration;
}
static void observe(void *ctx, const char *id, const char *state, int32_t value)
{
    host_t *h = ctx; unsigned i = 0;
    while (i < h->node_count && strcmp(h->nodes[i].id, id)) i++;
    if (i == h->node_count && h->node_count < RYZ_NEURO_NODES) { h->node_count++; strcpy(h->nodes[i].id, id); h->dirty = true; }
    if (i >= RYZ_NEURO_NODES) return;
    if (strcmp(h->nodes[i].state, state) || h->nodes[i].value != value) h->dirty = true;
    strcpy(h->nodes[i].state, state); h->nodes[i].value = value;
}
static void snapshot(host_t *h)
{
    if (h->live) printf("{\"event\":\"frame\",\"runtime\":\"%s\",\"catalog\":\"%s\",\"seq\":%llu,\"at\":%llu,\"color\":%u,\"pressed\":%s,\"nodes\":{", RYZ_NEURO_ABI, RYZ_NEURO_CATALOG, (unsigned long long)++h->sequence, (unsigned long long)now(h), h->color, h->pressed ? "true" : "false");
    else printf("{\"at\":%llu,\"color\":%u,\"nodes\":{", (unsigned long long)h->clock, h->color);
    for (unsigned i = 0; i < h->node_count; ++i) {
        observed_t *n = &h->nodes[i]; if (i) printf(",");
        printf("\"%s\":", n->id); if (*n->state) printf("\"%s\"", n->state); else printf("%ld", (long)n->value);
    }
    printf("}}\n"); fflush(stdout); h->dirty = false;
}
static void live_line(host_t *h, char *line)
{
    int pressed; char extra;
    if (sscanf(line, "touch %d %c", &pressed, &extra) == 1 && (pressed == 0 || pressed == 1)) {
        bool next = (bool)pressed;
        if (h->desired_pressed == next) return;
        if (h->live_touch_count >= LIVE_TOUCH_QUEUE) {
            snprintf(h->input_error, sizeof(h->input_error), "live touch queue full");
            h->input_failed = true; h->stopped = true; return;
        }
        unsigned tail = (h->live_touch_head + h->live_touch_count) % LIVE_TOUCH_QUEUE;
        h->live_touches[tail] = next; h->live_touch_count++; h->desired_pressed = next;
    } else if (!strcmp(line, "stop")) { h->live_touch_count = 0; h->stopped = true; }
    else {
        snprintf(h->input_error, sizeof(h->input_error), "invalid live simulator command");
        h->input_failed = true; h->stopped = true;
    }
}
static void live_apply_touch(host_t *h)
{
    if (!h->live_touch_count) return;
    uint64_t current = now(h);
    if (current < h->live_touch_ready_at) return;
    bool pressed = h->live_touches[h->live_touch_head];
    h->live_touch_head = (h->live_touch_head + 1) % LIVE_TOUCH_QUEUE; h->live_touch_count--;
    if (h->pressed != pressed) { h->pressed = pressed; h->dirty = true; }
    /* The generated touch receptor samples every 20 ms. Keep each accepted
     * level visible for a full sampling interval even when several commands
     * arrive in one pipe read. This preserves every press edge. */
    h->live_touch_ready_at = current + LIVE_TOUCH_DWELL_MS;
}
static void live_input(host_t *h)
{
    char bytes[128]; ssize_t length;
    while ((length = read(STDIN_FILENO, bytes, sizeof(bytes))) > 0) {
        for (ssize_t i = 0; i < length; ++i) {
            if (bytes[i] == '\n') {
                h->input[h->input_length] = 0;
                if (h->input_length && h->input[h->input_length - 1] == '\r') h->input[--h->input_length] = 0;
                live_line(h, h->input); h->input_length = 0;
            } else if (h->input_length + 1 < sizeof(h->input)) h->input[h->input_length++] = bytes[i];
            else {
                snprintf(h->input_error, sizeof(h->input_error), "live simulator command too long");
                h->input_failed = true; h->stopped = true; return;
            }
        }
    }
    if (length == 0) h->input_eof = true;
    else if (length < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        snprintf(h->input_error, sizeof(h->input_error), "live simulator input failed");
        h->input_failed = true; h->stopped = true;
    }
}
static void pause_ms(void *ctx, unsigned ms)
{
    host_t *h = ctx;
    if (h->live) {
        /* Flush changes from the preceding scheduler pass before accepting the
         * next input. A press is therefore observed by the shared runtime
         * before its resulting frame is published. */
        if (h->dirty && h->painted) snapshot(h);
        live_input(h);
        live_apply_touch(h);
        if (!h->stopped && !h->input_eof) {
            struct timespec delay = { .tv_sec = 0, .tv_nsec = (long)(ms ? ms : 1) * 1000000L };
            nanosleep(&delay, NULL);
        }
        return;
    }
    while (h->next_check < h->check_count && h->checks[h->next_check] <= h->clock) { snapshot(h); h->next_check++; }
    h->clock += ms;
    while (h->next_touch < h->touch_count && h->touches[h->next_touch].at <= h->clock) h->pressed = h->touches[h->next_touch++].pressed;
}
static void quoted(const char *s)
{
    putchar('"'); for (; *s; s++) { unsigned char c = *s; if (c == '"' || c == '\\') { putchar('\\'); putchar(c); } else if (c < 32) printf("\\u%04x", c); else putchar(c); } putchar('"');
}
int main(int argc, char **argv)
{
    bool live = argc == 3 && !strcmp(argv[1], "--live");
    if ((!live && argc != 2) || (live && argc != 3)) return 2;
    FILE *f = fopen(argv[live ? 2 : 1], "rb"); if (!f) return 2;
    char source[RYZ_LUA_SOURCE_MAX + 1]; size_t length = fread(source, 1, sizeof(source), f); fclose(f);
    if (length > RYZ_LUA_SOURCE_MAX) return 2;
    host_t h = { .started = clock(), .live = live, .live_started = monotonic_ms() };
    char line[100], command[20]; unsigned long long at; int pressed;
    if (live) {
        int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) return 2;
    } else {
        while (fgets(line, sizeof(line), stdin)) {
            if (sscanf(line, "%19s %llu %d", command, &at, &pressed) < 2 || at > 600000) return 2;
            if (!strcmp(command, "end")) h.duration = at;
            else if (!strcmp(command, "touch") && h.touch_count < 128 && sscanf(line, "%19s %llu %d", command, &at, &pressed) == 3 && (pressed == 0 || pressed == 1)) {
                if (h.touch_count && at < h.touches[h.touch_count - 1].at) return 2;
                h.touches[h.touch_count++] = (touch_t){at, pressed};
            } else if (!strcmp(command, "check") && h.check_count < 128) {
                if (h.check_count && at <= h.checks[h.check_count - 1]) return 2;
                h.checks[h.check_count++] = at;
            } else return 2;
        }
        if (!h.duration) return 2;
        while (h.next_touch < h.touch_count && h.touches[h.next_touch].at == 0) h.pressed = h.touches[h.next_touch++].pressed;
    }
    ryz_neuro_platform_t p = { .context = &h, .resize = resize, .now_ms = now, .pause_ms = pause_ms, .cancelled = cancelled, .finished = finished, .sample = sample, .paint = paint, .observe = observe };
    ryz_lua_result_t result; ryz_neuro_execute(source, length, "@main.lua", 0, &p, &result);
    if (live && h.input_failed) {
        result.ok = false; result.phase = "input";
        snprintf(result.error, sizeof(result.error), "%s", h.input_error);
    }
    if (live && h.dirty && h.painted) snapshot(&h);
    printf("{\"summary\":true,\"ok\":%s,\"runtime\":\"%s\",\"catalog\":\"%s\",\"peakBytes\":%u,\"phase\":", result.ok ? "true" : "false", RYZ_NEURO_ABI, RYZ_NEURO_CATALOG, (unsigned)result.peak_bytes);
    quoted(result.phase); printf(",\"error\":"); quoted(result.error); printf(",\"resources\":"); quoted(result.output); printf("}\n"); fflush(stdout);
    return result.ok ? 0 : 1;
}
