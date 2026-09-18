#include "workbench_boot_events.h"

#include <string.h>

static uint32_t elapsed(uint32_t now, uint32_t since)
{
    return now - since;
}

void ryz_workbench_boot_events_reset(ryz_workbench_boot_events_t *state,
                                    bool available)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->available = available;
    state->waiting_release = true;
}

static void quarantine(ryz_workbench_boot_events_t *state, bool failed, bool overflow)
{
    bool available = state->available;
    ryz_workbench_boot_events_reset(state, available);
    state->failed = failed;
    state->overflow = overflow;
}

static bool enqueue(ryz_workbench_boot_events_t *state, ryz_boot_event_kind_t kind,
                    uint32_t now_ms, uint32_t held_ms)
{
    if (state->count == RYZ_WORKBENCH_BOOT_EVENT_CAPACITY) {
        quarantine(state, false, true);
        return false;
    }
    uint8_t tail = (uint8_t)((state->head + state->count) % RYZ_WORKBENCH_BOOT_EVENT_CAPACITY);
    state->queue[tail] = (ryz_boot_event_t){
        .kind = kind,
        .timestamp_ms = now_ms,
        .held_ms = held_ms > RYZ_WORKBENCH_BOOT_EVENT_LONG_MS ?
            RYZ_WORKBENCH_BOOT_EVENT_LONG_MS : held_ms,
    };
    ++state->count;
    return true;
}

static bool emit_pending_click(ryz_workbench_boot_events_t *state, uint32_t now_ms)
{
    uint32_t held_ms = state->click_held_ms;
    state->click_pending = false;
    return enqueue(state, RYZ_BOOT_CLICK, now_ms, held_ms);
}

void ryz_workbench_boot_events_sample(ryz_workbench_boot_events_t *state,
                                     bool pressed, bool valid, uint32_t now_ms)
{
    if (!state || !state->available) return;
    if (!valid) {
        quarantine(state, true, false);
        return;
    }
    /* Do not accumulate new gestures behind an unobserved loss report. The
     * consumer must see OVERFLOW before a new release-guard epoch can begin. */
    if (state->overflow) return;
    if (state->waiting_release) {
        if (pressed) state->release_seen = false;
        else if (!state->release_seen) {
            state->release_seen = true;
            state->release_since_ms = now_ms;
        } else if (elapsed(now_ms, state->release_since_ms) >= RYZ_WORKBENCH_BOOT_EVENT_DEBOUNCE_MS) {
            state->waiting_release = false;
            state->failed = false;
            state->candidate_since_ms = now_ms;
        }
        return;
    }

    if (pressed != state->candidate_pressed) {
        state->candidate_pressed = pressed;
        state->candidate_since_ms = now_ms;
    }
    if (state->candidate_pressed != state->stable_pressed &&
        elapsed(now_ms, state->candidate_since_ms) >= RYZ_WORKBENCH_BOOT_EVENT_DEBOUNCE_MS) {
        state->stable_pressed = state->candidate_pressed;
        if (state->stable_pressed) {
            /* A debounced second press exactly at the boundary wins over the
             * pending single-click timeout processed below. */
            state->second_press = state->click_pending &&
                elapsed(now_ms, state->click_released_ms) <= RYZ_WORKBENCH_BOOT_EVENT_DOUBLE_MS;
            if (state->second_press) state->click_pending = false;
            else if (state->click_pending && !emit_pending_click(state, now_ms)) return;
            state->pressed_since_ms = now_ms;
            state->long_sent = false;
        } else {
            uint32_t held_ms = elapsed(now_ms, state->pressed_since_ms);
            if (!state->long_sent) {
                if (held_ms >= RYZ_WORKBENCH_BOOT_EVENT_LONG_MS) {
                    if (!enqueue(state, RYZ_BOOT_LONG_PRESS, now_ms, held_ms)) return;
                } else if (state->second_press) {
                    if (!enqueue(state, RYZ_BOOT_DOUBLE_CLICK, now_ms, held_ms)) return;
                } else {
                    state->click_pending = true;
                    state->click_released_ms = now_ms;
                    state->click_held_ms = held_ms;
                }
            }
            state->second_press = false;
            state->long_sent = false;
        }
    }
    /* A tentative release may still bounce. Defer classification until it
     * stabilizes or returns to pressed instead of emitting during that edge.
     * Held duration is measured between debounced transitions, not GPIO edges. */
    if (state->stable_pressed && state->candidate_pressed && !state->long_sent &&
        elapsed(now_ms, state->pressed_since_ms) >= RYZ_WORKBENCH_BOOT_EVENT_LONG_MS) {
        if (!enqueue(state, RYZ_BOOT_LONG_PRESS, now_ms, RYZ_WORKBENCH_BOOT_EVENT_LONG_MS)) return;
        state->long_sent = true;
        state->second_press = false;
    }
    if (state->click_pending &&
        elapsed(now_ms, state->click_released_ms) >= RYZ_WORKBENCH_BOOT_EVENT_DOUBLE_MS) {
        (void)emit_pending_click(state, now_ms);
    }
}

ryz_boot_result_t ryz_workbench_boot_events_poll(ryz_workbench_boot_events_t *state,
                                               ryz_boot_event_t *event)
{
    if (!state || !event) return RYZ_BOOT_FAILED;
    if (!state->available) return RYZ_BOOT_UNAVAILABLE;
    if (state->failed) return RYZ_BOOT_FAILED;
    if (state->overflow) {
        quarantine(state, false, false);
        return RYZ_BOOT_OVERFLOW;
    }
    if (!state->count) return RYZ_BOOT_EMPTY;
    *event = state->queue[state->head];
    state->head = (uint8_t)((state->head + 1U) % RYZ_WORKBENCH_BOOT_EVENT_CAPACITY);
    --state->count;
    return RYZ_BOOT_OK;
}
