#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L

#include "ryz_script_store.h"
#include "script_store_host.h"

#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FIRST INT64_C(1735689600)
#define SECOND INT64_C(1767225600)
#define LATER INT64_C(1893456000)
#define TIME_STAGE ".ryz-time-new"

static const char *test_root;
static const char sha_abc[] = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

/* Frozen on-disk compatibility fixtures, not private production declarations. */
typedef struct {
    char magic[8], target[41], source_sha[65], created[17], modified[17], record_sha[65];
} disk_time_t;
typedef struct {
    char magic[8], operation, had_target, target[41], old_sha[65], new_sha[65];
    char had_time;
    disk_time_t old_time, new_time;
    char record_sha[65];
} disk_journal_t;
typedef struct {
    char magic[8], operation, had_target, target[41], old_sha[65], new_sha[65], record_sha[65];
} disk_legacy_t;
_Static_assert(sizeof(disk_time_t) == 213, "time compatibility fixture");
_Static_assert(sizeof(disk_journal_t) == 673, "v3 compatibility fixture");
_Static_assert(sizeof(disk_legacy_t) == 246, "v1/v2 compatibility fixture");

/* Only legacy/external/corrupt disk fixtures use direct POSIX operations.
 * All behavior and recovery are exercised through the public Store API. */
static void path_for(const char *leaf, char out[PATH_MAX])
{
    assert(leaf && *leaf && !strchr(leaf, '/') && strcmp(leaf, ".") && strcmp(leaf, ".."));
    int length = snprintf(out, PATH_MAX, "%s/%s", test_root, leaf);
    assert(length > 0 && length < PATH_MAX);
}

static void fixture_write(const char *leaf, const void *data, size_t size)
{
    char path[PATH_MAX]; path_for(leaf, path);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    assert(fd >= 0);
    size_t written = 0;
    while (written < size) {
        ssize_t amount = write(fd, (const char *)data + written, size - written);
        assert(amount > 0); written += (size_t)amount;
    }
    assert(!fsync(fd)); assert(!close(fd));
}

static size_t fixture_read(const char *leaf, void *data, size_t capacity)
{
    char path[PATH_MAX]; path_for(leaf, path);
    int fd = open(path, O_RDONLY | O_NOFOLLOW); assert(fd >= 0);
    size_t total = 0;
    while (total < capacity) {
        ssize_t count = read(fd, (char *)data + total, capacity - total);
        assert(count >= 0); if (!count) break; total += (size_t)count;
    }
    char extra; assert(read(fd, &extra, 1) == 0); assert(!close(fd)); return total;
}

static bool exists(const char *leaf)
{
    char path[PATH_MAX]; path_for(leaf, path); struct stat info;
    return lstat(path, &info) == 0;
}

static void fixture_replace(const char *leaf, const void *data, size_t size)
{
    char path[PATH_MAX]; path_for(leaf, path);
    struct stat info; assert(lstat(path, &info) == 0 && S_ISREG(info.st_mode));
    assert(!unlink(path)); fixture_write(leaf, data, size);
}

static void assert_bytes(const char *leaf, const void *expected, size_t size)
{
    unsigned char bytes[1024]; assert(size <= sizeof(bytes));
    assert(fixture_read(leaf, bytes, sizeof(bytes)) == size);
    assert(!memcmp(bytes, expected, size));
}

static void sidecar(const char *name, char leaf[31])
{
    char hash[65]; script_store_host_fixture_sha256(name, strlen(name), hash);
    memcpy(leaf, ".ryz-t", 6); memcpy(leaf + 6, hash, 24); leaf[30] = 0;
}

static void seal_time(disk_time_t *record)
{
    script_store_host_fixture_sha256(record, offsetof(disk_time_t, record_sha), record->record_sha);
}

