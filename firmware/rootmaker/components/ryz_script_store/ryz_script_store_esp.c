#include "ryz_script_store_platform.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "mbedtls/sha256.h"
#include "sdkconfig.h"
#include "ryz_time.h"

#define STORE_ROOT "/scripts"
#define STORE_PARTITION "scripts"

/* In IDF's VFS a failed SPIFFS close can lose the VFS descriptor while the
 * underlying SPIFFS descriptor remains allocated. Never retry that numeric fd;
 * reject further filesystem work until the enclosing system is restarted. */
static atomic_bool s_io_fault;

bool ryz_script_store_io_healthy(void)
{
    return !atomic_load(&s_io_fault);
}

void ryz_script_store_latch_io_fault(void)
{
    atomic_store(&s_io_fault, true);
}

int64_t ryz_script_store_platform_utc(void)
{
    ryz_time_utc_sample_t sample;
    return ryz_time_sample_utc(&sample) == ESP_OK && sample.valid ? sample.unix_seconds : 0;
}

static esp_err_t io_error(int error)
{
    switch (error) {
    case ENOENT: return ESP_ERR_NOT_FOUND;
    case ENOMEM:
    case ENOSPC: return ESP_ERR_NO_MEM;
    case ENAMETOOLONG: return ESP_ERR_INVALID_SIZE;
    case EEXIST: return ESP_ERR_INVALID_STATE;
    case EACCES:
    case EPERM: return ESP_ERR_NOT_SUPPORTED;
    default: return ESP_FAIL;
    }
}

static esp_err_t valid_path(const char *path, bool root_allowed)
{
    if (atomic_load(&s_io_fault)) return ESP_ERR_INVALID_STATE;
    if (path == NULL) return ESP_ERR_INVALID_ARG;
    if (root_allowed && strcmp(path, STORE_ROOT) == 0) return ESP_OK;
    static const char prefix[] = STORE_ROOT "/";
    if (strncmp(path, prefix, sizeof(prefix) - 1U) != 0) return ESP_ERR_INVALID_ARG;
    const char *name = path + sizeof(prefix) - 1U;
    if (*name == '\0' || strchr(name, '/') != NULL ||
        strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return ESP_ERR_INVALID_ARG;
    /* VFS passes '/' + basename to SPIFFS. Keep the protocol's wider filename
     * grammar, but report the REAL configured on-disk limit without truncation. */
    if (strlen(name) + 1U > CONFIG_SPIFFS_OBJ_NAME_LEN - 1U) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

static esp_err_t close_file(int fd, esp_err_t result)
{
    if (close(fd) != 0) {
        atomic_store(&s_io_fault, true);
        return ESP_ERR_INVALID_STATE;
    }
    return result;
}

esp_err_t ryz_script_store_platform_info(
    const char *path, ryz_script_store_file_info_t *out_info)
{
    if (out_info == NULL) return ESP_ERR_INVALID_ARG;
    *out_info = (ryz_script_store_file_info_t){0};
    esp_err_t error = valid_path(path, true);
    if (error != ESP_OK) return error;
    if (strcmp(path, STORE_ROOT) == 0) {
        /* SPIFFS is flat; stat('/') is not a POSIX directory query. The Host
         * adapter instead uses lstat on a real directory and rejects symlinks. */
        out_info->exists = esp_spiffs_mounted(STORE_PARTITION);
        out_info->directory = out_info->exists;
        return ESP_OK;
    }
    struct stat info;
    if (stat(path, &info) != 0) return errno == ENOENT ? ESP_OK : io_error(errno);
    if (info.st_size < 0) return ESP_ERR_INVALID_SIZE;
    *out_info = (ryz_script_store_file_info_t){
        .exists = true, .regular = S_ISREG(info.st_mode),
        .directory = S_ISDIR(info.st_mode), .bytes = (size_t)info.st_size,
    };
    /* No lstat symbol or symlink objects exist in this IDF SPIFFS adapter. */
    return ESP_OK;
}

esp_err_t ryz_script_store_platform_visit(
    const char *root, ryz_script_store_visit_t visit, void *context)
{
    if (visit == NULL || root == NULL || strcmp(root, STORE_ROOT) != 0) return ESP_ERR_INVALID_ARG;
    if (atomic_load(&s_io_fault)) return ESP_ERR_INVALID_STATE;
    DIR *directory = opendir(root);
    if (directory == NULL) return io_error(errno);
    esp_err_t result = ESP_OK;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0) result = io_error(errno);
            break;
        }
        /* SPIFFS d_type is not POSIX DT_REG; core explicitly checks stat. */
        result = visit(context, entry->d_name);
        if (result != ESP_OK) break;
    }
    if (closedir(directory) != 0) {
        atomic_store(&s_io_fault, true);
        return ESP_ERR_INVALID_STATE;
    }
    return result;
}

