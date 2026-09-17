#include "workbench_io.h"

#include <stddef.h>

enum { IDLE, WRITING, PENDING, EXECUTING, COMPLETED, ACKNOWLEDGING };

void ryz_workbench_io_init(ryz_workbench_io_t *channel)
{
    atomic_init(&channel->state, IDLE);
    atomic_init(&channel->ticket, 0);
    channel->operation = NULL;
    channel->argument = NULL;
}

uint32_t ryz_workbench_io_submit(ryz_workbench_io_t *channel,
                                ryz_workbench_io_fn operation, void *argument)
{
    if (!channel || !operation) return 0;
    int expected = IDLE;
    if (!atomic_compare_exchange_strong(&channel->state, &expected, WRITING)) {
        return 0;
    }
    uint32_t ticket = atomic_load(&channel->ticket) + 1;
    if (!ticket) ticket = 1;
    atomic_store(&channel->ticket, ticket);
    channel->operation = operation;
    channel->argument = argument;
    atomic_store_explicit(&channel->state, PENDING, memory_order_release);
    return ticket;
}

bool ryz_workbench_io_dispatch(ryz_workbench_io_t *channel)
{
    if (!channel) return false;
    int expected = PENDING;
    if (!atomic_compare_exchange_strong(&channel->state, &expected, EXECUTING)) {
        return false;
    }
    channel->operation(channel->argument);
    atomic_store_explicit(&channel->state, COMPLETED, memory_order_release);
    return true;
}

bool ryz_workbench_io_completed(const ryz_workbench_io_t *channel,
                                uint32_t ticket)
{
    return channel && ticket != 0 &&
        atomic_load_explicit(&channel->state, memory_order_acquire) == COMPLETED &&
        atomic_load(&channel->ticket) == ticket;
}

bool ryz_workbench_io_acknowledge(ryz_workbench_io_t *channel, uint32_t ticket)
{
    if (!channel || !ticket) return false;
    int expected = COMPLETED;
    if (!atomic_compare_exchange_strong(&channel->state, &expected, ACKNOWLEDGING)) {
        return false;
    }
    if (atomic_load(&channel->ticket) != ticket) {
        atomic_store_explicit(&channel->state, COMPLETED, memory_order_release);
        return false;
    }
    channel->operation = NULL;
    channel->argument = NULL;
    atomic_store_explicit(&channel->state, IDLE, memory_order_release);
    return true;
}

bool ryz_workbench_io_idle(const ryz_workbench_io_t *channel)
{
    return channel &&
        atomic_load_explicit(&channel->state, memory_order_acquire) == IDLE;
}
