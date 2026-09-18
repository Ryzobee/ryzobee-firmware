#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "app_fs_host.h"

#include <CommonCrypto/CommonDigest.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[PATH_MAX];
fault_t app_fs_host_fault;
bool app_fs_host_io_fault, app_fs_host_allocation_failure, app_fs_host_reenter, app_fs_host_collide;
unsigned app_fs_host_platform_calls, app_fs_host_remove_skip;
size_t app_fs_host_allocation_count;
size_t app_fs_host_available_bytes = SIZE_MAX;

void app_fs_host_configure(const char *directory)
{
    assert(directory != NULL && strlen(directory) < sizeof(root));
    const char *leaf = strrchr(directory, '/');
    assert(leaf != NULL && strncmp(leaf + 1, "ryz-lua-fs-", 11) == 0);
    struct stat info;
    assert(lstat(directory, &info) == 0 && S_ISDIR(info.st_mode));
    strcpy(root, directory);
}

void app_fs_host_path(const char *name, char path[PATH_MAX])
{
    assert(name != NULL && strlen(name) == 28U && strncmp(name, ".rf", 3U) == 0 && strchr(name, '/') == NULL);
    assert(snprintf(path, PATH_MAX, "%s/%s", root, name) < PATH_MAX);
}

static ryz_fs_result_t map_error(void)
{
    return errno == ENOENT ? RYZ_FS_NOT_FOUND : errno == ENOSPC ? RYZ_FS_NO_SPACE : RYZ_FS_IO;
}

ryz_fs_result_t ryz_fs_platform_ready(void)
{
    ++app_fs_host_platform_calls;
    return app_fs_host_io_fault ? RYZ_FS_RECOVERY_REQUIRED : RYZ_FS_OK;
}

ryz_fs_result_t ryz_fs_platform_visit(ryz_fs_visit_fn visit, void *context)
{
    ++app_fs_host_platform_calls;
    DIR *directory = opendir(root);
    assert(directory != NULL);
    ryz_fs_result_t result = RYZ_FS_OK;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        result = visit(context, entry->d_name);
        if (result != RYZ_FS_OK) break;
    }
    assert(closedir(directory) == 0);
    return result;
}

ryz_fs_result_t ryz_fs_platform_read(const char *name, void *data, size_t capacity, size_t *size)
{
    ++app_fs_host_platform_calls;
    *size = 0;
    char path[PATH_MAX]; app_fs_host_path(name, path);
    struct stat statbuf;
    if (lstat(path, &statbuf) != 0) return map_error();
    if (!S_ISREG(statbuf.st_mode)) return RYZ_FS_CORRUPT;
    if ((uint64_t)statbuf.st_size > capacity) return RYZ_FS_TOO_LARGE;
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return map_error();
    size_t received = 0;
    while (received < (size_t)statbuf.st_size) {
        ssize_t count = read(fd, (uint8_t *)data + received, (size_t)statbuf.st_size - received);
        assert(count > 0);
        received += (size_t)count;
    }
    assert(close(fd) == 0);
    *size = received;
    return RYZ_FS_OK;
}

ryz_fs_result_t ryz_fs_platform_write_new(const char *name, const void *data, size_t size)
{
    ++app_fs_host_platform_calls;
    bool commit = name[27] == 'a' || name[27] == 'b';
    if (app_fs_host_fault == NO_SPACE) { app_fs_host_fault = NONE; return RYZ_FS_NO_SPACE; }
    char path[PATH_MAX]; app_fs_host_path(name, path);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) return map_error();
    bool short_write = (commit && app_fs_host_fault == SHORT_COMMIT) || (!commit && app_fs_host_fault == SHORT_DATA);
    size_t length = short_write ? size / 2U : size;
    assert(write(fd, data, length) == (ssize_t)length);
    assert(fsync(fd) == 0 && close(fd) == 0);
    if ((!commit && app_fs_host_fault == INTERRUPT_DATA) || (commit && app_fs_host_fault == INTERRUPT_COMMIT)) _exit(86);
    if ((commit && app_fs_host_fault == CLOSE_COMMIT) || (!commit && app_fs_host_fault == CLOSE_DATA)) {
        app_fs_host_fault = NONE; app_fs_host_io_fault = true; return RYZ_FS_RECOVERY_REQUIRED;
    }
    if (short_write || (!commit && app_fs_host_fault == SYNC_DATA)) { app_fs_host_fault = NONE; return RYZ_FS_IO; }
    return RYZ_FS_OK;
}