static disk_time_t make_time(const char *name, const char *sha, int64_t created, int64_t modified)
{
    disk_time_t record = {0}; memcpy(record.magic, "RYZTM01", 8);
    assert(strlen(name) < sizeof(record.target)); strcpy(record.target, name);
    strcpy(record.source_sha, sha);
    snprintf(record.created, sizeof(record.created), "%016" PRIx64, (uint64_t)created);
    snprintf(record.modified, sizeof(record.modified), "%016" PRIx64, (uint64_t)modified);
    seal_time(&record); return record;
}

static uint32_t revision(void)
{
    ryz_script_store_status_t status;
    assert(ryz_script_store_status(&status) == ESP_OK); return status.revision;
}

static void init_store(void)
{
    ryz_script_store_mutation_t result;
    assert(ryz_script_store_init(test_root, &result) == ESP_OK);
    ryz_script_store_status_t status;
    assert(ryz_script_store_status(&status) == ESP_OK && status.ready && !status.recovery_required);
}

static ryz_script_store_mutation_t put(const char *name, const char *source)
{
    ryz_script_store_mutation_t result;
    assert(ryz_script_store_put(name, source, strlen(source), NULL, &result) == ESP_OK);
    assert(result.commit == RYZ_SCRIPT_STORE_COMMITTED && !result.recovery_required && !result.cleanup_error);
    return result;
}

static void expect(const char *name, const char *source, int64_t created, int64_t modified)
{
    ryz_script_store_snapshot_t snapshot;
    assert(ryz_script_store_get(name, &snapshot) == ESP_OK);
    assert(!strcmp(snapshot.entry.name, name) && snapshot.entry.bytes == strlen(source));
    assert(!memcmp(snapshot.source, source, strlen(source) + 1));
    char digest[65]; script_store_host_fixture_sha256(source, strlen(source), digest);
    assert(!strcmp(snapshot.sha256, digest));
    assert(snapshot.times.created_unix == created && snapshot.times.modified_unix == modified);
    ryz_script_store_description_t raw;
    assert(ryz_script_store_describe(name, &raw) == ESP_OK);
    assert(!strcmp(raw.entry.name, name) && raw.entry.bytes == snapshot.entry.bytes);
    assert(!strcmp(raw.sha256, snapshot.sha256));
    assert(raw.times.created_unix == created && raw.times.modified_unix == modified);
    ryz_script_store_snapshot_free(&snapshot);
    assert(!snapshot.source && !snapshot.times.created_unix && !snapshot.times.modified_unix);
}

static void clean(void)
{
    assert(!exists(".ryz-txn") && !exists(".ryz-new") && !exists(".ryz-old") && !exists(TIME_STAGE));
    ryz_script_store_status_t status;
    assert(ryz_script_store_status(&status) == ESP_OK && !status.recovery_required);
}

static void basic(void)
{
    init_store(); script_store_host_utc(FIRST);
    ryz_script_store_mutation_t first = put("known.lua", "abc");
    assert(!strcmp(first.sha256, sha_abc)); expect("known.lua", "abc", FIRST, FIRST);
    char leaf[31]; sidecar("known.lua", leaf);
    unsigned char before[213]; assert(fixture_read(leaf, before, sizeof(before)) == sizeof(before));
    uint32_t old_revision = revision(); script_store_host_utc(LATER);
    put("known.lua", "abc"); assert(revision() == old_revision);
    assert_bytes(leaf, before, sizeof(before)); expect("known.lua", "abc", FIRST, FIRST);
    script_store_host_utc(SECOND); put("known.lua", "changed");
    expect("known.lua", "changed", FIRST, SECOND);
    script_store_host_utc(0); put("known.lua", "offline");
    expect("known.lua", "offline", FIRST, 0);
    script_store_host_utc(LATER); put("known.lua", "online");
    expect("known.lua", "online", FIRST, LATER); clean();
}

static void unknown_creation(void)
{
    fixture_write("legacy.lua", "old", 3); init_store();
    expect("legacy.lua", "old", 0, 0);
    script_store_host_utc(FIRST); put("legacy.lua", "abc");
    expect("legacy.lua", "abc", 0, FIRST);
    script_store_host_utc(SECOND); put("legacy.lua", "changed");
    expect("legacy.lua", "changed", 0, SECOND);
    script_store_host_utc(0); put("offline.lua", "abc");
    expect("offline.lua", "abc", 0, 0);
    script_store_host_utc(LATER); put("offline.lua", "changed");
    expect("offline.lua", "changed", 0, LATER); clean();
}

