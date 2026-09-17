#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ryz_apps.h"
#include "ryz_apps_platform.h"
#include "script_store_host.h"

/* Run the real boot-lifetime Apps task on a pthread. Only its OS scheduling
 * boundary is controlled: requests still go through the public Apps API,
 * reads through the production reader/Store, and hashes through CommonCrypto.
 * No test invokes an owner iteration or substitutes Store state. */
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker;
static void (*worker_function)(void *);
static bool worker_created, stopping;
static unsigned permits, cycles, wakes;
static unsigned init_calls, start_calls, deinit_calls;
static esp_err_t init_error, start_error;
static bool block_start, start_entered, release_start;
static _Thread_local unsigned lock_depth;
static const char *data_root;

static void wait_changed(void)
{
    struct timespec limit;
    assert(clock_gettime(CLOCK_REALTIME, &limit) == 0);
    limit.tv_sec += 5;
    assert(pthread_cond_timedwait(&changed, &gate, &limit) == 0);
}

static void take_permit(void)
{
    while (!permits && !stopping) wait_changed();
    if (stopping) {
        assert(pthread_mutex_unlock(&gate) == 0);
        pthread_exit(NULL);
    }
    --permits;
}

static void *worker_entry(void *unused)
{
    (void)unused;
    assert(pthread_mutex_lock(&gate) == 0);
    take_permit();
    assert(pthread_mutex_unlock(&gate) == 0);
    worker_function(NULL);
    abort();
}

esp_err_t ryz_apps_platform_init(void)
{
    ++init_calls;
    return init_error;
}

void ryz_apps_platform_deinit(void) { ++deinit_calls; }

esp_err_t ryz_apps_platform_start(void (*entry)(void *))
{
    ++start_calls;
    if (start_error != ESP_OK) return start_error;
    assert(pthread_mutex_lock(&gate) == 0);
    start_entered = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    while (block_start && !release_start) wait_changed();
    assert(pthread_mutex_unlock(&gate) == 0);
    assert(!worker_created);
    worker_function = entry;
    assert(pthread_create(&worker, NULL, worker_entry, NULL) == 0);
    worker_created = true;
    return ESP_OK;
}

void ryz_apps_platform_lock(void)
{
    assert(lock_depth == 0);
    assert(pthread_mutex_lock(&cache_lock) == 0);
    ++lock_depth;
}

void ryz_apps_platform_unlock(void)
{
    assert(lock_depth == 1);
    --lock_depth;
    assert(pthread_mutex_unlock(&cache_lock) == 0);
}

void ryz_apps_platform_wake(void)
{
    assert(lock_depth == 0);
    assert(pthread_mutex_lock(&gate) == 0);
    ++wakes;
    assert(pthread_cond_broadcast(&changed) == 0);
    assert(pthread_mutex_unlock(&gate) == 0);
}

void ryz_apps_platform_wait(void)
{
    assert(lock_depth == 0);
    assert(pthread_mutex_lock(&gate) == 0);
    ++cycles;
    assert(pthread_cond_broadcast(&changed) == 0);
    take_permit();
    assert(pthread_mutex_unlock(&gate) == 0);
}

static unsigned permit_cycle(void)
{
    assert(pthread_mutex_lock(&gate) == 0);
    const unsigned expected = cycles + 1U;
    ++permits;
    assert(pthread_cond_broadcast(&changed) == 0);
    assert(pthread_mutex_unlock(&gate) == 0);
    return expected;
}

static void wait_cycle(unsigned expected)
{
    assert(pthread_mutex_lock(&gate) == 0);
    while (cycles < expected) wait_changed();
    assert(pthread_mutex_unlock(&gate) == 0);
}

static void step(void) { wait_cycle(permit_cycle()); }

static unsigned wake_count(void)
{
    assert(pthread_mutex_lock(&gate) == 0);
    const unsigned count = wakes;
    assert(pthread_mutex_unlock(&gate) == 0);
    return count;
}

