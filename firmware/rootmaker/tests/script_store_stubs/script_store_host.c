#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L

#include "script_store_host.h"
#include "ryz_script_store_platform.h"

#include <CommonCrypto/CommonDigest.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

static char host_root[PATH_MAX];
typedef struct {
    script_store_host_fault_t kind;
    char source[PATH_MAX];
    char destination[PATH_MAX];
    unsigned skip;
    unsigned remaining;
    unsigned hits;
} host_fault_t;
static host_fault_t host_faults[8];
static size_t host_fault_count;
static bool capacity_override;
static size_t capacity_total, capacity_used;
static pthread_mutex_t sha_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sha_condition = PTHREAD_COND_INITIALIZER;
static bool sha_armed, sha_entered, sha_released;
static pthread_mutex_t capacity_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t capacity_condition = PTHREAD_COND_INITIALIZER;
static bool capacity_armed, capacity_entered, capacity_released;
static atomic_size_t platform_calls;
static atomic_size_t allocation_peak;
static atomic_bool fail_allocation;
static int64_t trusted_utc;

void script_store_host_utc(int64_t unix_seconds) { trusted_utc = unix_seconds; }
int64_t ryz_script_store_platform_utc(void) { return trusted_utc; }

static bool leaf_valid(const char *leaf)
{
    return leaf != NULL && leaf[0] != '\0' && strchr(leaf, '/') == NULL &&
           strcmp(leaf, ".") != 0 && strcmp(leaf, "..") != 0;
}

static bool path_valid(const char *path, bool allow_root)
{
    size_t root_length = strlen(host_root);
    if (path == NULL || root_length == 0U) return false;
    if (allow_root && strcmp(path, host_root) == 0) return true;
    return strncmp(path, host_root, root_length) == 0 &&
           path[root_length] == '/' && leaf_valid(path + root_length + 1U);
}

static const char *leaf_of(const char *path)
{
    if (path_valid(path, true) && strcmp(path, host_root) == 0)
        return strrchr(host_root, '/') + 1;
    assert(path_valid(path, false));
    return path + strlen(host_root) + 1U;
}

void script_store_host_configure(const char *temporary_root)
{
    struct stat st;
    assert(temporary_root != NULL && temporary_root[0] == '/');
    assert(strlen(temporary_root) < sizeof(host_root));
    assert(lstat(temporary_root, &st) == 0 && S_ISDIR(st.st_mode));
    /* Named test roots make accidental workspace/SDK mutation fail closed. */
    const char *name = strrchr(temporary_root, '/');
    assert(name != NULL && strncmp(name + 1, "ryz-script-", 11U) == 0);
    assert(temporary_root[strlen(temporary_root) - 1U] != '/');
    strcpy(host_root, temporary_root);
    memset(host_faults, 0, sizeof(host_faults));
    host_fault_count = 0U;
    capacity_override = false;
    trusted_utc = 0;
}

void script_store_host_fault(script_store_host_fault_t fault,
                             const char *source_leaf,
                             const char *destination_leaf,
                             unsigned skip_matches, unsigned failures)
{
    script_store_host_clear_fault();
    script_store_host_add_fault(fault, source_leaf, destination_leaf,
                                skip_matches, failures);
}

void script_store_host_add_fault(script_store_host_fault_t fault,
                                 const char *source_leaf,
                                 const char *destination_leaf,
                                 unsigned skip_matches, unsigned failures)
{
    assert(host_fault_count < sizeof(host_faults) / sizeof(host_faults[0]));
    host_fault_t *entry = &host_faults[host_fault_count++];
    assert(leaf_valid(source_leaf));
    assert(destination_leaf == NULL || leaf_valid(destination_leaf));
    assert(strlen(source_leaf) < sizeof(entry->source));
    assert(destination_leaf == NULL ||
           strlen(destination_leaf) < sizeof(entry->destination));
    *entry = (host_fault_t){.kind = fault, .skip = skip_matches, .remaining = failures};
    strcpy(entry->source, source_leaf);
    if (destination_leaf != NULL) strcpy(entry->destination, destination_leaf);
}

