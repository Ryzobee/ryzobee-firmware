#pragma once
#include "lua_runtime.h"

#define RYZ_NEURO_ABI "ryz-neuro/1"
#define RYZ_NEURO_CATALOG "ryz-signals/1"
#define RYZ_NEURO_NODES 12
#define RYZ_NEURO_PORTS 8
#define RYZ_NEURO_QUEUE 8

/* The only environment-dependent seam. All business execution and TLV routing
 * live in nervous_runtime.c, shared verbatim by host and ESP builds. */
typedef struct {
    void *context;
    void *(*resize)(void *ptr, size_t size);
    uint64_t (*now_ms)(void *context);
    void (*pause_ms)(void *context, unsigned ms);
    bool (*cancelled)(void *context);
    bool (*finished)(void *context); /* host scenario end; NULL on real device */
    int (*sample)(void *context);    /* 0 released, 1 pressed, -1 hardware error */
    int (*paint)(void *context, uint16_t color, bool begin); /* 0 pending, 1 complete, -1 error */
    void (*cleanup)(void *context); /* drain any pending native operation */
    void (*observe)(void *context, const char *node, const char *state, int32_t value);
    void (*output)(void *context, const char *text, size_t length);
} ryz_neuro_platform_t;

/* TLV wire format: little-endian uint16 T, uint16 L, at most 4 payload bytes.
 * No pointers, native structs, trace IDs, or unbounded nested values on the wire. */
bool ryz_neuro_decode(const uint8_t *bytes, size_t length, unsigned expected_type, int32_t *value);
void ryz_neuro_execute(const char *source, size_t length, const char *name,
                      uint32_t timeout_ms, const ryz_neuro_platform_t *platform,
                      ryz_lua_result_t *result);
