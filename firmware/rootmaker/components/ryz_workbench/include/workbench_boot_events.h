#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "ryz_lua_boot.h"

#define RYZ_WORKBENCH_BOOT_EVENT_CAPACITY 8U
#define RYZ_WORKBENCH_BOOT_EVENT_DEBOUNCE_MS 40U
#define RYZ_WORKBENCH_BOOT_EVENT_DOUBLE_MS 350U
#define RYZ_WORKBENCH_BOOT_EVENT_LONG_MS 3000U

/* Pure input recognizer and fixed FIFO. The owner supplies samples and
 * serializes sample/reset/poll; this module performs no GPIO or Lua calls.
 * It does not replace or change the independent 5-second system exit key. */
typedef struct {
    ryz_boot_event_t queue[RYZ_WORKBENCH_BOOT_EVENT_CAPACITY];
    uint8_t head, count;
    bool available, waiting_release, release_seen, failed, overflow;
    bool stable_pressed, candidate_pressed, long_sent, second_press;
    bool click_pending;
    uint32_t release_since_ms, candidate_since_ms, pressed_since_ms;
    uint32_t click_released_ms, click_held_ms;
} ryz_workbench_boot_events_t;

/* A new epoch never inherits a press, pending click or queued event. It arms
 * only after receiving 40 ms of valid, continuously released input. */
void ryz_workbench_boot_events_reset(ryz_workbench_boot_events_t *state,
                                    bool available);
void ryz_workbench_boot_events_sample(ryz_workbench_boot_events_t *state,
                                     bool pressed, bool valid, uint32_t now_ms);
/* Copy and consume at most one event. EMPTY/failure never modifies *event.
 * Overflow drops all queued and in-flight gestures and is reported once;
 * no new input is accepted until that report and a subsequent stable release.
 * A sample error reports FAILED until a fresh stable release rearms input. */
ryz_boot_result_t ryz_workbench_boot_events_poll(ryz_workbench_boot_events_t *state,
                                               ryz_boot_event_t *event);