esp_err_t ryz_script_store_platform_read(
    const char *path, void *bytes, size_t capacity, size_t *out_length)
{
    if (out_length == NULL || bytes == NULL) return ESP_ERR_INVALID_ARG;
    *out_length = 0;
    esp_err_t error = valid_path(path, false);
    if (error != ESP_OK) return error;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return io_error(errno);
    struct stat info;
    if (fstat(fd, &info) != 0) return close_file(fd, io_error(errno));
    if (!S_ISREG(info.st_mode)) return close_file(fd, ESP_ERR_NOT_SUPPORTED);
    if (info.st_size < 0 || (uint64_t)info.st_size > capacity) {
        return close_file(fd, ESP_ERR_INVALID_SIZE);
    }
    size_t received = 0;
    while (received < (size_t)info.st_size) {
        ssize_t count = read(fd, (uint8_t *)bytes + received, (size_t)info.st_size - received);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return close_file(fd, count == 0 ? ESP_FAIL : io_error(errno));
        received += (size_t)count;
    }
    uint8_t extra;
    ssize_t count;
    do { count = read(fd, &extra, 1); } while (count < 0 && errno == EINTR);
    error = count == 0 ? ESP_OK : count > 0 ? ESP_ERR_INVALID_SIZE : io_error(errno);
    error = close_file(fd, error);
    if (error == ESP_OK) *out_length = received;
    return error;
}

esp_err_t ryz_script_store_platform_write_new(
    const char *path, const void *bytes, size_t length)
{
    if (bytes == NULL || length == 0) return ESP_ERR_INVALID_ARG;
    esp_err_t error = valid_path(path, false);
    if (error != ESP_OK) return error;
    /* SPIFFS needs EXCL/TRUNC/APPEND with O_CREAT; never truncate artifacts. */
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return io_error(errno);
    size_t written = 0;
    while (written < length) {
        ssize_t count = write(fd, (const uint8_t *)bytes + written, length - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return close_file(fd, count == 0 ? ESP_FAIL : io_error(errno));
        written += (size_t)count;
    }
    if (fsync(fd) != 0) error = io_error(errno);
    return close_file(fd, error);
}

esp_err_t ryz_script_store_platform_file_sha256(
    const char *path, size_t *out_length, char out_sha256[65])
{
    if (out_length == NULL || out_sha256 == NULL) return ESP_ERR_INVALID_ARG;
    *out_length = 0;
    out_sha256[0] = '\0';
    esp_err_t error = valid_path(path, false);
    if (error != ESP_OK) return error;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return io_error(errno);
    struct stat info;
    if (fstat(fd, &info) != 0) return close_file(fd, io_error(errno));
    if (!S_ISREG(info.st_mode)) return close_file(fd, ESP_ERR_NOT_SUPPORTED);
    if (info.st_size < 0 || (uint64_t)info.st_size > SIZE_MAX) {
        return close_file(fd, ESP_ERR_INVALID_SIZE);
    }

    mbedtls_sha256_context hash;
    mbedtls_sha256_init(&hash);
    if (mbedtls_sha256_starts(&hash, 0) != 0) error = ESP_FAIL;
    uint8_t chunk[512];
    size_t received = 0;
    while (error == ESP_OK && received < (size_t)info.st_size) {
        size_t amount = (size_t)info.st_size - received;
        if (amount > sizeof(chunk)) amount = sizeof(chunk);
        ssize_t count = read(fd, chunk, amount);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            error = count == 0 ? ESP_FAIL : io_error(errno);
            break;
        }
        received += (size_t)count;
        if (mbedtls_sha256_update(&hash, chunk, (size_t)count) != 0) error = ESP_FAIL;
    }
    if (error == ESP_OK) {
        ssize_t count;
        do { count = read(fd, chunk, 1); } while (count < 0 && errno == EINTR);
        error = count == 0 ? ESP_OK : count > 0 ? ESP_ERR_INVALID_SIZE : io_error(errno);
    }
    unsigned char digest[32];
    if (error == ESP_OK && mbedtls_sha256_finish(&hash, digest) != 0) error = ESP_FAIL;
    mbedtls_sha256_free(&hash);
    error = close_file(fd, error);
    if (error != ESP_OK) return error;
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(digest); ++i) {
        out_sha256[2U * i] = hex[digest[i] >> 4];
        out_sha256[2U * i + 1U] = hex[digest[i] & 15U];
    }
    out_sha256[64] = '\0';
    *out_length = received;
    return ESP_OK;
}

