#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ryz_i2c_scan_bus.h"
#include "ryz_i2c_scan_platform.h"
#include "ryz_tool_pins.h"
#include "workbench_i2c_rpc.h"

/* Real scanner and RPC translation units plus IDF cJSON. Only their external
 * OS/bus/pin seams are substituted. Every control and observation below travels
 * through parsed JSON; no scanner public function is replaced or called here. */
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker;
static void (*worker_function)(void *);
static _Thread_local unsigned cache_depth;
static bool created, allow_worker, stopping, event, idle;
static bool acknowledgements[128];
static unsigned platform_starts, board_inits, probe_count;
static unsigned bus_ends, pair_checks;
static bool bus_held;
static esp_err_t end_error;
static ryz_i2c_scan_config_t begun_config;
static unsigned probes_by_address[128];
static unsigned blocks_remaining, blocked_probes, released_probes;
static int64_t now_ms = 1000;

static void wait_changed(void)
{
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 5;
    assert(pthread_cond_timedwait(&changed, &gate, &deadline) == 0);
}

static void stop_if_requested(void)
{
    if (stopping) {
        assert(pthread_mutex_unlock(&gate) == 0);
        pthread_exit(NULL);
    }
}

static void *worker_entry(void *unused)
{
    (void)unused;
    assert(pthread_mutex_lock(&gate) == 0);
    while (!allow_worker && !stopping) wait_changed();
    stop_if_requested();
    assert(pthread_mutex_unlock(&gate) == 0);
    worker_function(NULL);
    abort();
}

esp_err_t ryz_i2c_scan_platform_init(void) { return ESP_OK; }
void ryz_i2c_scan_platform_deinit(void) { assert(!created); }
esp_err_t ryz_i2c_scan_platform_start(void (*entry)(void *))
{
    assert(!created && entry);
    ++platform_starts;
    worker_function = entry;
    assert(pthread_create(&worker, NULL, worker_entry, NULL) == 0);
    created = true;
    return ESP_OK;
}
void ryz_i2c_scan_platform_lock(void)
{
    assert(!cache_depth);
    assert(pthread_mutex_lock(&cache_mutex) == 0);
    cache_depth = 1;
}
void ryz_i2c_scan_platform_unlock(void)
{
    assert(cache_depth == 1);
    cache_depth = 0;
    assert(pthread_mutex_unlock(&cache_mutex) == 0);
}
void ryz_i2c_scan_platform_wake(void)
{
    assert(pthread_mutex_lock(&gate) == 0);
    event = true; /* Binary wake, including give before wait. */
    assert(pthread_cond_broadcast(&changed) == 0);
    assert(pthread_mutex_unlock(&gate) == 0);
}
void ryz_i2c_scan_platform_wait(void)
{
    assert(!cache_depth);
    assert(pthread_mutex_lock(&gate) == 0);
    while (!event && !stopping) {
        idle = true;
        assert(pthread_cond_broadcast(&changed) == 0);
        wait_changed();
    }
    stop_if_requested();
    idle = false;
    event = false;
    assert(pthread_mutex_unlock(&gate) == 0);
}
void ryz_i2c_scan_platform_delay_ms(uint32_t milliseconds)
{
    assert(!cache_depth && milliseconds == 5);
    assert(pthread_mutex_lock(&gate) == 0);
    now_ms += milliseconds;
    stop_if_requested();
    assert(pthread_mutex_unlock(&gate) == 0);
    sched_yield(); /* Controlled logical time, not electrical timing evidence. */
}
int64_t ryz_i2c_scan_platform_now_ms(void)
{
    assert(pthread_mutex_lock(&gate) == 0);
    int64_t result = now_ms;
    assert(pthread_mutex_unlock(&gate) == 0);
    return result;
}

