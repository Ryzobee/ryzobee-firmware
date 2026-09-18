#include "workbench_boot_events.h"
#include "workbench_boot_key.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void sample(ryz_workbench_boot_events_t *s, bool pressed, uint32_t t)
{
    ryz_workbench_boot_events_sample(s, pressed, true, t);
}

static void expect_status(ryz_workbench_boot_events_t *s, ryz_boot_result_t result)
{
    ryz_boot_event_t event = {RYZ_BOOT_DOUBLE_CLICK, 123U, 456U};
    assert(ryz_workbench_boot_events_poll(s, &event) == result);
    assert(event.kind == RYZ_BOOT_DOUBLE_CLICK && event.timestamp_ms == 123U && event.held_ms == 456U);
}

static void expect_event(ryz_workbench_boot_events_t *s, ryz_boot_event_kind_t kind,
                         uint32_t timestamp, uint32_t held)
{
    ryz_boot_event_t event;
    assert(ryz_workbench_boot_events_poll(s, &event) == RYZ_BOOT_OK);
    assert(event.kind == kind && event.timestamp_ms == timestamp && event.held_ms == held);
    assert(held <= 3000U && (kind == RYZ_BOOT_LONG_PRESS || held < 3000U));
}

static void arm(ryz_workbench_boot_events_t *s, uint32_t start)
{
    ryz_workbench_boot_events_reset(s, true);
    sample(s, false, start);
    sample(s, false, start + 40U);
}

/* The final stable hold equals raw hold here because both edges debounce for
 * exactly 40 ms. Other cases deliberately exercise delayed owner sampling. */
static uint32_t tap(ryz_workbench_boot_events_t *s, uint32_t start, uint32_t hold)
{
    assert(hold >= 40U);
    sample(s, true, start);
    sample(s, true, start + 40U);
    sample(s, false, start + hold);
    sample(s, false, start + hold + 40U);
    return start + hold + 40U;
}

static void test_single(void)
{
    ryz_workbench_boot_events_t s; arm(&s, 0);
    uint32_t release = tap(&s, 100, 80);
    expect_status(&s, RYZ_BOOT_EMPTY);
    sample(&s, false, release + 349U); expect_status(&s, RYZ_BOOT_EMPTY);
    sample(&s, false, release + 350U); expect_event(&s, RYZ_BOOT_CLICK, release + 350U, 80);
    sample(&s, false, 5000); expect_status(&s, RYZ_BOOT_EMPTY);
}

static void test_double_boundary(void)
{
    for (uint32_t gap = 349U; gap <= 351U; ++gap) {
        ryz_workbench_boot_events_t s; arm(&s, 0);
        uint32_t release = tap(&s, 100, 80);
        uint32_t second = release + gap - 40U;
        sample(&s, true, second);
        sample(&s, true, second + 40U);
        if (gap <= 350U) expect_status(&s, RYZ_BOOT_EMPTY);
        else expect_event(&s, RYZ_BOOT_CLICK, second + 40U, 80);
        sample(&s, false, second + 100U);
        sample(&s, false, second + 140U);
        if (gap <= 350U) expect_event(&s, RYZ_BOOT_DOUBLE_CLICK, second + 140U, 100);
        else {
            expect_status(&s, RYZ_BOOT_EMPTY);
            sample(&s, false, second + 490U);
            expect_event(&s, RYZ_BOOT_CLICK, second + 490U, 100);
        }
        sample(&s, false, 2000); expect_status(&s, RYZ_BOOT_EMPTY);
    }
}

static void test_double_debounce_window(void)
{
    ryz_workbench_boot_events_t s; arm(&s, 0);
    uint32_t release = tap(&s, 100, 80);
    /* Raw press is inside 350 ms; stable press is not. No late double. */
    sample(&s, true, release + 340U);
    sample(&s, true, release + 350U);
    expect_event(&s, RYZ_BOOT_CLICK, release + 350U, 80);
    sample(&s, true, release + 380U);
    sample(&s, false, release + 440U);
    sample(&s, false, release + 480U);
    sample(&s, false, release + 830U);
    expect_event(&s, RYZ_BOOT_CLICK, release + 830U, 100);
    expect_status(&s, RYZ_BOOT_EMPTY);
}

static void test_bounce(void)
{
    ryz_workbench_boot_events_t s; arm(&s, 0);
    sample(&s, true, 100); sample(&s, true, 134); sample(&s, false, 135);
    sample(&s, false, 175); sample(&s, false, 600); expect_status(&s, RYZ_BOOT_EMPTY);
    sample(&s, true, 700); sample(&s, false, 720);
    sample(&s, true, 735); sample(&s, true, 774); sample(&s, true, 775);
    sample(&s, false, 800); sample(&s, true, 820); sample(&s, false, 830);
    sample(&s, false, 869); sample(&s, false, 870);
    sample(&s, false, 1220); expect_event(&s, RYZ_BOOT_CLICK, 1220, 95);
    expect_status(&s, RYZ_BOOT_EMPTY);
}

