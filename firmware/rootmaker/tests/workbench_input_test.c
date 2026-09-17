#include "workbench_input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static ryz_touch_sample_t sample(ryz_touch_event_t event, unsigned time)
{
    const bool pressed = event == RYZ_TOUCH_DOWN || event == RYZ_TOUCH_MOVE;
    return (ryz_touch_sample_t){
        .pressed = pressed, .has_position = pressed,
        .x = pressed ? time % 240 : 0, .y = pressed ? 20 : 0,
        .fingers = pressed, .event = event, .sampled_ms = time,
    };
}

static void push(workbench_input_t *state, ryz_touch_event_t event, unsigned time)
{
    const ryz_touch_sample_t value = sample(event, time);
    workbench_input_push(state, &value, ESP_OK);
}

static ryz_touch_sample_t read_event(workbench_input_t *state,
                                    ryz_touch_event_t event, unsigned time)
{
    ryz_touch_sample_t out;
    memset(&out, 0xa5, sizeof(out));
    CHECK(workbench_input_read(state, &out) == ESP_OK);
    CHECK(out.event == event);
    CHECK(out.sampled_ms == time);
    return out;
}

static void read_error(workbench_input_t *state, esp_err_t error)
{
    ryz_touch_sample_t out;
    memset(&out, 0xa5, sizeof(out));
    const esp_err_t actual = workbench_input_read(state, &out);
    if (actual != error) {
        fprintf(stderr, "expected read error %d, got %d\n", error, actual);
    }
    CHECK(actual == error);
    CHECK(!out.pressed && !out.has_position && out.event == RYZ_TOUCH_NONE);
    CHECK(out.x == 0 && out.y == 0 && out.sampled_ms == 0);
}

static void check_neutral(const ryz_touch_sample_t *out)
{
    CHECK(!out->pressed && !out->interrupted && !out->has_position &&
          out->event == RYZ_TOUCH_NONE);
    CHECK(out->x == 0 && out->y == 0 && out->fingers == 0);
    CHECK(out->gesture == 0 && out->raw_event == 0);
}

static void ready(workbench_input_t *state)
{
    workbench_input_reset(state);
    push(state, RYZ_TOUCH_NONE, 1);
    CHECK(!state->wait_release && state->count == 0);
}

static void fill(workbench_input_t *state)
{
    for (unsigned i = 0; i < WORKBENCH_INPUT_CAPACITY; ++i) {
        push(state, i % 2 ? RYZ_TOUCH_UP : RYZ_TOUCH_DOWN, i + 2);
    }
    CHECK(state->count == WORKBENCH_INPUT_CAPACITY);
}

static void test_reset(void)
{
    workbench_input_t state;
    memset(&state, 0xa5, sizeof(state));
    workbench_input_reset(&state);
    CHECK(state.wait_release && !state.sample_valid && !state.count);
    read_error(&state, ESP_ERR_INVALID_STATE);
    read_error(&state, ESP_ERR_INVALID_STATE);
    ryz_touch_sample_t held = sample(RYZ_TOUCH_DOWN, 2);
    held.gesture = 7;
    held.raw_event = 2;
    workbench_input_push(&state, &held, ESP_OK);
    ryz_touch_sample_t out = read_event(&state, RYZ_TOUCH_NONE, 2);
    check_neutral(&out);
    CHECK(state.wait_release);
    push(&state, RYZ_TOUCH_MOVE, 3);
    out = read_event(&state, RYZ_TOUCH_NONE, 3);
    check_neutral(&out);
    push(&state, RYZ_TOUCH_UP, 4);
    out = read_event(&state, RYZ_TOUCH_NONE, 4);
    check_neutral(&out);
    CHECK(!state.wait_release);
    push(&state, RYZ_TOUCH_DOWN, 5);
    out = read_event(&state, RYZ_TOUCH_DOWN, 5);
    CHECK(out.pressed && out.has_position);
    push(&state, RYZ_TOUCH_UP, 6);
    state.dropped = 9;
    workbench_input_reset(&state);
    CHECK(state.count == 0 && state.dropped == 0 &&
          state.wait_release && !state.recovering_press);
    read_error(&state, ESP_ERR_INVALID_STATE);
    workbench_input_reset(NULL);
    workbench_input_push(NULL, NULL, ESP_OK);
    CHECK(workbench_input_read(&state, NULL) == ESP_ERR_INVALID_ARG);
    read_error(NULL, ESP_ERR_INVALID_ARG);
}

