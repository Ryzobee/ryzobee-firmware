#pragma once

#include "ryz_i2c_scan_types.h"
#include "ryz_rgb_types.h"
#include "ryz_monitor_types.h"

typedef enum { RYZ_TOOL_I2C = 0, RYZ_TOOL_RGB, RYZ_TOOL_MONITOR, RYZ_TOOL_COUNT } ryz_tool_t;
typedef enum {
    RYZ_TOOL_I2C_CONFIGURE = 0, RYZ_TOOL_I2C_START, RYZ_TOOL_I2C_STATUS, RYZ_TOOL_I2C_CANCEL,
    RYZ_TOOL_RGB_SET, RYZ_TOOL_RGB_STATUS,
    RYZ_TOOL_MONITOR_CONFIGURE, RYZ_TOOL_MONITOR_START, RYZ_TOOL_MONITOR_STATUS,
    RYZ_TOOL_MONITOR_STOP, RYZ_TOOL_MONITOR_PAUSE, RYZ_TOOL_MONITOR_CLEAR, RYZ_TOOL_MONITOR_READ,
} ryz_tool_action_t;

typedef struct ryz_tool_request {
    ryz_tool_action_t action;
    uint32_t expected_revision, operation_id, view_generation, after_sequence;
    bool paused;
    union {
        ryz_i2c_scan_config_t i2c;
        ryz_rgb_color_t rgb;
        ryz_monitor_config_t monitor;
    } config;
} ryz_tool_request_t;
typedef struct ryz_tool_reply {
    int error_code; /* Diagnostic only; callback result determines success. */
    /* Accepted operation/revision/view identity, not completion evidence. */
    uint32_t operation_id, revision, view_generation;
    bool started; /* The tool core has a snapshot; not physical reception/PASS. */
    union {
        ryz_i2c_scan_snapshot_t i2c;
        ryz_rgb_snapshot_t rgb;
        ryz_monitor_snapshot_t monitor;
        ryz_monitor_page_t page;
    } state;
} ryz_tool_reply_t;

typedef enum {
    RYZ_TOOL_CALL_OK = 0, RYZ_TOOL_CALL_BUSY, RYZ_TOOL_CALL_INVALID,
    RYZ_TOOL_CALL_STATE, RYZ_TOOL_CALL_UNAVAILABLE, RYZ_TOOL_CALL_NO_MEMORY,
    RYZ_TOOL_CALL_FAILED,
} ryz_tool_call_result_t;

/* Optional synchronous application seam. No raw hardware or owner identity.
 * The trusted Adapter records an accepted native token before returning,
 * including when Lua subsequently fails to allocate its result table.
 * NULL is unavailable, NEVER a direct-hardware/bootstrap fallback.
 * Commands only validate/admit or copy bounded caches; no device waits.
 * Request/output are borrowed solely until return and must not be retained. */
typedef ryz_tool_call_result_t (*ryz_tool_call_fn)(void *context,
    const ryz_tool_request_t *request, ryz_tool_reply_t *out_reply);