static void test_long(void)
{
    ryz_workbench_boot_events_t s; arm(&s, 0);
    sample(&s, true, 100); sample(&s, true, 140);
    sample(&s, true, 3139); expect_status(&s, RYZ_BOOT_EMPTY);
    sample(&s, true, 3140); expect_event(&s, RYZ_BOOT_LONG_PRESS, 3140, 3000);
    sample(&s, true, 10000); expect_status(&s, RYZ_BOOT_EMPTY);
    sample(&s, false, 10020); sample(&s, false, 10060);
    sample(&s, false, 20000); expect_status(&s, RYZ_BOOT_EMPTY);
    uint32_t release = tap(&s, 21000, 60);
    sample(&s, false, release + 350U); expect_event(&s, RYZ_BOOT_CLICK, release + 350U, 60);
}

static void test_long_release_boundary(void)
{
    for (uint32_t held = 2999U; held <= 3001U; ++held) {
        ryz_workbench_boot_events_t s; arm(&s, 0);
        sample(&s, true, 100); sample(&s, true, 140);
        uint32_t raw_release = 140U + held - 40U;
        sample(&s, false, raw_release);
        sample(&s, false, raw_release + 39U); expect_status(&s, RYZ_BOOT_EMPTY);
        sample(&s, false, raw_release + 40U);
        if (held >= 3000U) expect_event(&s, RYZ_BOOT_LONG_PRESS, 140U + held, 3000);
        else {
            expect_status(&s, RYZ_BOOT_EMPTY);
            sample(&s, false, 140U + held + 350U);
            expect_event(&s, RYZ_BOOT_CLICK, 140U + held + 350U, held);
        }
        sample(&s, false, 5000); expect_status(&s, RYZ_BOOT_EMPTY);
    }
    ryz_workbench_boot_events_t s; arm(&s, 0);
    sample(&s, true, 100); sample(&s, true, 140);
    sample(&s, false, 3130); sample(&s, false, 3140);
    expect_status(&s, RYZ_BOOT_EMPTY); /* tentative release crossing long time */
    sample(&s, false, 3170); expect_event(&s, RYZ_BOOT_LONG_PRESS, 3170, 3000);
}

static void test_second_long(void)
{
    ryz_workbench_boot_events_t s; arm(&s, 0);
    (void)tap(&s, 100, 80);
    sample(&s, true, 300); sample(&s, true, 340);
    sample(&s, true, 3339); expect_status(&s, RYZ_BOOT_EMPTY);
    sample(&s, true, 3340); expect_event(&s, RYZ_BOOT_LONG_PRESS, 3340, 3000);
    sample(&s, false, 4000); sample(&s, false, 4040); sample(&s, false, 5000);
    expect_status(&s, RYZ_BOOT_EMPTY);
}

static void test_delayed_sampling(void)
{
    ryz_workbench_boot_events_t s; arm(&s, 0);
    sample(&s, true, 100); sample(&s, true, 140);
    sample(&s, false, 5000); expect_status(&s, RYZ_BOOT_EMPTY);
    sample(&s, false, 5040); expect_event(&s, RYZ_BOOT_LONG_PRESS, 5040, 3000);
    sample(&s, false, 5500); expect_status(&s, RYZ_BOOT_EMPTY);
    arm(&s, 0);
    sample(&s, true, 100); sample(&s, true, 140);
    sample(&s, true, 5000); expect_event(&s, RYZ_BOOT_LONG_PRESS, 5000, 3000);
    sample(&s, false, 5010); sample(&s, false, 5050);
    sample(&s, false, 6000); expect_status(&s, RYZ_BOOT_EMPTY);
}

static void test_triple(void)
{
    ryz_workbench_boot_events_t s; arm(&s, 0);
    (void)tap(&s, 100, 80);
    uint32_t second_release = tap(&s, 300, 80);
    expect_event(&s, RYZ_BOOT_DOUBLE_CLICK, second_release, 80);
    uint32_t third_release = tap(&s, 500, 80);
    sample(&s, false, third_release + 350U);
    expect_event(&s, RYZ_BOOT_CLICK, third_release + 350U, 80);
    expect_status(&s, RYZ_BOOT_EMPTY);
}