static void test_fifo(void)
{
    workbench_input_t state;
    ready(&state);
    push(&state, RYZ_TOUCH_DOWN, 2);
    push(&state, RYZ_TOUCH_MOVE, 3);
    push(&state, RYZ_TOUCH_MOVE, 4);
    push(&state, RYZ_TOUCH_UP, 5);
    push(&state, RYZ_TOUCH_DOWN, 6);
    push(&state, RYZ_TOUCH_MOVE, 7);
    push(&state, RYZ_TOUCH_MOVE, 8);
    CHECK(state.count == 5 && state.dropped == 0);
    (void)read_event(&state, RYZ_TOUCH_DOWN, 2);
    (void)read_event(&state, RYZ_TOUCH_MOVE, 4);
    (void)read_event(&state, RYZ_TOUCH_UP, 5);
    (void)read_event(&state, RYZ_TOUCH_DOWN, 6);
    ryz_touch_sample_t out = read_event(&state, RYZ_TOUCH_MOVE, 8);
    CHECK(out.x == 8);
    out = read_event(&state, RYZ_TOUCH_NONE, 8);
    CHECK(out.pressed && out.has_position && out.x == 8);
    push(&state, RYZ_TOUCH_UP, 9);
    push(&state, RYZ_TOUCH_NONE, 10);
    CHECK(state.count == 1);
    (void)read_event(&state, RYZ_TOUCH_UP, 9);
    out = read_event(&state, RYZ_TOUCH_NONE, 10);
    CHECK(!out.pressed);
    out = read_event(&state, RYZ_TOUCH_NONE, 10);
    CHECK(!out.pressed);
}

static void test_wrap(void)
{
    workbench_input_t state;
    ready(&state);
    fill(&state);
    for (unsigned i = 0; i < 8; ++i) {
        (void)read_event(&state, i % 2 ? RYZ_TOUCH_UP : RYZ_TOUCH_DOWN, i + 2);
    }
    for (unsigned i = 16; i < 23; ++i) {
        push(&state, i % 2 ? RYZ_TOUCH_UP : RYZ_TOUCH_DOWN, i + 2);
    }
    push(&state, RYZ_TOUCH_MOVE, 25);
    CHECK(state.count == WORKBENCH_INPUT_CAPACITY);
    push(&state, RYZ_TOUCH_MOVE, 26);
    CHECK(state.count == WORKBENCH_INPUT_CAPACITY && state.dropped == 0);
    for (unsigned i = 8; i < 23; ++i) {
        (void)read_event(&state, i % 2 ? RYZ_TOUCH_UP : RYZ_TOUCH_DOWN, i + 2);
    }
    (void)read_event(&state, RYZ_TOUCH_MOVE, 26);
    CHECK(state.count == 0 && !state.overflow_pending);
}

static void test_overflow(void)
{
    workbench_input_t state;
    ready(&state);
    fill(&state);
    push(&state, RYZ_TOUCH_DOWN, 18);
    CHECK(state.count == 0 && state.dropped == 17 && state.wait_release);
    read_error(&state, ESP_ERR_NO_MEM);
    ryz_touch_sample_t out = read_event(&state, RYZ_TOUCH_NONE, 18);
    check_neutral(&out);
    push(&state, RYZ_TOUCH_MOVE, 19);
    out = read_event(&state, RYZ_TOUCH_NONE, 19);
    check_neutral(&out);
    CHECK(state.wait_release);
    push(&state, RYZ_TOUCH_UP, 20);
    out = read_event(&state, RYZ_TOUCH_NONE, 20);
    check_neutral(&out);
    push(&state, RYZ_TOUCH_DOWN, 21);
    (void)read_event(&state, RYZ_TOUCH_DOWN, 21);
    CHECK(state.dropped == 17);
}

