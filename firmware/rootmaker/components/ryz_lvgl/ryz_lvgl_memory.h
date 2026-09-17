#pragma once

#include <stddef.h>

/* Target-only LVGL builtin allocator hook. The sole display owner prepares
 * this boot-lifetime PSRAM pool before lv_init; the capacity is unchanged. */
void *ryz_lvgl_memory_pool(size_t bytes);
