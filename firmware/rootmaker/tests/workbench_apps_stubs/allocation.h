#pragma once

#include <stddef.h>
#include <stdlib.h>

/* Only Controller/job_start translation units use this allocator boundary.
 * Real Store snapshots and cJSON keep their independent allocators. */
void *wb_apps_test_malloc(size_t bytes);
void wb_apps_test_free(void *pointer);
#define malloc wb_apps_test_malloc
#define free wb_apps_test_free