static void stop_worker(void)
{
    if (!worker_created) return;
    assert(pthread_mutex_lock(&gate) == 0);
    stopping = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    assert(pthread_mutex_unlock(&gate) == 0);
    assert(pthread_join(worker, NULL) == 0);
}

static void assert_zero(const void *value, size_t size)
{
    const unsigned char *bytes = value;
    for (size_t i = 0; i < size; ++i) assert(bytes[i] == 0);
}

static void assert_no_data(const ryz_apps_snapshot_t *value)
{
    assert_zero(&value->page, sizeof(value->page));
    assert_zero(&value->detail, sizeof(value->detail));
}

static ryz_apps_snapshot_t snapshot(void)
{
    ryz_apps_snapshot_t value;
    memset(&value, 0xa5, sizeof(value));
    const size_t calls = script_store_host_platform_calls();
    assert(ryz_apps_get_snapshot(&value) == ESP_OK);
    assert(script_store_host_platform_calls() == calls);
    return value;
}

static ryz_apps_snapshot_t settle(uint64_t id, ryz_apps_state_t state)
{
    for (unsigned i = 0; i < 8U; ++i) {
        ryz_apps_snapshot_t value = snapshot();
        if (value.request_id == id && value.completed_id == id &&
            value.state == state) {
            if (state == RYZ_APPS_READY || state == RYZ_APPS_EMPTY) {
                assert(value.store_status_valid && value.store_ready);
                assert(!value.recovery_required);
            }
            return value;
        }
        step();
    }
    ryz_apps_snapshot_t value = snapshot();
    fprintf(stderr, "request=%llu completed=%llu state=%d error=%d; expected=%llu/%d\n",
            (unsigned long long)value.request_id,
            (unsigned long long)value.completed_id, value.state, value.error,
            (unsigned long long)id, state);
    abort();
}

static uint64_t request_page(size_t offset, size_t limit, uint32_t revision)
{
    uint64_t id = 0;
    const size_t calls = script_store_host_platform_calls();
    const unsigned notifications = wake_count();
    assert(ryz_apps_request_page(offset, limit, revision, &id) == ESP_OK);
    assert(id != 0);
    assert(wake_count() > notifications);
    assert(script_store_host_platform_calls() == calls);
    const ryz_apps_snapshot_t value = snapshot();
    assert(value.request_id == id && value.view == RYZ_APPS_VIEW_CATALOG);
    assert(value.state == RYZ_APPS_LOADING);
    assert(!value.store_status_valid);
    assert_no_data(&value);
    return id;
}

static uint64_t request_detail(const char *name, uint32_t revision)
{
    uint64_t id = 0;
    const size_t calls = script_store_host_platform_calls();
    const unsigned notifications = wake_count();
    assert(ryz_apps_request_detail(name, revision, &id) == ESP_OK);
    assert(id != 0);
    assert(wake_count() > notifications);
    assert(script_store_host_platform_calls() == calls);
    const ryz_apps_snapshot_t value = snapshot();
    assert(value.request_id == id && value.view == RYZ_APPS_VIEW_DETAIL);
    assert(value.state == RYZ_APPS_LOADING);
    assert(!value.store_status_valid);
    assert_no_data(&value);
    return id;
}

static void close_apps(void)
{
    const size_t calls = script_store_host_platform_calls();
    const unsigned notifications = wake_count();
    assert(ryz_apps_close() == ESP_OK);
    assert(wake_count() > notifications);
    assert(script_store_host_platform_calls() == calls);
    const ryz_apps_snapshot_t value = snapshot();
    assert(value.view == RYZ_APPS_VIEW_NONE && value.state == RYZ_APPS_IDLE);
    assert_no_data(&value);
}

static void raw_file(const char *name, const void *bytes, size_t length)
{
    assert(name && !strchr(name, '/') && strcmp(name, ".") && strcmp(name, ".."));
    char path[PATH_MAX];
    assert(snprintf(path, sizeof(path), "%s/%s", data_root, name) > 0);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    assert(fd >= 0);
    const unsigned char *cursor = bytes;
    size_t left = length;
    while (left) {
        ssize_t count = write(fd, cursor, left);
        assert(count > 0);
        cursor += (size_t)count;
        left -= (size_t)count;
    }
    assert(close(fd) == 0);
}

