#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#define RYZ_LUA_SOURCE_MAX 16384
#define RYZ_LUA_OUTPUT_MAX 4096
#define RYZ_LUA_HEAP_LIMIT (256 * 1024)

typedef struct {
    bool ok;
    bool output_truncated;
    char output[RYZ_LUA_OUTPUT_MAX];
    char error[256];
    const char *phase;
    size_t peak_bytes;
    uint32_t elapsed_ms;
} ryz_lua_result_t;

struct ryz_runtime_io_request;
struct ryz_tool_request;
struct ryz_tool_reply;

/* Application-owned execution options; never modify the third-party VM. */
typedef struct {
    const atomic_bool *cancel;
    void (*output)(void *context, const char *bytes, size_t length);
    void *context;
    /* Trusted C-only synchronous call to the UI owner. A true return means
     * execution (or safe cancellation) was acknowledged, not I/O success.
     * Borrowed request/scene/text/context must remain alive until return.
     * NULL is the single-task bootstrap path, never a worker fallback. */
    bool (*io_call)(void *context, struct ryz_runtime_io_request *request);
    /* Optional trusted tool admission. int is ryz_tool_call_result_t; keep
     * this common header independent of the app-only tool value contract. */
    int (*tool_call)(void *context, const struct ryz_tool_request *request,
                     struct ryz_tool_reply *reply);
} ryz_lua_options_t;

void ryz_lua_execute_with_options(const char *source, size_t length, const char *name,
                                 uint32_t timeout_ms, const ryz_lua_options_t *options,
                                 ryz_lua_result_t *result);

void ryz_lua_execute(const char *source, size_t length, const char *name,
                     uint32_t timeout_ms, ryz_lua_result_t *result);
