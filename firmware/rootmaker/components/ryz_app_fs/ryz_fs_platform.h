#pragma once

#include "ryz_fs.h"

/* Private storage seam. Names are flat physical basenames, not user paths.
 * Implementations must reject symlinks, flush/close before success, and latch
 * descriptor-close failures. Failed mutations may already have changed disk. */
typedef ryz_fs_result_t (*ryz_fs_visit_fn)(void *context, const char *name);
ryz_fs_result_t ryz_fs_platform_ready(void);
ryz_fs_result_t ryz_fs_platform_visit(ryz_fs_visit_fn visit, void *context);
ryz_fs_result_t ryz_fs_platform_read(const char *name, void *data, size_t capacity, size_t *size);
ryz_fs_result_t ryz_fs_platform_write_new(const char *name, const void *data, size_t size);
ryz_fs_result_t ryz_fs_platform_append(const char *name, size_t expected_size, const void *data, size_t size);
/* Admission only, not a reservation against other components. */
ryz_fs_result_t ryz_fs_platform_reserve(size_t bytes);
ryz_fs_result_t ryz_fs_platform_remove(const char *name);
ryz_fs_result_t ryz_fs_platform_hash(const void *data, size_t size, char out_hex[65]);
void *ryz_fs_platform_alloc(size_t size);
void ryz_fs_platform_free(void *data);
