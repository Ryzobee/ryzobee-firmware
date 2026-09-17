#pragma once

#include <stdbool.h>
#include "cJSON.h"

/* Versioned JSON Adapter for the trusted, fixed-board scanner. The transport
 * must reject encoded NULs before dispatch: cJSON strings carry no byte length.
 * Unknown/duplicate fields are rejected. Only configure accepts pins/clock
 * and expected_revision; raw GPIO/I2C control is never exposed here.
 *
 * start/cancel success acknowledges a request, not scan completion. configure
 * acknowledges a software configuration CAS, never a scan or GPIO action.
 * v2 status separates current config from immutable historical scan_config.
 * Every success field is allocated before broker-gated admission; false never follows a
 * successful request. NULL is allocation failure / unknown acknowledgement,
 * so the caller must inspect status rather than blindly retry a mutation.
 * status is a cached read and never implicitly starts the worker or does I2C.
 * All non-NULL replies carry schema and boot_id. Returned JSON is caller-owned.
 */
bool ryz_workbench_i2c_rpc_supports(const char *operation);
cJSON *ryz_workbench_i2c_rpc(const cJSON *request, const char *boot_id);
