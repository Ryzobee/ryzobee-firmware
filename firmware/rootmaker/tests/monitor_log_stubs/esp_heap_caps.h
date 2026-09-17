#pragma once
#include <stddef.h>
#include <stdlib.h>
/* Environment allocator only; log ownership and filtering use real code. */
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
static inline void *heap_caps_calloc(size_t count, size_t bytes, unsigned caps)
{
    (void)caps;
    return calloc(count, bytes);
}
static inline void heap_caps_free(void *pointer) { free(pointer); }
