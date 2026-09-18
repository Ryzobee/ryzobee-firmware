/* Deterministic timing at the direct app-platform seam. The separate owner
 * suite runs both device entrypoints; neither test is physical GPIO evidence. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "app_runtime.h"

static unsigned time_calls, callback_calls, tests;
static uint64_t tick;
static bool expired_before, expire_after, cancel_before, cancel_after;
static char trusted_context;
static void *resize(void *ptr, size_t size)
{ if (!size) { free(ptr); return NULL; } return realloc(ptr, size); }
static uint64_t now(void *context)
{
    assert(context == &trusted_context);
    if (expired_before && time_calls++) return 100;
    return tick;
}
static void pause_ms(void *context, unsigned ms)
{ assert(context == &trusted_context); tick += ms ? ms : 1; }
static bool cancelled(void *context)
{ assert(context == &trusted_context); return cancel_before; }
static ryz_boot_result_t poll(void *context, ryz_boot_event_t *event)
{
    assert(context == &trusted_context); ++callback_calls;
    if (expire_after) tick = 100;
    if (cancel_after) cancel_before = true;
    *event = (ryz_boot_event_t){RYZ_BOOT_LONG_PRESS, UINT32_MAX, 3000};
    return RYZ_BOOT_OK;
}
static void run(const char *body, const char *phase, unsigned calls)
{
    char source[512]; snprintf(source, sizeof(source), "-- ryz-app/1\n%s", body);
    ryz_app_platform_t platform = {.context = &trusted_context, .resize = resize,
        .now_ms = now, .pause_ms = pause_ms, .cancelled = cancelled, .boot_poll = poll};
    ryz_lua_result_t result;
    ryz_app_execute(source, strlen(source), "@boot_test.lua", 50, &platform, &result);
    if (strcmp(result.phase, phase)) fprintf(stderr, "%s: %s\n", result.phase, result.error);
    assert(!strcmp(result.phase, phase) && callback_calls == calls);
    assert(!strstr(result.error, "ESCAPED")); ++tests;
    time_calls = callback_calls = 0; tick = 0;
    expired_before = expire_after = cancel_before = cancel_after = false;
}
int main(void)
{
    run("local e=assert(require('boot').poll()); assert(e.type=='long_press' "
        "and e.held_ms==3000 and e.timestamp_ms=='4294967295')", "done", 1);
    expired_before = true;
    run("require('boot').poll(); error('ESCAPED')", "timeout", 0);
    cancel_before = true;
    run("require('boot').poll(); error('ESCAPED')", "stopped", 0);
    expire_after = true;
    run("require('boot').poll(); error('ESCAPED')", "timeout", 1);
    cancel_after = true;
    run("require('boot').poll(); error('ESCAPED')", "stopped", 1);
    expire_after = true;
    run("coroutine.resume(coroutine.create(function() require('boot').poll() end)); "
        "error('ESCAPED')", "timeout", 1);
    cancel_after = true;
    run("coroutine.resume(coroutine.create(function() require('boot').poll() end)); "
        "error('ESCAPED')", "stopped", 1);
    printf("LUA_BOOT_PASS: %u real-facade direct callback and deterministic before/after guards\n", tests);
    return 0;
}