static void recreate(void)
{
    init_store(); script_store_host_utc(FIRST); put("same.lua", "abc");
    char leaf[31]; sidecar("same.lua", leaf);
    ryz_script_store_mutation_t result;
    assert(ryz_script_store_remove("same.lua", sha_abc, &result) == ESP_OK);
    assert(result.commit == RYZ_SCRIPT_STORE_COMMITTED && !exists(leaf)); clean();
    script_store_host_utc(SECOND); put("same.lua", "abc");
    expect("same.lua", "abc", SECOND, SECOND);
    assert(ryz_script_store_remove("same.lua", sha_abc, &result) == ESP_OK);
    script_store_host_utc(0); put("same.lua", "abc");
    expect("same.lua", "abc", 0, 0); clean();
}

static void copy_boot(void)
{
    init_store(); script_store_host_utc(FIRST); put("source.lua", "abc");
    script_store_host_utc(SECOND);
    ryz_script_store_mutation_t result;
    assert(ryz_script_store_copy_boot("source.lua", sha_abc, revision(), &result) == ESP_OK);
    expect("source.lua", "abc", FIRST, FIRST); expect("boot.lua", "abc", SECOND, SECOND);
    uint32_t before = revision(); script_store_host_utc(LATER);
    assert(ryz_script_store_copy_boot("source.lua", sha_abc, before, &result) == ESP_OK);
    assert(revision() == before); expect("boot.lua", "abc", SECOND, SECOND);
    assert(ryz_script_store_copy_boot("boot.lua", sha_abc, before, &result) == ESP_OK);
    assert(revision() == before);
    ryz_script_store_mutation_t source = put("source.lua", "changed");
    script_store_host_utc(0);
    assert(ryz_script_store_copy_boot("source.lua", source.sha256, revision(), &result) == ESP_OK);
    expect("boot.lua", "changed", SECOND, 0); clean();
}

static void bounded_clock(void)
{
    static const int64_t values[] = {0, -1, INT64_C(1704067199), INT64_C(253402300800), INT64_MAX,
                                     INT64_C(1704067200), INT64_C(253402300799)};
    init_store();
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        char name[24]; snprintf(name, sizeof(name), "clock%zu.lua", i);
        script_store_host_utc(values[i]); put(name, "abc");
        int64_t expected = i < 5 ? 0 : values[i]; expect(name, "abc", expected, expected);
    }
    script_store_host_utc(SECOND); put("backward.lua", "abc");
    script_store_host_utc(FIRST); put("backward.lua", "changed");
    expect("backward.lua", "changed", SECOND, FIRST); clean();
}

static void assert_incomplete(const ryz_script_store_mutation_t *result)
{
    assert(result->commit == RYZ_SCRIPT_STORE_COMMITTED);
    assert(result->cleanup_error != ESP_OK && result->recovery_required);
    ryz_script_store_status_t status;
    assert(ryz_script_store_status(&status) == ESP_OK);
    assert(status.ready && status.recovery_required && status.recovery_error != ESP_OK);
    assert(exists(".ryz-txn"));
}

