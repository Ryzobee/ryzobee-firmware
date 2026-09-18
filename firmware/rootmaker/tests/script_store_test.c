#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L

#include "ryz_script_store.h"
#include "script_store_host.h"

#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *test_root;
static const char sha_abc[] =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

static void path_for(const char *leaf, char path[PATH_MAX])
{
    assert(leaf != NULL && leaf[0] != '\0' && strchr(leaf, '/') == NULL);
    assert(strcmp(leaf, ".") != 0 && strcmp(leaf, "..") != 0);
    int written = snprintf(path, PATH_MAX, "%s/%s", test_root, leaf);
    assert(written > 0 && written < PATH_MAX);
}

/* Fixtures represent pre-existing/user-external filesystem state. Store
 * behavior itself is exercised exclusively through its public header. */
static void fixture_write(const char *leaf, const void *bytes, size_t length)
{
    char path[PATH_MAX];
    path_for(leaf, path);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    assert(fd >= 0);
    size_t done = 0U;
    while (done < length) {
        ssize_t count = write(fd, (const unsigned char *)bytes + done, length - done);
        assert(count > 0);
        done += (size_t)count;
    }
    assert(fsync(fd) == 0);
    assert(close(fd) == 0);
}

static void fixture_repeated(const char *leaf, unsigned char byte, size_t length)
{
    char path[PATH_MAX];
    path_for(leaf, path);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    assert(fd >= 0);
    unsigned char block[4096];
    memset(block, byte, sizeof(block));
    size_t done = 0U;
    while (done < length) {
        size_t requested = length - done;
        if (requested > sizeof(block)) requested = sizeof(block);
        ssize_t count = write(fd, block, requested);
        assert(count > 0);
        done += (size_t)count;
    }
    assert(fsync(fd) == 0);
    assert(close(fd) == 0);
}

static bool fixture_exists(const char *leaf)
{
    char path[PATH_MAX];
    path_for(leaf, path);
    struct stat st;
    return lstat(path, &st) == 0;
}

static void fixture_assert_bytes(const char *leaf, const void *bytes, size_t length)
{
    char path[PATH_MAX];
    path_for(leaf, path);
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    assert(fd >= 0);
    unsigned char buffer[RYZ_SCRIPT_STORE_SOURCE_MAX + 1U];
    assert(length < sizeof(buffer));
    ssize_t count = read(fd, buffer, sizeof(buffer));
    assert(count >= 0 && (size_t)count == length);
    assert(memcmp(buffer, bytes, length) == 0);
    assert(close(fd) == 0);
}

static void init_store(void)
{
    ryz_script_store_mutation_t recovery;
    assert(ryz_script_store_init(test_root, &recovery) == ESP_OK);
    ryz_script_store_status_t status;
    assert(ryz_script_store_status(&status) == ESP_OK);
    assert(status.ready && !status.recovery_required);
}

static ryz_script_store_mutation_t put(const char *name, const char *source)
{
    ryz_script_store_mutation_t result;
    assert(ryz_script_store_put(name, source, strlen(source), NULL, &result) == ESP_OK);
    assert(result.commit == RYZ_SCRIPT_STORE_COMMITTED);
    assert(result.cleanup_error == ESP_OK && !result.recovery_required);
    assert(result.bytes == strlen(source));
    return result;
}

static void expect_source(const char *name, const char *source)
{
    ryz_script_store_snapshot_t snapshot;
    assert(ryz_script_store_get(name, &snapshot) == ESP_OK);
    assert(strcmp(snapshot.entry.name, name) == 0);
    assert(snapshot.entry.bytes == strlen(source));
    assert(memcmp(snapshot.source, source, snapshot.entry.bytes + 1U) == 0);
    assert(strlen(snapshot.sha256) == 64U);
    ryz_script_store_snapshot_free(&snapshot);
}

static void expect_no_transaction_files(void)
{
    assert(!fixture_exists(".ryz-new"));
    assert(!fixture_exists(".ryz-old"));
    assert(!fixture_exists(".ryz-txn"));
}

static void test_validation(void)
{
    ryz_script_store_status_t status;
    ryz_script_store_mutation_t result;
    ryz_script_store_snapshot_t snapshot;
    ryz_script_store_page_t page;
    ryz_script_store_description_t description;
    assert(ryz_script_store_status(&status) == ESP_OK && !status.ready);
    assert(ryz_script_store_describe("valid.lua", &description) == ESP_ERR_INVALID_STATE);
    char digest[65];
    assert(ryz_script_store_source_sha256("abc", 3U, digest) == ESP_OK);
    assert(strcmp(digest, sha_abc) == 0);
    assert(ryz_script_store_source_sha256(NULL, 1U, digest) == ESP_ERR_INVALID_ARG);
    assert(digest[0] == '\0');
    assert(ryz_script_store_source_sha256("", 0U, digest) == ESP_ERR_INVALID_SIZE);
    assert(ryz_script_store_source_sha256("abc", 3U, NULL) == ESP_ERR_INVALID_ARG);
    assert(ryz_script_store_init(NULL, &result) == ESP_ERR_INVALID_ARG);
    assert(ryz_script_store_init("", &result) == ESP_ERR_INVALID_ARG);
    fixture_write("not-a-directory", "x", 1U);
    char path[PATH_MAX];
    path_for("not-a-directory", path);
    assert(ryz_script_store_init(path, &result) != ESP_OK);
    init_store();
    assert(ryz_script_store_init(path, &result) == ESP_ERR_INVALID_STATE);

    const char *invalid[] = {NULL, "", ".lua", "a", "a.LUA", "a.txt", "a.lua.bak",
        "../a.lua", "/a.lua", "x/a.lua", "a\\b.lua", "a b.lua", "a.b.lua",
        "a\nb.lua", ".hidden.lua", "é.lua", ".ryz-new", ".upload.tmp"};
    for (size_t i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(!ryz_script_store_valid_name(invalid[i]));
        assert(ryz_script_store_put(invalid[i], "abc", 3U, NULL, &result) ==
               ESP_ERR_INVALID_ARG);
        assert(ryz_script_store_get(invalid[i], &snapshot) == ESP_ERR_INVALID_ARG);
        assert(ryz_script_store_describe(invalid[i], &description) == ESP_ERR_INVALID_ARG);
        assert(ryz_script_store_remove(invalid[i], NULL, &result) == ESP_ERR_INVALID_ARG);
    }
    char longest[RYZ_SCRIPT_STORE_NAME_MAX + 1U];
    memset(longest, 'a', sizeof(longest));
    memcpy(longest + RYZ_SCRIPT_STORE_NAME_MAX - 4U, ".lua", 5U);
    assert(ryz_script_store_valid_name(longest));
    put(longest, "abc");
    expect_source(longest, "abc");
    char too_long[RYZ_SCRIPT_STORE_NAME_MAX + 2U];
    memset(too_long, 'a', sizeof(too_long));
    memcpy(too_long + RYZ_SCRIPT_STORE_NAME_MAX - 3U, ".lua", 5U);
    assert(!ryz_script_store_valid_name(too_long));
    assert(ryz_script_store_put(too_long, "abc", 3U, NULL, &result) == ESP_ERR_INVALID_ARG);
    assert(ryz_script_store_valid_name("A-z_09.lua"));
    assert(ryz_script_store_put("x.lua", NULL, 1U, NULL, &result) == ESP_ERR_INVALID_ARG);
    assert(ryz_script_store_put("x.lua", "", 0U, NULL, &result) != ESP_OK);
    char source[RYZ_SCRIPT_STORE_SOURCE_MAX + 1U];
    memset(source, 'x', sizeof(source));
    assert(ryz_script_store_put("x.lua", source, sizeof(source), NULL, &result) ==
           ESP_ERR_INVALID_SIZE);
    assert(ryz_script_store_source_sha256(source, sizeof(source), digest) ==
           ESP_ERR_INVALID_SIZE);
    const char nul_source[] = {'a', '\0', 'b'};
    assert(ryz_script_store_put("x.lua", nul_source, sizeof(nul_source), NULL, &result) != ESP_OK);
    assert(ryz_script_store_source_sha256(nul_source, sizeof(nul_source), digest) ==
           ESP_ERR_INVALID_ARG);
    const char *bad_hashes[] = {"a", "SHA256", "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD",
        "ga7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"};
    for (size_t i = 0U; i < sizeof(bad_hashes) / sizeof(bad_hashes[0]); ++i) {
        assert(ryz_script_store_put("x.lua", "abc", 3U, bad_hashes[i], &result) ==
               ESP_ERR_INVALID_ARG);
        assert(ryz_script_store_remove("x.lua", bad_hashes[i], &result) == ESP_ERR_INVALID_ARG);
    }
    assert(ryz_script_store_list(0U, 0U, 0U, &page) == ESP_ERR_INVALID_ARG);
    assert(ryz_script_store_list(0U, RYZ_SCRIPT_STORE_PAGE_MAX + 1U, 0U, &page) ==
           ESP_ERR_INVALID_ARG);
    assert(ryz_script_store_get("x.lua", NULL) == ESP_ERR_INVALID_ARG);
    assert(ryz_script_store_describe("x.lua", NULL) == ESP_ERR_INVALID_ARG);
    assert(ryz_script_store_list(0U, 1U, 0U, NULL) == ESP_ERR_INVALID_ARG);
    expect_no_transaction_files();
}