void script_store_host_clear_fault(void)
{
    memset(host_faults, 0, sizeof(host_faults));
    host_fault_count = 0U;
}

unsigned script_store_host_fault_hits(void)
{
    unsigned hits = 0U;
    for (size_t i = 0U; i < host_fault_count; ++i) hits += host_faults[i].hits;
    return hits;
}

static bool fault_hit(script_store_host_fault_t kind,
                      const char *source, const char *destination)
{
    for (size_t i = 0U; i < host_fault_count; ++i) {
        host_fault_t *entry = &host_faults[i];
        if (entry->remaining == 0U || entry->kind != kind ||
            strcmp(entry->source, leaf_of(source)) != 0 ||
            (destination == NULL && entry->destination[0] != '\0') ||
            (destination != NULL &&
             strcmp(entry->destination, leaf_of(destination)) != 0)) continue;
        if (entry->skip != 0U) { --entry->skip; continue; }
        --entry->remaining;
        ++entry->hits;
        return true;
    }
    return false;
}

void script_store_host_capacity(size_t total, size_t used)
{
    assert(used <= total);
    capacity_override = true;
    capacity_total = total;
    capacity_used = used;
}

void script_store_host_real_capacity(void) { capacity_override = false; }

size_t script_store_host_platform_calls(void)
{
    return atomic_load_explicit(&platform_calls, memory_order_relaxed);
}

void script_store_host_reset_allocation_peak(void)
{
    atomic_store_explicit(&allocation_peak, 0U, memory_order_relaxed);
}

size_t script_store_host_allocation_peak(void)
{
    return atomic_load_explicit(&allocation_peak, memory_order_relaxed);
}

void script_store_host_capacity_gate_arm(void)
{
    assert(pthread_mutex_lock(&capacity_mutex) == 0);
    capacity_armed = true;
    capacity_entered = false;
    capacity_released = false;
    assert(pthread_mutex_unlock(&capacity_mutex) == 0);
}

void script_store_host_capacity_gate_wait(void)
{
    assert(pthread_mutex_lock(&capacity_mutex) == 0);
    while (!capacity_entered)
        assert(pthread_cond_wait(&capacity_condition, &capacity_mutex) == 0);
    assert(pthread_mutex_unlock(&capacity_mutex) == 0);
}

void script_store_host_capacity_gate_release(void)
{
    assert(pthread_mutex_lock(&capacity_mutex) == 0);
    capacity_released = true;
    assert(pthread_cond_broadcast(&capacity_condition) == 0);
    assert(pthread_mutex_unlock(&capacity_mutex) == 0);
}

void script_store_host_sha_gate_arm(void)
{
    assert(pthread_mutex_lock(&sha_mutex) == 0);
    sha_armed = true;
    sha_entered = false;
    sha_released = false;
    assert(pthread_mutex_unlock(&sha_mutex) == 0);
}

void script_store_host_sha_gate_wait(void)
{
    assert(pthread_mutex_lock(&sha_mutex) == 0);
    while (!sha_entered) assert(pthread_cond_wait(&sha_condition, &sha_mutex) == 0);
    assert(pthread_mutex_unlock(&sha_mutex) == 0);
}

void script_store_host_sha_gate_release(void)
{
    assert(pthread_mutex_lock(&sha_mutex) == 0);
    sha_released = true;
    assert(pthread_cond_broadcast(&sha_condition) == 0);
    assert(pthread_mutex_unlock(&sha_mutex) == 0);
}