static void check_physical_owner(void)
{
    assert(!cache_depth && pthread_equal(worker, pthread_self()));
}
esp_err_t ryz_i2c_scan_validate_config(const ryz_i2c_scan_config_t *config)
{
    if (!config || (config->hz != 100000 && config->hz != 400000)) return ESP_ERR_INVALID_ARG;
    if (config->sda == 41 && config->scl == 40 && config->hz == 100000) return ESP_OK;
    return config->sda >= 2 && config->sda <= 5 && config->scl >= 2 && config->scl <= 5 &&
        config->sda != config->scl ? ESP_OK : ESP_ERR_INVALID_ARG;
}
esp_err_t ryz_tool_pins_check_pair(int first, int second)
{
    assert(first != second);
    ++pair_checks;
    return ESP_OK;
}
esp_err_t ryz_i2c_scan_bus_begin(const ryz_i2c_scan_config_t *config)
{
    check_physical_owner();
    assert(pthread_mutex_lock(&gate) == 0);
    ++board_inits;
    begun_config = *config;
    bus_held = config->sda != 41;
    assert(pthread_mutex_unlock(&gate) == 0);
    return ESP_OK;
}
esp_err_t ryz_i2c_scan_bus_end(void)
{
    check_physical_owner();
    assert(pthread_mutex_lock(&gate) == 0);
    ++bus_ends;
    if (end_error == ESP_OK) bus_held = false;
    assert(pthread_mutex_unlock(&gate) == 0);
    return end_error;
}
bool ryz_i2c_scan_bus_resources_held(void) { check_physical_owner(); return bus_held; }
esp_err_t ryz_i2c_scan_bus_probe(uint8_t address)
{
    check_physical_owner();
    assert(address >= 0x08 && address <= 0x77);
    assert(pthread_mutex_lock(&gate) == 0);
    ++probe_count;
    ++probes_by_address[address];
    if (address == 0x08 && blocks_remaining) {
        --blocks_remaining;
        ++blocked_probes;
        assert(pthread_cond_broadcast(&changed) == 0);
        while (released_probes < blocked_probes && !stopping) wait_changed();
        stop_if_requested();
    }
    esp_err_t result = acknowledgements[address] ? ESP_OK : ESP_ERR_NOT_FOUND;
    ++now_ms;
    assert(pthread_mutex_unlock(&gate) == 0);
    return result;
}

static cJSON *call(const char *json)
{
    cJSON *request = cJSON_ParseWithOpts(json, NULL, true);
    assert(cJSON_IsObject(request));
    cJSON *reply = ryz_workbench_i2c_rpc(request, "boot-current");
    cJSON_Delete(request);
    assert(reply);
    char *wire = cJSON_PrintUnformatted(reply);
    cJSON_Delete(reply);
    assert(wire);
    reply = cJSON_ParseWithOpts(wire, NULL, true);
    free(wire);
    assert(cJSON_IsObject(reply));
    return reply;
}
static const cJSON *field(const cJSON *object, const char *name)
{
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(object, name);
    assert(result);
    return result;
}
static unsigned number(const cJSON *object, const char *name)
{
    const cJSON *result = field(object, name);
    assert(cJSON_IsNumber(result) && result->valuedouble >= 0);
    return (unsigned)result->valuedouble;
}
static void text_equals(const cJSON *object, const char *name, const char *expected)
{
    const cJSON *result = field(object, name);
    assert(cJSON_IsString(result) && !strcmp(result->valuestring, expected));
}
static void common(const cJSON *reply, bool ok)
{
    assert(cJSON_IsBool(field(reply, "ok")));
    assert(cJSON_IsTrue(field(reply, "ok")) == ok);
    text_equals(reply, "schema", "ryz-i2c-scan/2");
    text_equals(reply, "boot_id", "boot-current");
}
static cJSON *status(void)
{
    cJSON *reply = call("{\"id\":\"status\",\"op\":\"i2c_scan\",\"action\":\"status\"}");
    common(reply, true);
    return reply;
}
static uint32_t start(void)
{
    cJSON *reply = call("{\"id\":\"start\",\"op\":\"i2c_scan\",\"action\":\"start\",\"boot_id\":\"boot-current\"}");
    common(reply, true);
    assert(cJSON_IsTrue(field(reply, "accepted")));
    uint32_t id = number(reply, "scan_id");
    assert(id);
    cJSON_Delete(reply);
    return id;
}
static cJSON *cancel(uint32_t id, const char *boot)
{
    char json[180];
    int length = snprintf(json, sizeof(json),
        "{\"id\":\"cancel\",\"op\":\"i2c_scan\",\"action\":\"cancel\",\"boot_id\":\"%s\",\"scan_id\":%u}",
        boot, (unsigned)id);
    assert(length > 0 && (size_t)length < sizeof(json));
    return call(json);
}
static void release_worker(void)
{
    assert(pthread_mutex_lock(&gate) == 0);
    allow_worker = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    assert(pthread_mutex_unlock(&gate) == 0);
}
static void wait_idle(void)
{
    assert(pthread_mutex_lock(&gate) == 0);
    while (!idle || event) wait_changed();
    assert(pthread_mutex_unlock(&gate) == 0);
}
static void stop_worker(void)
{
    assert(pthread_mutex_lock(&gate) == 0);
    stopping = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    assert(pthread_mutex_unlock(&gate) == 0);
    assert(pthread_join(worker, NULL) == 0);
}