static void test_error(void)
{
    workbench_input_t state;
    ready(&state);
    push(&state, RYZ_TOUCH_DOWN, 2);
    (void)read_event(&state, RYZ_TOUCH_DOWN, 2);
    const ryz_touch_sample_t stale = sample(RYZ_TOUCH_MOVE, 3);
    workbench_input_push(&state, &stale, ESP_ERR_TIMEOUT);
    CHECK(state.count == 0 && state.dropped == 0 &&
          !state.wait_release && state.recovering_press);
    CHECK(!state.sample_valid);
    ryz_touch_sample_t out;
    memset(&out, 0xa5, sizeof(out));
    CHECK(workbench_input_read(&state, &out) == ESP_OK);
    CHECK(out.pressed && out.interrupted && !out.has_position &&
          out.event == RYZ_TOUCH_NONE);
    CHECK(out.sampled_ms == 2);
    memset(&out, 0xa5, sizeof(out));
    CHECK(workbench_input_read(&state, &out) == ESP_OK);
    CHECK(out.pressed && !out.interrupted && !out.has_position &&
          out.event == RYZ_TOUCH_NONE);
    CHECK(out.sampled_ms == 2);
    workbench_input_push(&state, &stale, ESP_ERR_TIMEOUT);
    memset(&out, 0xa5, sizeof(out));
    CHECK(workbench_input_read(&state, &out) == ESP_OK);
    CHECK(out.pressed && !out.interrupted && !out.has_position &&
          out.event == RYZ_TOUCH_NONE);
    push(&state, RYZ_TOUCH_DOWN, 4);
    out = read_event(&state, RYZ_TOUCH_MOVE, 4);
    CHECK(out.pressed && out.has_position);
    CHECK(!state.wait_release && !state.recovering_press);
    push(&state, RYZ_TOUCH_UP, 5);
    (void)read_event(&state, RYZ_TOUCH_UP, 5);
    push(&state, RYZ_TOUCH_DOWN, 6);
    (void)read_event(&state, RYZ_TOUCH_DOWN, 6);
    push(&state, RYZ_TOUCH_UP, 7);
    (void)read_event(&state, RYZ_TOUCH_UP, 7);

    ready(&state);
    push(&state, RYZ_TOUCH_DOWN, 10);
    (void)read_event(&state, RYZ_TOUCH_DOWN, 10);
    workbench_input_push(&state, &stale, ESP_ERR_TIMEOUT);
    CHECK(workbench_input_read(&state, &out) == ESP_OK && out.interrupted);
    push(&state, RYZ_TOUCH_UP, 12);
    out = read_event(&state, RYZ_TOUCH_UP, 12);
    CHECK(!out.pressed && !out.has_position);

    ready(&state);
    for (unsigned i = 0; i < WORKBENCH_INPUT_TIMEOUT_BUDGET; ++i) {
        workbench_input_push(&state, &stale, ESP_ERR_TIMEOUT);
        CHECK(state.timeout_streak == i + 1);
        out = read_event(&state, RYZ_TOUCH_NONE, 1);
        check_neutral(&out);
    }
    workbench_input_push(&state, &stale, ESP_ERR_TIMEOUT);
    CHECK(state.sample_error == ESP_ERR_TIMEOUT && !state.sample_valid &&
          state.timeout_streak == WORKBENCH_INPUT_TIMEOUT_BUDGET + 1);
    read_error(&state, ESP_ERR_TIMEOUT);
    CHECK(state.wait_release && !state.recovering_press &&
          !state.interruption_pending &&
          state.timeout_streak == WORKBENCH_INPUT_TIMEOUT_BUDGET + 1);

    ready(&state);
    for (unsigned i = 0; i <= WORKBENCH_INPUT_TIMEOUT_BUDGET; ++i) {
        workbench_input_push(&state, &stale, ESP_ERR_TIMEOUT);
    }
    push(&state, RYZ_TOUCH_UP, 20);
    CHECK(state.timeout_exhausted_pending && !state.wait_release);
    read_error(&state, ESP_ERR_TIMEOUT);
    out = read_event(&state, RYZ_TOUCH_NONE, 20);
    check_neutral(&out);

    ready(&state);
    workbench_input_push(&state, NULL, ESP_FAIL);
    read_error(&state, ESP_FAIL);
    CHECK(state.wait_release);
    workbench_input_push(&state, NULL, ESP_OK);
    read_error(&state, ESP_ERR_INVALID_ARG);
    CHECK(state.wait_release);
}