static void test_roundtrip(void)
{
    init_store();
    ryz_script_store_page_t page;
    assert(ryz_script_store_list(0U, 16U, 0U, &page) == ESP_OK);
    assert(page.count == 0U && page.total == 0U);
    ryz_script_store_mutation_t result;
    assert(ryz_script_store_put("abc.lua", "abc", 3U, "", &result) == ESP_OK);
    assert(result.commit == RYZ_SCRIPT_STORE_COMMITTED && strcmp(result.sha256, sha_abc) == 0);
    ryz_script_store_snapshot_t original;
    assert(ryz_script_store_get("abc.lua", &original) == ESP_OK);
    assert(strcmp(original.sha256, sha_abc) == 0 && !original.entry.protected_file);
    assert(ryz_script_store_put("abc.lua", "new", 3U, "", &result) == ESP_ERR_INVALID_STATE);
    put("abc.lua", "new body");
    assert(original.entry.bytes == 3U && memcmp(original.source, "abc\0", 4U) == 0);
    assert(strcmp(original.sha256, sha_abc) == 0);
    expect_source("abc.lua", "new body");
    ryz_script_store_snapshot_free(&original);
    assert(original.source == NULL);
    ryz_script_store_snapshot_free(&original);
    ryz_script_store_snapshot_free(NULL);
    char *large = malloc(RYZ_SCRIPT_STORE_SOURCE_MAX + 1U);
    assert(large != NULL);
    memset(large, 'Z', RYZ_SCRIPT_STORE_SOURCE_MAX);
    large[RYZ_SCRIPT_STORE_SOURCE_MAX] = '\0';
    put("maximum.lua", large);
    expect_source("maximum.lua", large);
    free(large);
    assert(ryz_script_store_remove("abc.lua", NULL, &result) == ESP_OK);
    assert(result.commit == RYZ_SCRIPT_STORE_COMMITTED);
    assert(ryz_script_store_get("abc.lua", &original) == ESP_ERR_NOT_FOUND);
    assert(ryz_script_store_remove("abc.lua", NULL, &result) == ESP_ERR_NOT_FOUND);
    assert(ryz_script_store_get("maximum.lua", &original) == ESP_OK);
    assert(original.entry.bytes == RYZ_SCRIPT_STORE_SOURCE_MAX);
    for (size_t i = 0U; i < original.entry.bytes; ++i) assert(original.source[i] == 'Z');
    assert(original.source[original.entry.bytes] == '\0');
    ryz_script_store_snapshot_free(&original);
    expect_no_transaction_files();
}

static void test_catalog(void)
{
    for (unsigned i = 20U; i > 0U; --i) {
        char name[32];
        assert(snprintf(name, sizeof(name), "item_%02u.lua", i) > 0);
        fixture_write(name, "abc", 3U);
    }
    fixture_write("Z.lua", "abc", 3U);
    fixture_write("a.lua", "abc", 3U);
    fixture_write("readme.txt", "abc", 3U);
    fixture_write("bad name.lua", "abc", 3U);
    char path[PATH_MAX];
    path_for("dir.lua", path);
    assert(mkdir(path, 0700) == 0);
    path_for("link.lua", path);
    assert(symlink("a.lua", path) == 0);
    path_for("pipe.lua", path);
    assert(mkfifo(path, 0600) == 0);
    init_store();
    ryz_script_store_page_t page;
    assert(ryz_script_store_list(0U, 16U, 0U, &page) == ESP_OK);
    assert(page.count == 16U && page.total == 22U && page.offset == 0U);
    assert(strcmp(page.entries[0].name, "Z.lua") == 0);
    assert(strcmp(page.entries[1].name, "a.lua") == 0);
    uint32_t revision = page.revision;
    assert(revision != 0U);
    char previous[RYZ_SCRIPT_STORE_NAME_MAX + 1U] = "";
    size_t seen = 0U;
    for (size_t offset = 0U; offset < 22U; offset += 7U) {
        assert(ryz_script_store_list(offset, 7U, revision, &page) == ESP_OK);
        assert(page.total == 22U && page.offset == offset && page.count <= 7U);
        for (size_t i = 0U; i < page.count; ++i) {
            assert(strcmp(previous, page.entries[i].name) < 0);
            strcpy(previous, page.entries[i].name);
            assert(page.entries[i].bytes == 3U);
            ++seen;
        }
    }
    assert(seen == 22U);
    assert(ryz_script_store_list(22U, 1U, revision, &page) == ESP_OK);
    assert(page.count == 0U && page.total == 22U);
    assert(ryz_script_store_list(100U, 1U, revision, &page) == ESP_ERR_INVALID_ARG);
    put("new.lua", "abc");
    assert(ryz_script_store_list(0U, 16U, revision, &page) == ESP_ERR_INVALID_STATE);
    assert(ryz_script_store_list(0U, 16U, 0U, &page) == ESP_OK);
    assert(page.total == 23U && page.revision != revision);
}

static void test_nonregular_and_corrupt(void)
{
    fixture_write("target.txt", "keep", 4U);
    fixture_write("empty.lua", "", 0U);
    const char nul[] = {'a', '\0', 'b'};
    fixture_write("nul.lua", nul, sizeof(nul));
    char large[RYZ_SCRIPT_STORE_SOURCE_MAX + 1U];
    memset(large, 'x', sizeof(large));
    fixture_write("large.lua", large, sizeof(large));
    char path[PATH_MAX];
    path_for("dir.lua", path);
    assert(mkdir(path, 0700) == 0);
    path_for("link.lua", path);
    assert(symlink("target.txt", path) == 0);
    path_for("pipe.lua", path);
    assert(mkfifo(path, 0600) == 0);
    init_store();
    const char *nonregular[] = {"dir.lua", "link.lua", "pipe.lua"};
    ryz_script_store_snapshot_t snapshot;
    ryz_script_store_mutation_t result;
    for (size_t i = 0U; i < sizeof(nonregular) / sizeof(nonregular[0]); ++i) {
        assert(ryz_script_store_get(nonregular[i], &snapshot) != ESP_OK);
        assert(snapshot.source == NULL);
        assert(ryz_script_store_put(nonregular[i], "abc", 3U, NULL, &result) != ESP_OK);
        assert(ryz_script_store_remove(nonregular[i], NULL, &result) != ESP_OK);
        assert(fixture_exists(nonregular[i]));
    }
    assert(ryz_script_store_get("empty.lua", &snapshot) != ESP_OK);
    assert(snapshot.source == NULL);
    assert(ryz_script_store_get("nul.lua", &snapshot) != ESP_OK);
    assert(snapshot.source == NULL);
    assert(ryz_script_store_get("large.lua", &snapshot) == ESP_ERR_INVALID_SIZE);
    assert(snapshot.source == NULL);
    fixture_assert_bytes("target.txt", "keep", 4U);
    expect_no_transaction_files();
}

static void test_cas(void)
{
    init_store();
    put("selected.lua", "abc");
    ryz_script_store_snapshot_t selected;
    assert(ryz_script_store_get("selected.lua", &selected) == ESP_OK);
    ryz_script_store_mutation_t result;
    assert(ryz_script_store_put("selected.lua", "second", 6U, selected.sha256, &result) == ESP_OK);
    assert(ryz_script_store_put("selected.lua", "late", 4U, selected.sha256, &result) ==
           ESP_ERR_INVALID_STATE);
    assert(result.commit == RYZ_SCRIPT_STORE_NOT_COMMITTED);
    assert(ryz_script_store_remove("selected.lua", selected.sha256, &result) ==
           ESP_ERR_INVALID_STATE);
    expect_source("selected.lua", "second");
    assert(memcmp(selected.source, "abc\0", 4U) == 0);
    ryz_script_store_snapshot_free(&selected);
    assert(ryz_script_store_get("selected.lua", &selected) == ESP_OK);
    assert(ryz_script_store_remove("selected.lua", selected.sha256, &result) == ESP_OK);
    assert(ryz_script_store_put("selected.lua", "late", 4U, selected.sha256, &result) ==
           ESP_ERR_INVALID_STATE);
    ryz_script_store_snapshot_free(&selected);
    assert(ryz_script_store_put("selected.lua", "fresh", 5U, "", &result) == ESP_OK);
    expect_source("selected.lua", "fresh");
    expect_no_transaction_files();
}

static void test_boot(void)
{
    fixture_write("boot.lua", "legacy selftest", 15U);
    init_store();
    ryz_script_store_snapshot_t snapshot;
    assert(ryz_script_store_get("boot.lua", &snapshot) == ESP_OK);
    assert(!snapshot.entry.protected_file);
    ryz_script_store_mutation_t result;
    assert(ryz_script_store_put("boot.lua", "new", 3U, snapshot.sha256, &result) == ESP_OK);
    assert(result.commit == RYZ_SCRIPT_STORE_COMMITTED);
    assert(ryz_script_store_remove("boot.lua", snapshot.sha256, &result) == ESP_ERR_INVALID_STATE);
    ryz_script_store_snapshot_free(&snapshot);
    ryz_script_store_page_t page;
    assert(ryz_script_store_list(0U, 16U, 0U, &page) == ESP_OK);
    assert(page.total == 1U && page.count == 1U && !page.entries[0].protected_file);
    assert(strcmp(page.entries[0].name, "boot.lua") == 0);
    expect_source("boot.lua", "new");
    assert(ryz_script_store_get("boot.lua", &snapshot) == ESP_OK);
    assert(ryz_script_store_remove("boot.lua", snapshot.sha256, &result) == ESP_OK);
    ryz_script_store_snapshot_free(&snapshot);
    assert(!fixture_exists("boot.lua"));
    expect_no_transaction_files();
}

