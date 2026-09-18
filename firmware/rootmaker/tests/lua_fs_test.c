/* The real bounded Lua facade, with an explicit deterministic storage peer.
 * Backend durability and actual SPIFFS behaviour are separate tests. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_runtime.h"

typedef struct {
    bool used;
    char name[RYZ_FS_NAME_MAX + 1U];
    size_t size;
    unsigned char data[RYZ_FS_MAX_FILE_BYTES];
} file_t;
typedef struct { char id[RYZ_FS_APP_ID_MAX + 1U]; file_t files[RYZ_FS_MAX_FILES]; } store_t;
static store_t stores[4];
static uint64_t tick;
static unsigned calls, tests;
static bool no_backend, cancel_now, cancel_after_call, timeout_after_call;
static bool fail_allocations, fail_read_allocation, fail_read_result;
static bool fail_scratch_after_info;
static int malformed;
static ryz_fs_result_t injected;
static size_t live_bytes, live_blocks;
typedef union { max_align_t alignment; struct { size_t size; } value; } allocation_t;

static void *resize(void *pointer, size_t size)
{
    allocation_t *old = pointer ? (allocation_t *)pointer - 1 : NULL;
    size_t old_size = old ? old->value.size : 0;
    if (!size) {
        if (old) { live_bytes -= old_size; --live_blocks; free(old); }
        return NULL;
    }
    if (fail_allocations || (fail_read_allocation && size >= 8192 && size < 9000)) return NULL;
    allocation_t *next = realloc(old, sizeof(*next) + size);
    if (!next) return NULL;
    next->value.size = size;
    live_bytes = live_bytes - old_size + size;
    if (!old) ++live_blocks;
    return next + 1;
}

static uint64_t now(void *context) { (void)context; return tick; }
static void pause_ms(void *context, unsigned milliseconds)
{ (void)context; tick += milliseconds ? milliseconds : 1; }
static bool cancelled(void *context) { (void)context; return cancel_now; }

static store_t *find_store(const char *id)
{
    for (unsigned i = 0; i < 4; ++i) {
        if (!stores[i].id[0]) { strcpy(stores[i].id, id); return &stores[i]; }
        if (!strcmp(stores[i].id, id)) return &stores[i];
    }
    assert(!"too many test namespaces");
    return NULL;
}

static int compare_entries(const void *a, const void *b)
{ return strcmp(((const ryz_fs_entry_t *)a)->name, ((const ryz_fs_entry_t *)b)->name); }

static ryz_fs_result_t peer(void *context, const char *app_id,
                           const ryz_fs_request_t *request, ryz_fs_response_t *response)
{
    assert(context == stores);
    ++calls;
    assert(strstr(app_id, ".lua") && !strchr(app_id, '/') && !strchr(app_id, '@'));
    if (cancel_after_call) cancel_now = true;
    if (timeout_after_call) tick += 100;
    if (injected != RYZ_FS_OK) {
        memset(response, 0xff, sizeof(*response)); /* No failed payload is usable. */
        return injected;
    }
    if (malformed) {
        if (request->op == RYZ_FS_READ) response->size = request->read_capacity + 1;
        else if (request->op == RYZ_FS_INFO) response->info.used_bytes = (size_t)-1;
        else if (malformed == 1) response->count = request->entries_capacity + 1;
        else {
            response->count = 2;
            strcpy(request->entries[0].name, "b");
            strcpy(request->entries[1].name, "a");
            if (malformed == 3) strcpy(request->entries[1].name, "b");
            if (malformed == 4) memset(request->entries[0].name, 'x', sizeof(request->entries[0].name));
            if (malformed == 5) strcpy(request->entries[0].name, "../secret");
            if (malformed == 6) request->entries[0].size = RYZ_FS_MAX_FILE_BYTES + 1;
        }
        return RYZ_FS_OK;
    }
    store_t *store = find_store(app_id);
    file_t *file = NULL, *free_slot = NULL;
    size_t used = 0, count = 0;
    for (unsigned i = 0; i < RYZ_FS_MAX_FILES; ++i) {
        file_t *candidate = &store->files[i];
        if (!candidate->used) { if (!free_slot) free_slot = candidate; continue; }
        used += candidate->size;
        ++count;
        if (request->name && !strcmp(candidate->name, request->name)) file = candidate;
    }
    if (request->op == RYZ_FS_INFO) {
        response->info = (ryz_fs_info_t){used, count, RYZ_FS_MAX_FILE_BYTES,
            RYZ_FS_QUOTA_BYTES, RYZ_FS_MAX_FILES};
        if (fail_scratch_after_info) fail_read_allocation = true;
    } else if (request->op == RYZ_FS_LIST) {
        assert(request->entries_capacity == RYZ_FS_MAX_FILES);
        for (unsigned i = 0; i < RYZ_FS_MAX_FILES; ++i) {
            if (!store->files[i].used) continue;
            ryz_fs_entry_t *entry = &request->entries[response->count++];
            strcpy(entry->name, store->files[i].name);
            entry->size = store->files[i].size;
        }
        qsort(request->entries, response->count, sizeof(*request->entries), compare_entries);
    } else if (request->op == RYZ_FS_WRITE) {
        assert(request->size <= RYZ_FS_MAX_FILE_BYTES);
        if (!file && !free_slot) return RYZ_FS_TOO_MANY_FILES;
        if (used - (file ? file->size : 0) + request->size > RYZ_FS_QUOTA_BYTES) return RYZ_FS_QUOTA;
        if (!file) file = free_slot;
        file->used = true;
        strcpy(file->name, request->name);
        file->size = request->size;
        memcpy(file->data, request->data, request->size);
    } else {
        if (!file) return RYZ_FS_NOT_FOUND;
        if (request->op == RYZ_FS_REMOVE) memset(file, 0, sizeof(*file));
        else {
            assert(request->op == RYZ_FS_READ && request->read_capacity >= file->size);
            memcpy(request->read_buffer, file->data, file->size);
            response->size = file->size;
            if (fail_read_result) fail_allocations = true;
        }
    }
    return RYZ_FS_OK;
}