static void test_overflow_release(void)
{
    workbench_input_t state;
    ready(&state);
    fill(&state);
    push(&state, RYZ_TOUCH_DOWN, 18);
    push(&state, RYZ_TOUCH_UP, 19);
    push(&state, RYZ_TOUCH_DOWN, 20);
    CHECK(!state.wait_release && state.count == 1);
    read_error(&state, ESP_ERR_NO_MEM);
    (void)read_event(&state, RYZ_TOUCH_DOWN, 20);
    (void)read_event(&state, RYZ_TOUCH_NONE, 20);
}

static void test_error_overflow(void)
{
    workbench_input_t state;
    ready(&state);
    fill(&state);
    state.dropped = UINT32_MAX - 2;
    push(&state, RYZ_TOUCH_DOWN, 18);
    CHECK(state.dropped == UINT32_MAX);
    workbench_input_push(&state, NULL, ESP_ERR_INVALID_RESPONSE);
    read_error(&state, ESP_ERR_INVALID_RESPONSE);
    CHECK(state.overflow_pending);
    read_error(&state, ESP_ERR_INVALID_RESPONSE);
    push(&state, RYZ_TOUCH_UP, 19);
    read_error(&state, ESP_ERR_NO_MEM);
    ryz_touch_sample_t out = read_event(&state, RYZ_TOUCH_NONE, 19);
    check_neutral(&out);
    push(&state, RYZ_TOUCH_DOWN, 20);
    workbench_input_push(&state, NULL, ESP_FAIL);
    CHECK(state.dropped == UINT32_MAX);
    read_error(&state, ESP_FAIL);
}

static void test_protocol(void)
{
    workbench_input_t state;
    workbench_input_reset(&state);
    const uint8_t held[] = {0, 1, 0, 45, 0, 50};
    const uint8_t released[] = {0, 0, 0x40, 0, 0, 0};
    ryz_touch_sample_t input;
    CHECK(ryz_touch_decode(held, &input));
    ryz_touch_normalize_event(false, false, &input);
    input.sampled_ms = 2;
    workbench_input_push(&state, &input, ESP_OK);
    ryz_touch_sample_t out = read_event(&state, RYZ_TOUCH_NONE, 2);
    check_neutral(&out);
    CHECK(ryz_touch_decode(released, &input));
    ryz_touch_normalize_event(true, true, &input);
    input.sampled_ms = 3;
    CHECK(input.event == RYZ_TOUCH_UP);
    workbench_input_push(&state, &input, ESP_OK);
    (void)read_event(&state, RYZ_TOUCH_NONE, 3);
    CHECK(ryz_touch_decode(held, &input));
    ryz_touch_normalize_event(true, false, &input);
    input.sampled_ms = 4;
    workbench_input_push(&state, &input, ESP_OK);
    out = read_event(&state, RYZ_TOUCH_DOWN, 4);
    CHECK(out.x == 45 && out.y == 50);
    /* A different app cannot inherit this still-held physical finger. */
    workbench_input_reset(&state);
    ryz_touch_normalize_event(true, true, &input);
    input.sampled_ms = 5;
    workbench_input_push(&state, &input, ESP_OK);
    out = read_event(&state, RYZ_TOUCH_NONE, 5);
    check_neutral(&out);
    CHECK(state.wait_release);
}