esp_err_t ryz_script_store_platform_info(
    const char *path, ryz_script_store_file_info_t *out_info)
{
    atomic_fetch_add_explicit(&platform_calls, 1U, memory_order_relaxed);
    if (!path_valid(path, true) || out_info == NULL) return ESP_ERR_INVALID_ARG;
    memset(out_info, 0, sizeof(*out_info));
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT ? ESP_OK : ESP_FAIL;
    if (st.st_size < 0) return ESP_ERR_INVALID_SIZE;
    out_info->exists = true;
    out_info->regular = S_ISREG(st.st_mode);
    out_info->directory = S_ISDIR(st.st_mode);
    out_info->bytes = (size_t)st.st_size;
    return ESP_OK;
}

esp_err_t ryz_script_store_platform_visit(
    const char *root, ryz_script_store_visit_t visit, void *context)
{
    atomic_fetch_add_explicit(&platform_calls, 1U, memory_order_relaxed);
    if (!path_valid(root, true) || strcmp(root, host_root) != 0 || visit == NULL)
        return ESP_ERR_INVALID_ARG;
    if (fault_hit(SCRIPT_HOST_VISIT_INVALID_STATE, root, NULL)) return ESP_ERR_INVALID_STATE;
    DIR *directory = opendir(root);
    if (directory == NULL) return ESP_FAIL;
    esp_err_t error = ESP_OK;
    while (true) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (entry == NULL) { if (errno != 0) error = ESP_FAIL; break; }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        error = visit(context, entry->d_name);
        if (error != ESP_OK) break;
    }
    if (closedir(directory) != 0 && error == ESP_OK) error = ESP_FAIL;
    return error;
}

esp_err_t ryz_script_store_platform_read(
    const char *path, void *bytes, size_t capacity, size_t *out_length)
{
    atomic_fetch_add_explicit(&platform_calls, 1U, memory_order_relaxed);
    if (!path_valid(path, false) || bytes == NULL || out_length == NULL)
        return ESP_ERR_INVALID_ARG;
    *out_length = 0U;
    if (fault_hit(SCRIPT_HOST_READ_INVALID_STATE, path, NULL)) return ESP_ERR_INVALID_STATE;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    struct stat st;
    esp_err_t error = ESP_OK;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) error = ESP_ERR_INVALID_STATE;
    else if (st.st_size < 0 || (uintmax_t)st.st_size > capacity)
        error = ESP_ERR_INVALID_SIZE;
    size_t done = 0U;
    while (error == ESP_OK && done < capacity) {
        ssize_t count = read(fd, (unsigned char *)bytes + done, capacity - done);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { error = ESP_FAIL; break; }
        if (count == 0) break;
        done += (size_t)count;
    }
    if (error == ESP_OK && done == capacity) {
        unsigned char extra;
        ssize_t count;
        do { count = read(fd, &extra, 1U); } while (count < 0 && errno == EINTR);
        if (count > 0) error = ESP_ERR_INVALID_SIZE;
        if (count < 0) error = ESP_FAIL;
    }
    if (close(fd) != 0 && error == ESP_OK) error = ESP_FAIL;
    if (error == ESP_OK) *out_length = done;
    return error;
}

esp_err_t ryz_script_store_platform_write_new(
    const char *path, const void *bytes, size_t length)
{
    atomic_fetch_add_explicit(&platform_calls, 1U, memory_order_relaxed);
    if (!path_valid(path, false) || bytes == NULL) return ESP_ERR_INVALID_ARG;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) return ESP_FAIL;
    bool partial = fault_hit(SCRIPT_HOST_WRITE_PARTIAL, path, NULL);
    bool close_error = fault_hit(SCRIPT_HOST_WRITE_CLOSE, path, NULL);
    size_t requested = partial ? length / 2U : length;
    size_t done = 0U;
    esp_err_t error = ESP_OK;
    while (done < requested) {
        ssize_t count = write(fd, (const unsigned char *)bytes + done,
                              requested - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { error = ESP_FAIL; break; }
        done += (size_t)count;
    }
    if (fsync(fd) != 0) error = ESP_FAIL;
    if (close(fd) != 0) error = ESP_FAIL;
    return partial || close_error ? ESP_FAIL : error;
}

