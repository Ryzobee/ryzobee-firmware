#pragma once

#include <stdbool.h>
#include "cJSON.h"

/* Trusted board RGB JSON Adapter. The transport rejects encoded NULs before
 * dispatch (cJSON strings have no original byte length). Returned JSON belongs
 * to the caller; every reply carries schema and boot_id. set/off success only
 * acknowledges admission, not a completed frame or optical observation.
 *
 * cleanup requires the original boot_id/request_id and never sends black.
 * All successful ACK fields are allocated before broker-gated admission. A false reply never
 * follows successful admission; NULL is no/unknown ACK and must not cause an
 * automatic retry. status copies cache without starting the worker/hardware.
 * Existing completed color is historical and requires output_known to be
 * trusted as current driver state; it is never physical LED readback.
 */
bool ryz_workbench_rgb_rpc_supports(const char *operation);
cJSON *ryz_workbench_rgb_rpc(const cJSON *request, const char *boot_id);
