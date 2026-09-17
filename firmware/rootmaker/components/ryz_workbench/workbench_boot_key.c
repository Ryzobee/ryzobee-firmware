#include "workbench_boot_key.h"

#include <string.h>

static uint32_t elapsed(uint32_t now, uint32_t since)
{
    return now - since;
}

void ryz_workbench_boot_key_reset(ryz_workbench_boot_key_t *state)
{
    if (state) memset(state, 0, sizeof(*state));
}

bool ryz_workbench_boot_key_update(ryz_workbench_boot_key_t *state,
                                   bool pressed, uint32_t now_ms)
{
    if (!state) return false;

    if (pressed != state->candidate_pressed) {
        state->candidate_pressed = pressed;
        state->candidate_since_ms = now_ms;
    }
    if (state->candidate_pressed != state->stable_pressed &&
        elapsed(now_ms, state->candidate_since_ms) >=
            RYZ_WORKBENCH_BOOT_KEY_DEBOUNCE_MS) {
        state->stable_pressed = state->candidate_pressed;
        if (state->stable_pressed) {
            state->pressed_since_ms = now_ms;
            state->long_press_sent = false;
        } else {
            state->long_press_sent = false;
        }
    }
    if (state->stable_pressed && !state->long_press_sent &&
        elapsed(now_ms, state->pressed_since_ms) >=
            RYZ_WORKBENCH_BOOT_KEY_HOLD_MS) {
        state->long_press_sent = true;
        return true;
    }
    return false;
}