esp_err_t ryz_script_store_platform_rename(const char *from, const char *to)
{
    atomic_fetch_add_explicit(&platform_calls, 1U, memory_order_relaxed);
    if (!path_valid(from, false) || !path_valid(to, false)) return ESP_ERR_INVALID_ARG;
    struct stat st;
    if (lstat(to, &st) == 0 || errno != ENOENT) return ESP_ERR_INVALID_STATE;
    if (lstat(from, &st) != 0 || !S_ISREG(st.st_mode)) return ESP_ERR_INVALID_STATE;
    if (fault_hit(SCRIPT_HOST_RENAME_BEFORE, from, to)) return ESP_FAIL;
    if (rename(from, to) != 0) return ESP_FAIL;
    if (fault_hit(SCRIPT_HOST_RENAME_INTERRUPT_AFTER, from, to)) _Exit(86);
    return fault_hit(SCRIPT_HOST_RENAME_AFTER, from, to) ? ESP_FAIL : ESP_OK;
}

esp_err_t ryz_script_store_platform_remove(const char *path)
{
    atomic_fetch_add_explicit(&platform_calls, 1U, memory_order_relaxed);
    if (!path_valid(path, false)) return ESP_ERR_INVALID_ARG;
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT ? ESP_OK : ESP_FAIL;
    if (!S_ISREG(st.st_mode)) return ESP_ERR_INVALID_STATE;
    if (fault_hit(SCRIPT_HOST_REMOVE_BEFORE, path, NULL)) return ESP_FAIL;
    if (unlink(path) != 0) return ESP_FAIL;
    return fault_hit(SCRIPT_HOST_REMOVE_AFTER, path, NULL) ? ESP_FAIL : ESP_OK;
}

esp_err_t ryz_script_store_platform_capacity(
    const char *root, size_t *out_total, size_t *out_used)
{
    atomic_fetch_add_explicit(&platform_calls, 1U, memory_order_relaxed);
    if (!path_valid(root, true) || strcmp(root, host_root) != 0 ||
        out_total == NULL || out_used == NULL) return ESP_ERR_INVALID_ARG;
    assert(pthread_mutex_lock(&capacity_mutex) == 0);
    if (capacity_armed) {
        capacity_armed = false;
        capacity_entered = true;
        assert(pthread_cond_broadcast(&capacity_condition) == 0);
        while (!capacity_released)
            assert(pthread_cond_wait(&capacity_condition, &capacity_mutex) == 0);
    }
    assert(pthread_mutex_unlock(&capacity_mutex) == 0);
    if (fault_hit(SCRIPT_HOST_CAPACITY_INVALID_STATE, root, NULL))
        return ESP_ERR_INVALID_STATE;
    if (capacity_override) {
        *out_total = capacity_total;
        *out_used = capacity_used;
        return ESP_OK;
    }
    struct statvfs stats;
    if (statvfs(root, &stats) != 0) return ESP_FAIL;
    *out_total = (size_t)stats.f_blocks * (size_t)stats.f_frsize;
    *out_used = *out_total - (size_t)stats.f_bfree * (size_t)stats.f_frsize;
    return ESP_OK;
}

static void sha_gate(void)
{
    assert(pthread_mutex_lock(&sha_mutex) == 0);
    if (sha_armed) {
        sha_armed = false;
        sha_entered = true;
        assert(pthread_cond_broadcast(&sha_condition) == 0);
        while (!sha_released)
            assert(pthread_cond_wait(&sha_condition, &sha_mutex) == 0);
    }
    assert(pthread_mutex_unlock(&sha_mutex) == 0);
}