static void reset(void)
{
    assert(!live_bytes && !live_blocks);
    memset(stores, 0, sizeof(stores));
    calls = 0; tick = 0;
    no_backend = cancel_now = cancel_after_call = timeout_after_call = false;
    fail_allocations = fail_read_allocation = fail_read_result = fail_scratch_after_info = false;
    malformed = 0; injected = RYZ_FS_OK;
}

static void run_as(const char *body, const char *name, const char *phase)
{
    char source[4096];
    int length = snprintf(source, sizeof(source), "-- ryz-app/1\n%s", body);
    assert(length > 0 && (size_t)length < sizeof(source));
    ryz_app_platform_t platform = {.context = stores, .resize = resize, .now_ms = now,
        .pause_ms = pause_ms, .cancelled = cancelled, .fs_call = no_backend ? NULL : peer};
    ryz_lua_result_t result;
    ryz_app_execute(source, (size_t)length, name, 100, &platform, &result);
    if (strcmp(result.phase, phase))
        fprintf(stderr, "expected %s, got %s: %s\n%s\n", phase, result.phase, result.error, body);
    assert(!strcmp(result.phase, phase));
    assert(result.ok == !strcmp(phase, "done"));
    assert(result.peak_bytes <= RYZ_LUA_HEAP_LIMIT);
    assert(!live_bytes && !live_blocks);
    ++tests;
}
static void run(const char *body, const char *phase) { run_as(body, "@alpha.lua", phase); }

