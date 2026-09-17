#pragma once
#include_next <stdatomic.h>

/* One deterministic unsuccessful try-lock, without a production test hook.
 * This tests retained state after contention, not RTOS scheduling fairness. */
static inline bool scripts_real_test_and_set(volatile atomic_flag *object, memory_order order)
{ return atomic_flag_test_and_set_explicit(object, order); }
bool scripts_test_and_set(volatile atomic_flag *object, memory_order order);
#undef atomic_flag_test_and_set_explicit
#define atomic_flag_test_and_set_explicit(object, order) scripts_test_and_set(object, order)
