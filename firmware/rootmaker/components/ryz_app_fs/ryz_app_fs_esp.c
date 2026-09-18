#include "ryz_fs_platform.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "mbedtls/sha256.h"
#include "ryz_script_store.h"
#include "sdkconfig.h"

#define ROOT "/scripts"
#define PHYSICAL_LEN 28U
_Static_assert(CONFIG_SPIFFS_OBJ_NAME_LEN >= PHYSICAL_LEN + 2U,
               "Lua storage names need 28 basename bytes plus slash and NUL");

static atomic_bool s_io_fault;

static ryz_fs_result_t io_error(int error)
{
    switch (error) {
    case ENOENT: return RYZ_FS_NOT_FOUND;
    case ENOSPC: return RYZ_FS_NO_SPACE;
    case ENOMEM: return RYZ_FS_NO_MEMORY;
    case EBUSY: return RYZ_FS_BUSY;
    case EEXIST: return RYZ_FS_RECOVERY_REQUIRED;
    default: return RYZ_FS_IO;
    }
}

static void latch_fault(void)
{
    atomic_store(&s_io_fault, true);
    ryz_script_store_latch_io_fault();
}

ryz_fs_result_t ryz_fs_platform_ready(void)
{
    if (atomic_load(&s_io_fault) || !ryz_script_store_io_healthy()) return RYZ_FS_RECOVERY_REQUIRED;
    return esp_spiffs_mounted("scripts") ? RYZ_FS_OK : RYZ_FS_UNAVAILABLE;
}

static ryz_fs_result_t make_path(const char *name, char path[sizeof(ROOT) + 1U + PHYSICAL_LEN])
{
    ryz_fs_result_t result = ryz_fs_platform_ready();
    if (result != RYZ_FS_OK) return result;
    if (name == NULL || strlen(name) != PHYSICAL_LEN || strncmp(name, ".rf", 3U) != 0) return RYZ_FS_INVALID_NAME;
    for (size_t i = 3; i < PHYSICAL_LEN - 1U; ++i) {
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) return RYZ_FS_INVALID_NAME;
    }
    char slot = name[PHYSICAL_LEN - 1U];
    if (slot != '0' && slot != '1' && slot != 'a' && slot != 'b') return RYZ_FS_INVALID_NAME;
    snprintf(path, sizeof(ROOT) + 1U + PHYSICAL_LEN, ROOT "/%s", name);
    return RYZ_FS_OK;
}

static ryz_fs_result_t close_file(int fd, ryz_fs_result_t result)
{
    if (close(fd) != 0) { latch_fault(); return RYZ_FS_RECOVERY_REQUIRED; }
    return result;
}

ryz_fs_result_t ryz_fs_platform_visit(ryz_fs_visit_fn visit, void *context)
{
    ryz_fs_result_t result = ryz_fs_platform_ready();
    if (result != RYZ_FS_OK) return result;
    DIR *directory = opendir(ROOT);
    if (directory == NULL) return io_error(errno);
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (entry == NULL) { if (errno != 0) result = io_error(errno); break; }
        result = visit(context, entry->d_name);
        if (result != RYZ_FS_OK) break;
    }
    if (closedir(directory) != 0) { latch_fault(); return RYZ_FS_RECOVERY_REQUIRED; }
    return result;
}

ryz_fs_result_t ryz_fs_platform_read(const char *name, void *data, size_t capacity, size_t *size)
{
    *size = 0;
    char path[sizeof(ROOT) + 1U + PHYSICAL_LEN];
    ryz_fs_result_t result = make_path(name, path);
    if (result != RYZ_FS_OK) return result;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return io_error(errno);
    struct stat statbuf;
    if (fstat(fd, &statbuf) != 0) return close_file(fd, io_error(errno));
    if (!S_ISREG(statbuf.st_mode)) return close_file(fd, RYZ_FS_CORRUPT);
    if (statbuf.st_size < 0 || (uint64_t)statbuf.st_size > capacity) return close_file(fd, RYZ_FS_TOO_LARGE);
    size_t received = 0;
    while (received < (size_t)statbuf.st_size) {
        ssize_t count = read(fd, (uint8_t *)data + received, (size_t)statbuf.st_size - received);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return close_file(fd, count == 0 ? RYZ_FS_IO : io_error(errno));
        received += (size_t)count;
    }
    uint8_t extra;
    ssize_t count;
    do { count = read(fd, &extra, 1); } while (count < 0 && errno == EINTR);
    result = count == 0 ? RYZ_FS_OK : count > 0 ? RYZ_FS_TOO_LARGE : io_error(errno);
    result = close_file(fd, result);
    if (result == RYZ_FS_OK) *size = received;
    return result;
}

