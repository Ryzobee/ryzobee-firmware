#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "workbench_io.h"

/* No replacement implementation or platform stub: every channel transition
 * below executes workbench_io.c. A condition-variable gate controls the real
 * dispatch thread and the callback's blocked interval, without timed sleeps. */
typedef struct {
    ryz_workbench_io_t channel;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    pthread_t thread;
    bool ready;
    bool stop;
    unsigned permits;
    unsigned cycles;
    unsigned calls;
    unsigned producers_ready;
    bool release_producers;
    bool last_dispatched;
} fixture_t;

typedef struct {
    fixture_t *fixture;
    atomic_bool cancelled;
    bool cleanup;
    bool block;
    bool entered;
    bool release;
    unsigned calls;
    uint32_t input;
    uint32_t output;
    uint32_t checksum;
} payload_t;

static void lock(fixture_t *fixture)
{
    assert(pthread_mutex_lock(&fixture->mutex) == 0);
}

static void unlock(fixture_t *fixture)
{
    assert(pthread_mutex_unlock(&fixture->mutex) == 0);
}

static void signal_changed(fixture_t *fixture)
{
    assert(pthread_cond_broadcast(&fixture->changed) == 0);
}

static void await_changed(fixture_t *fixture)
{
    struct timespec limit;
    assert(clock_gettime(CLOCK_REALTIME, &limit) == 0);
    limit.tv_sec += 5;
    assert(pthread_cond_timedwait(&fixture->changed, &fixture->mutex, &limit) == 0);
}

static void *owner_entry(void *argument)
{
    fixture_t *fixture = argument;
    lock(fixture);
    fixture->ready = true;
    signal_changed(fixture);
    for (;;) {
        while (!fixture->stop && !fixture->permits) await_changed(fixture);
        if (fixture->stop) break;
        --fixture->permits;
        unlock(fixture);
        bool dispatched = ryz_workbench_io_dispatch(&fixture->channel);
        lock(fixture);
        fixture->last_dispatched = dispatched;
        ++fixture->cycles;
        signal_changed(fixture);
    }
    unlock(fixture);
    return NULL;
}

static void fixture_init(fixture_t *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    ryz_workbench_io_init(&fixture->channel);
    assert(pthread_mutex_init(&fixture->mutex, NULL) == 0);
    assert(pthread_cond_init(&fixture->changed, NULL) == 0);
    assert(pthread_create(&fixture->thread, NULL, owner_entry, fixture) == 0);
    lock(fixture);
    while (!fixture->ready) await_changed(fixture);
    unlock(fixture);
}

static void fixture_destroy(fixture_t *fixture)
{
    assert(ryz_workbench_io_idle(&fixture->channel));
    lock(fixture);
    fixture->stop = true;
    signal_changed(fixture);
    unlock(fixture);
    assert(pthread_join(fixture->thread, NULL) == 0);
    assert(pthread_cond_destroy(&fixture->changed) == 0);
    assert(pthread_mutex_destroy(&fixture->mutex) == 0);
}

static unsigned permit_dispatch(fixture_t *fixture)
{
    lock(fixture);
    unsigned cycle = fixture->cycles + 1;
    ++fixture->permits;
    signal_changed(fixture);
    unlock(fixture);
    return cycle;
}

static bool await_cycle(fixture_t *fixture, unsigned cycle)
{
    lock(fixture);
    while (fixture->cycles < cycle) await_changed(fixture);
    bool dispatched = fixture->last_dispatched;
    unlock(fixture);
    return dispatched;
}

static void payload_init(payload_t *payload, fixture_t *fixture, uint32_t input)
{
    memset(payload, 0, sizeof(*payload));
    payload->fixture = fixture;
    payload->input = input;
    atomic_init(&payload->cancelled, false);
}