static void time_fault(unsigned fault)
{
    static const script_store_host_fault_t kinds[] = {
        SCRIPT_HOST_WRITE_PARTIAL, SCRIPT_HOST_WRITE_CLOSE,
        SCRIPT_HOST_REMOVE_BEFORE, SCRIPT_HOST_REMOVE_AFTER,
        SCRIPT_HOST_RENAME_BEFORE, SCRIPT_HOST_RENAME_AFTER,
    };
    assert(fault < sizeof(kinds) / sizeof(kinds[0]));
    init_store(); script_store_host_utc(FIRST); put("safe.lua", "abc");
    char leaf[31]; sidecar("safe.lua", leaf);
    script_store_host_utc(SECOND);
    script_store_host_fault(kinds[fault], fault == 2 || fault == 3 ? leaf : TIME_STAGE,
                            fault >= 4 ? leaf : NULL, 0, 1);
    ryz_script_store_mutation_t result;
    assert(ryz_script_store_put("safe.lua", "changed", 7, sha_abc, &result) == ESP_FAIL);
    assert(script_store_host_fault_hits() == 1); assert_incomplete(&result);
    expect("safe.lua", "changed", 0, 0);
    ryz_script_store_mutation_t rejected;
    assert(ryz_script_store_put("safe.lua", "other", 5, NULL, &rejected) == ESP_ERR_INVALID_STATE);
    assert(rejected.commit == RYZ_SCRIPT_STORE_NOT_COMMITTED);
}

static void delete_fault(unsigned fault)
{
    assert(fault < 2); init_store(); script_store_host_utc(FIRST); put("safe.lua", "abc");
    char leaf[31]; sidecar("safe.lua", leaf);
    script_store_host_fault(fault ? SCRIPT_HOST_REMOVE_AFTER : SCRIPT_HOST_REMOVE_BEFORE,
                            leaf, NULL, 0, 1);
    ryz_script_store_mutation_t result;
    assert(ryz_script_store_remove("safe.lua", sha_abc, &result) == ESP_FAIL);
    assert(script_store_host_fault_hits() == 1); assert_incomplete(&result);
    ryz_script_store_description_t absent;
    assert(ryz_script_store_describe("safe.lua", &absent) == ESP_ERR_NOT_FOUND);
    assert(!exists("safe.lua"));
}

static void reopen(const char *which)
{
    script_store_host_utc(LATER);
    ryz_script_store_mutation_t result;
    assert(ryz_script_store_init(test_root, &result) == ESP_OK);
    if (!strcmp(which, "put")) {
        assert(result.commit == RYZ_SCRIPT_STORE_COMMITTED);
        expect("safe.lua", "changed", FIRST, SECOND);
    } else if (!strcmp(which, "rollback")) {
        assert(result.commit == RYZ_SCRIPT_STORE_NOT_COMMITTED);
        expect("safe.lua", "abc", FIRST, FIRST);
    } else if (!strcmp(which, "remove")) {
        assert(result.commit == RYZ_SCRIPT_STORE_COMMITTED);
        ryz_script_store_description_t absent;
        assert(ryz_script_store_describe("safe.lua", &absent) == ESP_ERR_NOT_FOUND);
        char leaf[31]; sidecar("safe.lua", leaf); assert(!exists(leaf));
        put("safe.lua", "abc"); expect("safe.lua", "abc", LATER, LATER);
    } else if (!strcmp(which, "persisted")) {
        script_store_host_utc(0); expect("safe.lua", "abc", FIRST, FIRST);
    } else assert(false);
    clean();
}

static void interrupt_at(unsigned boundary)
{
    init_store(); script_store_host_utc(FIRST); put("safe.lua", "abc");
    script_store_host_utc(SECOND);
    char leaf[31]; sidecar("safe.lua", leaf);
    static const char *const from[] = {"safe.lua", ".ryz-new", TIME_STAGE, "safe.lua"};
    static const char *const to[] = {".ryz-old", "safe.lua", NULL, ".ryz-old"};
    assert(boundary < 4);
    script_store_host_fault(SCRIPT_HOST_RENAME_INTERRUPT_AFTER,
                            from[boundary], boundary == 2 ? leaf : to[boundary], 0, 1);
    ryz_script_store_mutation_t result;
    if (boundary == 3) (void)ryz_script_store_remove("safe.lua", sha_abc, &result);
    else (void)ryz_script_store_put("safe.lua", "changed", 7, sha_abc, &result);
    assert(!"expected fixture process interruption");
}

