#pragma once

#include <stdint.h>

/* Copy-only BOOT input. GPIO configuration and the system exit gesture remain
 * owned by C. The callback consumes at most one event and never calls Lua. */
typedef enum {
    RYZ_BOOT_CLICK,
    RYZ_BOOT_DOUBLE_CLICK,
    RYZ_BOOT_LONG_PRESS,
} ryz_boot_event_kind_t;

typedef struct {
    ryz_boot_event_kind_t kind;
    uint32_t timestamp_ms; /* Monotonic, modulo 2^32; Lua receives decimal text. */
    uint32_t held_ms;      /* Debounced final press; capped at the 3 s threshold. */
} ryz_boot_event_t;

typedef enum {
    RYZ_BOOT_OK,
    RYZ_BOOT_EMPTY,
    RYZ_BOOT_UNAVAILABLE,
    RYZ_BOOT_BUSY,
    RYZ_BOOT_OVERFLOW,
    RYZ_BOOT_FAILED,
} ryz_boot_result_t;

typedef ryz_boot_result_t (*ryz_boot_poll_fn)(void *context, ryz_boot_event_t *event);