static void complete_with_ack(void)
{
    cJSON *reply = status();
    text_equals(reply, "phase", "unstarted");
    assert(!cJSON_IsTrue(field(reply, "empty")));
    cJSON_Delete(reply);
    assert(platform_starts == 0 && board_inits == 0 && probe_count == 0);
    acknowledgements[0x15] = acknowledgements[0x19] = true;
    uint32_t id = start();
    reply = status();
    text_equals(reply, "phase", "queued");
    assert(number(reply, "scan_id") == id && !cJSON_IsTrue(field(reply, "empty")));
    assert(board_inits == 0 && probe_count == 0);
    cJSON_Delete(reply);
    release_worker();
    wait_idle();
    reply = status();
    text_equals(reply, "phase", "completed");
    assert(number(reply, "scan_id") == id && number(reply, "completed_addresses") == 112);
    assert(number(reply, "ack_count") == 2 && number(reply, "nack_count") == 110);
    assert(!cJSON_IsTrue(field(reply, "empty")) && number(reply, "unscanned_count") == 0);
    const cJSON *results = field(reply, "results");
    const cJSON *ack = field(results, "ack");
    assert(cJSON_GetArraySize(ack) == 2);
    assert(cJSON_GetArrayItem(ack, 0)->valueint == 0x15);
    assert(cJSON_GetArrayItem(ack, 1)->valueint == 0x19);
    assert(cJSON_GetArraySize(field(results, "nack")) == 110);
    assert(probe_count == 112 && board_inits == 1);
    for (unsigned address = 0; address < 128; ++address)
        assert(probes_by_address[address] == (address >= 8 && address <= 0x77 ? 1U : 0U));
    cJSON_Delete(reply);
    reply = cancel(id, "boot-current");
    common(reply, false); /* A completed scan is not a newly accepted cancel. */
    assert(number(reply, "error_code") == ESP_ERR_INVALID_STATE);
    cJSON_Delete(reply);
}

static void complete_empty(void)
{
    uint32_t id = start();
    cJSON *reply = status();
    text_equals(reply, "phase", "queued");
    assert(!cJSON_IsTrue(field(reply, "empty")) && number(reply, "unscanned_count") == 112);
    cJSON_Delete(reply);
    release_worker();
    wait_idle();
    reply = status();
    text_equals(reply, "phase", "completed");
    assert(number(reply, "scan_id") == id && number(reply, "completed_addresses") == 112);
    assert(cJSON_IsTrue(field(reply, "empty")) && number(reply, "nack_count") == 112);
    assert(number(reply, "ack_count") == 0 && number(reply, "busy_count") == 0);
    assert(number(reply, "timeout_count") == 0 && number(reply, "error_count") == 0);
    assert(number(reply, "unscanned_count") == 0 && number(reply, "probe_calls") == 112);
    const cJSON *results = field(reply, "results");
    const cJSON *nacks = field(results, "nack");
    assert(cJSON_GetArraySize(nacks) == 112);
    for (unsigned index = 0; index < 112; ++index)
        assert(cJSON_GetArrayItem(nacks, (int)index)->valueint == (int)index + 8);
    assert(probe_count == 112 && board_inits == 1);
    cJSON_Delete(reply);
}

static void arm_first_probe(void)
{
    assert(pthread_mutex_lock(&gate) == 0);
    ++blocks_remaining;
    assert(pthread_mutex_unlock(&gate) == 0);
}
static void wait_blocked_probe(unsigned expected)
{
    assert(pthread_mutex_lock(&gate) == 0);
    while (blocked_probes != expected) wait_changed();
    assert(pthread_mutex_unlock(&gate) == 0);
}
static void release_probe(unsigned expected)
{
    assert(pthread_mutex_lock(&gate) == 0);
    assert(blocked_probes == expected);
    released_probes = expected;
    assert(pthread_cond_broadcast(&changed) == 0);
    assert(pthread_mutex_unlock(&gate) == 0);
}