static void retry_stage_cleanup(unsigned after)
{
    time_fault(0); script_store_host_clear_fault(); script_store_host_utc(LATER);
    script_store_host_fault(after ? SCRIPT_HOST_REMOVE_AFTER : SCRIPT_HOST_REMOVE_BEFORE,
                            TIME_STAGE, NULL, 0, 1);
    ryz_script_store_mutation_t result;
    assert(ryz_script_store_recover(&result) == ESP_FAIL);
    assert(script_store_host_fault_hits() == 1); assert_incomplete(&result);
    expect("safe.lua", "changed", 0, 0);
    script_store_host_clear_fault();
    assert(ryz_script_store_recover(&result) == ESP_OK);
    assert(result.commit == RYZ_SCRIPT_STORE_COMMITTED);
    expect("safe.lua", "changed", FIRST, SECOND); clean();
}

static void expect_blocked_recovery(void)
{
    ryz_script_store_mutation_t result;
    assert(ryz_script_store_recover(&result) != ESP_OK && result.recovery_required);
    assert(ryz_script_store_init(test_root, &result) != ESP_OK && result.recovery_required);
    ryz_script_store_status_t status;
    assert(ryz_script_store_status(&status) == ESP_OK && status.ready && status.recovery_required);
    assert(status.recovery_error != ESP_OK);
}

static void corrupt_sidecar(unsigned kind)
{
    assert(kind < 8); init_store();
    if (kind != 6) fixture_write("safe.lua", "abc", 3);
    disk_time_t record = make_time(kind == 2 ? "other.lua" : "safe.lua", sha_abc, FIRST, FIRST);
    char leaf[31]; sidecar(kind == 7 ? "different.lua" : "safe.lua", leaf);
    size_t bytes = sizeof(record);
    if (kind == 0) bytes = 7;
    if (kind == 1) record.record_sha[0] = record.record_sha[0] == '0' ? '1' : '0';
    if (kind == 3) { script_store_host_fixture_sha256("wrong", 5, record.source_sha); seal_time(&record); }
    if (kind == 4) { memcpy(record.created, "7fffffffffffffff", 16); seal_time(&record); }
    if (kind == 5) { memset(record.target, 'x', sizeof(record.target)); seal_time(&record); }
    fixture_write(leaf, &record, bytes);
    if (kind != 6) expect("safe.lua", "abc", 0, 0);
    if (kind != 7) {
        ryz_script_store_mutation_t rejected;
        assert(ryz_script_store_put("safe.lua", "changed", 7, NULL, &rejected) == ESP_ERR_INVALID_STATE);
        assert(rejected.commit == RYZ_SCRIPT_STORE_NOT_COMMITTED);
    }
    expect_blocked_recovery(); assert_bytes(leaf, &record, bytes);
    if (kind != 6) assert_bytes("safe.lua", "abc", 3);
    assert(!exists(".ryz-txn") && !exists(TIME_STAGE));
}

static void corrupt_stage(unsigned kind)
{
    assert(kind < 3); time_fault(0); script_store_host_clear_fault();
    disk_journal_t journal; assert(fixture_read(".ryz-txn", &journal, sizeof(journal)) == sizeof(journal));
    disk_time_t bad = journal.new_time;
    if (kind == 0) memcpy(bad.magic, "unknown!", 8);
    else if (kind == 1) bad.record_sha[3] = bad.record_sha[3] == '0' ? '1' : '0';
    else { snprintf(bad.modified, sizeof(bad.modified), "%016" PRIx64, (uint64_t)LATER); seal_time(&bad); }
    size_t bytes = kind == 0 ? 8 : sizeof(bad);
    fixture_replace(TIME_STAGE, &bad, bytes);
    expect_blocked_recovery(); assert_bytes(TIME_STAGE, &bad, bytes);
    assert_bytes(".ryz-txn", &journal, sizeof(journal));
    expect("safe.lua", "changed", 0, 0); assert_bytes(".ryz-old", "abc", 3);
}

