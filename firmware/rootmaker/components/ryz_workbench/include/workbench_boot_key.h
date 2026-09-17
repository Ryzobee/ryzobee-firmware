#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RYZ_WORKBENCH_BOOT_KEY_HOLD_MS 5000U
#define RYZ_WORKBENCH_BOOT_KEY_DEBOUNCE_MS 40U

/* Pure, owner-task state machine for the active-low BOOT key. Keeping the
 * timing independent from GPIO makes the long-press contract host-testable. */
typedef struct {
    bool stable_pressed;
    bool candidate_pressed;
    bool long_press_sent;
    uint32_t candidate_since_ms;
    uint32_t pressed_since_ms;
} ryz_workbench_boot_key_t;

void ryz_workbench_boot_key_reset(ryz_workbench_boot_key_t *state);

/* Feed one debounced sample. Returns true exactly once per press after the
 * key has remained pressed for five seconds; release rearms the state. */
bool ryz_workbench_boot_key_update(ryz_workbench_boot_key_t *state,
                                   bool pressed, uint32_t now_ms);