static void digest_hex(const unsigned char digest[CC_SHA256_DIGEST_LENGTH],
                        char out_hex[65])
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0U; i < CC_SHA256_DIGEST_LENGTH; ++i) {
        out_hex[i * 2U] = hex[digest[i] >> 4U];
        out_hex[i * 2U + 1U] = hex[digest[i] & 15U];
    }
    out_hex[64] = '\0';
}

esp_err_t ryz_script_store_platform_sha256(
    const void *bytes, size_t length, char out_hex[65])
{
    atomic_fetch_add_explicit(&platform_calls, 1U, memory_order_relaxed);
    if (bytes == NULL || out_hex == NULL || length > UINT32_MAX)
        return ESP_ERR_INVALID_ARG;
    sha_gate();
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    if (CC_SHA256(bytes, (CC_LONG)length, digest) == NULL) return ESP_FAIL;
    digest_hex(digest, out_hex);
    return ESP_OK;
}

esp_err_t ryz_script_store_platform_file_sha256(
    const char *path, size_t *out_length, char out_hex[65])
{
    atomic_fetch_add_explicit(&platform_calls, 1U, memory_order_relaxed);
    if (!path_valid(path, false) || out_length == NULL || out_hex == NULL)
        return ESP_ERR_INVALID_ARG;
    *out_length = 0U;
    out_hex[0] = '\0';
    if (fault_hit(SCRIPT_HOST_READ_INVALID_STATE, path, NULL)) return ESP_ERR_INVALID_STATE;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    struct stat before;
    esp_err_t error = ESP_OK;
    if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode)) error = ESP_ERR_NOT_SUPPORTED;
    else if (before.st_size < 0 || (uintmax_t)before.st_size > SIZE_MAX)
        error = ESP_ERR_INVALID_SIZE;
    if (error == ESP_OK) sha_gate();
    CC_SHA256_CTX context;
    if (error == ESP_OK && CC_SHA256_Init(&context) != 1) error = ESP_FAIL;
    unsigned char buffer[4096];
    size_t done = 0U;
    while (error == ESP_OK) {
        ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { error = ESP_FAIL; break; }
        if (count == 0) break;
        if ((size_t)count > SIZE_MAX - done) { error = ESP_ERR_INVALID_SIZE; break; }
        if (CC_SHA256_Update(&context, buffer, (CC_LONG)count) != 1) {
            error = ESP_FAIL;
            break;
        }
        done += (size_t)count;
    }
    struct stat after;
    if (error == ESP_OK && (fstat(fd, &after) != 0 || after.st_size < 0 ||
                            (uintmax_t)after.st_size != done ||
                            before.st_size != after.st_size)) error = ESP_ERR_INVALID_SIZE;
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    if (error == ESP_OK && CC_SHA256_Final(digest, &context) != 1) error = ESP_FAIL;
    if (close(fd) != 0) error = ESP_FAIL;
    if (error == ESP_OK) { *out_length = done; digest_hex(digest, out_hex); }
    return error;
}

void *ryz_script_store_platform_alloc(size_t bytes)
{
    atomic_fetch_add_explicit(&platform_calls, 1U, memory_order_relaxed);
    size_t previous = atomic_load_explicit(&allocation_peak, memory_order_relaxed);
    while (bytes > previous &&
           !atomic_compare_exchange_weak_explicit(&allocation_peak, &previous, bytes,
                                                  memory_order_relaxed, memory_order_relaxed)) {}
    return atomic_exchange(&fail_allocation, false) ? NULL : malloc(bytes);
}

void script_store_host_fail_allocation_once(void) { atomic_store(&fail_allocation, true); }

void script_store_host_fixture_sha256(const void *bytes, size_t length, char out[65])
{ assert(ryz_script_store_platform_sha256(bytes, length, out) == ESP_OK); }

void ryz_script_store_platform_free(void *allocation)
{
    atomic_fetch_add_explicit(&platform_calls, 1U, memory_order_relaxed);
    free(allocation);
}