static void ready_current(workbench_input_t *state)
{
    workbench_input_reset(state);
    CHECK(state->queued);
    workbench_input_set_event_mode(state, false);
    CHECK(!state->queued && state->wait_release && !state->read_valid);
    push(state, RYZ_TOUCH_NONE, 1);
    CHECK(!state->wait_release && state->count == 0);
}

static void test_current_no_queue(void)
{
    workbench_input_t state;
    ready_current(&state);
    for (unsigned i = 2; i < 2002; ++i) {
        push(&state, i % 2 ? RYZ_TOUCH_UP : RYZ_TOUCH_DOWN, i);
    }
    CHECK(state.count == 0 && state.dropped == 0 && !state.overflow_pending);
    CHECK(!state.wait_release && !state.read_valid);
    ryz_touch_sample_t out = read_event(&state, RYZ_TOUCH_NONE, 2001);
    CHECK(!out.pressed); /* Missed taps are not replayed as historical events. */
    push(&state, RYZ_TOUCH_MOVE, 2002);
    out = read_event(&state, RYZ_TOUCH_DOWN, 2002);
    CHECK(out.pressed && out.has_position && out.x == 2002 % 240);
    CHECK(state.count == 0 && state.dropped == 0 && !state.overflow_pending);
}

static void test_current_events(void)
{
    workbench_input_t state;
    ready_current(&state);
    push(&state, RYZ_TOUCH_MOVE, 2);
    (void)read_event(&state, RYZ_TOUCH_DOWN, 2);
    CHECK(state.read_valid && state.read_pressed);
    (void)read_event(&state, RYZ_TOUCH_MOVE, 2);
    /* Physical edges between caller reads are not a raw-reader event queue. */
    push(&state, RYZ_TOUCH_UP, 3);
    push(&state, RYZ_TOUCH_DOWN, 4);
    (void)read_event(&state, RYZ_TOUCH_MOVE, 4);
    push(&state, RYZ_TOUCH_UP, 5);
    (void)read_event(&state, RYZ_TOUCH_UP, 5);
    CHECK(state.read_valid && !state.read_pressed);
    (void)read_event(&state, RYZ_TOUCH_NONE, 5);
    /* Ignore the physical reader's normalized NONE/DOWN classification. */
    ryz_touch_sample_t held = sample(RYZ_TOUCH_MOVE, 6);
    held.event = RYZ_TOUCH_NONE;
    held.raw_event = 2;
    workbench_input_push(&state, &held, ESP_OK);
    ryz_touch_sample_t out = read_event(&state, RYZ_TOUCH_DOWN, 6);
    CHECK(out.raw_event == 2 && out.pressed);
    (void)read_event(&state, RYZ_TOUCH_MOVE, 6);
}

static void test_current_history(void)
{
    workbench_input_t state;
    ready_current(&state);
    push(&state, RYZ_TOUCH_DOWN, 2);
    (void)read_event(&state, RYZ_TOUCH_DOWN, 2);
    CHECK(workbench_input_read(&state, NULL) == ESP_ERR_INVALID_ARG);
    CHECK(!state.read_valid && !state.read_pressed);
    (void)read_event(&state, RYZ_TOUCH_DOWN, 2);
    workbench_input_push(&state, NULL, ESP_ERR_TIMEOUT);
    CHECK(!state.read_valid && !state.read_pressed && state.wait_release);
    read_error(&state, ESP_ERR_TIMEOUT);
    push(&state, RYZ_TOUCH_MOVE, 3);
    ryz_touch_sample_t out = read_event(&state, RYZ_TOUCH_NONE, 3);
    check_neutral(&out);
    CHECK(state.wait_release && !state.read_pressed);
    push(&state, RYZ_TOUCH_UP, 4);
    out = read_event(&state, RYZ_TOUCH_NONE, 4);
    check_neutral(&out);
    push(&state, RYZ_TOUCH_MOVE, 5);
    (void)read_event(&state, RYZ_TOUCH_DOWN, 5);
    workbench_input_push(&state, NULL, ESP_OK);
    CHECK(!state.read_valid && state.wait_release);
    /* Even if the caller never observes the failure, old read history is gone. */
    push(&state, RYZ_TOUCH_UP, 6);
    (void)read_event(&state, RYZ_TOUCH_NONE, 6);
    push(&state, RYZ_TOUCH_DOWN, 7);
    (void)read_event(&state, RYZ_TOUCH_DOWN, 7);
    workbench_input_reset(&state);
    CHECK(state.queued && !state.read_valid && !state.read_pressed);
    workbench_input_set_event_mode(&state, false);
    read_error(&state, ESP_ERR_INVALID_STATE);
    push(&state, RYZ_TOUCH_MOVE, 8);
    out = read_event(&state, RYZ_TOUCH_NONE, 8);
    check_neutral(&out);
    CHECK(state.wait_release);
}