static void initialize_store(void)
{
    ryz_script_store_mutation_t result;
    assert(ryz_script_store_init(data_root, &result) == ESP_OK);
}

static ryz_script_store_mutation_t put(const char *name, const char *source)
{
    ryz_script_store_mutation_t result;
    assert(ryz_script_store_put(name, source, strlen(source), NULL, &result) == ESP_OK);
    assert(result.commit == RYZ_SCRIPT_STORE_COMMITTED);
    return result;
}

static void remove_script(const char *name)
{
    ryz_script_store_mutation_t result;
    assert(ryz_script_store_remove(name, NULL, &result) == ESP_OK);
    assert(result.commit == RYZ_SCRIPT_STORE_COMMITTED);
}

static void start_apps(void)
{
    assert(ryz_apps_start() == ESP_OK);
    assert(ryz_apps_start() == ESP_OK);
    assert(start_calls == 1U && init_calls == 1U);
    step();
}

static void *concurrent_start(void *opaque)
{
    esp_err_t *result = opaque;
    *result = ryz_apps_start();
    return NULL;
}

static void case_start(void)
{
    ryz_apps_snapshot_t value;
    memset(&value, 0xa5, sizeof(value));
    assert(ryz_apps_get_snapshot(&value) == ESP_ERR_INVALID_STATE);
    assert_zero(&value, sizeof(value));
    uint64_t id = 999;
    assert(ryz_apps_request_page(0, 1, 0, &id) == ESP_ERR_INVALID_STATE && id == 0);
    assert(ryz_apps_close() == ESP_ERR_INVALID_STATE);
    init_error = ESP_ERR_NO_MEM;
    assert(ryz_apps_start() == ESP_ERR_NO_MEM);
    assert(init_calls == 1U && start_calls == 0U);
    init_error = ESP_OK;
    start_error = ESP_FAIL;
    assert(ryz_apps_start() == ESP_FAIL);
    assert(init_calls == 2U && start_calls == 1U && deinit_calls >= 1U);
    start_error = ESP_OK;
    block_start = true;
    esp_err_t result = ESP_FAIL;
    pthread_t starter;
    assert(pthread_create(&starter, NULL, concurrent_start, &result) == 0);
    assert(pthread_mutex_lock(&gate) == 0);
    while (!start_entered) wait_changed();
    assert(pthread_mutex_unlock(&gate) == 0);
    assert(ryz_apps_start() == ESP_ERR_INVALID_STATE);
    assert(pthread_mutex_lock(&gate) == 0);
    release_start = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    assert(pthread_mutex_unlock(&gate) == 0);
    assert(pthread_join(starter, NULL) == 0 && result == ESP_OK);
    assert(ryz_apps_start() == ESP_OK);
    assert(init_calls == 3U && start_calls == 2U);
    step();
    id = request_page(0, 1, 0);
    value = settle(id, RYZ_APPS_FAILED);
    assert(value.error == ESP_ERR_INVALID_STATE && value.store_status_valid);
    assert(!value.store_ready);
    assert_no_data(&value);
    size_t calls = script_store_host_platform_calls();
    for (unsigned i = 0; i < 4U; ++i) step();
    assert(script_store_host_platform_calls() == calls);
    initialize_store();
    id = request_page(0, 1, 0);
    value = settle(id, RYZ_APPS_EMPTY);
    assert(value.page.total == 0 && value.store_ready);
}