static void test_legacy_artifacts(void)
{
    fixture_write("keep.lua", "abc", 3U);
    fixture_write(".upload.bak", "unknown user bytes", 18U);
    fixture_write(".upload.tmp", "partial old upload", 18U);
    ryz_script_store_mutation_t result;
    assert(ryz_script_store_init(test_root, &result) != ESP_OK);
    ryz_script_store_status_t status;
    assert(ryz_script_store_status(&status) == ESP_OK);
    assert(status.ready && status.recovery_required && status.legacy_backup_present);
    expect_source("keep.lua", "abc");
    ryz_script_store_page_t page;
    assert(ryz_script_store_list(0U, 16U, 0U, &page) == ESP_OK && page.total == 1U);
    assert(ryz_script_store_put("new.lua", "abc", 3U, NULL, &result) != ESP_OK);
    assert(ryz_script_store_remove("keep.lua", NULL, &result) != ESP_OK);
    assert(ryz_script_store_recover(&result) != ESP_OK);
    assert(ryz_script_store_init(test_root, &result) != ESP_OK);
    fixture_assert_bytes(".upload.bak", "unknown user bytes", 18U);
    fixture_assert_bytes(".upload.tmp", "partial old upload", 18U);
    expect_source("keep.lua", "abc");
}

typedef struct { esp_err_t error; ryz_script_store_snapshot_t snapshot; } reader_t;

static void *reader_thread(void *context)
{
    reader_t *reader = context;
    reader->error = ryz_script_store_get("held.lua", &reader->snapshot);
    return NULL;
}

static void test_concurrency(void)
{
    init_store();
    put("held.lua", "abc");
    script_store_host_sha_gate_arm();
    reader_t reader = {0};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, reader_thread, &reader) == 0);
    script_store_host_sha_gate_wait();
    ryz_script_store_mutation_t result;
    ryz_script_store_snapshot_t snapshot;
    ryz_script_store_page_t page;
    ryz_script_store_status_t status;
    ryz_script_store_description_t description;
    assert(ryz_script_store_put("other.lua", "abc", 3U, NULL, &result) == ESP_ERR_TIMEOUT);
    assert(ryz_script_store_remove("held.lua", NULL, &result) == ESP_ERR_TIMEOUT);
    assert(ryz_script_store_get("held.lua", &snapshot) == ESP_ERR_TIMEOUT);
    assert(ryz_script_store_describe("held.lua", &description) == ESP_ERR_TIMEOUT);
    assert(ryz_script_store_list(0U, 16U, 0U, &page) == ESP_ERR_TIMEOUT);
    assert(ryz_script_store_status(&status) == ESP_ERR_TIMEOUT);
    assert(ryz_script_store_recover(&result) == ESP_ERR_TIMEOUT);
    assert(ryz_script_store_init(test_root, &result) == ESP_ERR_TIMEOUT);
    char digest[65];
    assert(ryz_script_store_source_sha256("abc", 3U, digest) == ESP_OK);
    assert(strcmp(digest, sha_abc) == 0); /* Stateless helper never takes Store lock. */
    script_store_host_sha_gate_release();
    assert(pthread_join(thread, NULL) == 0);
    assert(reader.error == ESP_OK && strcmp(reader.snapshot.sha256, sha_abc) == 0);
    ryz_script_store_snapshot_free(&reader.snapshot);
    expect_source("held.lua", "abc");
    assert(ryz_script_store_get("other.lua", &snapshot) == ESP_ERR_NOT_FOUND);
    put("other.lua", "now succeeds");
    expect_no_transaction_files();
}

static void test_capacity(void)
{
    init_store();
    put("old.lua", "abc");
    script_store_host_capacity(100U, 99U);
    ryz_script_store_mutation_t result;
    assert(ryz_script_store_put("old.lua", "replacement", 11U, NULL, &result) != ESP_OK);
    assert(result.commit == RYZ_SCRIPT_STORE_NOT_COMMITTED);
    expect_source("old.lua", "abc");
    expect_no_transaction_files();
    script_store_host_real_capacity();
    put("old.lua", "replacement");
    expect_source("old.lua", "replacement");
}

static void test_index_limit(void)
{
    for (unsigned i = 0U; i < RYZ_SCRIPT_STORE_INDEX_MAX; ++i) {
        char name[32];
        assert(snprintf(name, sizeof(name), "item_%03u.lua", i) > 0);
        fixture_write(name, "abc", 3U);
    }
    init_store();
    ryz_script_store_page_t page;
    assert(ryz_script_store_list(0U, 16U, 0U, &page) == ESP_OK);
    assert(page.total == RYZ_SCRIPT_STORE_INDEX_MAX && page.count == 16U);
    ryz_script_store_mutation_t result;
    assert(ryz_script_store_put("overflow.lua", "abc", 3U, "", &result) == ESP_ERR_INVALID_SIZE);
    assert(result.commit == RYZ_SCRIPT_STORE_NOT_COMMITTED);
    assert(!fixture_exists("overflow.lua"));
    expect_no_transaction_files();
    fixture_write("overflow.lua", "abc", 3U);
    assert(ryz_script_store_list(0U, 16U, 0U, &page) == ESP_ERR_INVALID_SIZE);
    assert(page.count == 0U && page.total == 0U);
    ryz_script_store_status_t status;
    assert(ryz_script_store_status(&status) == ESP_OK);
    const uint32_t before = status.revision;
    ryz_script_store_bulk_t bulk;
    assert(ryz_script_store_delete_all(before, &bulk) == ESP_ERR_INVALID_SIZE);
    assert(bulk.total == 0U && bulk.removed == 0U && !bulk.complete);
    assert(!bulk.outcome_unknown && !bulk.recovery_required && bulk.revision == before);
    assert(fixture_exists("overflow.lua") && fixture_exists("item_255.lua"));
    expect_no_transaction_files();
    expect_source("item_000.lua", "abc");
}

typedef struct {
    script_store_host_fault_t fault;
    const char *from;
    const char *to;
    ryz_script_store_commit_t commit;
    bool cleanup_failure;
} fault_case_t;

static void test_put_io_failures(void)
{
    init_store();
    const fault_case_t cases[] = {
        {SCRIPT_HOST_WRITE_CLOSE, ".ryz-txn", NULL, RYZ_SCRIPT_STORE_NOT_COMMITTED, false},
        {SCRIPT_HOST_WRITE_PARTIAL, ".ryz-new", NULL, RYZ_SCRIPT_STORE_NOT_COMMITTED, false},
        {SCRIPT_HOST_WRITE_CLOSE, ".ryz-new", NULL, RYZ_SCRIPT_STORE_NOT_COMMITTED, false},
        {SCRIPT_HOST_RENAME_BEFORE, "fault.lua", ".ryz-old", RYZ_SCRIPT_STORE_NOT_COMMITTED, false},
        {SCRIPT_HOST_RENAME_AFTER, "fault.lua", ".ryz-old", RYZ_SCRIPT_STORE_NOT_COMMITTED, false},
        {SCRIPT_HOST_RENAME_BEFORE, ".ryz-new", "fault.lua", RYZ_SCRIPT_STORE_NOT_COMMITTED, false},
        {SCRIPT_HOST_RENAME_AFTER, ".ryz-new", "fault.lua", RYZ_SCRIPT_STORE_COMMITTED, false},
        {SCRIPT_HOST_REMOVE_BEFORE, ".ryz-old", NULL, RYZ_SCRIPT_STORE_COMMITTED, true},
        {SCRIPT_HOST_REMOVE_AFTER, ".ryz-old", NULL, RYZ_SCRIPT_STORE_COMMITTED, true},
        {SCRIPT_HOST_REMOVE_BEFORE, ".ryz-txn", NULL, RYZ_SCRIPT_STORE_COMMITTED, true},
        {SCRIPT_HOST_REMOVE_AFTER, ".ryz-txn", NULL, RYZ_SCRIPT_STORE_COMMITTED, true},
    };
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        put("fault.lua", "old complete source");
        script_store_host_fault(cases[i].fault, cases[i].from, cases[i].to, 0U, 1U);
        ryz_script_store_mutation_t result;
        assert(ryz_script_store_put("fault.lua", "new complete source", 19U, NULL, &result) == ESP_FAIL);
        assert(script_store_host_fault_hits() == 1U);
        assert(result.commit == cases[i].commit);
        assert(result.recovery_required == cases[i].cleanup_failure);
        assert((result.cleanup_error != ESP_OK) == cases[i].cleanup_failure);
        const char *visible = cases[i].commit == RYZ_SCRIPT_STORE_COMMITTED ?
            "new complete source" : "old complete source";
        expect_source("fault.lua", visible);
        if (result.recovery_required) {
            ryz_script_store_mutation_t blocked;
            assert(ryz_script_store_put("blocked.lua", "abc", 3U, NULL, &blocked) ==
                   ESP_ERR_INVALID_STATE);
            assert(blocked.commit == RYZ_SCRIPT_STORE_NOT_COMMITTED);
        }
        script_store_host_clear_fault();
        init_store();
        expect_source("fault.lua", visible);
        expect_no_transaction_files();
    }
    /* Missing destination: pre-commit failure must not create a runnable file. */
    script_store_host_fault(SCRIPT_HOST_RENAME_BEFORE, ".ryz-new", "fresh.lua", 0U, 1U);
    ryz_script_store_mutation_t result;
    assert(ryz_script_store_put("fresh.lua", "abc", 3U, "", &result) == ESP_FAIL);
    assert(script_store_host_fault_hits() == 1U);
    assert(result.commit == RYZ_SCRIPT_STORE_NOT_COMMITTED && !result.recovery_required);
    ryz_script_store_snapshot_t snapshot;
    assert(ryz_script_store_get("fresh.lua", &snapshot) == ESP_ERR_NOT_FOUND);
    script_store_host_clear_fault();
    init_store();
    expect_no_transaction_files();
}