static void stage_prefix(unsigned kind)
{
    static const size_t sizes[] = {0, 7, 106, 213};
    assert(kind < 8); time_fault(0); script_store_host_clear_fault();
    disk_journal_t journal; assert(fixture_read(".ryz-txn", &journal, sizeof(journal)) == sizeof(journal));
    const disk_time_t *record = kind < 4 ? &journal.new_time : &journal.old_time;
    fixture_replace(TIME_STAGE, record, sizes[kind % 4]);
    script_store_host_utc(LATER); ryz_script_store_mutation_t result;
    assert(ryz_script_store_recover(&result) == ESP_OK && result.commit == RYZ_SCRIPT_STORE_COMMITTED);
    expect("safe.lua", "changed", FIRST, SECOND); clean();
}

static void orphan_stage(void)
{
    fixture_write("safe.lua", "abc", 3); fixture_write(TIME_STAGE, "unidentified", 12);
    ryz_script_store_mutation_t result;
    assert(ryz_script_store_init(test_root, &result) == ESP_ERR_INVALID_STATE && result.recovery_required);
    expect_blocked_recovery(); assert_bytes(TIME_STAGE, "unidentified", 12);
    expect("safe.lua", "abc", 0, 0);
}

static void raw_sources(void)
{
    static const char *const names[] = {"empty.lua", "nul.lua", "large.lua"};
    char bytes[RYZ_SCRIPT_STORE_SOURCE_MAX + 1U]; memset(bytes, 'x', sizeof(bytes)); bytes[1] = 0;
    const size_t sizes[] = {0, 3, sizeof(bytes)};
    for (size_t i = 0; i < 3; ++i) {
        fixture_write(names[i], bytes, sizes[i]);
        char digest[65], leaf[31]; script_store_host_fixture_sha256(bytes, sizes[i], digest);
        disk_time_t record = make_time(names[i], digest, FIRST, SECOND);
        sidecar(names[i], leaf); fixture_write(leaf, &record, sizeof(record));
    }
    fixture_write(".user-private", "keep", 4); init_store();
    for (size_t i = 0; i < 3; ++i) {
        ryz_script_store_description_t raw;
        assert(ryz_script_store_describe(names[i], &raw) == ESP_OK && raw.entry.bytes == sizes[i]);
        char digest[65]; script_store_host_fixture_sha256(bytes, sizes[i], digest);
        assert(!strcmp(raw.sha256, digest) && raw.times.created_unix == FIRST && raw.times.modified_unix == SECOND);
        ryz_script_store_snapshot_t invalid;
        assert(ryz_script_store_get(names[i], &invalid) == (i == 1 ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_SIZE));
        assert(!invalid.source && !invalid.times.created_unix && !invalid.times.modified_unix);
        ryz_script_store_mutation_t result;
        assert(ryz_script_store_remove(names[i], digest, &result) == ESP_OK);
        char leaf[31]; sidecar(names[i], leaf); assert(!exists(leaf));
    }
    assert_bytes(".user-private", "keep", 4); clean();
}

static void bulk_cleanup(void)
{
    init_store(); script_store_host_utc(FIRST); put("a.lua", "abc"); put("b.lua", "abc");
    put("boot.lua", "abc"); char leaf[31]; sidecar("b.lua", leaf);
    script_store_host_fault(SCRIPT_HOST_REMOVE_BEFORE, leaf, NULL, 0, 1);
    ryz_script_store_bulk_t result;
    assert(ryz_script_store_delete_all(revision(), &result) == ESP_FAIL);
    assert(result.total == 3 && result.removed == 2 && !result.complete && !result.outcome_unknown);
    assert(result.cleanup_error != ESP_OK && result.recovery_required && !strcmp(result.failed_name, "b.lua"));
    assert(!exists("a.lua") && !exists("b.lua")); expect("boot.lua", "abc", 0, 0);
    script_store_host_clear_fault(); script_store_host_utc(LATER);
    ryz_script_store_mutation_t recovery;
    assert(ryz_script_store_recover(&recovery) == ESP_OK);
    assert(!exists(leaf)); expect("boot.lua", "abc", FIRST, FIRST);
    assert(ryz_script_store_delete_all(revision(), &result) == ESP_OK && result.removed == 1 && result.complete);
    sidecar("boot.lua", leaf); assert(!exists(leaf)); sidecar("a.lua", leaf); assert(!exists(leaf)); clean();
}