static void case_arguments(void)
{
    initialize_store();
    start_apps();
    assert(ryz_apps_get_snapshot(NULL) == ESP_ERR_INVALID_ARG);
    uint64_t id = 999;
    assert(ryz_apps_request_page(0, 0, 0, &id) == ESP_ERR_INVALID_ARG && id == 0);
    id = 999;
    assert(ryz_apps_request_page(0, RYZ_SCRIPT_STORE_PAGE_MAX + 1U, 0, &id) ==
           ESP_ERR_INVALID_ARG && id == 0);
    id = 999;
    assert(ryz_apps_request_page(RYZ_SCRIPT_STORE_INDEX_MAX + 1U, 1, 0, &id) ==
           ESP_ERR_INVALID_ARG && id == 0);
    assert(ryz_apps_request_page(0, 1, 0, NULL) == ESP_ERR_INVALID_ARG);
    const char *names[] = {NULL, "", "../a.lua", "a.txt", "bad name.lua"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        id = 999;
        assert(ryz_apps_request_detail(names[i], 1, &id) == ESP_ERR_INVALID_ARG && id == 0);
    }
    id = 999;
    assert(ryz_apps_request_detail("a.lua", 0, &id) == ESP_ERR_INVALID_ARG && id == 0);
    assert(ryz_apps_request_detail("a.lua", 1, NULL) == ESP_ERR_INVALID_ARG);
    id = request_page(0, 1, 0);
    ryz_apps_snapshot_t value = settle(id, RYZ_APPS_EMPTY);
    assert(value.page.total == 0 && value.page.count == 0 && value.page.revision != 0);
    id = request_page(1, 1, 0);
    value = settle(id, RYZ_APPS_FAILED);
    assert(value.error != ESP_OK);
    assert_no_data(&value);
    close_apps();
    value = snapshot();
    assert(value.view == RYZ_APPS_VIEW_NONE && value.state == RYZ_APPS_IDLE);
    assert_no_data(&value);
}

static void case_catalog(void)
{
    raw_file("notes.txt", "not a script", 12);
    char path[PATH_MAX];
    assert(snprintf(path, sizeof(path), "%s/directory.lua", data_root) > 0);
    assert(mkdir(path, 0700) == 0);
    assert(snprintf(path, sizeof(path), "%s/link.lua", data_root) > 0);
    assert(symlink("a.lua", path) == 0);
    initialize_store();
    put("c.lua", "return 3");
    put("a.lua", "return 1");
    put("b.lua", "return 2");
    start_apps();
    uint64_t id = request_page(0, 2, 0);
    ryz_apps_snapshot_t value = settle(id, RYZ_APPS_READY);
    assert(value.page.total == 3 && value.page.count == 2 && value.page.offset == 0);
    assert(strcmp(value.page.entries[0].name, "a.lua") == 0);
    assert(strcmp(value.page.entries[1].name, "b.lua") == 0);
    const uint32_t original_revision = value.page.revision;
    value.page.entries[0].name[0] = 'z';
    value = snapshot();
    assert(strcmp(value.page.entries[0].name, "a.lua") == 0);
    const size_t calls = script_store_host_platform_calls();
    for (unsigned i = 0; i < 4U; ++i) { step(); (void)snapshot(); }
    assert(script_store_host_platform_calls() == calls);
    id = request_page(2, 2, original_revision);
    value = settle(id, RYZ_APPS_READY);
    assert(value.page.count == 1 && strcmp(value.page.entries[0].name, "c.lua") == 0);
    put("d.lua", "return 4");
    for (unsigned i = 0; i < 4U; ++i) step();
    value = snapshot();
    assert(value.state == RYZ_APPS_READY && value.request_id > id);
    assert(value.completed_id == value.request_id && value.page.revision != original_revision);
    assert(value.page.offset == 2 && value.page.count == 2 && value.page.total == 4);
    assert(strcmp(value.page.entries[1].name, "d.lua") == 0);
    remove_script("d.lua");
    remove_script("c.lua");
    remove_script("b.lua");
    for (unsigned i = 0; i < 4U; ++i) step();
    value = snapshot();
    assert(value.state == RYZ_APPS_READY && value.page.offset == 0);
    assert(value.page.total == 1 && value.page.count == 1);
    assert(strcmp(value.page.entries[0].name, "a.lua") == 0);
    id = request_page(0, 2, original_revision);
    value = settle(id, RYZ_APPS_STALE);
    assert(value.error == ESP_ERR_INVALID_STATE);
    assert_no_data(&value);
}