static void execute_payload(void *argument)
{
    payload_t *payload = argument;
    fixture_t *fixture = payload->fixture;
    assert(pthread_equal(pthread_self(), fixture->thread));
    lock(fixture);
    ++fixture->calls;
    ++payload->calls;
    payload->entered = true;
    signal_changed(fixture);
    while (payload->block && !payload->release) await_changed(fixture);
    unlock(fixture);

    /* This mirrors the protocol's responsibility boundary, not the actual
     * runtime adapter: cancellation is inspected by the operation, and a
     * mandatory cleanup operation ignores it. Writes occur after the gate is
     * unlocked so visibility tests rely on channel completion publication. */
    payload->output = atomic_load(&payload->cancelled) && !payload->cleanup
        ? UINT32_MAX : payload->input ^ UINT32_C(0xa55aa55a);
    payload->checksum = payload->output + payload->input;
}

static void await_entered(payload_t *payload)
{
    lock(payload->fixture);
    while (!payload->entered) await_changed(payload->fixture);
    unlock(payload->fixture);
}

static void release_payload(payload_t *payload)
{
    lock(payload->fixture);
    payload->release = true;
    signal_changed(payload->fixture);
    unlock(payload->fixture);
}

static void await_api_completion(fixture_t *fixture, uint32_t ticket)
{
    /* Deliberately observe the public completion API before awaiting the
     * owner's cycle condition variable: that mutex must not accidentally be
     * the synchronization which makes the borrowed result readable. */
    struct timespec start;
    assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    while (!ryz_workbench_io_completed(&fixture->channel, ticket)) {
        struct timespec now;
        assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        assert(now.tv_sec - start.tv_sec < 5);
        sched_yield();
    }
}

static void complete_and_ack(fixture_t *fixture, uint32_t ticket)
{
    assert(ticket != 0);
    assert(await_cycle(fixture, permit_dispatch(fixture)));
    assert(ryz_workbench_io_completed(&fixture->channel, ticket));
    assert(ryz_workbench_io_acknowledge(&fixture->channel, ticket));
    assert(ryz_workbench_io_idle(&fixture->channel));
}

static void test_invalid(void)
{
    fixture_t fixture;
    fixture_init(&fixture);
    assert(ryz_workbench_io_idle(&fixture.channel));
    assert(!ryz_workbench_io_idle(NULL));
    assert(!ryz_workbench_io_dispatch(NULL));
    assert(!ryz_workbench_io_completed(NULL, 1));
    assert(!ryz_workbench_io_completed(&fixture.channel, 0));
    assert(!ryz_workbench_io_acknowledge(NULL, 1));
    assert(!ryz_workbench_io_acknowledge(&fixture.channel, 0));
    assert(!ryz_workbench_io_acknowledge(&fixture.channel, 1));
    assert(!ryz_workbench_io_submit(NULL, execute_payload, NULL));
    assert(!ryz_workbench_io_submit(&fixture.channel, NULL, NULL));
    assert(!await_cycle(&fixture, permit_dispatch(&fixture)));
    assert(fixture.calls == 0);
    fixture_destroy(&fixture);
}

typedef struct { payload_t payload; uint32_t ticket; } producer_t;

static void *producer_entry(void *argument)
{
    producer_t *producer = argument;
    fixture_t *fixture = producer->payload.fixture;
    lock(fixture);
    ++fixture->producers_ready;
    signal_changed(fixture);
    while (!fixture->release_producers) await_changed(fixture);
    unlock(fixture);
    producer->ticket = ryz_workbench_io_submit(
        &fixture->channel, execute_payload, &producer->payload);
    return NULL;
}

static void test_producers(void)
{
    enum { PRODUCERS = 16 };
    fixture_t fixture;
    producer_t producers[PRODUCERS];
    pthread_t threads[PRODUCERS];
    fixture_init(&fixture);
    for (unsigned i = 0; i < PRODUCERS; ++i) {
        payload_init(&producers[i].payload, &fixture, i + 1);
        producers[i].ticket = 0;
        assert(pthread_create(&threads[i], NULL, producer_entry, &producers[i]) == 0);
    }
    lock(&fixture);
    while (fixture.producers_ready != PRODUCERS) await_changed(&fixture);
    fixture.release_producers = true;
    signal_changed(&fixture);
    unlock(&fixture);
    unsigned successes = 0;
    unsigned winner = 0;
    for (unsigned i = 0; i < PRODUCERS; ++i) {
        assert(pthread_join(threads[i], NULL) == 0);
        if (producers[i].ticket) { ++successes; winner = i; }
        assert(producers[i].payload.calls == 0);
    }
    assert(successes == 1);
    assert(fixture.calls == 0); /* A producer never executes its callback. */
    complete_and_ack(&fixture, producers[winner].ticket);
    assert(fixture.calls == 1);
    for (unsigned i = 0; i < PRODUCERS; ++i) {
        assert(producers[i].payload.calls == (unsigned)(i == winner));
    }
    fixture_destroy(&fixture);
}