static void test_remove_io_failures(void)
{
    init_store();
    const fault_case_t cases[] = {
        {SCRIPT_HOST_WRITE_CLOSE, ".ryz-txn", NULL, RYZ_SCRIPT_STORE_NOT_COMMITTED, false},
        {SCRIPT_HOST_RENAME_BEFORE, "fault.lua", ".ryz-old", RYZ_SCRIPT_STORE_NOT_COMMITTED, false},
        {SCRIPT_HOST_RENAME_AFTER, "fault.lua", ".ryz-old", RYZ_SCRIPT_STORE_COMMITTED, false},
        {SCRIPT_HOST_REMOVE_BEFORE, ".ryz-old", NULL, RYZ_SCRIPT_STORE_COMMITTED, true},
        {SCRIPT_HOST_REMOVE_AFTER, ".ryz-old", NULL, RYZ_SCRIPT_STORE_COMMITTED, true},
        {SCRIPT_HOST_REMOVE_BEFORE, ".ryz-txn", NULL, RYZ_SCRIPT_STORE_COMMITTED, true},
        {SCRIPT_HOST_REMOVE_AFTER, ".ryz-txn", NULL, RYZ_SCRIPT_STORE_COMMITTED, true},
    };
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        put("fault.lua", "old complete source");
        script_store_host_fault(cases[i].fault, cases[i].from, cases[i].to, 0U, 1U);
        ryz_script_store_mutation_t result;
        assert(ryz_script_store_remove("fault.lua", NULL, &result) == ESP_FAIL);
        assert(script_store_host_fault_hits() == 1U);
        assert(result.commit == cases[i].commit);
        assert(result.recovery_required == cases[i].cleanup_failure);
        assert((result.cleanup_error != ESP_OK) == cases[i].cleanup_failure);
        if (cases[i].commit == RYZ_SCRIPT_STORE_COMMITTED) {
            ryz_script_store_snapshot_t snapshot;
            assert(ryz_script_store_get("fault.lua", &snapshot) == ESP_ERR_NOT_FOUND);
        } else expect_source("fault.lua", "old complete source");
        script_store_host_clear_fault();
        init_store();
        if (cases[i].commit == RYZ_SCRIPT_STORE_COMMITTED) {
            ryz_script_store_snapshot_t snapshot;
            assert(ryz_script_store_get("fault.lua", &snapshot) == ESP_ERR_NOT_FOUND);
        } else expect_source("fault.lua", "old complete source");
        expect_no_transaction_files();
    }
}

static void test_partial_journal(void)
{
    init_store();
    put("safe.lua", "unchanged complete source");
    script_store_host_fault(SCRIPT_HOST_WRITE_PARTIAL, ".ryz-txn", NULL, 0U, 1U);
    ryz_script_store_mutation_t result;
    assert(ryz_script_store_put("safe.lua", "new", 3U, NULL, &result) == ESP_FAIL);
    assert(script_store_host_fault_hits() == 1U);
    assert(result.commit == RYZ_SCRIPT_STORE_COMMIT_UNKNOWN && result.recovery_required);
    assert(fixture_exists(".ryz-txn"));
    assert(!fixture_exists(".ryz-new") && !fixture_exists(".ryz-old"));
    expect_source("safe.lua", "unchanged complete source");
    script_store_host_clear_fault();
    assert(ryz_script_store_init(test_root, &result) == ESP_ERR_INVALID_STATE);
    assert(result.commit == RYZ_SCRIPT_STORE_COMMIT_UNKNOWN && result.recovery_required);
    assert(ryz_script_store_recover(&result) == ESP_ERR_INVALID_STATE);
    assert(ryz_script_store_put("other.lua", "abc", 3U, NULL, &result) == ESP_ERR_INVALID_STATE);
    assert(ryz_script_store_remove("safe.lua", NULL, &result) == ESP_ERR_INVALID_STATE);
    expect_source("safe.lua", "unchanged complete source");
    assert(fixture_exists(".ryz-txn"));
}

static void test_rollback_io_failures(void)
{
    init_store();
    const script_store_host_fault_t failures[] = {
        SCRIPT_HOST_RENAME_BEFORE, SCRIPT_HOST_RENAME_AFTER,
    };
    for (size_t i = 0U; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        put("fault.lua", "old source");
        script_store_host_fault(SCRIPT_HOST_RENAME_BEFORE, ".ryz-new", "fault.lua", 0U, 1U);
        script_store_host_add_fault(failures[i], ".ryz-old", "fault.lua", 0U, 1U);
        ryz_script_store_mutation_t result;
        assert(ryz_script_store_put("fault.lua", "new source", 10U, NULL, &result) == ESP_FAIL);
        assert(script_store_host_fault_hits() == 2U);
        assert(result.commit == RYZ_SCRIPT_STORE_COMMIT_UNKNOWN && result.recovery_required);
        ryz_script_store_mutation_t blocked;
        assert(ryz_script_store_remove("fault.lua", NULL, &blocked) == ESP_ERR_INVALID_STATE);
        if (failures[i] == SCRIPT_HOST_RENAME_BEFORE) {
            ryz_script_store_snapshot_t snapshot;
            assert(ryz_script_store_get("fault.lua", &snapshot) == ESP_ERR_NOT_FOUND);
            fixture_assert_bytes(".ryz-old", "old source", 10U);
        } else expect_source("fault.lua", "old source");
        script_store_host_clear_fault();
        assert(ryz_script_store_init(test_root, &result) == ESP_OK);
        assert(result.commit == RYZ_SCRIPT_STORE_NOT_COMMITTED && !result.recovery_required);
        expect_source("fault.lua", "old source");
        expect_no_transaction_files();
    }
}

static void test_stage_cleanup_failures(void)
{
    init_store();
    put("safe.lua", "old source");
    const script_store_host_fault_t failures[] = {
        SCRIPT_HOST_REMOVE_BEFORE, SCRIPT_HOST_REMOVE_AFTER,
    };
    for (size_t i = 0U; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        script_store_host_fault(SCRIPT_HOST_WRITE_PARTIAL, ".ryz-new", NULL, 0U, 1U);
        script_store_host_add_fault(failures[i], ".ryz-new", NULL, 0U, 1U);
        ryz_script_store_mutation_t result;
        assert(ryz_script_store_put("safe.lua", "new source", 10U, NULL, &result) == ESP_FAIL);
        assert(script_store_host_fault_hits() == 2U);
        assert(result.commit == RYZ_SCRIPT_STORE_NOT_COMMITTED && result.recovery_required);
        assert(result.cleanup_error == ESP_FAIL);
        expect_source("safe.lua", "old source");
        assert(fixture_exists(".ryz-new") == (failures[i] == SCRIPT_HOST_REMOVE_BEFORE));
        script_store_host_clear_fault();
        assert(ryz_script_store_init(test_root, &result) == ESP_OK);
        assert(result.commit == RYZ_SCRIPT_STORE_NOT_COMMITTED && !result.recovery_required);
        expect_source("safe.lua", "old source");
        expect_no_transaction_files();
    }
}

static void test_orphan_artifacts(void)
{
    fixture_write("safe.lua", "abc", 3U);
    fixture_write(".ryz-old", "unknown original", 16U);
    fixture_write(".ryz-new", "partial", 7U);
    ryz_script_store_mutation_t result;
    assert(ryz_script_store_init(test_root, &result) == ESP_ERR_INVALID_STATE);
    assert(result.commit == RYZ_SCRIPT_STORE_COMMIT_UNKNOWN && result.recovery_required);
    expect_source("safe.lua", "abc");
    assert(ryz_script_store_put("safe.lua", "new", 3U, NULL, &result) == ESP_ERR_INVALID_STATE);
    assert(ryz_script_store_recover(&result) == ESP_ERR_INVALID_STATE);
    fixture_assert_bytes(".ryz-old", "unknown original", 16U);
    fixture_assert_bytes(".ryz-new", "partial", 7U);
}

static void test_io_degraded_status(void)
{
    init_store();
    put("safe.lua", "abc");
    const script_store_host_fault_t failures[] = {
        SCRIPT_HOST_READ_INVALID_STATE, SCRIPT_HOST_VISIT_INVALID_STATE,
    };
    for (size_t i = 0U; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        const char *leaf = failures[i] == SCRIPT_HOST_READ_INVALID_STATE ?
            "safe.lua" : strrchr(test_root, '/') + 1;
        script_store_host_fault(failures[i], leaf, NULL, 0U, 1U);
        if (failures[i] == SCRIPT_HOST_READ_INVALID_STATE) {
            ryz_script_store_snapshot_t snapshot;
            assert(ryz_script_store_get("safe.lua", &snapshot) == ESP_ERR_INVALID_STATE);
            assert(snapshot.source == NULL);
        } else {
            ryz_script_store_page_t page;
            assert(ryz_script_store_list(0U, 16U, 0U, &page) == ESP_ERR_INVALID_STATE);
            assert(page.count == 0U && page.total == 0U);
        }
        assert(script_store_host_fault_hits() == 1U);
        ryz_script_store_status_t status;
        assert(ryz_script_store_status(&status) == ESP_OK);
        assert(status.ready && status.recovery_required);
        assert(status.recovery_error == ESP_ERR_INVALID_STATE);
        ryz_script_store_mutation_t result;
        assert(ryz_script_store_put("safe.lua", "new", 3U, NULL, &result) == ESP_ERR_INVALID_STATE);
        script_store_host_clear_fault();
        init_store();
        expect_source("safe.lua", "abc");
    }
    /* Ordinary missing data/invalid sources do not latch an I/O-wide fault. */
    ryz_script_store_snapshot_t snapshot;
    assert(ryz_script_store_get("missing.lua", &snapshot) == ESP_ERR_NOT_FOUND);
    ryz_script_store_status_t status;
    assert(ryz_script_store_status(&status) == ESP_OK && !status.recovery_required);
}

