#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "workbench_write_guard.h"

/* Restart the exact completed OTA attempt, only after acquiring the same
 * reservation used by every final Job admission and runtime Store writer.
 * Busy/dirty/unknown storage is rejected, never automatically recovered.
 * The reservation remains held until the OTA call fails/returns; a real
 * nonreturning reboot keeps it held. This is not an all-peripheral shutdown
 * claim and does not establish user autorun, rescue, or OTA source policy. */
esp_err_t ryz_workbench_restart_updated(uint32_t expected_attempt,
                                      const ryz_workbench_write_guard_t *guard);
