#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

/* One synchronous borrowed call, executed only by the UI owner. This is a
 * trusted C executor, never a user-supplied function or Lua callback. */
typedef void (*ryz_workbench_io_fn)(void *argument);

typedef struct {
    atomic_int state;
    atomic_uint ticket;
    ryz_workbench_io_fn operation;
    void *argument;
} ryz_workbench_io_t;

/* Initialize before either task starts. At most one producer may own a ticket;
 * other submissions fail immediately. A caller retains all borrowed data until
 * completion AND acknowledgement. Cancellation never releases an in-flight
 * argument; the trusted operation decides how to honour cancellation safely. */
void ryz_workbench_io_init(ryz_workbench_io_t *channel);
uint32_t ryz_workbench_io_submit(ryz_workbench_io_t *channel,
                                ryz_workbench_io_fn operation, void *argument);
bool ryz_workbench_io_completed(const ryz_workbench_io_t *channel,
                                uint32_t ticket);
bool ryz_workbench_io_acknowledge(ryz_workbench_io_t *channel, uint32_t ticket);

/* UI-owner task only. Executes at most one call; never waits for a producer.
 * The call itself may take as long as its underlying native operation. */
bool ryz_workbench_io_dispatch(ryz_workbench_io_t *channel);
bool ryz_workbench_io_idle(const ryz_workbench_io_t *channel);