static void corrupt_journal(unsigned kind)
{
    assert(kind < 5); time_fault(0); script_store_host_clear_fault();
    disk_journal_t journal; assert(fixture_read(".ryz-txn", &journal, sizeof(journal)) == sizeof(journal));
    if (kind == 0) { strcpy(journal.new_time.source_sha, sha_abc); seal_time(&journal.new_time); }
    if (kind == 1) { strcpy(journal.old_time.target, "other.lua"); seal_time(&journal.old_time); }
    if (kind == 2) journal.had_time = '0';
    if (kind == 3) { memcpy(journal.new_time.modified, "0000000000000001", 16); seal_time(&journal.new_time); }
    if (kind == 4) { journal.had_target = '0'; journal.old_sha[0] = 0; }
    script_store_host_fixture_sha256(&journal, offsetof(disk_journal_t, record_sha), journal.record_sha);
    fixture_replace(".ryz-txn", &journal, sizeof(journal));
    unsigned char stage[213]; size_t stage_bytes = fixture_read(TIME_STAGE, stage, sizeof(stage));
    expect_blocked_recovery(); assert_bytes(".ryz-txn", &journal, sizeof(journal));
    assert_bytes(TIME_STAGE, stage, stage_bytes); assert_bytes(".ryz-old", "abc", 3);
    expect("safe.lua", "changed", 0, 0);
}

static void persist_corrupt(void)
{
    fixture_write("safe.lua", "abc", 3);
    char leaf[31]; sidecar("safe.lua", leaf); fixture_write(leaf, "bad time", 8);
}

static void reopen_corrupt(void)
{
    script_store_host_utc(LATER); ryz_script_store_mutation_t result;
    assert(ryz_script_store_init(test_root, &result) == ESP_ERR_INVALID_STATE);
    assert(result.recovery_required); expect_blocked_recovery();
    char leaf[31]; sidecar("safe.lua", leaf); assert_bytes(leaf, "bad time", 8);
    expect("safe.lua", "abc", 0, 0);
}

static void legacy(unsigned version, unsigned kind)
{
    assert((version == 1 || version == 2) && kind < 4);
    fixture_write("safe.lua", "abc", 3);
    disk_legacy_t journal = {.operation = 'P', .had_target = '0'};
    memcpy(journal.magic, version == 1 ? "RYZST01" : "RYZST02", 8);
    strcpy(journal.target, "safe.lua"); strcpy(journal.new_sha, sha_abc);
    script_store_host_fixture_sha256(&journal, offsetof(disk_legacy_t, record_sha), journal.record_sha);
    fixture_write(".ryz-txn", &journal, sizeof(journal));
    disk_time_t record = make_time("safe.lua", sha_abc, FIRST, FIRST);
    char leaf[31]; sidecar("safe.lua", leaf);
    if (kind == 1) fixture_write(leaf, &record, sizeof(record));
    if (kind == 2) fixture_write(TIME_STAGE, &record, 7);
    if (kind == 3) fixture_write(leaf, "bad", 3);
    script_store_host_utc(LATER); ryz_script_store_mutation_t result;
    if (!kind) {
        assert(ryz_script_store_init(test_root, &result) == ESP_OK);
        assert(result.commit == RYZ_SCRIPT_STORE_COMMITTED);
        expect("safe.lua", "abc", 0, 0); clean();
    } else {
        assert(ryz_script_store_init(test_root, &result) == ESP_ERR_INVALID_STATE);
        assert(result.commit == RYZ_SCRIPT_STORE_COMMIT_UNKNOWN && result.recovery_required);
        expect_blocked_recovery(); assert_bytes(".ryz-txn", &journal, sizeof(journal));
        assert_bytes("safe.lua", "abc", 3);
        if (kind == 1) assert_bytes(leaf, &record, sizeof(record));
        if (kind == 2) assert_bytes(TIME_STAGE, &record, 7);
        if (kind == 3) assert_bytes(leaf, "bad", 3);
    }
}

