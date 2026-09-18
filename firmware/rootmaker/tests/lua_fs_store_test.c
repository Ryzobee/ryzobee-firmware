#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "app_fs_stubs/app_fs_host.h"

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

static ryz_fs_result_t call(const char *app, ryz_fs_request_t request, ryz_fs_response_t *response)
{
    ryz_fs_response_t discarded;
    return ryz_app_fs_call(NULL, app, &request, response ? response : &discarded);
}

static ryz_fs_result_t put(const char *app, const char *name, const void *data, size_t size)
{
    return call(app, (ryz_fs_request_t){.op = RYZ_FS_WRITE, .name = name, .data = data, .size = size}, NULL);
}

static ryz_fs_result_t del(const char *app, const char *name)
{
    return call(app, (ryz_fs_request_t){.op = RYZ_FS_REMOVE, .name = name}, NULL);
}

static ryz_fs_result_t get(const char *app, const char *name, void *data, size_t capacity, size_t *size)
{
    ryz_fs_response_t response;
    ryz_fs_result_t result = call(app, (ryz_fs_request_t){.op = RYZ_FS_READ, .name = name,
        .read_buffer = data, .read_capacity = capacity}, &response);
    *size = response.size;
    return result;
}

static void expect_value(const char *app, const char *name, const void *data, size_t size)
{
    uint8_t bytes[RYZ_FS_MAX_FILE_BYTES];
    size_t received = 0;
    assert(get(app, name, bytes, sizeof(bytes), &received) == RYZ_FS_OK);
    assert(received == size && memcmp(bytes, data, size) == 0);
}

static void test_validation(void)
{
    unsigned before = app_fs_host_platform_calls;
    const char *bad_names[] = {"", ".private", "../foo", "a/b", "a\\b", "ab..cd", "a b", "abcdefghijklmnopqrstuvwxyz", "中文"};
    for (size_t i = 0; i < sizeof(bad_names) / sizeof(bad_names[0]); ++i)
        assert(put("test.lua", bad_names[i], "x", 1) == RYZ_FS_INVALID_NAME);
    const char *bad_apps[] = {"", "@test.lua", "/test.lua", "test", "a.b.lua", "a..lua", "a/b.lua", "abcdefghijklmnopqrstuvwxyz01234567890.lua"};
    for (size_t i = 0; i < sizeof(bad_apps) / sizeof(bad_apps[0]); ++i)
        assert(put(bad_apps[i], "state", "x", 1) == RYZ_FS_INVALID_APP);
    assert(put("test.lua", "state", "x", RYZ_FS_MAX_FILE_BYTES + 1U) == RYZ_FS_TOO_LARGE);
    assert(app_fs_host_platform_calls == before);
    assert(put("_tool.lua", "A_.-0", "x", 1) == RYZ_FS_OK);
    assert(put("-tool.lua", "0.cfg", "x", 1) == RYZ_FS_OK);
}

static void test_roundtrip(void)
{
    uint8_t binary[] = {0, 1, 255, 0, 42};
    assert(put("test.lua", "z.bin", binary, sizeof(binary)) == RYZ_FS_OK);
    assert(put("test.lua", "empty", NULL, 0) == RYZ_FS_OK);
    expect_value("test.lua", "z.bin", binary, sizeof(binary));
    expect_value("test.lua", "empty", "", 0);
    assert(put("test.lua", "z.bin", "replaced", 8) == RYZ_FS_OK);
    expect_value("test.lua", "z.bin", "replaced", 8);
    ryz_fs_entry_t entries[RYZ_FS_MAX_FILES]; ryz_fs_response_t response;
    assert(call("test.lua", (ryz_fs_request_t){.op = RYZ_FS_LIST, .entries = entries,
        .entries_capacity = RYZ_FS_MAX_FILES}, &response) == RYZ_FS_OK);
    assert(response.count == 2 && strcmp(entries[0].name, "empty") == 0 && entries[0].size == 0);
    assert(strcmp(entries[1].name, "z.bin") == 0 && entries[1].size == 8);
    assert(call("test.lua", (ryz_fs_request_t){.op = RYZ_FS_INFO}, &response) == RYZ_FS_OK);
    assert(response.info.used_bytes == 8 && response.info.file_count == 2);
    assert(response.info.max_file_bytes == 8192 && response.info.quota_bytes == 32768 && response.info.max_files == 16);
    assert(del("test.lua", "z.bin") == RYZ_FS_OK && del("test.lua", "z.bin") == RYZ_FS_NOT_FOUND);
    size_t size; assert(get("test.lua", "z.bin", binary, sizeof(binary), &size) == RYZ_FS_NOT_FOUND);
    assert(size == 0);
}