static void test_roundtrip(void)
{
    reset();
    run("local f=require('fs'); assert(f==require('fs')); "
        "assert(io==nil and os==nil and package==nil and loadfile==nil and load==nil); "
        "assert(f.open==nil and f.rename==nil and f.mkdir==nil and f.namespace==nil); "
        "local i=assert(f.info()); assert(i.used_bytes==0 and i.file_count==0); "
        "assert(i.max_file_bytes==8192 and i.quota_bytes==32768 and i.max_files==16); "
        "assert(#assert(f.list())==0); local s,e=f.read('absent'); assert(s==nil and e=='not_found'); "
        "s,e=f.remove('absent'); assert(s==nil and e=='not_found'); "
        "local bytes='a'..string.char(0,255,1)..'z'; assert(f.write('z.bin',bytes)); "
        "assert(f.read('z.bin')==bytes); assert(f.write('a.bin','')); assert(f.read('a.bin')==''); "
        "local l=assert(f.list()); assert(#l==2 and l[1].name=='a.bin' and l[1].size==0 "
        "and l[2].name=='z.bin' and l[2].size==5); "
        "i=assert(f.info()); assert(i.used_bytes==5 and i.file_count==2); "
        "assert(f.write('z.bin','NEW')); assert(f.read('z.bin')=='NEW'); "
        "assert(f.remove('a.bin')); assert(f.info().file_count==1)", "done");
    /* A new VM reads committed peer state; another namespace sees none. */
    run_as("local f=require('fs'); assert(f.read('z.bin')=='NEW')", "alpha.lua", "done");
    run_as("local f=require('fs'); local s,e=f.read('z.bin'); assert(s==nil and e=='not_found'); "
        "assert(f.write('z.bin','BETA'))", "@beta.lua", "done");
    run("assert(require('fs').read('z.bin')=='NEW')", "done");
    run_as("assert(require('fs').read('z.bin')=='BETA')", "@beta.lua", "done");
    run("local f=require('fs'); local s=string.rep(string.char(0,255),4096); "
        "assert(f.write('full.bin',s)); assert(f.read('full.bin')==s); "
        "assert(f.list()[1].name=='full.bin' and f.list()[1].size==8192)", "done");
    run("local f=require('fs'); local c=coroutine.create(function() "
        "assert(f.write('co.bin','shared')); coroutine.yield(); assert(f.read('co.bin')=='main') end); "
        "assert(coroutine.resume(c)); assert(f.read('co.bin')=='shared'); "
        "assert(f.write('co.bin','main')); assert(coroutine.resume(c))", "done");
}

static void test_invalid_arguments(void)
{
    static const char *bad[] = {
        "f.read()", "f.read(1)", "f.read({})", "f.read(true)", "f.read(nil)",
        "f.read('x',1)", "f.write('x')", "f.write('x',1)", "f.write('x',{})",
        "f.write('x',nil)", "f.write('x','ok',true)", "f.remove()", "f.remove(1)",
        "f.remove('x',true)", "f.list(1)", "f.info(nil)", "f.write('x',string.rep('x',8193))",
        "f.read('')", "f.read('.')", "f.read('..')", "f.read('../x')", "f.read('/x')",
        "f.read('x/y')", "f.read('x\\\\y')", "f.read('a..b')", "f.read('.hidden')",
        "f.read('has space')", "f.read(string.char(0))", "f.read('x'..string.char(0)..'y')",
        "f.read(string.rep('a',25))", "f.read(string.char(195,169))", "require('fs\\0secret')",
        "f.read('x:stream')", "f.read('-name')", "f.write('../x','bad')", "f.remove('../x')",
    };
    reset();
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        char body[256]; snprintf(body, sizeof(body), "local f=require('fs'); %s", bad[i]);
        run(body, "runtime"); assert(!calls);
    }
    run("local f=require('fs'); assert(f.write('A-Z_09.name','')); "
        "assert(f.write(string.rep('x',24),''))", "done");
}

