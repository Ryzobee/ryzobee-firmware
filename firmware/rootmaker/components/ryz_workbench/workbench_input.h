#pragma once

#include "touch.h"

#define WORKBENCH_INPUT_CAPACITY 16
#define WORKBENCH_INPUT_TIMEOUT_BUDGET 8

/* Only the UI owner calls these functions. A worker accesses read through the
 * owner's request channel, never by sharing this state across tasks. */
typedef struct {
    ryz_touch_sample_t samples[WORKBENCH_INPUT_CAPACITY];
    ryz_touch_sample_t latest;
    uint8_t head;
    uint8_t count;
    bool queued;
    bool sample_valid;
    bool wait_release;
    bool overflow_pending;
    bool read_valid;
    bool read_pressed;
    /* Deliver cancellation exactly once when a queued app had already seen a
     * press whose release became unknowable after a physical timeout. */
    bool interruption_pending;
    /* Display policy cancellation is independent of timeout recovery. It
     * survives later physical samples until the consumer has discarded its
     * capture, and is valid in queued and current-sample modes. */
    bool suppression_pending;
    uint32_t suppression_ms;
    /* Preserve the last delivered pressed state until one successful physical
     * sample proves whether that same gesture is still held or released. */
    bool recovering_press;
    /* Budget exhaustion is a terminal consumer event. It must survive a
     * successful physical sample racing ahead of the app's next read. */
    bool timeout_exhausted_pending;
    /* Consecutive queued ESP_ERR_TIMEOUT samples already hidden from the app.
     * A persistent bus failure becomes an explicit error after the budget. */
    uint8_t timeout_streak;
    esp_err_t sample_error;
    /* Saturating count of queued/incoming events discarded on overflow, a
     * failed sample, or policy cancellation, since the last reset.
     * Coalesced MOVE is not a drop. */
    uint32_t dropped;
} workbench_input_t;

/* Start a queued app input epoch. Old events and diagnostics are cleared.
 * No read succeeds before the next successful physical sample; an already-held
 * finger stays hidden until a successful physical release has been observed. */
void workbench_input_reset(workbench_input_t *state);

/* Select after reset: app input uses queued events; legacy/neuro input reads
 * the current sample. Clears queued events, pending overflow, and consumer
 * read history without changing the physical release guard or latest sample.
 * This is an epoch/configuration operation, not a per-read mode switch. */
void workbench_input_set_event_mode(workbench_input_t *state, bool queued);

/* Feed every physical read, including its actual error. Queued DOWN/UP stay
 * FIFO; only adjacent tail MOVE coalesces. NONE updates the fallback sample.
 * Current-sample mode only updates latest, never accumulates or overflows.
 * Errors and overflow clear the queue and re-arm the release guard. A queued
 * app masks up to WORKBENCH_INPUT_TIMEOUT_BUDGET consecutive ESP_ERR_TIMEOUT
 * samples so a transient shared-bus timeout cannot terminate the VM. If the
 * app already observed a press, it receives one internal interruption marker
 * per recovery episode while raw input stays logically held. Persistent
 * timeout is latched until the consumer reads it; other modes and other errors
 * stay explicit. */
void workbench_input_push(workbench_input_t *state,
                          const ryz_touch_sample_t *sample, esp_err_t error);

/* A physical read consumed by display wake/configuration policy still counts
 * as an observation. Preserve real errors; valid samples become neutral and
 * cancel an already-delivered contact once using interrupted NONE without
 * coordinates or an UP. An observed physical release ends quarantine, while
 * a held contact stays hidden through its eventual release. Cancellation is
 * delivered before a later genuine DOWN, even if the worker reads slowly. */
void workbench_input_suppress(workbench_input_t *state,
                             const ryz_touch_sample_t *sample, esp_err_t error);

/* In queued mode pop an event, or return latest with event=NONE. Current-sample
 * mode returns latest and normalizes its phase relative to this consumer's
 * previous successful read, not the physical reader's previous sample.
 * During the release guard, successful samples are neutral (no pressed,
 * position, or gesture).
 * Failed physical reads remain errors until a new successful physical read,
 * except a bounded run of queued-app ESP_ERR_TIMEOUT, which is returned as a
 * safe NONE sample (with interrupted=true once if managed UI capture had to be
 * cancelled). Persistent timeout becomes ESP_ERR_TIMEOUT instead of a
 * synthetic release.
 * Overflow is reported as ESP_ERR_NO_MEM once; an outstanding physical error
 * takes precedence without consuming that report. Errors always clear out. */
esp_err_t workbench_input_read(void *state, ryz_touch_sample_t *out);