static void test_isolation(void)
{
    assert(put("a.lua", "state", "alpha", 5) == RYZ_FS_OK);
    assert(put("b.lua", "state", "beta", 4) == RYZ_FS_OK);
    expect_value("a.lua", "state", "alpha", 5); expect_value("b.lua", "state", "beta", 4);
    assert(del("a.lua", "state") == RYZ_FS_OK); expect_value("b.lua", "state", "beta", 4);
    assert(del("b.lua", "state") == RYZ_FS_OK);
    app_fs_host_collide = true;
    assert(put("one.lua", "x", "one", 3) == RYZ_FS_OK);
    uint8_t bytes[8]; size_t size;
    assert(get("two.lua", "x", bytes, sizeof(bytes), &size) == RYZ_FS_CORRUPT);
    assert(put("two.lua", "x", "two", 3) == RYZ_FS_CORRUPT);
    assert(del("two.lua", "x") == RYZ_FS_CORRUPT);
    expect_value("one.lua", "x", "one", 3);
}

static void test_quota(bool global)
{
    uint8_t *payload = malloc(RYZ_FS_MAX_FILE_BYTES); assert(payload); memset(payload, 42, RYZ_FS_MAX_FILE_BYTES);
    unsigned apps = global ? 4U : 1U;
    for (unsigned a = 0; a < apps; ++a) {
        char app[20]; snprintf(app, sizeof(app), "app%u.lua", a);
        for (unsigned n = 0; n < 4; ++n) {
            char name[20]; snprintf(name, sizeof(name), "file%u", n);
            assert(put(app, name, payload, RYZ_FS_MAX_FILE_BYTES) == RYZ_FS_OK);
        }
    }
    assert(put(global ? "extra.lua" : "app0.lua", "extra", "x", 1) == RYZ_FS_QUOTA);
    assert(put("app0.lua", "file0", "x", 1) == RYZ_FS_OK);
    assert(put(global ? "extra.lua" : "app0.lua", "extra", "x", 1) == RYZ_FS_OK);
    if (!global) {
        for (unsigned n = 5; n < 16; ++n) {
            char name[20]; snprintf(name, sizeof(name), "file%u", n);
            assert(put("app0.lua", name, NULL, 0) == RYZ_FS_OK);
        }
        assert(put("app0.lua", "excess", NULL, 0) == RYZ_FS_TOO_MANY_FILES);
        assert(del("app0.lua", "file0") == RYZ_FS_OK);
        assert(put("app0.lua", "excess", NULL, 0) == RYZ_FS_OK);
    }
    free(payload);
}

static void test_global_count(void)
{
    for (unsigned a = 0; a < 8; ++a) {
        char app[20]; snprintf(app, sizeof(app), "app%u.lua", a);
        for (unsigned n = 0; n < 16; ++n) {
            char name[20]; snprintf(name, sizeof(name), "file%u", n);
            assert(put(app, name, NULL, 0) == RYZ_FS_OK);
        }
    }
    assert(put("extra.lua", "extra", NULL, 0) == RYZ_FS_TOO_MANY_FILES);
    assert(del("app0.lua", "file0") == RYZ_FS_OK);
    assert(put("extra.lua", "extra", NULL, 0) == RYZ_FS_OK);
}