static void test_oversized_maintenance(void)
{
    const size_t large_size = 131089U;
    const char *large_sha = "ec66e5e1a869692e4bb7385d813a9bd54e38fdfdc5a430d400646de6d2216d3e";
    fixture_repeated("remove_large.lua", 'L', large_size);
    fixture_repeated("repair_large.lua", 'L', large_size);
    fixture_repeated("boot.lua", 'L', large_size);
    init_store();
    script_store_host_reset_allocation_peak();
    ryz_script_store_description_t description;
    assert(ryz_script_store_describe("remove_large.lua", &description) == ESP_OK);
    assert(description.entry.bytes == large_size && !description.entry.protected_file);
    assert(strcmp(description.sha256, large_sha) == 0);
    assert(script_store_host_allocation_peak() == 0U);
    ryz_script_store_snapshot_t snapshot;
    assert(ryz_script_store_get("remove_large.lua", &snapshot) == ESP_ERR_INVALID_SIZE);
    assert(snapshot.source == NULL);
    assert(script_store_host_allocation_peak() == 0U);
    ryz_script_store_mutation_t result;
    assert(ryz_script_store_remove("remove_large.lua", sha_abc, &result) == ESP_ERR_INVALID_STATE);
    assert(ryz_script_store_remove("remove_large.lua", description.sha256, &result) == ESP_OK);
    assert(result.commit == RYZ_SCRIPT_STORE_COMMITTED);
    assert(ryz_script_store_describe("remove_large.lua", &description) == ESP_ERR_NOT_FOUND);
    assert(description.entry.name[0] == '\0' && description.sha256[0] == '\0');
    assert(ryz_script_store_describe("repair_large.lua", &description) == ESP_OK);
    assert(description.entry.bytes == large_size && strcmp(description.sha256, large_sha) == 0);
    assert(ryz_script_store_put("repair_large.lua", "abc", 3U, description.sha256, &result) == ESP_OK);
    assert(result.commit == RYZ_SCRIPT_STORE_COMMITTED);
    assert(script_store_host_allocation_peak() == 0U); /* No whole-file core allocation. */
    expect_source("repair_large.lua", "abc");
    assert(ryz_script_store_describe("boot.lua", &description) == ESP_OK);
    assert(description.entry.bytes == large_size && !description.entry.protected_file);
    assert(strcmp(description.sha256, large_sha) == 0);
    assert(ryz_script_store_get("boot.lua", &snapshot) == ESP_ERR_INVALID_SIZE);
    assert(ryz_script_store_put("boot.lua", "abc", 3U, description.sha256, &result) == ESP_OK);
    assert(ryz_script_store_remove("boot.lua", sha_abc, &result) == ESP_OK);
    assert(!fixture_exists("boot.lua"));
    expect_no_transaction_files();
}

static void test_corrupt_maintenance(void)
{
    fixture_write("empty.lua", "", 0U);
    const char nul[] = {'a', '\0', 'b'};
    fixture_write("nul.lua", nul, sizeof(nul));
    char path[PATH_MAX];
    path_for("link.lua", path);
    assert(symlink("nul.lua", path) == 0);
    path_for("dir.lua", path);
    assert(mkdir(path, 0700) == 0);
    path_for("pipe.lua", path);
    assert(mkfifo(path, 0600) == 0);
    init_store();
    ryz_script_store_description_t description;
    const char *bad[] = {"link.lua", "dir.lua", "pipe.lua"};
    for (size_t i = 0U; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        assert(ryz_script_store_describe(bad[i], &description) == ESP_ERR_NOT_SUPPORTED);
        assert(description.entry.name[0] == '\0' && description.sha256[0] == '\0');
    }
    assert(ryz_script_store_describe("empty.lua", &description) == ESP_OK);
    assert(description.entry.bytes == 0U);
    assert(strcmp(description.sha256,
                  "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
    ryz_script_store_mutation_t result;
    assert(ryz_script_store_remove("empty.lua", description.sha256, &result) == ESP_OK);
    assert(result.commit == RYZ_SCRIPT_STORE_COMMITTED);
    assert(ryz_script_store_describe("nul.lua", &description) == ESP_OK);
    assert(description.entry.bytes == 3U);
    assert(strcmp(description.sha256,
                  "59b271ae1bbcb1d31d41929817f4b16fb439eb4f31520b5ad1d5ce98920a7138") == 0);
    assert(ryz_script_store_put("nul.lua", "repaired", 8U, description.sha256, &result) == ESP_OK);
    assert(result.commit == RYZ_SCRIPT_STORE_COMMITTED);
    expect_source("nul.lua", "repaired");
    expect_no_transaction_files();
}

static void test_describe_degraded_status(void)
{
    fixture_repeated("large.lua", 'L', 131089U);
    init_store();
    script_store_host_fault(SCRIPT_HOST_READ_INVALID_STATE, "large.lua", NULL, 0U, 1U);
    ryz_script_store_description_t description;
    assert(ryz_script_store_describe("large.lua", &description) == ESP_ERR_INVALID_STATE);
    assert(script_store_host_fault_hits() == 1U);
    assert(description.entry.name[0] == '\0' && description.sha256[0] == '\0');
    ryz_script_store_status_t status;
    size_t calls = script_store_host_platform_calls();
    assert(ryz_script_store_status(&status) == ESP_OK);
    assert(script_store_host_platform_calls() == calls);
    assert(status.ready && status.recovery_required && !status.capacity_valid);
    assert(status.recovery_error == ESP_ERR_INVALID_STATE && status.capacity_error == ESP_ERR_INVALID_STATE);
    assert(status.total_bytes == 0U && status.used_bytes == 0U);
    script_store_host_clear_fault();
    init_store();
    assert(ryz_script_store_describe("large.lua", &description) == ESP_OK);
}

typedef struct { esp_err_t error; ryz_script_store_mutation_t result; } recovery_thread_t;

static void *recover_thread(void *context)
{
    recovery_thread_t *recovery = context;
    recovery->error = ryz_script_store_recover(&recovery->result);
    return NULL;
}

static void test_status_cached_capacity(void)
{
    script_store_host_capacity(100000U, 1234U);
    init_store();
    ryz_script_store_status_t status;
    size_t calls = script_store_host_platform_calls();
    assert(ryz_script_store_status(&status) == ESP_OK);
    assert(script_store_host_platform_calls() == calls);
    assert(status.capacity_valid && status.capacity_error == ESP_OK);
    assert(status.total_bytes == 100000U && status.used_bytes == 1234U);
    script_store_host_capacity(200000U, 4321U);
    const char *root_leaf = strrchr(test_root, '/') + 1;
    script_store_host_fault(SCRIPT_HOST_CAPACITY_INVALID_STATE, root_leaf, NULL, 0U, 1U);
    for (unsigned i = 0U; i < 100U; ++i) {
        assert(ryz_script_store_status(&status) == ESP_OK);
        assert(status.capacity_valid && status.total_bytes == 100000U && status.used_bytes == 1234U);
    }
    assert(script_store_host_platform_calls() == calls);
    assert(script_store_host_fault_hits() == 0U);
    ryz_script_store_mutation_t result;
    assert(ryz_script_store_recover(&result) == ESP_ERR_INVALID_STATE);
    assert(script_store_host_fault_hits() == 1U);
    calls = script_store_host_platform_calls();
    assert(ryz_script_store_status(&status) == ESP_OK);
    assert(script_store_host_platform_calls() == calls);
    assert(!status.capacity_valid && status.capacity_error == ESP_ERR_INVALID_STATE);
    assert(status.total_bytes == 0U && status.used_bytes == 0U && status.recovery_required);
    script_store_host_clear_fault();
    init_store();
    assert(ryz_script_store_status(&status) == ESP_OK);
    assert(status.capacity_valid && status.total_bytes == 200000U && status.used_bytes == 4321U);

    script_store_host_capacity_gate_arm();
    calls = script_store_host_platform_calls();
    assert(ryz_script_store_status(&status) == ESP_OK);
    assert(script_store_host_platform_calls() == calls); /* Armed capacity gate not entered. */
    recovery_thread_t recovery = {0};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, recover_thread, &recovery) == 0);
    script_store_host_capacity_gate_wait();
    calls = script_store_host_platform_calls();
    assert(ryz_script_store_status(&status) == ESP_ERR_TIMEOUT);
    assert(!status.ready && !status.capacity_valid);
    assert(script_store_host_platform_calls() == calls); /* Busy status never queries FS. */
    script_store_host_capacity_gate_release();
    assert(pthread_join(thread, NULL) == 0 && recovery.error == ESP_OK);
    calls = script_store_host_platform_calls();
    assert(ryz_script_store_status(&status) == ESP_OK);
    assert(script_store_host_platform_calls() == calls);
    assert(status.capacity_valid && status.total_bytes == 200000U && status.used_bytes == 4321U);

    script_store_host_capacity(300000U, 5678U);
    put("refresh.lua", "abc");
    assert(ryz_script_store_status(&status) == ESP_OK);
    assert(status.total_bytes == 300000U && status.used_bytes == 5678U);
    script_store_host_capacity(400000U, 6789U);
    assert(ryz_script_store_remove("refresh.lua", NULL, &result) == ESP_OK);
    assert(ryz_script_store_status(&status) == ESP_OK);
    assert(status.total_bytes == 400000U && status.used_bytes == 6789U);
    /* Lua data shares the volume but must not mutate the script revision.
     * Only the worker's explicit refresh queries the filesystem. */
    uint32_t revision = status.revision;
    script_store_host_capacity(500000U, 7890U);
    assert(ryz_script_store_refresh_capacity() == ESP_OK);
    calls = script_store_host_platform_calls();
    assert(ryz_script_store_status(&status) == ESP_OK);
    assert(script_store_host_platform_calls() == calls);
    assert(status.capacity_valid && status.revision == revision);
    assert(status.total_bytes == 500000U && status.used_bytes == 7890U);
    script_store_host_real_capacity();
}