static void test_mode_epoch(void)
{
    workbench_input_t state;
    ready(&state);
    fill(&state);
    push(&state, RYZ_TOUCH_DOWN, 18);
    CHECK(state.wait_release && state.overflow_pending);
    workbench_input_set_event_mode(&state, false);
    CHECK(!state.queued && !state.count && !state.overflow_pending);
    CHECK(state.wait_release && !state.read_valid);
    ryz_touch_sample_t out = read_event(&state, RYZ_TOUCH_NONE, 18);
    check_neutral(&out);
    push(&state, RYZ_TOUCH_UP, 19);
    (void)read_event(&state, RYZ_TOUCH_NONE, 19);
    push(&state, RYZ_TOUCH_DOWN, 20);
    (void)read_event(&state, RYZ_TOUCH_DOWN, 20);
    workbench_input_set_event_mode(&state, false);
    CHECK(!state.read_valid && !state.wait_release);
    (void)read_event(&state, RYZ_TOUCH_DOWN, 20);
    workbench_input_set_event_mode(&state, true);
    CHECK(state.queued && !state.read_valid && !state.count);
    push(&state, RYZ_TOUCH_UP, 21);
    (void)read_event(&state, RYZ_TOUCH_UP, 21);
    push(&state, RYZ_TOUCH_DOWN, 22);
    CHECK(state.count == 1);
    workbench_input_set_event_mode(&state, false);
    CHECK(state.count == 0);
    (void)read_event(&state, RYZ_TOUCH_DOWN, 22);
    workbench_input_set_event_mode(NULL, false);
}

static void test_suppressed_autostart(void)
{
    for(unsigned queued=0;queued<2;++queued) {
        workbench_input_t state;
        workbench_input_reset(&state);
        workbench_input_set_event_mode(&state,queued!=0);
        const ryz_touch_sample_t released=sample(RYZ_TOUCH_NONE,100);
        workbench_input_suppress(&state,&released,ESP_OK);
        ryz_touch_sample_t out=read_event(&state,RYZ_TOUCH_NONE,100);
        check_neutral(&out);
        push(&state,RYZ_TOUCH_DOWN,101);
        out=read_event(&state,RYZ_TOUCH_DOWN,101);
        CHECK(out.pressed && out.has_position && !out.interrupted);
    }
}