static void test_write_faults(void)
{
    assert(put("test.lua", "state", "old", 3) == RYZ_FS_OK);
    fault_t faults[] = {SHORT_DATA, SYNC_DATA, NO_SPACE};
    for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
        app_fs_host_fault = faults[i];
        assert(put("test.lua", "state", "new", 3) == (faults[i] == NO_SPACE ? RYZ_FS_NO_SPACE : RYZ_FS_IO));
        expect_value("test.lua", "state", "old", 3);
    }
    assert(put("test.lua", "state", "new", 3) == RYZ_FS_OK);
    expect_value("test.lua", "state", "new", 3);
}

static void test_close_faults(void)
{
    assert(put("test.lua", "state", "old", 3) == RYZ_FS_OK);
    app_fs_host_fault = CLOSE_DATA; assert(put("test.lua", "state", "new", 3) == RYZ_FS_RECOVERY_REQUIRED);
    uint8_t data[8]; size_t size;
    assert(get("test.lua", "state", data, sizeof(data), &size) == RYZ_FS_RECOVERY_REQUIRED);
    app_fs_host_io_fault = false; /* Simulated platform restart, not an API offered to Lua. */
    expect_value("test.lua", "state", "old", 3);
    app_fs_host_fault = CLOSE_COMMIT; assert(put("test.lua", "state", "new", 3) == RYZ_FS_COMMIT_UNKNOWN);
    assert(get("test.lua", "state", data, sizeof(data), &size) == RYZ_FS_RECOVERY_REQUIRED);
    app_fs_host_io_fault = false;
    expect_value("test.lua", "state", "new", 3);
}

static void test_torn_commit(void)
{
    assert(put("test.lua", "state", "old", 3) == RYZ_FS_OK);
    app_fs_host_fault = SHORT_COMMIT; assert(put("test.lua", "state", "new", 3) == RYZ_FS_COMMIT_UNKNOWN);
    uint8_t data[8]; size_t size;
    assert(get("test.lua", "state", data, sizeof(data), &size) == RYZ_FS_RECOVERY_REQUIRED);
    assert(put("test.lua", "state", "new", 3) == RYZ_FS_RECOVERY_REQUIRED);
    assert(del("test.lua", "state") == RYZ_FS_OK);
    assert(get("test.lua", "state", data, sizeof(data), &size) == RYZ_FS_NOT_FOUND);
    assert(put("test.lua", "state", "reset", 5) == RYZ_FS_OK);
}

static void test_corrupt(void)
{
    assert(put("test.lua", "state", "old", 3) == RYZ_FS_OK);
    assert(put("test.lua", "state", "new", 3) == RYZ_FS_OK);
    app_fs_host_corrupt("test.lua", "state", '1', 97);
    uint8_t bytes[8]; size_t size;
    assert(get("test.lua", "state", bytes, sizeof(bytes), &size) == RYZ_FS_CORRUPT);
    assert(del("test.lua", "state") == RYZ_FS_OK);
    assert(put("test.lua", "state", "next", 4) == RYZ_FS_OK);
    app_fs_host_corrupt("test.lua", "state", 'a', 99);
    assert(get("test.lua", "state", bytes, sizeof(bytes), &size) == RYZ_FS_RECOVERY_REQUIRED);
    assert(del("test.lua", "state") == RYZ_FS_OK);
}

static void test_nonregular(void)
{
    app_fs_host_allocation_failure = true;
    assert(put("test.lua", "state", "new", 3) == RYZ_FS_NO_MEMORY);
    app_fs_host_allocation_failure = false;
    char name[29], path[PATH_MAX]; app_fs_host_fixture_name("test.lua", "state", 'a', name); app_fs_host_path(name, path);
    assert(symlink("/does-not-exist", path) == 0);
    uint8_t bytes[8]; size_t size;
    assert(get("test.lua", "state", bytes, sizeof(bytes), &size) == RYZ_FS_CORRUPT);
    assert(del("test.lua", "state") == RYZ_FS_CORRUPT);
    assert(unlink(path) == 0 && mkdir(path, 0700) == 0);
    assert(get("test.lua", "state", bytes, sizeof(bytes), &size) == RYZ_FS_CORRUPT);
}

