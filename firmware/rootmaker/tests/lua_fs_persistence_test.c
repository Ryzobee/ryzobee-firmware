/* Actual checked-in example -> frozen Lua -> application facade -> real
 * storage core. Only its platform seam uses temporary POSIX files. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_runtime.h"
#include "app_fs_host.h"

static uint64_t tick;
static void *resize(void *pointer, size_t size)
{
    if (!size) { free(pointer); return NULL; }
    return realloc(pointer, size);
}
static uint64_t now(void *context) { (void)context; return tick; }
static void pause_ms(void *context, unsigned milliseconds)
{ (void)context; tick += milliseconds ? milliseconds : 1; }

static int execute(const char *source, size_t length, const char *name)
{
    ryz_app_platform_t platform = {.resize = resize, .now_ms = now,
        .pause_ms = pause_ms, .fs_call = ryz_app_fs_call};
    ryz_lua_result_t result;
    ryz_app_execute(source, length, name, 10000, &platform, &result);
    assert(result.peak_bytes <= RYZ_LUA_HEAP_LIMIT);
    printf("PHASE %s\n%s", result.phase, result.output);
    if (!result.ok) fprintf(stderr, "%s\n", result.error);
    return result.ok ? 0 : 3;
}

int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    app_fs_host_configure(argv[1]);
    if (!strcmp(argv[2], "demo")) {
        FILE *file = fopen(argv[3], "rb");
        assert(file);
        char *source = malloc(RYZ_LUA_SOURCE_MAX + 1U);
        assert(source);
        size_t size = fread(source, 1, RYZ_LUA_SOURCE_MAX + 1U, file);
        assert(!ferror(file) && size <= RYZ_LUA_SOURCE_MAX);
        assert(fclose(file) == 0);
        int status = execute(source, size, "@fs_demo.lua");
        free(source);
        return status;
    }
    const char *source = NULL;
    const char *name = "@fs_demo.lua";
    if (!strcmp(argv[2], "foreign")) {
        name = "@different_app.lua";
        source = "-- ryz-app/1\nlocal f=require('fs'); local s,e=f.read('counter.txt'); "
            "assert(s==nil and e=='not_found'); assert(#f.list()==0); "
            "local i=assert(f.info()); assert(i.used_bytes==0 and i.file_count==0); "
            "print('ISOLATION_PASS')";
    } else if (!strcmp(argv[2], "invalid_counter")) {
        source = "-- ryz-app/1\nassert(require('fs').write('counter.txt','BROKEN_COUNTER')); "
            "print('INVALID_COUNTER_SAVED')";
    } else if (!strcmp(argv[2], "check_invalid")) {
        source = "-- ryz-app/1\nassert(require('fs').read('counter.txt')=='BROKEN_COUNTER'); "
            "print('INVALID_COUNTER_PRESERVED')";
    } else if (!strcmp(argv[2], "corrupt_record")) {
        /* Counter generation 2 occupies data slot 1. The host helper resolves
         * its hashed physical key; the Lua script never sees the disk path. */
        app_fs_host_corrupt("fs_demo.lua", "counter.txt", '1', 96);
        puts("COUNTER_RECORD_CORRUPTED");
        return 0;
    } else return 2;
    return execute(source, strlen(source), name);
}