static void test_suppressed_contact(void)
{
    for(unsigned queued=0;queued<2;++queued) {
        workbench_input_t state;
        workbench_input_reset(&state);
        workbench_input_set_event_mode(&state,queued!=0);
        push(&state,RYZ_TOUCH_NONE,1);
        push(&state,RYZ_TOUCH_DOWN,2);
        (void)read_event(&state,RYZ_TOUCH_DOWN,2);
        const ryz_touch_sample_t held=sample(RYZ_TOUCH_MOVE,3);
        workbench_input_suppress(&state,&held,ESP_OK);
        ryz_touch_sample_t out=read_event(&state,RYZ_TOUCH_NONE,3);
        CHECK(out.interrupted && !out.pressed && !out.has_position);
        out=read_event(&state,RYZ_TOUCH_NONE,3);
        check_neutral(&out);
        workbench_input_suppress(&state,&held,ESP_OK);
        out=read_event(&state,RYZ_TOUCH_NONE,3);
        check_neutral(&out); /* One cancellation, not one per filtered sample. */
        push(&state,RYZ_TOUCH_MOVE,4);
        out=read_event(&state,RYZ_TOUCH_NONE,4);
        check_neutral(&out); /* Same contact cannot re-arm a UI click. */
        const ryz_touch_sample_t released=sample(RYZ_TOUCH_UP,5);
        workbench_input_suppress(&state,&released,ESP_OK);
        out=read_event(&state,RYZ_TOUCH_NONE,5);
        check_neutral(&out); /* Physical UP is consumed, never an app click. */
        push(&state,RYZ_TOUCH_DOWN,6);
        out=read_event(&state,RYZ_TOUCH_DOWN,6);
        CHECK(out.pressed && !out.interrupted);
        push(&state,RYZ_TOUCH_UP,7);
        out=read_event(&state,RYZ_TOUCH_UP,7);
        CHECK(!out.pressed && !out.interrupted);
    }
}

static void test_suppressed_slow_reader(void)
{
    for(unsigned queued=0;queued<2;++queued) {
        workbench_input_t state;
        workbench_input_reset(&state);
        workbench_input_set_event_mode(&state,queued!=0);
        push(&state,RYZ_TOUCH_NONE,1);
        push(&state,RYZ_TOUCH_DOWN,2);
        (void)read_event(&state,RYZ_TOUCH_DOWN,2);
        push(&state,RYZ_TOUCH_MOVE,3); /* Not yet delivered; must be discarded. */
        const ryz_touch_sample_t released=sample(RYZ_TOUCH_UP,4);
        workbench_input_suppress(&state,&released,ESP_OK);
        push(&state,RYZ_TOUCH_NONE,5);
        push(&state,RYZ_TOUCH_DOWN,6); /* A new contact before the worker reads. */
        ryz_touch_sample_t out=read_event(&state,RYZ_TOUCH_NONE,4);
        CHECK(out.interrupted && !out.pressed && !out.has_position);
        out=read_event(&state,RYZ_TOUCH_DOWN,6);
        CHECK(out.pressed && out.has_position && !out.interrupted);
        push(&state,RYZ_TOUCH_UP,7);
        out=read_event(&state,RYZ_TOUCH_UP,7);
        CHECK(!out.interrupted);

        push(&state,RYZ_TOUCH_DOWN,8);
        (void)read_event(&state,RYZ_TOUCH_DOWN,8);
        const ryz_touch_sample_t cancelled_hold=sample(RYZ_TOUCH_MOVE,9);
        workbench_input_suppress(&state,&cancelled_hold,ESP_OK);
        push(&state,RYZ_TOUCH_MOVE,10);
        push(&state,RYZ_TOUCH_UP,11);
        out=read_event(&state,RYZ_TOUCH_NONE,9);
        CHECK(out.interrupted && !out.pressed && !out.has_position);
        out=read_event(&state,RYZ_TOUCH_NONE,11);
        check_neutral(&out); /* Never replay a post-cancel MOVE or UP. */
        push(&state,RYZ_TOUCH_DOWN,12);
        (void)read_event(&state,RYZ_TOUCH_DOWN,12);
        push(&state,RYZ_TOUCH_UP,13);
        (void)read_event(&state,RYZ_TOUCH_UP,13);

        workbench_input_reset(&state);
        workbench_input_set_event_mode(&state,queued!=0);
        const ryz_touch_sample_t held=sample(RYZ_TOUCH_DOWN,14);
        workbench_input_suppress(&state,&held,ESP_OK);
        out=read_event(&state,RYZ_TOUCH_NONE,14);
        check_neutral(&out); /* Boot/wake press was never delivered: no cancel. */
        push(&state,RYZ_TOUCH_MOVE,15);
        out=read_event(&state,RYZ_TOUCH_NONE,15);
        check_neutral(&out);
        push(&state,RYZ_TOUCH_UP,16);
        out=read_event(&state,RYZ_TOUCH_NONE,16);
        check_neutral(&out);
        push(&state,RYZ_TOUCH_DOWN,17);
        (void)read_event(&state,RYZ_TOUCH_DOWN,17);
    }
}

