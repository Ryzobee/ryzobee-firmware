#pragma once

#include <stddef.h>
#include <stdint.h>

#define RYZ_FS_NAME_MAX 24U
#define RYZ_FS_APP_ID_MAX 40U
#define RYZ_FS_MAX_FILE_BYTES 8192U
#define RYZ_FS_QUOTA_BYTES 32768U
#define RYZ_FS_MAX_FILES 16U
#define RYZ_FS_GLOBAL_QUOTA_BYTES 131072U
#define RYZ_FS_GLOBAL_MAX_FILES 128U

typedef enum {
    RYZ_FS_OK = 0,
    RYZ_FS_UNAVAILABLE,
    RYZ_FS_INVALID_NAME,
    RYZ_FS_INVALID_APP,
    RYZ_FS_TOO_LARGE,
    RYZ_FS_QUOTA,
    RYZ_FS_TOO_MANY_FILES,
    RYZ_FS_BUSY,
    RYZ_FS_NOT_FOUND,
    RYZ_FS_CORRUPT,
    RYZ_FS_IO,
    RYZ_FS_NO_SPACE,
    RYZ_FS_NO_MEMORY,
    RYZ_FS_COMMIT_UNKNOWN,
    RYZ_FS_RECOVERY_REQUIRED,
} ryz_fs_result_t;

typedef enum { RYZ_FS_READ, RYZ_FS_WRITE, RYZ_FS_REMOVE, RYZ_FS_LIST, RYZ_FS_INFO } ryz_fs_op_t;

typedef struct {
    char name[RYZ_FS_NAME_MAX + 1U];
    size_t size;
} ryz_fs_entry_t;

typedef struct {
    size_t used_bytes;
    size_t file_count;
    size_t max_file_bytes;
    size_t quota_bytes;
    size_t max_files;
} ryz_fs_info_t;

/* Caller's buffers remain owned by caller; no pointers are retained. Read and
 * write support empty values and embedded NUL bytes. LIST is lexicographically
 * sorted. Adapter must validate app_id and name, including their lengths. */
typedef struct {
    ryz_fs_op_t op;
    const char *name;
    const void *data;
    size_t size;
    void *read_buffer;
    size_t read_capacity;
    ryz_fs_entry_t *entries;
    size_t entries_capacity;
} ryz_fs_request_t;

typedef struct {
    size_t size;
    size_t count;
    ryz_fs_info_t info;
} ryz_fs_response_t;

/* app_id comes only from the trusted launcher, never from Lua arguments.
 * Non-OK results have no usable output. write/remove success means the adapter
 * checked write/flush/close and read-back, not a power-loss atomicity guarantee. */
typedef ryz_fs_result_t (*ryz_fs_call_fn)(
    void *context, const char *app_id, const ryz_fs_request_t *request,
    ryz_fs_response_t *response);

/* Firmware implementation; Host runners may instead supply their own callback. */
ryz_fs_result_t ryz_app_fs_call(
    void *context, const char *app_id, const ryz_fs_request_t *request,
    ryz_fs_response_t *response);