esp_err_t ryz_script_store_platform_rename(const char *from, const char *to)
{
    esp_err_t error = valid_path(from, false);
    if (error != ESP_OK) return error;
    error = valid_path(to, false);
    if (error != ESP_OK) return error;
    ryz_script_store_file_info_t source, destination;
    error = ryz_script_store_platform_info(from, &source);
    if (error != ESP_OK) return error;
    if (!source.exists) return ESP_ERR_NOT_FOUND;
    if (!source.regular) return ESP_ERR_NOT_SUPPORTED;
    error = ryz_script_store_platform_info(to, &destination);
    if (error != ESP_OK) return error;
    if (destination.exists) return ESP_ERR_INVALID_STATE;
    return rename(from, to) == 0 ? ESP_OK : io_error(errno);
}

esp_err_t ryz_script_store_platform_remove(const char *path)
{
    ryz_script_store_file_info_t info;
    esp_err_t error = ryz_script_store_platform_info(path, &info);
    if (error != ESP_OK || !info.exists) return error;
    if (!info.regular) return ESP_ERR_NOT_SUPPORTED;
    return unlink(path) == 0 ? ESP_OK : io_error(errno);
}

esp_err_t ryz_script_store_platform_capacity(
    const char *root, size_t *out_total, size_t *out_used)
{
    if (root == NULL || strcmp(root, STORE_ROOT) != 0 ||
        out_total == NULL || out_used == NULL) return ESP_ERR_INVALID_ARG;
    if (atomic_load(&s_io_fault)) return ESP_ERR_INVALID_STATE;
    return esp_spiffs_info(STORE_PARTITION, out_total, out_used);
}

esp_err_t ryz_script_store_platform_sha256(
    const void *bytes, size_t length, char out_hex[65])
{
    if (bytes == NULL || out_hex == NULL) return ESP_ERR_INVALID_ARG;
    out_hex[0] = '\0';
    unsigned char digest[32];
    if (mbedtls_sha256(bytes, length, digest, 0) != 0) return ESP_FAIL;
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(digest); ++i) {
        out_hex[2U * i] = hex[digest[i] >> 4];
        out_hex[2U * i + 1U] = hex[digest[i] & 15U];
    }
    out_hex[64] = '\0';
    return ESP_OK;
}

void *ryz_script_store_platform_alloc(size_t bytes)
{
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void ryz_script_store_platform_free(void *allocation)
{
    heap_caps_free(allocation);
}
