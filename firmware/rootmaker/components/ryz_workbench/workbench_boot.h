#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* UI-owner-only boot state, not the user application's outcome or an online
 * requirement. Once confirmed, a Lua display lease cannot undo this milestone. */
typedef struct {
    bool confirmed;
    int64_t retry_at_us;
} ryz_workbench_boot_t;

typedef struct {
    bool services_ready;
    bool font_ready;
    bool touch_ready;
    bool ui_active;
    bool render_dirty;
} ryz_workbench_boot_inputs_t;

typedef struct {
    bool runtime_ready;
    bool attempted;
    esp_err_t error;
} ryz_workbench_boot_result_t;

/* Sole boot-health/Job-admission decision. No file, network or Lua work here;
 * confirm is the existing running-image confirmation operation. Its failure
 * retains the existing five-second retry interval and never starts a Job. */
ryz_workbench_boot_result_t ryz_workbench_boot_step(
    ryz_workbench_boot_t *state, const ryz_workbench_boot_inputs_t *inputs,
    int64_t now_us, esp_err_t (*confirm)(void *), void *context);
