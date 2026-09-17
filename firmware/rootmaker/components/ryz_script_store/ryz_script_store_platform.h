#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Internal platform seam. Firmware uses SPIFFS/POSIX + mbedTLS; Host tests use
 * real temporary-directory I/O + SHA and may inject failures in these calls.
 * A failed write/rename/remove MAY have left artifacts: the core re-inspects
 * and recovers them, rather than treating failure as proof of no mutation.
 * info/read/visit/capacity returning INVALID_STATE means a platform-wide I/O fault,
 * not a missing/malformed file; the core publishes that degraded state. */
typedef struct {
    bool exists;
    bool regular;
    bool directory;
    size_t bytes;
} ryz_script_store_file_info_t;

typedef esp_err_t (*ryz_script_store_visit_t)(void *context, const char *name);

esp_err_t ryz_script_store_platform_info(
    const char *path, ryz_script_store_file_info_t *out_info);
esp_err_t ryz_script_store_platform_visit(
    const char *root, ryz_script_store_visit_t visit, void *context);
/* Refuse nonregular files/symlinks, return actual length, reject oversize. */
esp_err_t ryz_script_store_platform_read(
    const char *path, void *bytes, size_t capacity, size_t *out_length);
/* Raw regular-file digest with bounded chunk memory, no whole-file allocation.
 * Same no-follow / typed sticky I/O failure contract as read. */
esp_err_t ryz_script_store_platform_file_sha256(
    const char *path, size_t *out_length, char out_sha256[65]);
/* Exclusive creation only. Flush/sync and close before returning success. */
esp_err_t ryz_script_store_platform_write_new(
    const char *path, const void *bytes, size_t length);
/* Never overwrite an existing destination. */
esp_err_t ryz_script_store_platform_rename(const char *from, const char *to);
/* Remove a regular file only. Missing is success; no directory deletion. */
esp_err_t ryz_script_store_platform_remove(const char *path);
esp_err_t ryz_script_store_platform_capacity(
    const char *root, size_t *out_total, size_t *out_used);
esp_err_t ryz_script_store_platform_sha256(
    const void *bytes, size_t length, char out_hex[65]);
void *ryz_script_store_platform_alloc(size_t bytes);
void ryz_script_store_platform_free(void *allocation);
/* One non-waiting sample of trusted UTC. Zero means unavailable/uninitialized/
 * busy/invalid. Never wait for network, consult raw mtime or set system time. */
int64_t ryz_script_store_platform_utc(void);