static void test_borrowed_cancel(void)
{
    fixture_t fixture;
    payload_t payload;
    payload_t later;
    fixture_init(&fixture);
    payload_init(&payload, &fixture, 41);
    payload_init(&later, &fixture, 42);
    payload.block = true;
    uint32_t ticket = ryz_workbench_io_submit(&fixture.channel, execute_payload, &payload);
    unsigned cycle = permit_dispatch(&fixture);
    await_entered(&payload);
    atomic_store(&payload.cancelled, true);
    assert(!ryz_workbench_io_completed(&fixture.channel, ticket));
    assert(!ryz_workbench_io_acknowledge(&fixture.channel, ticket));
    assert(!ryz_workbench_io_idle(&fixture.channel));
    assert(!ryz_workbench_io_submit(&fixture.channel, execute_payload, &later));
    assert(!ryz_workbench_io_dispatch(&fixture.channel));
    assert(payload.output == 0); /* Borrowed storage remains in-flight. */
    release_payload(&payload);
    await_api_completion(&fixture, ticket);
    assert(payload.output == UINT32_MAX);
    assert(payload.calls == 1);
    assert(!ryz_workbench_io_submit(&fixture.channel, execute_payload, &later));
    assert(await_cycle(&fixture, cycle));
    assert(ryz_workbench_io_acknowledge(&fixture.channel, ticket));
    uint32_t next = ryz_workbench_io_submit(&fixture.channel, execute_payload, &later);
    assert(next != ticket);
    complete_and_ack(&fixture, next);
    assert(later.calls == 1 && payload.calls == 1);
    fixture_destroy(&fixture);
}

static void test_done_ack(void)
{
    fixture_t fixture;
    payload_t payload;
    fixture_init(&fixture);
    payload_init(&payload, &fixture, 12);
    uint32_t ticket = ryz_workbench_io_submit(&fixture.channel, execute_payload, &payload);
    assert(!ryz_workbench_io_acknowledge(&fixture.channel, ticket));
    assert(await_cycle(&fixture, permit_dispatch(&fixture)));
    assert(ryz_workbench_io_completed(&fixture.channel, ticket));
    assert(!ryz_workbench_io_idle(&fixture.channel));
    assert(!ryz_workbench_io_acknowledge(&fixture.channel, ticket + 1));
    assert(ryz_workbench_io_completed(&fixture.channel, ticket));
    assert(!ryz_workbench_io_submit(&fixture.channel, execute_payload, &payload));
    assert(!await_cycle(&fixture, permit_dispatch(&fixture)));
    assert(payload.calls == 1);
    assert(ryz_workbench_io_acknowledge(&fixture.channel, ticket));
    assert(!ryz_workbench_io_completed(&fixture.channel, ticket));
    assert(!ryz_workbench_io_acknowledge(&fixture.channel, ticket));
    assert(!await_cycle(&fixture, permit_dispatch(&fixture)));
    assert(payload.calls == 1);
    fixture_destroy(&fixture);
}