static void test_remove_fault(void)
{
    assert(put("test.lua", "state", "old", 3) == RYZ_FS_OK);
    app_fs_host_fault = REMOVE_FAIL; app_fs_host_remove_skip = 0;
    assert(del("test.lua", "state") == RYZ_FS_COMMIT_UNKNOWN);
    uint8_t bytes[8]; size_t size;
    assert(get("test.lua", "state", bytes, sizeof(bytes), &size) == RYZ_FS_NOT_FOUND);
    assert(del("test.lua", "state") == RYZ_FS_OK);
}

static void test_ack_faults(void)
{
    fault_t failures[] = {SHORT_ACK, SYNC_ACK, CLOSE_ACK};
    assert(put("test.lua", "state", "old", 3) == RYZ_FS_OK);
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        app_fs_host_fault = failures[i];
        assert(put("test.lua", "state", "new", 3) == RYZ_FS_COMMIT_UNKNOWN);
        if (failures[i] == CLOSE_ACK) {
            uint8_t bytes[8]; size_t size;
            assert(get("test.lua", "state", bytes, sizeof(bytes), &size) == RYZ_FS_RECOVERY_REQUIRED);
            app_fs_host_io_fault = false;
        }
        /* Publication succeeded, but ACK completion was not reported as true. */
        expect_value("test.lua", "state", "new", 3);
        assert(put("test.lua", "state", "old", 3) == RYZ_FS_OK);
    }
}

static void drop_record(const char *app, const char *name, char suffix)
{
    char physical[29], path[PATH_MAX];
    app_fs_host_fixture_name(app, name, suffix, physical); app_fs_host_path(physical, path);
    assert(unlink(path) == 0);
}

static void test_missing_commit(void)
{
    assert(put("test.lua", "state", "old", 3) == RYZ_FS_OK);
    assert(put("test.lua", "state", "new", 3) == RYZ_FS_OK);
    drop_record("test.lua", "state", 'b');
    uint8_t bytes[8]; size_t size;
    assert(get("test.lua", "state", bytes, sizeof(bytes), &size) == RYZ_FS_RECOVERY_REQUIRED);
    drop_record("test.lua", "state", 'a');
    assert(get("test.lua", "state", bytes, sizeof(bytes), &size) == RYZ_FS_RECOVERY_REQUIRED);
    assert(del("test.lua", "state") == RYZ_FS_OK);
    /* Zero-byte values carry persistence evidence too. */
    assert(put("test.lua", "state", NULL, 0) == RYZ_FS_OK);
    drop_record("test.lua", "state", 'a');
    assert(get("test.lua", "state", bytes, sizeof(bytes), &size) == RYZ_FS_RECOVERY_REQUIRED);
    assert(del("test.lua", "state") == RYZ_FS_OK);
}

static void test_old_commit(void)
{
    assert(put("test.lua", "state", "old", 3) == RYZ_FS_OK);
    assert(put("test.lua", "state", "new", 3) == RYZ_FS_OK);
    app_fs_host_corrupt("test.lua", "state", 'a', 99);
    uint8_t bytes[8]; size_t size;
    assert(get("test.lua", "state", bytes, sizeof(bytes), &size) == RYZ_FS_RECOVERY_REQUIRED);
    assert(del("test.lua", "state") == RYZ_FS_OK);
}