ryz_fs_result_t ryz_fs_platform_reserve(size_t bytes)
{
    ++app_fs_host_platform_calls;
    return bytes <= app_fs_host_available_bytes ? RYZ_FS_OK : RYZ_FS_NO_SPACE;
}

ryz_fs_result_t ryz_fs_platform_append(const char *name, size_t expected_size, const void *data, size_t size)
{
    ++app_fs_host_platform_calls;
    char path[PATH_MAX]; app_fs_host_path(name, path);
    int fd = open(path, O_WRONLY | O_APPEND | O_NOFOLLOW); assert(fd >= 0);
    struct stat info; assert(fstat(fd, &info) == 0 && (uint64_t)info.st_size == expected_size);
    size_t length = app_fs_host_fault == SHORT_ACK ? size / 2U : size;
    assert(write(fd, data, length) == (ssize_t)length && fsync(fd) == 0 && close(fd) == 0);
    if (app_fs_host_fault == CLOSE_ACK) {
        app_fs_host_fault = NONE; app_fs_host_io_fault = true; return RYZ_FS_RECOVERY_REQUIRED;
    }
    if (app_fs_host_fault == SHORT_ACK || app_fs_host_fault == SYNC_ACK) {
        app_fs_host_fault = NONE; return RYZ_FS_IO;
    }
    return RYZ_FS_OK;
}

ryz_fs_result_t ryz_fs_platform_remove(const char *name)
{
    ++app_fs_host_platform_calls;
    char path[PATH_MAX]; app_fs_host_path(name, path);
    struct stat statbuf;
    if (lstat(path, &statbuf) != 0) return errno == ENOENT ? RYZ_FS_OK : map_error();
    if (!S_ISREG(statbuf.st_mode)) return RYZ_FS_CORRUPT;
    if (app_fs_host_fault == REMOVE_FAIL) {
        if (app_fs_host_remove_skip == 0) { app_fs_host_fault = NONE; return RYZ_FS_IO; }
        --app_fs_host_remove_skip;
    }
    assert(unlink(path) == 0);
    if (app_fs_host_fault == INTERRUPT_REMOVE && name[27] == '0') _exit(86);
    return RYZ_FS_OK;
}

ryz_fs_result_t ryz_fs_platform_hash(const void *data, size_t size, char out_hex[65])
{
    ++app_fs_host_platform_calls;
    if (app_fs_host_reenter) {
        app_fs_host_reenter = false;
        ryz_fs_request_t request = {.op = RYZ_FS_INFO};
        ryz_fs_response_t response;
        assert(ryz_app_fs_call(NULL, "nested.lua", &request, &response) == RYZ_FS_BUSY);
    }
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data, (CC_LONG)size, digest);
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(digest); ++i) {
        out_hex[2U * i] = hex[digest[i] >> 4]; out_hex[2U * i + 1U] = hex[digest[i] & 15U];
    }
    out_hex[64] = '\0';
    if (app_fs_host_collide) memset(out_hex, 'a', 24U);
    return RYZ_FS_OK;
}

void *ryz_fs_platform_alloc(size_t size)
{
    if (app_fs_host_allocation_failure) return NULL;
    void *pointer = malloc(size);
    if (pointer != NULL) ++app_fs_host_allocation_count;
    return pointer;
}

void ryz_fs_platform_free(void *data)
{
    if (data != NULL) { assert(app_fs_host_allocation_count > 0); --app_fs_host_allocation_count; }
    free(data);
}

void app_fs_host_fixture_name(const char *app, const char *name, char suffix, char output[29])
{
    char value[80], hash[65];
    size_t app_size = strlen(app), name_size = strlen(name);
    memcpy(value, app, app_size + 1U);
    memcpy(value + app_size + 1U, name, name_size);
    assert(ryz_fs_platform_hash(value, app_size + 1U + name_size, hash) == RYZ_FS_OK);
    memcpy(output, ".rf", 3); memcpy(output + 3, hash, 24); output[27] = suffix; output[28] = 0;
}

void app_fs_host_corrupt(const char *app, const char *name, char suffix, off_t offset)
{
    char physical[29], path[PATH_MAX]; app_fs_host_fixture_name(app, name, suffix, physical); app_fs_host_path(physical, path);
    int fd = open(path, O_RDWR); assert(fd >= 0);
    uint8_t byte; assert(pread(fd, &byte, 1, offset) == 1); byte ^= 0x20;
    assert(pwrite(fd, &byte, 1, offset) == 1 && close(fd) == 0);
}