static void test_new_file_scan_degraded(void)
{
    init_store();
    const char *root_leaf = strrchr(test_root, '/') + 1;
    script_store_host_fault(SCRIPT_HOST_VISIT_INVALID_STATE, root_leaf, NULL, 0U, 1U);
    script_store_host_add_fault(SCRIPT_HOST_CAPACITY_INVALID_STATE, root_leaf, NULL, 0U, 1U);
    ryz_script_store_mutation_t result;
    assert(ryz_script_store_put("new.lua", "abc", 3U, "", &result) == ESP_ERR_INVALID_STATE);
    assert(script_store_host_fault_hits() == 2U);
    assert(result.commit == RYZ_SCRIPT_STORE_NOT_COMMITTED && result.recovery_required);
    ryz_script_store_status_t status;
    size_t calls = script_store_host_platform_calls();
    assert(ryz_script_store_status(&status) == ESP_OK);
    assert(script_store_host_platform_calls() == calls);
    assert(status.ready && status.recovery_required && !status.capacity_valid);
    assert(status.recovery_error == ESP_ERR_INVALID_STATE && status.capacity_error == ESP_ERR_INVALID_STATE);
    assert(status.total_bytes == 0U && status.used_bytes == 0U);
    assert(!fixture_exists("new.lua"));
    expect_no_transaction_files();
    script_store_host_clear_fault();
    init_store();
    put("new.lua", "abc");
    expect_source("new.lua", "abc");
}

static void test_interrupted_large_put(void)
{
    fixture_repeated("restart.lua", 'L', 131089U);
    init_store();
    script_store_host_fault(SCRIPT_HOST_RENAME_INTERRUPT_AFTER, "restart.lua", ".ryz-old", 0U, 1U);
    ryz_script_store_mutation_t result;
    (void)ryz_script_store_put("restart.lua", "new source", 10U, NULL, &result);
    assert(false && "named interruption did not execute");
}

static void test_reopen_large_rollback(void)
{
    assert(fixture_exists(".ryz-txn") && fixture_exists(".ryz-new") && fixture_exists(".ryz-old"));
    assert(!fixture_exists("restart.lua"));
    script_store_host_reset_allocation_peak();
    ryz_script_store_mutation_t recovery;
    assert(ryz_script_store_init(test_root, &recovery) == ESP_OK);
    assert(recovery.commit == RYZ_SCRIPT_STORE_NOT_COMMITTED && !recovery.recovery_required);
    ryz_script_store_description_t description;
    assert(ryz_script_store_describe("restart.lua", &description) == ESP_OK);
    assert(description.entry.bytes == 131089U);
    assert(strcmp(description.sha256,
                  "ec66e5e1a869692e4bb7385d813a9bd54e38fdfdc5a430d400646de6d2216d3e") == 0);
    ryz_script_store_snapshot_t snapshot;
    assert(ryz_script_store_get("restart.lua", &snapshot) == ESP_ERR_INVALID_SIZE);
    assert(snapshot.source == NULL && script_store_host_allocation_peak() == 0U);
    expect_no_transaction_files();
    assert(ryz_script_store_put("restart.lua", "after recovery", 14U,
                                description.sha256, &recovery) == ESP_OK);
    expect_source("restart.lua", "after recovery");
}

static void test_interrupted_large_remove(void)
{
    fixture_repeated("restart.lua", 'L', 131089U);
    init_store();
    script_store_host_fault(SCRIPT_HOST_RENAME_INTERRUPT_AFTER, "restart.lua", ".ryz-old", 0U, 1U);
    ryz_script_store_mutation_t result;
    (void)ryz_script_store_remove("restart.lua", NULL, &result);
    assert(false && "named interruption did not execute");
}

static void test_interrupted_put_before_commit(void)
{
    init_store();
    put("restart.lua", "old source");
    script_store_host_fault(SCRIPT_HOST_RENAME_INTERRUPT_AFTER, "restart.lua", ".ryz-old", 0U, 1U);
    ryz_script_store_mutation_t result;
    (void)ryz_script_store_put("restart.lua", "new source", 10U, NULL, &result);
    assert(false && "named interruption did not execute");
}

static void test_reopen_rollback(void)
{
    assert(fixture_exists(".ryz-txn") && fixture_exists(".ryz-new") && fixture_exists(".ryz-old"));
    assert(!fixture_exists("restart.lua"));
    ryz_script_store_mutation_t recovery;
    assert(ryz_script_store_init(test_root, &recovery) == ESP_OK);
    assert(recovery.commit == RYZ_SCRIPT_STORE_NOT_COMMITTED && !recovery.recovery_required);
    expect_source("restart.lua", "old source");
    expect_no_transaction_files();
    put("restart.lua", "after recovery");
    expect_source("restart.lua", "after recovery");
}

static void test_interrupted_put_after_commit(void)
{
    init_store();
    put("restart.lua", "old source");
    script_store_host_fault(SCRIPT_HOST_RENAME_INTERRUPT_AFTER, ".ryz-new", "restart.lua", 0U, 1U);
    ryz_script_store_mutation_t result;
    (void)ryz_script_store_put("restart.lua", "new source", 10U, NULL, &result);
    assert(false && "named interruption did not execute");
}

static void test_reopen_put_commit(void)
{
    assert(fixture_exists(".ryz-txn") && !fixture_exists(".ryz-new") && fixture_exists(".ryz-old"));
    ryz_script_store_mutation_t recovery;
    assert(ryz_script_store_init(test_root, &recovery) == ESP_OK);
    assert(recovery.commit == RYZ_SCRIPT_STORE_COMMITTED && !recovery.recovery_required);
    expect_source("restart.lua", "new source");
    expect_no_transaction_files();
    init_store();
    expect_source("restart.lua", "new source");
}

static void test_interrupted_remove(void)
{
    init_store();
    put("restart.lua", "old source");
    script_store_host_fault(SCRIPT_HOST_RENAME_INTERRUPT_AFTER, "restart.lua", ".ryz-old", 0U, 1U);
    ryz_script_store_mutation_t result;
    (void)ryz_script_store_remove("restart.lua", NULL, &result);
    assert(false && "named interruption did not execute");
}

static void test_reopen_remove_commit(void)
{
    assert(fixture_exists(".ryz-txn") && !fixture_exists(".ryz-new") && fixture_exists(".ryz-old"));
    ryz_script_store_mutation_t recovery;
    assert(ryz_script_store_init(test_root, &recovery) == ESP_OK);
    assert(recovery.commit == RYZ_SCRIPT_STORE_COMMITTED && !recovery.recovery_required);
    ryz_script_store_snapshot_t snapshot;
    assert(ryz_script_store_get("restart.lua", &snapshot) == ESP_ERR_NOT_FOUND);
    expect_no_transaction_files();
    put("restart.lua", "replacement");
    expect_source("restart.lua", "replacement");
}

typedef void (*test_fn_t)(void);
static uint32_t revision(void)
{
    ryz_script_store_status_t status;
    assert(ryz_script_store_status(&status) == ESP_OK);
    return status.revision;
}

static void test_copy_boot(void)
{
    init_store();
    ryz_script_store_mutation_t selected = put("selected.lua", "abc"), result;
    put("boot.lua", "previous startup");
    uint32_t before = revision();
    assert(ryz_script_store_copy_boot("selected.lua", sha_abc, before, &result) == ESP_OK);
    assert(result.commit == RYZ_SCRIPT_STORE_COMMITTED && result.bytes == 3);
    assert(!strcmp(result.sha256, sha_abc) && result.revision > before);
    expect_source("boot.lua", "abc"); expect_source("selected.lua", "abc");
    before = revision();
    script_store_host_capacity(128, 128);
    assert(ryz_script_store_copy_boot("boot.lua", sha_abc, before, &result) == ESP_OK);
    assert(result.commit == RYZ_SCRIPT_STORE_COMMITTED && result.revision == before);
    assert(ryz_script_store_copy_boot("selected.lua", sha_abc, before, &result) == ESP_OK);
    assert(result.commit == RYZ_SCRIPT_STORE_COMMITTED && result.revision == before);
    script_store_host_real_capacity();
    put("selected.lua", "source changed");
    assert(ryz_script_store_copy_boot("selected.lua", selected.sha256, before, &result) == ESP_ERR_INVALID_STATE);
    assert(result.commit == RYZ_SCRIPT_STORE_NOT_COMMITTED);
    assert(ryz_script_store_copy_boot("selected.lua", selected.sha256, revision(), &result) == ESP_ERR_INVALID_STATE);
    expect_source("boot.lua", "abc");
    assert(ryz_script_store_copy_boot("selected.lua", sha_abc, 0, &result) == ESP_ERR_INVALID_ARG);
    assert(ryz_script_store_copy_boot("../bad.lua", sha_abc, before, &result) == ESP_ERR_INVALID_ARG);
    assert(ryz_script_store_copy_boot("selected.lua", NULL, before, &result) == ESP_ERR_INVALID_ARG);
    assert(ryz_script_store_copy_boot("selected.lua", "", before, &result) == ESP_ERR_INVALID_ARG);
    assert(ryz_script_store_copy_boot("selected.lua", sha_abc, before, NULL) == ESP_ERR_INVALID_ARG);
    assert(ryz_script_store_copy_boot("missing.lua", sha_abc, revision(), &result) == ESP_ERR_NOT_FOUND);
    expect_no_transaction_files();
}