ryz_fs_result_t ryz_fs_platform_write_new(const char *name, const void *data, size_t size)
{
    char path[sizeof(ROOT) + 1U + PHYSICAL_LEN];
    ryz_fs_result_t result = make_path(name, path);
    if (result != RYZ_FS_OK) return result;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return io_error(errno);
    size_t written = 0;
    while (written < size) {
        ssize_t count = write(fd, (const uint8_t *)data + written, size - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { result = count == 0 ? RYZ_FS_IO : io_error(errno); break; }
        written += (size_t)count;
    }
    if (result == RYZ_FS_OK && fsync(fd) != 0) result = io_error(errno);
    result = close_file(fd, result);
    /* Failure may still have consumed pages. Update cached STORAGE off the UI
     * task without changing the script catalogue revision. */
    ryz_script_store_refresh_capacity();
    return result;
}

ryz_fs_result_t ryz_fs_platform_append(const char *name, size_t expected_size, const void *data, size_t size)
{
    char path[sizeof(ROOT) + 1U + PHYSICAL_LEN];
    ryz_fs_result_t result = make_path(name, path);
    if (result != RYZ_FS_OK) return result;
    int fd = open(path, O_WRONLY | O_APPEND);
    if (fd < 0) return io_error(errno);
    struct stat statbuf;
    if (fstat(fd, &statbuf) != 0) return close_file(fd, io_error(errno));
    if (!S_ISREG(statbuf.st_mode) || statbuf.st_size < 0 || (uint64_t)statbuf.st_size != expected_size)
        return close_file(fd, RYZ_FS_CORRUPT);
    size_t written = 0;
    while (written < size) {
        ssize_t count = write(fd, (const uint8_t *)data + written, size - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { result = count == 0 ? RYZ_FS_IO : io_error(errno); break; }
        written += (size_t)count;
    }
    if (result == RYZ_FS_OK && fsync(fd) != 0) result = io_error(errno);
    result = close_file(fd, result);
    ryz_script_store_refresh_capacity();
    return result;
}

ryz_fs_result_t ryz_fs_platform_reserve(size_t bytes)
{
    ryz_fs_result_t result = ryz_fs_platform_ready();
    if (result != RYZ_FS_OK) return result;
    size_t total = 0, used = 0;
    if (esp_spiffs_info("scripts", &total, &used) != ESP_OK) return RYZ_FS_IO;
    return used <= total && bytes <= total - used ? RYZ_FS_OK : RYZ_FS_NO_SPACE;
}

ryz_fs_result_t ryz_fs_platform_remove(const char *name)
{
    char path[sizeof(ROOT) + 1U + PHYSICAL_LEN];
    ryz_fs_result_t result = make_path(name, path);
    if (result != RYZ_FS_OK) return result;
    struct stat statbuf;
    if (stat(path, &statbuf) != 0) return errno == ENOENT ? RYZ_FS_OK : io_error(errno);
    if (!S_ISREG(statbuf.st_mode)) return RYZ_FS_CORRUPT;
    result = unlink(path) == 0 ? RYZ_FS_OK : io_error(errno);
    ryz_script_store_refresh_capacity();
    return result;
}

ryz_fs_result_t ryz_fs_platform_hash(const void *data, size_t size, char out_hex[65])
{
    unsigned char digest[32];
    if (mbedtls_sha256(data, size, digest, 0) != 0) return RYZ_FS_IO;
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(digest); ++i) {
        out_hex[i * 2U] = hex[digest[i] >> 4];
        out_hex[i * 2U + 1U] = hex[digest[i] & 15U];
    }
    out_hex[64] = '\0';
    return RYZ_FS_OK;
}

void *ryz_fs_platform_alloc(size_t size)
{
    void *allocation = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return allocation != NULL ? allocation : heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

void ryz_fs_platform_free(void *data) { heap_caps_free(data); }