static void case_page_boundary(void)
{
    initialize_store();
    for (unsigned i = 0; i < 17U; ++i) {
        char name[20];
        assert(snprintf(name, sizeof(name), "n%02u.lua", i) > 0);
        put(name, "return 1");
    }
    start_apps();
    uint64_t id = request_page(16, 1, 0);
    ryz_apps_snapshot_t value = settle(id, RYZ_APPS_READY);
    assert(value.page.total == 17 && value.page.offset == 16 && value.page.count == 1);
    remove_script("n16.lua");
    for (unsigned i = 0; i < 4U; ++i) step();
    value = snapshot();
    assert(value.state == RYZ_APPS_READY && value.request_id > id);
    assert(value.page.total == 16 && value.page.offset == 0 && value.page.count == 1);
    assert(strcmp(value.page.entries[0].name, "n00.lua") == 0);
    id = value.request_id;
    put("n16.lua", "return 2");
    for (unsigned i = 0; i < 4U; ++i) step();
    value = snapshot();
    assert(value.state == RYZ_APPS_READY && value.request_id > id);
    assert(value.page.total == 17 && value.page.offset == 0 && value.page.count == 1);
    id = request_page(17, 1, 0);
    value = settle(id, RYZ_APPS_FAILED);
    assert(value.error == ESP_ERR_INVALID_ARG);
    assert_no_data(&value);
}

static void case_detail(void)
{
    const unsigned char nul = 0;
    char oversized[RYZ_SCRIPT_STORE_SOURCE_MAX + 1U];
    memset(oversized, 'x', sizeof(oversized));
    raw_file("empty.lua", "", 0);
    raw_file("nul.lua", &nul, 1);
    raw_file("large.lua", oversized, sizeof(oversized));
    initialize_store();
    const char *source = "-- @author: Alice\n-- @version: 1.2\n"
        "-- @description: owned metadata\nthis is not valid Lua !";
    script_store_host_utc(INT64_C(1709164800));
    const ryz_script_store_mutation_t written = put("a.lua", source);
    start_apps();
    uint64_t id = request_page(0, 16, 0);
    ryz_apps_snapshot_t value = settle(id, RYZ_APPS_READY);
    assert(value.page.total == 4);
    const uint32_t revision = value.page.revision;
    id = request_detail("a.lua", revision);
    value = settle(id, RYZ_APPS_READY);
    assert(value.detail.source_valid && value.detail.source_error == ESP_OK);
    assert(value.detail.revision == revision && value.detail.file.entry.bytes == strlen(source));
    assert(strcmp(value.detail.file.sha256, written.sha256) == 0);
    assert(value.detail.file.times.created_unix == INT64_C(1709164800));
    assert(value.detail.file.times.modified_unix == INT64_C(1709164800));
    assert(strcmp(value.detail.metadata.author, "Alice") == 0);
    assert(strcmp(value.detail.metadata.version, "1.2") == 0);
    assert(strcmp(value.detail.metadata.description, "owned metadata") == 0);
    /* A borrowed name from a previous public snapshot is a valid request. */
    id = request_detail(value.detail.file.entry.name, revision);
    value = settle(id, RYZ_APPS_READY);
    assert(strcmp(value.detail.metadata.author, "Alice") == 0);
    static const struct {
        const char *name;
        size_t length;
        esp_err_t error;
        const char *sha;
    } invalid[] = {
        {"empty.lua", 0, ESP_ERR_INVALID_SIZE,
         "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"nul.lua", 1, ESP_ERR_INVALID_ARG,
         "6e340b9cffb37a989ca544e6bb780a2c78901d3fb33738768511a30617afa01d"},
        {"large.lua", RYZ_SCRIPT_STORE_SOURCE_MAX + 1U, ESP_ERR_INVALID_SIZE,
         "39061532d423f2d385027b9d40fc07e2bedf7bf3151e8f8bdd5ff005cd5136d1"},
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        id = request_detail(invalid[i].name, revision);
        value = settle(id, RYZ_APPS_READY);
        assert(!value.detail.source_valid && value.detail.source_error == invalid[i].error);
        assert(value.detail.file.entry.bytes == invalid[i].length);
        assert(strcmp(value.detail.file.entry.name, invalid[i].name) == 0);
        assert(strcmp(value.detail.file.sha256, invalid[i].sha) == 0);
        assert(value.detail.file.times.created_unix == 0 && value.detail.file.times.modified_unix == 0);
        assert_zero(&value.detail.metadata, sizeof(value.detail.metadata));
    }
    id = request_detail("a.lua", revision);
    value = settle(id, RYZ_APPS_READY);
    const ryz_apps_detail_t retained = value.detail;
    script_store_host_utc(0);
    put("a.lua", "-- @author: Bob\nreturn 2");
    for (unsigned i = 0; i < 3U; ++i) step();
    value = snapshot();
    assert(value.state == RYZ_APPS_STALE);
    assert_no_data(&value);
    assert(strcmp(retained.metadata.author, "Alice") == 0);
    assert(retained.file.times.created_unix == INT64_C(1709164800));
    const size_t calls = script_store_host_platform_calls();
    for (unsigned i = 0; i < 4U; ++i) step();
    assert(script_store_host_platform_calls() == calls);
    id = request_detail("a.lua", revision);
    value = settle(id, RYZ_APPS_STALE);
    assert_no_data(&value);
    ryz_script_store_status_t status;
    assert(ryz_script_store_status(&status) == ESP_OK);
    id = request_detail("a.lua", status.revision);
    value = settle(id, RYZ_APPS_READY);
    assert(value.detail.file.times.created_unix == INT64_C(1709164800));
    assert(value.detail.file.times.modified_unix == 0);
}