static void test_copy_boot_bad_sources_and_admission(void)
{
    fixture_write("empty.lua", "", 0);
    const char nul[] = {'a', 0, 'b'};
    fixture_write("nul.lua", nul, sizeof(nul));
    fixture_repeated("huge.lua", 'x', RYZ_SCRIPT_STORE_SOURCE_MAX + 1U);
    char path[PATH_MAX]; path_for("link.lua", path); assert(symlink("nul.lua", path) == 0);
    init_store(); put("boot.lua", "old boot");
    const char *names[] = {"empty.lua", "nul.lua", "huge.lua"};
    const esp_err_t errors[] = {ESP_ERR_INVALID_SIZE, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_SIZE};
    ryz_script_store_mutation_t result;
    for (unsigned i=0; i<3; ++i) {
        ryz_script_store_description_t description;
        assert(ryz_script_store_describe(names[i], &description) == ESP_OK);
        assert(ryz_script_store_copy_boot(names[i], description.sha256, revision(), &result) == errors[i]);
        assert(result.commit == RYZ_SCRIPT_STORE_NOT_COMMITTED);
        expect_source("boot.lua", "old boot");
    }
    assert(ryz_script_store_copy_boot("link.lua", sha_abc, revision(), &result) == ESP_ERR_NOT_SUPPORTED);
    put("selected.lua", "abc");
    uint32_t before = revision();
    script_store_host_fail_allocation_once();
    assert(ryz_script_store_copy_boot("selected.lua", sha_abc, before, &result) == ESP_ERR_NO_MEM);
    script_store_host_capacity(100, 99);
    assert(ryz_script_store_copy_boot("selected.lua", sha_abc, before, &result) == ESP_ERR_NO_MEM);
    script_store_host_real_capacity();
    assert(revision() == before);
    expect_source("boot.lua", "old boot"); expect_no_transaction_files();
}

static void test_copy_boot_faults(void)
{
    init_store(); put("selected.lua", "abc");
    const fault_case_t cases[] = {
        {SCRIPT_HOST_WRITE_PARTIAL, ".ryz-new", NULL, RYZ_SCRIPT_STORE_NOT_COMMITTED, false},
        {SCRIPT_HOST_RENAME_AFTER, "boot.lua", ".ryz-old", RYZ_SCRIPT_STORE_NOT_COMMITTED, false},
        {SCRIPT_HOST_RENAME_BEFORE, ".ryz-new", "boot.lua", RYZ_SCRIPT_STORE_NOT_COMMITTED, false},
        {SCRIPT_HOST_RENAME_AFTER, ".ryz-new", "boot.lua", RYZ_SCRIPT_STORE_COMMITTED, false},
        {SCRIPT_HOST_REMOVE_BEFORE, ".ryz-old", NULL, RYZ_SCRIPT_STORE_COMMITTED, true},
    };
    for (unsigned i=0; i<sizeof(cases)/sizeof(cases[0]); ++i) {
        put("boot.lua", "old boot");
        uint32_t before = revision();
        script_store_host_fault(cases[i].fault, cases[i].from, cases[i].to, 0, 1);
        ryz_script_store_mutation_t result;
        assert(ryz_script_store_copy_boot("selected.lua", sha_abc, before, &result) == ESP_FAIL);
        assert(result.commit == cases[i].commit && result.recovery_required == cases[i].cleanup_failure);
        expect_source("boot.lua", result.commit == RYZ_SCRIPT_STORE_COMMITTED ? "abc" : "old boot");
        expect_source("selected.lua", "abc");
        script_store_host_clear_fault(); init_store(); expect_no_transaction_files();
    }
}

static void test_delete_all(void)
{
    fixture_write("boot.lua", "abc", 3); fixture_write("tool_i2c.lua", "abc", 3);
    fixture_write("empty.lua", "", 0); fixture_write("nul.lua", "a\0b", 3);
    fixture_repeated("large.lua", 'x', 131089);
    fixture_write("readme.txt", "keep", 4); fixture_write(".system-private", "keep", 4);
    fixture_write("bad name.lua", "keep", 4);
    char path[PATH_MAX]; path_for("dir.lua", path); assert(mkdir(path,0700)==0);
    path_for("link.lua", path); assert(symlink("readme.txt",path)==0);
    path_for("pipe.lua", path); assert(mkfifo(path,0600)==0);
    init_store(); uint32_t before = revision();
    ryz_script_store_bulk_t result;
    assert(ryz_script_store_delete_all(before,&result)==ESP_OK);
    assert(result.total==5 && result.removed==5 && result.complete && !result.outcome_unknown);
    assert(!result.cleanup_error && !result.recovery_required && !result.failed_name[0]);
    assert(result.revision==before+5);
    ryz_script_store_page_t page;
    assert(ryz_script_store_list(0,16,0,&page)==ESP_OK && !page.total);
    const char *kept[]={"readme.txt",".system-private","bad name.lua","dir.lua","link.lua","pipe.lua"};
    for(unsigned i=0;i<sizeof(kept)/sizeof(kept[0]);++i) assert(fixture_exists(kept[i]));
    assert(ryz_script_store_delete_all(before,&result)==ESP_ERR_INVALID_STATE && !result.complete);
    before=revision();
    assert(ryz_script_store_delete_all(before,&result)==ESP_OK);
    assert(!result.total && !result.removed && result.complete && result.revision==before);
    expect_no_transaction_files();
}

static void test_delete_all_admission(void)
{
    ryz_script_store_bulk_t result;
    memset(&result,0xff,sizeof(result));
    assert(ryz_script_store_delete_all(0,&result)==ESP_ERR_INVALID_ARG);
    assert(!result.total && !result.removed && !result.complete && !result.revision);
    assert(ryz_script_store_delete_all(1,NULL)==ESP_ERR_INVALID_ARG);
    assert(ryz_script_store_delete_all(1,&result)==ESP_ERR_INVALID_STATE);
    init_store(); put("boot.lua","abc"); uint32_t selected=revision(); put("new.lua","abc");
    assert(ryz_script_store_delete_all(selected,&result)==ESP_ERR_INVALID_STATE);
    assert(!result.total && !result.removed && !result.complete && !result.failed_name[0]);
    script_store_host_fail_allocation_once();
    assert(ryz_script_store_delete_all(revision(),&result)==ESP_ERR_NO_MEM && !result.removed);
    script_store_host_capacity(4096,4095);
    assert(ryz_script_store_delete_all(revision(),&result)==ESP_ERR_NO_MEM);
    assert(result.total==2 && !result.removed && !result.complete && !result.outcome_unknown);
    script_store_host_real_capacity();
    expect_source("boot.lua","abc"); expect_source("new.lua","abc"); expect_no_transaction_files();
    fixture_write(".upload.bak","unknown user bytes",18);
    assert(ryz_script_store_delete_all(revision(),&result)==ESP_ERR_INVALID_STATE && result.recovery_required);
    assert(!result.removed && fixture_exists(".upload.bak") && fixture_exists("boot.lua"));
}

static void test_delete_all_faults(void)
{
    init_store();
    for(unsigned failure=0;failure<6;++failure) {
        put("a.lua","abc"); put("b.lua","abc"); put("boot.lua","abc");
        if(failure==0) script_store_host_fault(SCRIPT_HOST_RENAME_BEFORE,"b.lua",".ryz-old",0,1);
        else if(failure==1) script_store_host_fault(SCRIPT_HOST_RENAME_AFTER,"b.lua",".ryz-old",0,1);
        else if(failure==2) script_store_host_fault(SCRIPT_HOST_REMOVE_BEFORE,".ryz-old",NULL,1,1);
        else if(failure==3) script_store_host_fault(SCRIPT_HOST_REMOVE_AFTER,".ryz-txn",NULL,1,1);
        else if(failure==4) script_store_host_fault(SCRIPT_HOST_REMOVE_BEFORE,".ryz-old",NULL,2,1);
        else script_store_host_fault(SCRIPT_HOST_WRITE_PARTIAL,".ryz-txn",NULL,1,1);
        ryz_script_store_bulk_t result;
        assert(ryz_script_store_delete_all(revision(),&result)==ESP_FAIL);
        size_t removed=failure==0 || failure==5 ? 1 : failure==4 ? 3 : 2;
        assert(result.total==3 && result.removed==removed);
        assert(result.complete==(failure==4) && result.outcome_unknown==(failure==5));
        assert(!strcmp(result.failed_name,failure==4 ? "boot.lua" : "b.lua"));
        assert(result.recovery_required==(failure>=2));
        assert((result.cleanup_error!=ESP_OK)==(failure>=2 && failure<=4));
        assert(!fixture_exists("a.lua"));
        assert(fixture_exists("b.lua")== (removed==1));
        assert(fixture_exists("boot.lua")== (removed<3));
        script_store_host_clear_fault();
        if(failure==5) {
            assert(fixture_exists(".ryz-txn"));
            assert(ryz_script_store_delete_all(revision(),&result)==ESP_ERR_INVALID_STATE);
            assert(!result.removed && result.recovery_required);
        } else { init_store(); expect_no_transaction_files(); }
    }
}

static void test_combined_postcommit_capacity_fault(void)
{
    init_store();
    put("selected.lua", "abc");
    const char *root_leaf = strrchr(test_root, '/') + 1;
    uint32_t before = revision();
    /* The transaction's capacity admission succeeds; only its final cached
     * status refresh observes a permanent platform failure after commit. */
    script_store_host_fault(SCRIPT_HOST_CAPACITY_INVALID_STATE, root_leaf, NULL, 1U, 1U);
    ryz_script_store_mutation_t copy;
    assert(ryz_script_store_copy_boot("selected.lua", sha_abc, before, &copy) == ESP_ERR_INVALID_STATE);
    assert(copy.commit == RYZ_SCRIPT_STORE_COMMITTED && copy.revision == before + 1U);
    assert(copy.recovery_required && copy.cleanup_error == ESP_OK);
    expect_source("boot.lua", "abc");
    expect_no_transaction_files();
    script_store_host_clear_fault();
    init_store();

    before = revision();
    /* Bulk admission plus the two per-file admissions succeed. The final
     * refresh fails after both independent transactions have completed. */
    script_store_host_fault(SCRIPT_HOST_CAPACITY_INVALID_STATE, root_leaf, NULL, 3U, 1U);
    ryz_script_store_bulk_t bulk;
    assert(ryz_script_store_delete_all(before, &bulk) == ESP_ERR_INVALID_STATE);
    assert(bulk.total == 2U && bulk.removed == 2U && bulk.complete);
    assert(!bulk.outcome_unknown && bulk.recovery_required && bulk.cleanup_error == ESP_OK);
    assert(!bulk.failed_name[0] && bulk.revision == before + 2U);
    assert(!fixture_exists("selected.lua") && !fixture_exists("boot.lua"));
    expect_no_transaction_files();
    ryz_script_store_status_t status;
    assert(ryz_script_store_status(&status) == ESP_OK);
    assert(status.recovery_required && !status.capacity_valid);
    assert(status.capacity_error == ESP_ERR_INVALID_STATE);
    script_store_host_clear_fault();
    init_store();
}