static void test_first_orphan(void)
{
    app_fs_host_fault = SHORT_DATA;
    assert(put("test.lua", "state", "new", 3) == RYZ_FS_IO);
    uint8_t bytes[8]; size_t size;
    assert(get("test.lua", "state", bytes, sizeof(bytes), &size) == RYZ_FS_RECOVERY_REQUIRED);
    assert(del("test.lua", "state") == RYZ_FS_OK);
    assert(put("test.lua", "state", "new", 3) == RYZ_FS_OK);
}

static void test_reserve(void)
{
    assert(put("test.lua", "state", "old", 3) == RYZ_FS_OK);
    app_fs_host_available_bytes = 8192;
    assert(put("test.lua", "state", "new", 3) == RYZ_FS_NO_SPACE);
    expect_value("test.lua", "state", "old", 3);
    assert(del("test.lua", "state") == RYZ_FS_OK);
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    app_fs_host_configure(argv[2]);
    const char *name = argv[1];
    if (strcmp(name, "validation") == 0) test_validation();
    else if (strcmp(name, "roundtrip") == 0) test_roundtrip();
    else if (strcmp(name, "isolation") == 0) test_isolation();
    else if (strcmp(name, "quota") == 0) test_quota(false);
    else if (strcmp(name, "global_quota") == 0) test_quota(true);
    else if (strcmp(name, "global_count") == 0) test_global_count();
    else if (strcmp(name, "write_faults") == 0) test_write_faults();
    else if (strcmp(name, "close_faults") == 0) test_close_faults();
    else if (strcmp(name, "torn_commit") == 0) test_torn_commit();
    else if (strcmp(name, "corrupt") == 0) test_corrupt();
    else if (strcmp(name, "nonregular") == 0) test_nonregular();
    else if (strcmp(name, "remove_fault") == 0) test_remove_fault();
    else if (strcmp(name, "ack_faults") == 0) test_ack_faults();
    else if (strcmp(name, "missing_commit") == 0) test_missing_commit();
    else if (strcmp(name, "old_commit") == 0) test_old_commit();
    else if (strcmp(name, "first_orphan") == 0) test_first_orphan();
    else if (strcmp(name, "reserve") == 0) test_reserve();
    else if (strcmp(name, "busy") == 0) {
        app_fs_host_reenter = true; assert(put("test.lua", "state", "x", 1) == RYZ_FS_OK);
        expect_value("test.lua", "state", "x", 1);
    } else if (strncmp(name, "interrupt_", 10) == 0) {
        if (strcmp(name, "interrupt_first") == 0) {
            app_fs_host_fault = INTERRUPT_DATA;
            (void)put("test.lua", "state", "new", 3);
            assert(false);
        }
        assert(put("test.lua", "state", "old", 3) == RYZ_FS_OK);
        if (strcmp(name, "interrupt_remove") == 0) { app_fs_host_fault = INTERRUPT_REMOVE; (void)del("test.lua", "state"); }
        else { app_fs_host_fault = strcmp(name, "interrupt_data") == 0 ? INTERRUPT_DATA : INTERRUPT_COMMIT; (void)put("test.lua", "state", "new", 3); }
        assert(false);
    } else if (strcmp(name, "reopen_old") == 0) expect_value("test.lua", "state", "old", 3);
    else if (strcmp(name, "reopen_new") == 0) expect_value("test.lua", "state", "new", 3);
    else if (strcmp(name, "reopen_removed") == 0) {
        uint8_t bytes[8]; size_t size; assert(get("test.lua", "state", bytes, sizeof(bytes), &size) == RYZ_FS_NOT_FOUND);
        assert(del("test.lua", "state") == RYZ_FS_OK);
    } else if (strcmp(name, "reopen_orphan") == 0) {
        uint8_t bytes[8]; size_t size;
        assert(get("test.lua", "state", bytes, sizeof(bytes), &size) == RYZ_FS_RECOVERY_REQUIRED);
        assert(del("test.lua", "state") == RYZ_FS_OK);
    } else assert(false);
    assert(app_fs_host_allocation_count == 0);
    printf("LUA_FS_STORE_PASS %s\n", name);
    return 0;
}
