#include "workbench_input.h"

#include <stddef.h>
#include <string.h>

static void discard_events(workbench_input_t *state, unsigned additional)
{
    const unsigned lost = state->count + additional;
    state->dropped = UINT32_MAX - state->dropped < lost
        ? UINT32_MAX : state->dropped + lost;
    state->head = 0;
    state->count = 0;
}

static ryz_touch_sample_t neutral_sample(uint32_t sampled_ms)
{
    return (ryz_touch_sample_t){
        .event = RYZ_TOUCH_NONE,
        .sampled_ms = sampled_ms,
    };
}

void workbench_input_reset(workbench_input_t *state)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->queued = true;
    state->wait_release = true;
    state->sample_error = ESP_ERR_INVALID_STATE;
}

void workbench_input_set_event_mode(workbench_input_t *state, bool queued)
{
    if (!state) return;
    state->queued = queued;
    state->head = 0;
    state->count = 0;
    state->overflow_pending = false;
    state->read_valid = false;
    state->read_pressed = false;
    state->interruption_pending = false;
    state->suppression_pending = false;
    state->suppression_ms = 0;
    state->recovering_press = false;
    state->timeout_exhausted_pending = false;
    state->timeout_streak = 0;
}

void workbench_input_push(workbench_input_t *state,
                          const ryz_touch_sample_t *sample, esp_err_t error)
{
    if (!state) return;
    if (error == ESP_OK && !sample) error = ESP_ERR_INVALID_ARG;
    if (error != ESP_OK) {
        const uint32_t last_sampled_ms = state->latest.sampled_ms;
        discard_events(state, 0);
        state->sample_valid = false;
        state->sample_error = error;
        const bool mask_timeout = state->queued && error == ESP_ERR_TIMEOUT &&
                                  state->timeout_streak <
                                      WORKBENCH_INPUT_TIMEOUT_BUDGET;
        if (mask_timeout) {
            ++state->timeout_streak;
            const bool begin_recovery = !state->recovering_press &&
                                        state->read_valid && state->read_pressed;
            state->recovering_press |= begin_recovery;
            state->interruption_pending |= begin_recovery;
            state->wait_release = !state->recovering_press;
        } else {
            state->timeout_streak = state->queued && error == ESP_ERR_TIMEOUT
                ? WORKBENCH_INPUT_TIMEOUT_BUDGET + 1 : 0;
            state->timeout_exhausted_pending |=
                state->queued && error == ESP_ERR_TIMEOUT;
            state->wait_release = true;
        }
        /* No physical sample completed, so preserve the last known timestamp
         * instead of making the public clock jump backwards to zero. */
        state->latest = neutral_sample(last_sampled_ms);
        state->latest.pressed = state->recovering_press;
        if (!mask_timeout) {
            state->read_valid = false;
            state->read_pressed = false;
            state->interruption_pending = false;
            state->recovering_press = false;
        }
        return;
    }

    state->sample_valid = true;
    state->sample_error = ESP_OK;
    state->timeout_streak = 0;
    ryz_touch_sample_t recovered;
    if (state->recovering_press) {
        recovered = *sample;
        recovered.interrupted = false;
        recovered.event = sample->pressed ? RYZ_TOUCH_MOVE : RYZ_TOUCH_UP;
        if (!sample->pressed) {
            recovered.has_position = false;
            recovered.x = 0;
            recovered.y = 0;
            recovered.fingers = 0;
        }
        sample = &recovered;
        state->recovering_press = false;
        state->wait_release = false;
    }
    if (state->wait_release) {
        state->latest = neutral_sample(sample->sampled_ms);
        if (!sample->pressed) state->wait_release = false;
        return; /* The release which disarms the guard is not an app UP. */
    }
    state->latest = *sample;
    if (!state->queued) return;
    if (sample->event == RYZ_TOUCH_NONE) return;

    if (sample->event == RYZ_TOUCH_MOVE && state->count) {
        const unsigned tail =
            (state->head + state->count - 1) % WORKBENCH_INPUT_CAPACITY;
        if (state->samples[tail].event == RYZ_TOUCH_MOVE) {
            state->samples[tail] = *sample;
            return;
        }
    }
    if (state->count == WORKBENCH_INPUT_CAPACITY) {
        discard_events(state, 1);
        state->overflow_pending = true;
        state->wait_release = true;
        state->latest = neutral_sample(sample->sampled_ms);
        return;
    }
    const unsigned tail =
        (state->head + state->count) % WORKBENCH_INPUT_CAPACITY;
    state->samples[tail] = *sample;
    ++state->count;
}

