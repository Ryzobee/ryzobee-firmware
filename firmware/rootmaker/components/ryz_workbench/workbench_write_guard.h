#pragma once

#include <stdbool.h>

/* Trusted native writers share the Job owner's admission gate. Acquire only
 * immediately around synchronous Store mutation, never around HTTP receive,
 * response transmission or UI work. No Job/Controller mutex spans Store I/O.
 *
 * A successful begin reserves the writer slot atomically with every final
 * Job admission and must be paired with exactly one end, including Store
 * errors/unknown commit. A rejected begin owns nothing and must not call end.
 * The callbacks are mandatory for mutation; NULL must fail closed. They may
 * not reenter the calling controller. Reads/prepared Job snapshots do not
 * acquire this lease; the Job repeats final admission after preparation. */
typedef struct {
    void *context;
    bool (*try_begin_write)(void *context);
    void (*end_write)(void *context);
} ryz_workbench_write_guard_t;