static void test_suppressed_errors(void)
{
    for(unsigned queued=0;queued<2;++queued) {
        workbench_input_t state;
        workbench_input_reset(&state);
        workbench_input_set_event_mode(&state,queued!=0);
        push(&state,RYZ_TOUCH_NONE,1);
        push(&state,RYZ_TOUCH_DOWN,2);
        (void)read_event(&state,RYZ_TOUCH_DOWN,2);
        const ryz_touch_sample_t held=sample(RYZ_TOUCH_MOVE,3);
        workbench_input_suppress(&state,&held,ESP_OK);
        workbench_input_suppress(&state,NULL,ESP_ERR_INVALID_RESPONSE);
        read_error(&state,ESP_ERR_INVALID_RESPONSE); /* Cancellation cannot mask IO. */
        workbench_input_suppress(&state,&held,ESP_OK);
        ryz_touch_sample_t out=read_event(&state,RYZ_TOUCH_NONE,3);
        CHECK(out.interrupted && !out.pressed);
        out=read_event(&state,RYZ_TOUCH_NONE,3);
        check_neutral(&out);
        workbench_input_suppress(&state,NULL,ESP_OK);
        read_error(&state,ESP_ERR_INVALID_ARG);
        const ryz_touch_sample_t released=sample(RYZ_TOUCH_NONE,4);
        workbench_input_suppress(&state,&released,ESP_OK);
        for(unsigned i=0;i<=WORKBENCH_INPUT_TIMEOUT_BUDGET;++i)
            workbench_input_suppress(&state,NULL,ESP_ERR_TIMEOUT);
        if(queued) {
            /* A subsequent successful policy sample must not hide exhaustion. */
            workbench_input_suppress(&state,&released,ESP_OK);
            read_error(&state,ESP_ERR_TIMEOUT);
        } else {
            read_error(&state,ESP_ERR_TIMEOUT);
            workbench_input_suppress(&state,&released,ESP_OK);
        }
        out=read_event(&state,RYZ_TOUCH_NONE,4);
        check_neutral(&out);
    }
    workbench_input_suppress(NULL,NULL,ESP_OK);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    if (!strcmp(argv[1], "reset")) test_reset();
    else if (!strcmp(argv[1], "fifo")) test_fifo();
    else if (!strcmp(argv[1], "wrap")) test_wrap();
    else if (!strcmp(argv[1], "overflow")) test_overflow();
    else if (!strcmp(argv[1], "error")) test_error();
    else if (!strcmp(argv[1], "overflow_release")) test_overflow_release();
    else if (!strcmp(argv[1], "error_overflow")) test_error_overflow();
    else if (!strcmp(argv[1], "protocol")) test_protocol();
    else if (!strcmp(argv[1], "current_no_queue")) test_current_no_queue();
    else if (!strcmp(argv[1], "current_events")) test_current_events();
    else if (!strcmp(argv[1], "current_history")) test_current_history();
    else if (!strcmp(argv[1], "mode_epoch")) test_mode_epoch();
    else if (!strcmp(argv[1], "suppressed_autostart")) test_suppressed_autostart();
    else if (!strcmp(argv[1], "suppressed_contact")) test_suppressed_contact();
    else if (!strcmp(argv[1], "suppressed_slow_reader")) test_suppressed_slow_reader();
    else if (!strcmp(argv[1], "suppressed_errors")) test_suppressed_errors();
    else CHECK(false);
    printf("WORKBENCH_INPUT_PASS: %s\n", argv[1]);
    return 0;
}