static void test_wrap(void)
{
    ryz_workbench_boot_events_t s;
    uint32_t start = UINT32_MAX - 200U;
    arm(&s, start);
    uint32_t release = tap(&s, start + 100U, 80);
    sample(&s, false, release + 350U);
    expect_event(&s, RYZ_BOOT_CLICK, release + 350U, 80);
    arm(&s, start);
    (void)tap(&s, start + 100U, 80);
    release = tap(&s, start + 300U, 80);
    expect_event(&s, RYZ_BOOT_DOUBLE_CLICK, release, 80);
    arm(&s, start);
    sample(&s, true, start + 100U); sample(&s, true, start + 140U);
    sample(&s, true, start + 3140U);
    expect_event(&s, RYZ_BOOT_LONG_PRESS, start + 3140U, 3000);
    expect_status(&s, RYZ_BOOT_EMPTY);
}

static uint32_t enqueue_click(ryz_workbench_boot_events_t *s, unsigned index)
{
    uint32_t release = tap(s, 100U + index * 600U, 80);
    sample(s, false, release + 350U);
    return release + 350U;
}

static void test_fifo(void)
{
    ryz_workbench_boot_events_t s; arm(&s, 0);
    for (unsigned i = 0; i < 8; ++i) (void)enqueue_click(&s, i);
    for (unsigned i = 0; i < 4; ++i) expect_event(&s, RYZ_BOOT_CLICK, 570U + i * 600U, 80);
    for (unsigned i = 8; i < 12; ++i) (void)enqueue_click(&s, i);
    for (unsigned i = 4; i < 12; ++i) expect_event(&s, RYZ_BOOT_CLICK, 570U + i * 600U, 80);
    expect_status(&s, RYZ_BOOT_EMPTY);
}

static void test_overflow(void)
{
    ryz_workbench_boot_events_t s; arm(&s, 0);
    for (unsigned i = 0; i < 9; ++i) (void)enqueue_click(&s, i);
    expect_status(&s, RYZ_BOOT_OVERFLOW); expect_status(&s, RYZ_BOOT_EMPTY);
    sample(&s, true, 6000); sample(&s, true, 12000); expect_status(&s, RYZ_BOOT_EMPTY);
    sample(&s, false, 12020); sample(&s, false, 12059);
    sample(&s, true, 12060); sample(&s, false, 12080); sample(&s, false, 12120);
    uint32_t release = tap(&s, 12200, 80);
    sample(&s, false, release + 350U); expect_event(&s, RYZ_BOOT_CLICK, release + 350U, 80);
    expect_status(&s, RYZ_BOOT_EMPTY);
}

static void test_overflow_delayed_poll(void)
{
    ryz_workbench_boot_events_t s; arm(&s, 0);
    for (unsigned i = 0; i < 9; ++i) (void)enqueue_click(&s, i);
    sample(&s, false, 6000); sample(&s, false, 6040);
    uint32_t release = tap(&s, 6100, 80);
    sample(&s, false, release + 350U);
    sample(&s, true, 7000); sample(&s, true, 7040);
    expect_status(&s, RYZ_BOOT_OVERFLOW);
    sample(&s, true, 11000); expect_status(&s, RYZ_BOOT_EMPTY);
    sample(&s, false, 11020); sample(&s, false, 11060);
    release = tap(&s, 11100, 80);
    sample(&s, false, release + 350U);
    expect_event(&s, RYZ_BOOT_CLICK, release + 350U, 80);
    expect_status(&s, RYZ_BOOT_EMPTY);
}

static void test_error(void)
{
    ryz_workbench_boot_events_t s; arm(&s, 0);
    (void)enqueue_click(&s, 0);
    sample(&s, true, 800); sample(&s, true, 840);
    ryz_workbench_boot_events_sample(&s, true, false, 900);
    expect_status(&s, RYZ_BOOT_FAILED); expect_status(&s, RYZ_BOOT_FAILED);
    sample(&s, true, 9000); expect_status(&s, RYZ_BOOT_FAILED);
    sample(&s, false, 9020); sample(&s, false, 9059); expect_status(&s, RYZ_BOOT_FAILED);
    sample(&s, false, 9060); expect_status(&s, RYZ_BOOT_EMPTY);
    uint32_t release = tap(&s, 9200, 80);
    sample(&s, false, release + 350U); expect_event(&s, RYZ_BOOT_CLICK, release + 350U, 80);
    release = tap(&s, 10000, 80);
    ryz_workbench_boot_events_sample(&s, false, false, release + 1U);
    sample(&s, false, release + 2U); sample(&s, false, release + 42U);
    sample(&s, false, release + 500U); expect_status(&s, RYZ_BOOT_EMPTY);
}