static void factory_dates(void)
{
    init_store(); /* No NTP clock: dates must already be in the factory image. */
    const char *names[] = {"tool_monitor.lua", "tool_i2c.lua", "tool_hardware.lua"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        ryz_script_store_snapshot_t snapshot;
        assert(ryz_script_store_get(names[i], &snapshot) == ESP_OK);
        fprintf(stderr, "%s CREATED=%" PRId64 " MODIFIED=%" PRId64 "\n",
                names[i], snapshot.times.created_unix, snapshot.times.modified_unix);
        assert(snapshot.times.created_unix == FIRST);
        assert(snapshot.times.modified_unix == FIRST);
        ryz_script_store_description_t described;
        assert(ryz_script_store_describe(names[i], &described) == ESP_OK);
        assert(described.times.created_unix == FIRST && described.times.modified_unix == FIRST);
        ryz_script_store_snapshot_free(&snapshot);
    }
    script_store_host_utc(SECOND);
    put("tool_monitor.lua", "print('updated')\n");
    expect("tool_monitor.lua", "print('updated')\n", FIRST, SECOND);
    script_store_host_utc(0);
    put("tool_monitor.lua", "print('offline edit')\n");
    expect("tool_monitor.lua", "print('offline edit')\n", FIRST, 0);
}

int main(int argc, char **argv)
{
    assert(argc == 3); test_root = argv[2]; script_store_host_configure(test_root);
    if (!strcmp(argv[1], "factory")) factory_dates();
    else if (!strcmp(argv[1], "basic")) basic();
    else if (!strcmp(argv[1], "unknown_creation")) unknown_creation();
    else if (!strcmp(argv[1], "recreate")) recreate();
    else if (!strcmp(argv[1], "copy_boot")) copy_boot();
    else if (!strcmp(argv[1], "bounded_clock")) bounded_clock();
    else if (!strncmp(argv[1], "time_fault_", 11)) time_fault((unsigned)atoi(argv[1] + 11));
    else if (!strncmp(argv[1], "delete_fault_", 13)) delete_fault((unsigned)atoi(argv[1] + 13));
    else if (!strcmp(argv[1], "reopen_corrupt")) reopen_corrupt();
    else if (!strncmp(argv[1], "reopen_", 7)) reopen(argv[1] + 7);
    else if (!strncmp(argv[1], "interrupt_", 10)) interrupt_at((unsigned)atoi(argv[1] + 10));
    else if (!strncmp(argv[1], "stage_retry_", 12)) retry_stage_cleanup((unsigned)atoi(argv[1] + 12));
    else if (!strncmp(argv[1], "sidecar_bad_", 12)) corrupt_sidecar((unsigned)atoi(argv[1] + 12));
    else if (!strncmp(argv[1], "stage_bad_", 10)) corrupt_stage((unsigned)atoi(argv[1] + 10));
    else if (!strncmp(argv[1], "stage_prefix_", 13)) stage_prefix((unsigned)atoi(argv[1] + 13));
    else if (!strcmp(argv[1], "orphan_stage")) orphan_stage();
    else if (!strcmp(argv[1], "raw_sources")) raw_sources();
    else if (!strcmp(argv[1], "bulk_cleanup")) bulk_cleanup();
    else if (!strncmp(argv[1], "journal_bad_", 12)) corrupt_journal((unsigned)atoi(argv[1] + 12));
    else if (!strcmp(argv[1], "persist_corrupt")) persist_corrupt();
    else if (!strncmp(argv[1], "legacy_", 7)) {
        unsigned version, kind; assert(sscanf(argv[1], "legacy_%u_%u", &version, &kind) == 2);
        legacy(version, kind);
    }
    else if (!strcmp(argv[1], "persist")) { init_store(); script_store_host_utc(FIRST); put("safe.lua", "abc"); }
    else { fprintf(stderr, "Unknown case: %s\n", argv[1]); return 2; }
    printf("SCRIPT_STORE_TIME_PASS %s\n", argv[1]); return 0;
}
