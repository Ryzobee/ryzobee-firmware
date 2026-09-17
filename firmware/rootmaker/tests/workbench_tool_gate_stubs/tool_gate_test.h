#pragma once
#include <assert.h>
#include "ryz_tools.h"

/* External-gate Adapter only. This proves RPC callback placement and release,
 * not real broker ownership/concurrency; broker behavior has a separate suite. */
extern unsigned test_gate_calls;
extern bool test_gate_active;
extern esp_err_t test_gate_error;
static inline void test_gate_reset_calls(void)
{
    assert(!test_gate_active);
    test_gate_calls = 0;
}