static void case_snapshot_identity(void)
{
    initialize_store();
    const char *source_a = "-- @author: Alice\n-- @version: A\nreturn 1";
    const char *source_b = "-- @author: Bob\n-- @version: BBB\nreturn 999";
    const ryz_script_store_mutation_t selected = put("a.lua", source_a);
    start_apps();
    uint64_t id = request_page(0, 16, 0);
    ryz_apps_snapshot_t value = settle(id, RYZ_APPS_READY);
    const uint32_t revision = value.page.revision;
    script_store_host_sha_gate_arm();
    id = request_detail("a.lua", revision);
    const unsigned expected = permit_cycle();
    script_store_host_sha_gate_wait();
    value = snapshot();
    assert(value.state == RYZ_APPS_LOADING);
    assert_no_data(&value);
    /* The real Store has already copied A, before its SHA calculation. This
     * deliberately named fixture-only external edit probes immutable snapshot
     * consumption, not supported application writes or revision discovery. */
    raw_file("a.lua", source_b, strlen(source_b));
    script_store_host_sha_gate_release();
    wait_cycle(expected);
    value = settle(id, RYZ_APPS_READY);
    assert(value.detail.file.entry.bytes == strlen(source_a));
    assert(strcmp(value.detail.file.sha256, selected.sha256) == 0);
    assert(strcmp(value.detail.metadata.author, "Alice") == 0);
    assert(strcmp(value.detail.metadata.version, "A") == 0);
    id = request_detail(value.detail.file.entry.name, revision);
    /* A subsequent read detects the deliberately out-of-band source/sidecar
     * mismatch. Do not reuse A's timestamps or publish B as a coherent app. */
    value = settle(id, RYZ_APPS_FAILED);
    assert_no_data(&value);
    assert(value.recovery_required);
    char sha_b[65];
    assert(ryz_script_store_source_sha256(source_b, strlen(source_b), sha_b) == ESP_OK);
    ryz_script_store_description_t raw;
    assert(ryz_script_store_describe("a.lua", &raw) == ESP_OK);
    assert(raw.entry.bytes == strlen(source_b) && strcmp(raw.sha256, sha_b) == 0);
    assert(raw.times.created_unix == 0 && raw.times.modified_unix == 0);
}