void workbench_input_suppress(workbench_input_t *state,
                             const ryz_touch_sample_t *sample, esp_err_t error)
{
    if (!state) return;
    if (error != ESP_OK || !sample) {
        workbench_input_push(state, sample, error);
        return;
    }
    discard_events(state, 0);
    if (!state->suppression_pending && state->read_valid && state->read_pressed) {
        state->suppression_pending = true;
        state->suppression_ms = sample->sampled_ms;
    }
    state->sample_valid = true;
    state->sample_error = ESP_OK;
    state->latest = neutral_sample(sample->sampled_ms);
    state->wait_release = sample->pressed;
    state->recovering_press = false;
    state->interruption_pending = false;
    state->read_valid = false;
    state->read_pressed = false;
    state->timeout_streak = 0;
}

esp_err_t workbench_input_read(void *opaque, ryz_touch_sample_t *out)
{
    workbench_input_t *state = opaque;
    if (!out) {
        if (state) {
            state->read_valid = false;
            state->read_pressed = false;
        }
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!state) return ESP_ERR_INVALID_ARG;
    if (state->timeout_exhausted_pending) {
        state->timeout_exhausted_pending = false;
        state->read_valid = false;
        state->read_pressed = false;
        return ESP_ERR_TIMEOUT;
    }
    if (state->queued && state->interruption_pending) {
        *out = neutral_sample(state->latest.sampled_ms);
        out->pressed = true;
        out->interrupted = true;
        state->interruption_pending = false;
        state->read_valid = true;
        state->read_pressed = true;
        return ESP_OK;
    }
    if (state->sample_error != ESP_OK || !state->sample_valid) {
        if (state->queued && state->sample_error == ESP_ERR_TIMEOUT &&
            state->timeout_streak > 0 &&
            state->timeout_streak <= WORKBENCH_INPUT_TIMEOUT_BUDGET) {
            *out = neutral_sample(state->latest.sampled_ms);
            out->pressed = state->latest.pressed;
            state->read_valid = true;
            state->read_pressed = out->pressed;
            return ESP_OK;
        }
        state->read_valid = false;
        state->read_pressed = false;
        return state->sample_error != ESP_OK
            ? state->sample_error : ESP_ERR_INVALID_STATE;
    }
    if (state->overflow_pending) {
        state->overflow_pending = false;
        state->read_valid = false;
        state->read_pressed = false;
        return ESP_ERR_NO_MEM;
    }
    if (state->suppression_pending) {
        *out = neutral_sample(state->suppression_ms);
        out->interrupted = true;
        state->suppression_pending = false;
        state->read_valid = true;
        state->read_pressed = false;
        return ESP_OK;
    }
    if (!state->queued) {
        *out = state->latest;
        ryz_touch_normalize_event(state->read_valid, state->read_pressed, out);
        state->read_valid = true;
        state->read_pressed = out->pressed;
        return ESP_OK;
    }
    if (state->count) {
        *out = state->samples[state->head];
        state->head = (state->head + 1) % WORKBENCH_INPUT_CAPACITY;
        --state->count;
    } else {
        *out = state->latest;
        out->event = RYZ_TOUCH_NONE;
    }
    state->read_valid = true;
    state->read_pressed = out->pressed;
    return ESP_OK;
}