static void cancel_and_restart(void)
{
    arm_first_probe();
    uint32_t first = start();
    release_worker();
    wait_blocked_probe(1);
    cJSON *reply = status();
    text_equals(reply, "phase", "running");
    assert(number(reply, "scan_id") == first && number(reply, "probe_calls") == 1);
    assert(probe_count == 1);
    cJSON_Delete(reply);

    reply = cancel(first, "boot-current");
    common(reply, true);
    assert(cJSON_IsTrue(field(reply, "accepted")) && number(reply, "scan_id") == first);
    assert(!cJSON_GetObjectItemCaseSensitive(reply, "phase")); /* ACK is not completion. */
    cJSON_Delete(reply);
    reply = status();
    text_equals(reply, "phase", "cancelling");
    assert(number(reply, "probe_calls") == 1 && !cJSON_IsTrue(field(reply, "empty")));
    cJSON_Delete(reply);
    reply = call("{\"op\":\"i2c_scan\",\"action\":\"start\",\"boot_id\":\"boot-current\"}");
    common(reply, false);
    assert(number(reply, "error_code") == ESP_ERR_INVALID_STATE);
    cJSON_Delete(reply);

    release_probe(1);
    wait_idle();
    reply = status();
    text_equals(reply, "phase", "cancelled");
    assert(number(reply, "scan_id") == first && number(reply, "probe_calls") == 1);
    assert(number(reply, "completed_addresses") == 0 && number(reply, "unscanned_count") == 112);
    assert(!cJSON_IsTrue(field(reply, "empty")) && probe_count == 1);
    cJSON_Delete(reply);
    reply = cancel(first, "boot-current");
    common(reply, true); /* Same terminal token is idempotent. */
    cJSON_Delete(reply);

    arm_first_probe();
    uint32_t second = start();
    assert(second == first + 1);
    wait_blocked_probe(2);
    reply = cancel(second, "boot-old");
    common(reply, false);
    assert(number(reply, "error_code") == ESP_ERR_INVALID_STATE);
    cJSON_Delete(reply);
    reply = cancel(first, "boot-current");
    common(reply, false);
    assert(number(reply, "error_code") == ESP_ERR_INVALID_STATE);
    cJSON_Delete(reply);
    reply = status();
    text_equals(reply, "phase", "running");
    assert(number(reply, "scan_id") == second && number(reply, "probe_calls") == 1);
    assert(number(reply, "completed_addresses") == 0);
    cJSON_Delete(reply);

    release_probe(2);
    wait_idle();
    reply = status();
    text_equals(reply, "phase", "completed");
    assert(number(reply, "scan_id") == second && cJSON_IsTrue(field(reply, "empty")));
    assert(number(reply, "probe_calls") == 112 && number(reply, "nack_count") == 112);
    assert(platform_starts == 1 && board_inits == 2 && probe_count == 113);
    /* The cancelled generation admitted only its first probe. The fresh
     * generation traversed each address once; no old retry/probe resurfaced. */
    for (unsigned address = 0; address < 128; ++address) {
        unsigned expected = address == 8 ? 2 : address > 8 && address <= 0x77 ? 1 : 0;
        assert(probes_by_address[address] == expected);
    }
    cJSON_Delete(reply);
}

static cJSON *configure(unsigned sda, unsigned scl, unsigned hz, unsigned revision)
{
    char json[240];
    int length = snprintf(json, sizeof(json),
        "{\"op\":\"i2c_scan\",\"action\":\"configure\",\"boot_id\":\"boot-current\","
        "\"sda\":%u,\"scl\":%u,\"hz\":%u,\"expected_revision\":%u}",
        sda, scl, hz, revision);
    assert(length > 0 && (size_t)length < sizeof(json));
    return call(json);
}
static void config_equals(const cJSON *reply, const char *name,
                          unsigned sda, unsigned scl, unsigned hz)
{
    const cJSON *config = field(reply, name);
    assert(number(config, "sda") == sda && number(config, "scl") == scl && number(config, "hz") == hz);
}

