#pragma once
#include <stdbool.h>
#include <stddef.h>
/* Fault injection is confined to the hardware/OS adapter. */
void ryz_host_heap_fail_after(size_t successful_allocations);
void ryz_host_heap_allow(void);
size_t ryz_host_heap_blocks(void);
size_t ryz_host_heap_bytes(void);
size_t ryz_host_heap_peak(void);
void ryz_host_mutex_fail(bool fail);