static void test_reset_held(void)
{
    ryz_workbench_boot_events_t s;
    ryz_workbench_boot_events_reset(&s, true);
    sample(&s, true, 0); sample(&s, true, 10000); expect_status(&s, RYZ_BOOT_EMPTY);
    sample(&s, false, 10020); sample(&s, false, 10060);
    (void)tap(&s, 10100, 80); /* pending click must not cross reset */
    ryz_workbench_boot_events_reset(&s, true);
    sample(&s, false, 11000); sample(&s, false, 11040); sample(&s, false, 11500);
    expect_status(&s, RYZ_BOOT_EMPTY);
    sample(&s, true, 12000); sample(&s, true, 12040);
    ryz_workbench_boot_events_reset(&s, true);
    sample(&s, true, 13000); sample(&s, true, 19000); expect_status(&s, RYZ_BOOT_EMPTY);
    sample(&s, false, 19020); sample(&s, false, 19060);
    uint32_t release = tap(&s, 19100, 80);
    sample(&s, false, release + 350U); expect_event(&s, RYZ_BOOT_CLICK, release + 350U, 80);
    arm(&s, 0);
    for (unsigned i = 0; i < 8; ++i) (void)enqueue_click(&s, i);
    ryz_workbench_boot_events_reset(&s, true);
    expect_status(&s, RYZ_BOOT_EMPTY);
    sample(&s, false, 6000); sample(&s, false, 6040);
    release = tap(&s, 6100, 80);
    sample(&s, false, release + 350U);
    expect_event(&s, RYZ_BOOT_CLICK, release + 350U, 80);
    expect_status(&s, RYZ_BOOT_EMPTY);
}

static void test_release_guard(void)
{
    ryz_workbench_boot_events_t s;
    ryz_workbench_boot_events_reset(&s, true);
    sample(&s, false, 0); sample(&s, false, 39);
    sample(&s, true, 40); sample(&s, true, 5000); expect_status(&s, RYZ_BOOT_EMPTY);
    sample(&s, false, 5020); sample(&s, false, 5060);
    uint32_t release = tap(&s, 5100, 40);
    sample(&s, false, release + 350U); expect_event(&s, RYZ_BOOT_CLICK, release + 350U, 40);
}

static void test_unavailable(void)
{
    ryz_workbench_boot_events_t s;
    ryz_workbench_boot_events_reset(&s, false);
    sample(&s, false, 0); sample(&s, false, 40);
    (void)tap(&s, 100, 80); sample(&s, false, 1000);
    expect_status(&s, RYZ_BOOT_UNAVAILABLE);
    ryz_workbench_boot_events_sample(&s, false, false, 2000);
    expect_status(&s, RYZ_BOOT_UNAVAILABLE);
    ryz_workbench_boot_events_reset(NULL, true);
    ryz_workbench_boot_events_sample(NULL, true, false, 0);
    ryz_boot_event_t event;
    assert(ryz_workbench_boot_events_poll(NULL, &event) == RYZ_BOOT_FAILED);
    assert(ryz_workbench_boot_events_poll(&s, NULL) == RYZ_BOOT_FAILED);
}

static void test_system_exit_independent(void)
{
    ryz_workbench_boot_events_t events; arm(&events, 0);
    ryz_workbench_boot_key_t exit_key; ryz_workbench_boot_key_reset(&exit_key);
    unsigned exits = 0, long_events = 0;
    for (uint32_t t = 60; t <= 6000; t += 20) {
        bool pressed = t >= 100 && t < 5500;
        sample(&events, pressed, t);
        if (ryz_workbench_boot_key_update(&exit_key, pressed, t)) {
            ++exits; assert(t == 5140U);
        }
        ryz_boot_event_t event;
        ryz_boot_result_t result = ryz_workbench_boot_events_poll(&events, &event);
        if (result == RYZ_BOOT_OK) {
            ++long_events;
            assert(event.kind == RYZ_BOOT_LONG_PRESS && event.timestamp_ms == 3140U && event.held_ms == 3000U);
        } else assert(result == RYZ_BOOT_EMPTY);
    }
    assert(exits == 1 && long_events == 1);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    struct { const char *name; void (*run)(void); } cases[] = {
        {"single", test_single}, {"double_boundary", test_double_boundary},
        {"double_debounce_window", test_double_debounce_window}, {"bounce", test_bounce},
        {"long", test_long}, {"long_release_boundary", test_long_release_boundary},
        {"delayed_sampling", test_delayed_sampling},
        {"second_long", test_second_long}, {"triple", test_triple}, {"wrap", test_wrap},
        {"fifo", test_fifo}, {"overflow", test_overflow},
        {"overflow_delayed_poll", test_overflow_delayed_poll}, {"error", test_error},
        {"reset_held", test_reset_held}, {"release_guard", test_release_guard},
        {"unavailable", test_unavailable}, {"system_exit", test_system_exit_independent},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (!strcmp(argv[1], cases[i].name)) {
            cases[i].run(); printf("BOOT_EVENTS_PASS %s\n", argv[1]); return 0;
        }
    }
    return 2;
}