static void case_supersede(void)
{
    initialize_store();
    put("a.lua", "-- @author: Alice\nreturn 1");
    put("b.lua", "-- @author: Bob\nreturn 2");
    start_apps();
    uint64_t id = request_page(0, 16, 0);
    ryz_apps_snapshot_t value = settle(id, RYZ_APPS_READY);
    const uint32_t revision = value.page.revision;
    script_store_host_sha_gate_arm();
    const uint64_t old_id = request_detail("a.lua", revision);
    unsigned expected = permit_cycle();
    script_store_host_sha_gate_wait();
    uint64_t replaced = request_detail("b.lua", revision);
    assert(replaced > old_id);
    close_apps();
    value = snapshot();
    assert(value.state == RYZ_APPS_IDLE && value.view == RYZ_APPS_VIEW_NONE);
    assert_no_data(&value);
    script_store_host_sha_gate_release();
    wait_cycle(expected);
    for (unsigned i = 0; i < 3U; ++i) step();
    value = snapshot();
    assert(value.state == RYZ_APPS_IDLE && value.view == RYZ_APPS_VIEW_NONE);
    assert_no_data(&value);
    const size_t calls = script_store_host_platform_calls();
    for (unsigned i = 0; i < 3U; ++i) step();
    assert(script_store_host_platform_calls() == calls);

    script_store_host_sha_gate_arm();
    id = request_detail("a.lua", revision);
    assert(id > replaced);
    expected = permit_cycle();
    script_store_host_sha_gate_wait();
    replaced = request_page(0, 1, revision);
    const uint64_t latest = request_detail("b.lua", revision);
    assert(latest > replaced && replaced > id);
    script_store_host_sha_gate_release();
    wait_cycle(expected);
    value = snapshot();
    assert(value.request_id == latest);
    assert(value.state == RYZ_APPS_LOADING || value.state == RYZ_APPS_READY);
    if (value.state == RYZ_APPS_LOADING) assert_no_data(&value);
    value = settle(latest, RYZ_APPS_READY);
    assert(strcmp(value.detail.file.entry.name, "b.lua") == 0);
    assert(strcmp(value.detail.metadata.author, "Bob") == 0);
}

static void case_read_failure(void)
{
    initialize_store();
    put("a.lua", "return 1");
    start_apps();
    uint64_t id = request_page(0, 16, 0);
    ryz_apps_snapshot_t value = settle(id, RYZ_APPS_READY);
    const uint32_t revision = value.page.revision;
    id = request_detail("missing.lua", revision);
    value = settle(id, RYZ_APPS_FAILED);
    assert(value.error == ESP_ERR_NOT_FOUND);
    assert_no_data(&value);
    size_t calls = script_store_host_platform_calls();
    for (unsigned i = 0; i < 5U; ++i) step();
    assert(script_store_host_platform_calls() == calls);
    id = request_page(0, 16, 0);
    value = settle(id, RYZ_APPS_READY);
    const char *leaf = strrchr(data_root, '/');
    assert(leaf != NULL);
    script_store_host_fault(SCRIPT_HOST_VISIT_INVALID_STATE, leaf + 1, NULL, 0, 1);
    id = request_page(0, 16, 0);
    value = settle(id, RYZ_APPS_FAILED);
    assert(value.error == ESP_ERR_INVALID_STATE && value.recovery_required);
    assert_no_data(&value);
    assert(script_store_host_fault_hits() == 1U);
    calls = script_store_host_platform_calls();
    for (unsigned i = 0; i < 5U; ++i) step();
    assert(script_store_host_platform_calls() == calls);
}