typedef struct { bool bulk; uint32_t expected; esp_err_t error;
    ryz_script_store_bulk_t all; ryz_script_store_mutation_t copy; } combined_job_t;
static void *combined_worker(void *context)
{
    combined_job_t *job=context;
    job->error=job->bulk ? ryz_script_store_delete_all(job->expected,&job->all) :
        ryz_script_store_copy_boot("selected.lua",sha_abc,job->expected,&job->copy);
    return NULL;
}
static void test_combined_operations_hold_one_lock(void)
{
    init_store(); put("selected.lua","abc");
    for(unsigned i=0;i<2;++i) {
        combined_job_t job={.bulk=i!=0,.expected=revision()};
        script_store_host_sha_gate_arm(); pthread_t worker;
        assert(pthread_create(&worker,NULL,combined_worker,&job)==0);
        script_store_host_sha_gate_wait();
        ryz_script_store_bulk_t all; ryz_script_store_mutation_t copy;
        ryz_script_store_status_t status;
        assert(ryz_script_store_status(&status)==ESP_ERR_TIMEOUT);
        assert(ryz_script_store_delete_all(job.expected,&all)==ESP_ERR_TIMEOUT && !all.removed);
        assert(ryz_script_store_copy_boot("selected.lua",sha_abc,job.expected,&copy)==ESP_ERR_TIMEOUT);
        assert(ryz_script_store_put("selected.lua","changed",7,NULL,&copy)==ESP_ERR_TIMEOUT);
        assert(ryz_script_store_remove("boot.lua",NULL,&copy)==ESP_ERR_TIMEOUT);
        script_store_host_sha_gate_release(); assert(pthread_join(worker,NULL)==0 && job.error==ESP_OK);
        if(!i) expect_source("boot.lua","abc");
        else assert(job.all.total==2 && job.all.removed==2 && job.all.complete);
    }
}

/* Historic serialized record fixture, not a production test-only entry point. */
typedef struct { char magic[8], operation, had_target, target[41], old_sha[65], new_sha[65], record_sha[65]; } historic_journal_t;
_Static_assert(sizeof(historic_journal_t)==246,"journal fixture byte layout");
static void historic_journal(bool boot, bool remove)
{
    const char *name=boot ? "boot.lua" : "legacy.lua";
    fixture_write(remove ? ".ryz-old" : name,"abc",3);
    historic_journal_t journal={.operation=remove?'R':'P',.had_target=remove?'1':'0'};
    memcpy(journal.magic,"RYZST01",8); strcpy(journal.target,name);
    strcpy(remove ? journal.old_sha : journal.new_sha,sha_abc);
    script_store_host_fixture_sha256(&journal,offsetof(historic_journal_t,record_sha),journal.record_sha);
    fixture_write(".ryz-txn",&journal,sizeof(journal));
    ryz_script_store_mutation_t result;
    if(boot) {
        assert(ryz_script_store_init(test_root,&result)==ESP_ERR_INVALID_STATE);
        assert(result.commit==RYZ_SCRIPT_STORE_COMMIT_UNKNOWN && result.recovery_required);
        fixture_assert_bytes(".ryz-txn",&journal,sizeof(journal));
        fixture_assert_bytes(name,"abc",3);
    } else {
        assert(ryz_script_store_init(test_root,&result)==ESP_OK);
        assert(result.commit==RYZ_SCRIPT_STORE_COMMITTED && !result.recovery_required);
        assert(fixture_exists(name)==!remove); expect_no_transaction_files();
    }
}
static void test_legacy_journal_put(void) { historic_journal(false,false); }
static void test_legacy_journal_remove(void) { historic_journal(false,true); }
static void test_legacy_journal_boot_still_rejected(void) { historic_journal(true,false); }

static void test_interrupt_bulk(void)
{
    init_store(); put("a.lua","abc"); put("b.lua","abc"); put("boot.lua","abc");
    script_store_host_fault(SCRIPT_HOST_RENAME_INTERRUPT_AFTER,"b.lua",".ryz-old",0,1);
    ryz_script_store_bulk_t result;
    (void)ryz_script_store_delete_all(revision(),&result);
    assert(false);
}
static void test_reopen_bulk_does_not_resume(void)
{
    ryz_script_store_mutation_t recovery;
    assert(ryz_script_store_init(test_root,&recovery)==ESP_OK);
    assert(recovery.commit==RYZ_SCRIPT_STORE_COMMITTED && !recovery.recovery_required);
    assert(!fixture_exists("a.lua") && !fixture_exists("b.lua"));
    expect_source("boot.lua","abc"); expect_no_transaction_files();
    ryz_script_store_bulk_t result;
    assert(ryz_script_store_delete_all(revision(),&result)==ESP_OK);
    assert(result.total==1 && result.removed==1 && result.complete);
}

static void interrupt_boot(bool after)
{
    init_store(); put("selected.lua","abc"); put("boot.lua","old boot");
    script_store_host_fault(SCRIPT_HOST_RENAME_INTERRUPT_AFTER,
                            after ? ".ryz-new" : "boot.lua",after ? "boot.lua" : ".ryz-old",0,1);
    ryz_script_store_mutation_t result;
    (void)ryz_script_store_copy_boot("selected.lua",sha_abc,revision(),&result);
    assert(false);
}
static void test_interrupt_boot_before(void) { interrupt_boot(false); }
static void test_interrupt_boot_after(void) { interrupt_boot(true); }
static void reopen_boot(bool after)
{
    ryz_script_store_mutation_t recovery;
    assert(ryz_script_store_init(test_root,&recovery)==ESP_OK);
    assert(recovery.commit==(after ? RYZ_SCRIPT_STORE_COMMITTED : RYZ_SCRIPT_STORE_NOT_COMMITTED));
    assert(!recovery.recovery_required); expect_source("boot.lua",after?"abc":"old boot");
    expect_source("selected.lua","abc"); expect_no_transaction_files();
}
static void test_reopen_boot_before(void) { reopen_boot(false); }
static void test_reopen_boot_after(void) { reopen_boot(true); }

typedef struct { const char *name; test_fn_t run; } test_case_t;

int main(int argc, char **argv)
{
    assert(argc == 3);
    test_root = argv[2];
    script_store_host_configure(test_root);
    const test_case_t cases[] = {
        {"validation", test_validation}, {"roundtrip", test_roundtrip},
        {"catalog", test_catalog}, {"nonregular", test_nonregular_and_corrupt},
        {"cas", test_cas}, {"boot", test_boot}, {"legacy", test_legacy_artifacts},
        {"concurrency", test_concurrency}, {"capacity", test_capacity},
        {"index_limit", test_index_limit},
        {"put_io", test_put_io_failures}, {"remove_io", test_remove_io_failures},
        {"partial_journal", test_partial_journal},
        {"rollback_io", test_rollback_io_failures},
        {"stage_cleanup", test_stage_cleanup_failures},
        {"orphan_artifacts", test_orphan_artifacts},
        {"io_degraded", test_io_degraded_status},
        {"oversized_maintenance", test_oversized_maintenance},
        {"corrupt_maintenance", test_corrupt_maintenance},
        {"describe_degraded", test_describe_degraded_status},
        {"status_cache", test_status_cached_capacity},
        {"new_scan_degraded", test_new_file_scan_degraded},
        {"interrupt_large_put", test_interrupted_large_put},
        {"reopen_large_rollback", test_reopen_large_rollback},
        {"interrupt_large_remove", test_interrupted_large_remove},
        {"interrupt_before_commit", test_interrupted_put_before_commit},
        {"reopen_rollback", test_reopen_rollback},
        {"interrupt_after_commit", test_interrupted_put_after_commit},
        {"reopen_put_commit", test_reopen_put_commit},
        {"interrupt_remove", test_interrupted_remove},
        {"reopen_remove_commit", test_reopen_remove_commit},
        {"copy_boot", test_copy_boot}, {"copy_boot_bad", test_copy_boot_bad_sources_and_admission},
        {"copy_boot_faults", test_copy_boot_faults}, {"delete_all", test_delete_all},
        {"bulk_admission", test_delete_all_admission}, {"bulk_faults", test_delete_all_faults},
        {"combined_capacity_fault", test_combined_postcommit_capacity_fault},
        {"combined_lock", test_combined_operations_hold_one_lock},
        {"v1_put", test_legacy_journal_put}, {"v1_remove", test_legacy_journal_remove},
        {"v1_boot", test_legacy_journal_boot_still_rejected},
        {"interrupt_bulk", test_interrupt_bulk}, {"reopen_bulk", test_reopen_bulk_does_not_resume},
        {"interrupt_boot_before", test_interrupt_boot_before}, {"interrupt_boot_after", test_interrupt_boot_after},
        {"reopen_boot_before", test_reopen_boot_before}, {"reopen_boot_after", test_reopen_boot_after},
    };
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (strcmp(argv[1], cases[i].name) == 0) {
            cases[i].run();
            printf("SCRIPT_STORE_PASS %s\n", argv[1]);
            return 0;
        }
    }
    fprintf(stderr, "Unknown Script Store test: %s\n", argv[1]);
    return 2;
}
