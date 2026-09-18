#pragma once
#include "ryz_fs_platform.h"
#include <stdbool.h>
#include <limits.h>
#include <sys/types.h>

typedef enum {
    NONE, SHORT_DATA, SHORT_COMMIT, SYNC_DATA, CLOSE_DATA, CLOSE_COMMIT,
    NO_SPACE, REMOVE_FAIL, INTERRUPT_DATA, INTERRUPT_COMMIT, INTERRUPT_REMOVE,
    SHORT_ACK, SYNC_ACK, CLOSE_ACK,
} fault_t;
extern fault_t app_fs_host_fault;
extern bool app_fs_host_io_fault, app_fs_host_allocation_failure, app_fs_host_reenter, app_fs_host_collide;
extern unsigned app_fs_host_platform_calls, app_fs_host_remove_skip;
extern size_t app_fs_host_allocation_count;
extern size_t app_fs_host_available_bytes;
void app_fs_host_configure(const char *root);
void app_fs_host_fixture_name(const char *app, const char *name, char suffix, char output[29]);
void app_fs_host_path(const char *name, char path[PATH_MAX]);
void app_fs_host_corrupt(const char *app, const char *name, char suffix, off_t offset);