static void test_unavailable_and_errors(void)
{
    reset(); no_backend = true;
    run("local f=require('fs'); for _,v in ipairs({{'read','x'},{'write','x','ok'}, "
        "{'remove','x'},{'list'},{'info'}}) do local s,e=f[v[1]](table.unpack(v,2)); "
        "assert(s==nil and e=='unavailable') end", "done");
    assert(!calls);
    no_backend = false;
    static const char *names[] = {"=eval", "chunk", "@/scripts/alpha.lua", "@../alpha.lua",
        "@alpha..lua", "@.lua", "@alpha.LUA", "@", "", "@a.b.lua",
        "@aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.lua"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        run_as("local s,e=require('fs').info(); assert(s==nil and e=='unavailable')", names[i], "done");
    assert(!calls);
    run_as("assert(require('fs').info())", "@_alpha.lua", "done");
    run_as("assert(require('fs').info())", "@-alpha.lua", "done");
    run_as("assert(require('fs').info())", "@aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.lua", "done");
    static const struct { ryz_fs_result_t result; const char *name; } errors[] = {
        {RYZ_FS_UNAVAILABLE,"unavailable"}, {RYZ_FS_INVALID_NAME,"invalid_name"},
        {RYZ_FS_INVALID_APP,"invalid_app"}, {RYZ_FS_TOO_LARGE,"too_large"},
        {RYZ_FS_QUOTA,"quota"}, {RYZ_FS_TOO_MANY_FILES,"too_many_files"},
        {RYZ_FS_BUSY,"busy"}, {RYZ_FS_NOT_FOUND,"not_found"}, {RYZ_FS_CORRUPT,"corrupt"},
        {RYZ_FS_IO,"io"}, {RYZ_FS_NO_SPACE,"no_space"}, {RYZ_FS_NO_MEMORY,"no_memory"},
        {RYZ_FS_COMMIT_UNKNOWN,"commit_unknown"}, {RYZ_FS_RECOVERY_REQUIRED,"recovery_required"},
    };
    for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        char body[512];
        injected = errors[i].result;
        snprintf(body, sizeof(body), "local f=require('fs'); "
            "for _,v in ipairs({{'read','x'},{'write','x','ok'},{'remove','x'},{'list'},{'info'}}) "
            "do local s,e=f[v[1]](table.unpack(v,2)); assert(s==nil and e=='%s') end", errors[i].name);
        run(body, "done");
    }
    reset(); malformed = 1;
    run("local f=require('fs'); local s,e=f.read('x'); assert(s==nil and e=='io'); "
        "s,e=f.info(); assert(s==nil and e=='io')", "done");
    for (int i = 1; i <= 6; ++i) {
        malformed = i;
        run("local s,e=require('fs').list(); assert(s==nil and e=='io')", "done");
    }
}

static void test_limits_and_termination(void)
{
    reset();
    run("local f=require('fs'); local s=string.rep('x',8192); "
        "for i=1,4 do assert(f.write('full'..i,s)) end; "
        "local ok,e=f.write('extra','x'); assert(ok==nil and e=='quota'); "
        "assert(f.write('full1',string.rep('y',8192))); "
        "assert(f.write('empty','')); assert(f.info().used_bytes==32768)", "done");
    reset();
    run("local f=require('fs'); for i=1,16 do assert(f.write('f'..i,'')) end; "
        "local ok,e=f.write('extra',''); assert(ok==nil and e=='too_many_files'); "
        "assert(f.remove('f8')); assert(f.write('extra','')); assert(#f.list()==16)", "done");
    reset(); cancel_now = true;
    run("require('fs').write('x','no')", "stopped"); assert(!calls);
    reset(); cancel_after_call = true;
    run("require('fs').write('x','yes'); error('escaped cancellation')", "stopped"); assert(calls == 1);
    reset(); cancel_after_call = true;
    run("local c=coroutine.create(function() require('fs').write('x','yes') end); "
        "coroutine.resume(c); error('swallowed cancellation')", "stopped"); assert(calls == 1);
    reset(); timeout_after_call = true;
    run("require('fs').info(); error('escaped deadline')", "timeout"); assert(calls == 1);
    reset(); timeout_after_call = true;
    run("local c=coroutine.create(function() require('fs').info() end); "
        "coroutine.resume(c); error('swallowed deadline')", "timeout"); assert(calls == 1);
    reset();
    run("assert(require('fs').write('large',string.rep('X',8192)))", "done");
    calls = 0; fail_read_result = true;
    run("require('fs').read('large')", "memory"); assert(calls == 1);
    fail_read_result = fail_allocations = false;
    calls = 0; fail_scratch_after_info = true;
    run("local f=require('fs'); f.info(); f.read('large')", "memory"); assert(calls == 1);
    fail_read_allocation = fail_scratch_after_info = false;
    calls = 0;
    run("local f=require('fs'); local keep={}; while true do keep[#keep+1]=f.read('large')..#keep end", "memory");
    assert(calls > 1);
    /* Read payload and scratch memory also clean up when its post-call guard
     * terminates the job, including from a coroutine. */
    calls = 0; cancel_after_call = true;
    run("local c=coroutine.create(function() require('fs').read('large') end); "
        "coroutine.resume(c)", "stopped"); assert(calls == 1);
    reset();
}

int main(void)
{
    test_roundtrip();
    test_invalid_arguments();
    test_unavailable_and_errors();
    test_limits_and_termination();
    printf("LUA_FS_PASS: %u real-facade cases; byte roundtrip, isolation, errors, limits, coroutine cancellation and OOM cleanup\n", tests);
    return 0;
}