static void configure_history(void)
{
    cJSON *reply = status();
    text_equals(reply, "phase", "unstarted");
    config_equals(reply, "config", 41, 40, 100000);
    assert(number(reply, "config_revision") == 1 && number(reply, "scan_config_revision") == 0);
    assert(cJSON_IsNull(field(reply, "scan_config")) && !cJSON_IsTrue(field(reply, "resources_held")));
    cJSON_Delete(reply);
    reply = configure(2, 3, 400000, 1);
    common(reply, true);
    assert(cJSON_IsTrue(field(reply, "accepted")) && number(reply, "config_revision") == 2);
    config_equals(reply, "config", 2, 3, 400000);
    cJSON_Delete(reply);
    reply = status();
    text_equals(reply, "phase", "idle");
    assert(number(reply, "scan_id") == 0 && cJSON_IsNull(field(reply, "scan_config")));
    assert(platform_starts == 1 && board_inits == 0 && probe_count == 0 && bus_ends == 0);
    cJSON_Delete(reply);
    uint32_t id = start();
    reply = configure(4, 5, 100000, 2);
    common(reply, false);
    assert(number(reply, "error_code") == ESP_ERR_INVALID_STATE && pair_checks == 1);
    cJSON_Delete(reply);
    release_worker();
    wait_idle();
    assert(begun_config.sda == 2 && begun_config.scl == 3 && begun_config.hz == 400000);
    reply = configure(4, 5, 100000, 1);
    common(reply, false);
    assert(number(reply, "error_code") == ESP_ERR_INVALID_STATE);
    cJSON_Delete(reply);
    reply = configure(4, 5, 100000, 2);
    common(reply, true);
    assert(number(reply, "config_revision") == 3);
    cJSON_Delete(reply);
    reply = status();
    text_equals(reply, "phase", "completed");
    assert(number(reply, "scan_id") == id && cJSON_IsTrue(field(reply, "empty")));
    config_equals(reply, "config", 4, 5, 100000);
    config_equals(reply, "scan_config", 2, 3, 400000);
    assert(number(reply, "config_revision") == 3 && number(reply, "scan_config_revision") == 2);
    assert(number(reply, "nack_count") == 112 && number(reply, "cleanup_error") == ESP_OK);
    assert(!cJSON_IsTrue(field(reply, "resources_held")));
    assert(board_inits == 1 && bus_ends == 1 && probe_count == 112 && pair_checks == 2);
    cJSON_Delete(reply);
}

static void cleanup_retry(void)
{
    cJSON *reply = configure(2, 3, 400000, 1);
    common(reply, true);
    cJSON_Delete(reply);
    end_error = ESP_ERR_TIMEOUT;
    uint32_t id = start();
    release_worker();
    wait_idle();
    reply = status();
    text_equals(reply, "phase", "failed");
    assert(number(reply, "error_code") == ESP_ERR_TIMEOUT && number(reply, "cleanup_error") == ESP_ERR_TIMEOUT);
    assert(cJSON_IsTrue(field(reply, "resources_held")) && !cJSON_IsTrue(field(reply, "empty")));
    assert(number(reply, "nack_count") == 112 && number(reply, "scan_id") == id);
    cJSON_Delete(reply);
    reply = configure(4, 5, 100000, 2);
    common(reply, false);
    assert(number(reply, "error_code") == ESP_ERR_INVALID_STATE && pair_checks == 1);
    cJSON_Delete(reply);
    assert(pthread_mutex_lock(&gate) == 0);
    end_error = ESP_OK;
    assert(pthread_mutex_unlock(&gate) == 0);
    reply = cancel(id, "boot-current");
    common(reply, true);
    assert(cJSON_IsTrue(field(reply, "accepted")) && number(reply, "scan_id") == id);
    cJSON_Delete(reply);
    wait_idle();
    reply = status();
    text_equals(reply, "phase", "cancelled");
    assert(number(reply, "error_code") == ESP_OK && number(reply, "cleanup_error") == ESP_OK);
    assert(!cJSON_IsTrue(field(reply, "resources_held")) && !cJSON_IsTrue(field(reply, "empty")));
    assert(number(reply, "scan_id") == id && number(reply, "nack_count") == 112);
    assert(board_inits == 1 && bus_ends == 2 && probe_count == 112);
    cJSON_Delete(reply);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "ack")) complete_with_ack();
    else if (!strcmp(argv[1], "empty")) complete_empty();
    else if (!strcmp(argv[1], "cancel_restart")) cancel_and_restart();
    else if (!strcmp(argv[1], "configure_history")) configure_history();
    else if (!strcmp(argv[1], "cleanup_retry")) cleanup_retry();
    else abort();
    stop_worker();
    printf("I2C_SCAN_INTEGRATION_PASS %s\n", argv[1]);
    return 0;
}