static void case_same_revision_recovery(void)
{
    initialize_store();
    put("a.lua", "return 1");
    start_apps();
    uint64_t id = request_page(0, 16, 0);
    ryz_apps_snapshot_t value = settle(id, RYZ_APPS_READY);
    const uint32_t revision = value.page.revision;
    script_store_host_fault(SCRIPT_HOST_READ_INVALID_STATE, "a.lua", NULL, 0, 1);
    ryz_script_store_snapshot_t source;
    assert(ryz_script_store_get("a.lua", &source) == ESP_ERR_INVALID_STATE);
    ryz_script_store_snapshot_free(&source);
    ryz_script_store_status_t status;
    assert(ryz_script_store_status(&status) == ESP_OK);
    assert(status.revision == revision && status.recovery_required);
    for (unsigned i = 0; i < 3U; ++i) step();
    value = snapshot();
    assert(value.state == RYZ_APPS_STALE || value.state == RYZ_APPS_FAILED);
    assert(value.store_status_valid && value.recovery_required);
    assert(value.error == ESP_ERR_INVALID_STATE);
    assert_no_data(&value);
    const size_t calls = script_store_host_platform_calls();
    for (unsigned i = 0; i < 5U; ++i) step();
    assert(script_store_host_platform_calls() == calls);
}

static void *hold_store_read(void *opaque)
{
    esp_err_t *error = opaque;
    ryz_script_store_snapshot_t source;
    *error = ryz_script_store_get("a.lua", &source);
    ryz_script_store_snapshot_free(&source);
    return NULL;
}

static void case_store_busy(void)
{
    initialize_store();
    put("a.lua", "return 1");
    start_apps();
    uint64_t id = request_page(0, 16, 0);
    ryz_apps_snapshot_t value = settle(id, RYZ_APPS_READY);
    const uint32_t revision = value.page.revision;
    script_store_host_sha_gate_arm();
    pthread_t holder;
    esp_err_t error = ESP_FAIL;
    assert(pthread_create(&holder, NULL, hold_store_read, &error) == 0);
    script_store_host_sha_gate_wait();
    /* Status contention is not a new offline/recovery state, and does not
     * revoke a previously completed page. Snapshot/request calls do not wait. */
    const size_t calls = script_store_host_platform_calls();
    for (unsigned i = 0; i < 3U; ++i) step();
    value = snapshot();
    assert(value.state == RYZ_APPS_READY && value.request_id == id);
    assert(value.store_status_valid && value.store_ready);
    assert(!value.recovery_required && value.page.total == 1);
    assert(script_store_host_platform_calls() == calls);
    id = request_detail("a.lua", revision);
    value = settle(id, RYZ_APPS_FAILED);
    assert(value.error == ESP_ERR_TIMEOUT);
    assert(!value.store_status_valid); /* Unknown, not a confirmed offline Store. */
    assert_no_data(&value);
    script_store_host_sha_gate_release();
    assert(pthread_join(holder, NULL) == 0 && error == ESP_OK);
    const size_t after_read = script_store_host_platform_calls();
    for (unsigned i = 0; i < 5U; ++i) step();
    assert(script_store_host_platform_calls() == after_read);
    value = snapshot();
    assert(value.state == RYZ_APPS_FAILED && value.request_id == id);
    id = request_detail("a.lua", revision);
    value = settle(id, RYZ_APPS_READY);
    assert(value.detail.source_valid);
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    data_root = argv[2];
    script_store_host_configure(data_root);
    if (strcmp(argv[1], "start") == 0) case_start();
    else if (strcmp(argv[1], "arguments") == 0) case_arguments();
    else if (strcmp(argv[1], "catalog") == 0) case_catalog();
    else if (strcmp(argv[1], "page_boundary") == 0) case_page_boundary();
    else if (strcmp(argv[1], "detail") == 0) case_detail();
    else if (strcmp(argv[1], "snapshot_identity") == 0) case_snapshot_identity();
    else if (strcmp(argv[1], "supersede") == 0) case_supersede();
    else if (strcmp(argv[1], "read_failure") == 0) case_read_failure();
    else if (strcmp(argv[1], "same_revision_recovery") == 0) case_same_revision_recovery();
    else if (strcmp(argv[1], "store_busy") == 0) case_store_busy();
    else abort();
    stop_worker();
    printf("APPS_PASS %s\n", argv[1]);
    return 0;
}
