#pragma once
#include <stdbool.h>
#include "cJSON.h"

/* Caller-owned JSON. Transport must reject encoded NULs before dispatch.
 * All success ACK allocation precedes broker-gated admission. NULL means no or
 * unknown ACK, never permission to retry. Async operation_id is not completion.
 * read/status are non-consuming cache copies, never UART0/stdout interception.
 * SYSTEM means filtered ESP diagnostics, not complete logs or Lua/RPC output.
 */
bool ryz_workbench_monitor_rpc_supports(const char *operation);
cJSON *ryz_workbench_monitor_rpc(const cJSON *request, const char *boot_id);