static void test_stale_ticket(void)
{
    fixture_t fixture;
    payload_t payload;
    fixture_init(&fixture);
    payload_init(&payload, &fixture, 33);
    uint32_t old = ryz_workbench_io_submit(&fixture.channel, execute_payload, &payload);
    complete_and_ack(&fixture, old);
    uint32_t next = ryz_workbench_io_submit(&fixture.channel, execute_payload, &payload);
    assert(next != old);
    assert(!ryz_workbench_io_completed(&fixture.channel, old));
    assert(!ryz_workbench_io_acknowledge(&fixture.channel, old));
    assert(await_cycle(&fixture, permit_dispatch(&fixture)));
    assert(!ryz_workbench_io_acknowledge(&fixture.channel, old));
    assert(!ryz_workbench_io_completed(&fixture.channel, old));
    assert(ryz_workbench_io_completed(&fixture.channel, next));
    assert(!ryz_workbench_io_submit(&fixture.channel, execute_payload, &payload));
    assert(ryz_workbench_io_acknowledge(&fixture.channel, next));
    assert(payload.calls == 2);
    fixture_destroy(&fixture);
}

static void test_cleanup(void)
{
    fixture_t fixture;
    payload_t payload;
    fixture_init(&fixture);
    payload_init(&payload, &fixture, 71);
    payload.cleanup = true;
    atomic_store(&payload.cancelled, true);
    uint32_t ticket = ryz_workbench_io_submit(&fixture.channel, execute_payload, &payload);
    assert(ticket);
    assert(!ryz_workbench_io_completed(&fixture.channel, ticket));
    complete_and_ack(&fixture, ticket);
    assert(payload.calls == 1);
    assert(payload.output == (payload.input ^ UINT32_C(0xa55aa55a)));
    fixture_destroy(&fixture);
}

static void test_visibility(void)
{
    fixture_t fixture;
    payload_t payload;
    fixture_init(&fixture);
    uint32_t previous = 0;
    for (uint32_t round = 1; round <= 2048; ++round) {
        payload_init(&payload, &fixture, round * UINT32_C(65537));
        uint32_t ticket = ryz_workbench_io_submit(&fixture.channel, execute_payload, &payload);
        assert(ticket && ticket != previous);
        unsigned cycle = permit_dispatch(&fixture);
        await_entered(&payload);
        await_api_completion(&fixture, ticket);
        assert(payload.output == (payload.input ^ UINT32_C(0xa55aa55a)));
        assert(payload.checksum == payload.output + payload.input);
        assert(payload.calls == 1);
        assert(await_cycle(&fixture, cycle));
        assert(ryz_workbench_io_acknowledge(&fixture.channel, ticket));
        assert(!ryz_workbench_io_completed(&fixture.channel, ticket));
        /* Returning/replacing the borrowed payload is safe only now. */
        memset(&payload, 0, sizeof(payload));
        assert(!await_cycle(&fixture, permit_dispatch(&fixture)));
        previous = ticket;
    }
    assert(fixture.calls == 2048);
    fixture_destroy(&fixture);
}

static void test_wrap(void)
{
    fixture_t fixture;
    payload_t payload;
    fixture_init(&fixture);
    payload_init(&payload, &fixture, 1);
    /* Seed only the public atomic counter while idle, to reach the wrap
     * boundary without billions of calls. State transitions remain real. */
    atomic_store(&fixture.channel.ticket, UINT32_MAX - 1);
    uint32_t last = ryz_workbench_io_submit(&fixture.channel, execute_payload, &payload);
    assert(last == UINT32_MAX);
    complete_and_ack(&fixture, last);
    uint32_t first = ryz_workbench_io_submit(&fixture.channel, execute_payload, &payload);
    assert(first == 1);
    assert(!ryz_workbench_io_acknowledge(&fixture.channel, last));
    complete_and_ack(&fixture, first);
    fixture_destroy(&fixture);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "invalid")) test_invalid();
    else if (!strcmp(argv[1], "producers")) test_producers();
    else if (!strcmp(argv[1], "borrowed_cancel")) test_borrowed_cancel();
    else if (!strcmp(argv[1], "done_ack")) test_done_ack();
    else if (!strcmp(argv[1], "stale_ticket")) test_stale_ticket();
    else if (!strcmp(argv[1], "cleanup")) test_cleanup();
    else if (!strcmp(argv[1], "visibility")) test_visibility();
    else if (!strcmp(argv[1], "wrap")) test_wrap();
    else abort();
    printf("WORKBENCH_IO_PASS %s\n", argv[1]);
    return 0;
}
