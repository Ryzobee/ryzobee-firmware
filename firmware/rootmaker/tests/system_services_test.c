#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "ryz_ota.h"
#include "ryz_system_services.h"
#include "ryz_system_services_platform.h"
#include "ryz_system_metrics_platform.h"

/* Only the OS seam and public service dependencies are substituted. The
 * production owner_task runs on its own pthread. Tests never invoke a private
 * owner function or pump an equivalent test implementation from the UI. */
static pthread_mutex_t s_gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_changed = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t s_snapshot_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t s_worker;
static pthread_t s_main;
static bool s_created;
static bool s_stopping;
static unsigned s_permits;
static unsigned s_completed_cycles;
static int64_t s_now;
static void (*s_entry)(void *);
static _Thread_local unsigned s_lock_depth;
static _Thread_local unsigned s_dependency_depth;
static unsigned s_platform_init_calls;
static unsigned s_platform_start_calls;
static unsigned s_platform_deinit_calls;
static esp_err_t s_platform_init_error;
static esp_err_t s_platform_start_error;
static bool s_block_platform_start;
static bool s_platform_start_entered;
static bool s_release_platform_start;

/* Main modifies fixtures only at a condition-variable cycle barrier. The
 * one blocking dependency below uses the same gate for its explicit handshake. */
static esp_err_t s_errors[RYZ_SYSTEM_STAGE_COUNT];
static unsigned s_calls[RYZ_SYSTEM_STAGE_COUNT];
static ryz_provisioning_snapshot_t s_network;
static ryz_time_snapshot_t s_time;
static struct { bool network; bool clock; } s_prerequisites[128];
static unsigned s_prerequisite_count;
static bool s_time_network_arguments[128];
static unsigned s_time_network_count;
static struct {
    unsigned count;
    esp_err_t error;
    uint32_t suppressed;
} s_reports[RYZ_SYSTEM_STAGE_COUNT];
static bool s_block_reprovision;
static bool s_reprovision_entered;
static bool s_release_reprovision;
static bool s_block_network_snapshot;
static bool s_network_snapshot_entered;
static bool s_release_network_snapshot;
static atomic_bool s_ota_held;
static ryz_ota_snapshot_t s_ota = {.cleanup_confirmed = true};
static unsigned s_network_control_calls;
static unsigned s_open_portal_calls;
static bool s_block_open_portal, s_open_portal_entered, s_release_open_portal;
static bool s_check_release_reentry;
static unsigned s_temperature_calls,s_idle_calls;
static float s_temperature=36.65f;
static esp_err_t s_temperature_error,s_idle_error;
static uint64_t s_idle[2];
static uint64_t s_idle_sample_offset[2];
static bool s_block_temperature,s_temperature_entered,s_release_temperature;
static unsigned s_boot_fallback_calls;
static esp_err_t s_boot_fallback_error;
static bool s_boot_became_online, s_block_boot_fallback, s_boot_fallback_entered, s_release_boot_fallback;
static int64_t s_start_elapsed_us;
static void timed_wait(void);

esp_err_t ryz_system_metrics_platform_temperature(float *out)
{
    assert(pthread_equal(pthread_self(),s_worker) && !s_lock_depth);
    ++s_temperature_calls;
    ryz_system_services_snapshot_t snapshot;
    assert(ryz_system_services_get_snapshot(&snapshot)==ESP_OK);
    assert(pthread_mutex_lock(&s_gate)==0);
    s_temperature_entered=true;
    assert(pthread_cond_broadcast(&s_changed)==0);
    while(s_block_temperature && !s_release_temperature) timed_wait();
    assert(pthread_mutex_unlock(&s_gate)==0);
    *out=s_temperature; /* Even a failure deliberately writes dirty output. */
    return s_temperature_error;
}

esp_err_t ryz_system_metrics_platform_idle(uint64_t idle[2],uint64_t sampled[2])
{
    assert(pthread_equal(pthread_self(),s_worker) && !s_lock_depth);
    ++s_idle_calls;
    for(unsigned core=0;core<2;++core) {
        idle[core]=s_idle[core];
        sampled[core]=(s_now>=0 ? (uint64_t)s_now : 0)+s_idle_sample_offset[core];
    }
    return s_idle_error;
}

esp_err_t ryz_ota_network_hold(bool hold)
{
    assert(s_lock_depth == 0);
    atomic_store(&s_ota_held, hold);
    if (!hold && s_check_release_reentry) {
        uint32_t id = 0;
        assert(ryz_system_services_request_network(RYZ_SYSTEM_NETWORK_OFF, &id) == ESP_ERR_INVALID_STATE);
        assert(!id); /* Completion must not clear a newer hold. */
    }
    return ESP_OK;
}

esp_err_t ryz_ota_get_snapshot(ryz_ota_snapshot_t *out)
{
    assert(s_lock_depth == 0 && pthread_equal(pthread_self(), s_worker));
    *out = s_ota;
    out->network_held = atomic_load(&s_ota_held);
    return ESP_OK;
}

static struct timespec deadline(void)
{
    struct timespec value;
    assert(clock_gettime(CLOCK_REALTIME, &value) == 0);
    value.tv_sec += 3;
    return value;
}

static void timed_wait(void)
{
    struct timespec limit = deadline();
    assert(pthread_cond_timedwait(&s_changed, &s_gate, &limit) == 0);
}

static void take_permit(void)
{
    while (s_permits == 0 && !s_stopping) timed_wait();
    if (s_stopping) {
        assert(pthread_mutex_unlock(&s_gate) == 0);
        pthread_exit(NULL);
    }
    --s_permits;
}

static void *worker_entry(void *unused)
{
    (void)unused;
    assert(pthread_mutex_lock(&s_gate) == 0);
    take_permit();
    assert(pthread_mutex_unlock(&s_gate) == 0);
    s_entry(NULL);
    abort();
}

esp_err_t ryz_system_services_platform_init(void)
{
    ++s_platform_init_calls;
    return s_platform_init_error;
}

void ryz_system_services_platform_deinit(void)
{
    ++s_platform_deinit_calls;
}

esp_err_t ryz_system_services_platform_start(void (*entry)(void *))
{
    ++s_platform_start_calls;
    if (s_platform_start_error != ESP_OK) return s_platform_start_error;
    assert(pthread_mutex_lock(&s_gate) == 0);
    s_platform_start_entered = true;
    assert(pthread_cond_broadcast(&s_changed) == 0);
    while (s_block_platform_start && !s_release_platform_start) timed_wait();
    assert(pthread_mutex_unlock(&s_gate) == 0);
    assert(!s_created);
    s_entry = entry;
    assert(pthread_create(&s_worker, NULL, worker_entry, NULL) == 0);
    s_created = true;
    return ESP_OK;
}

void ryz_system_services_platform_lock(void)
{
    assert(s_lock_depth == 0);
    assert(pthread_mutex_lock(&s_snapshot_mutex) == 0);
    ++s_lock_depth;
}

void ryz_system_services_platform_unlock(void)
{
    assert(s_lock_depth == 1);
    --s_lock_depth;
    assert(pthread_mutex_unlock(&s_snapshot_mutex) == 0);
}

int64_t ryz_system_services_platform_now_us(void)
{
    return s_now;
}

void ryz_system_services_platform_wait_ms(uint32_t milliseconds)
{
    assert(milliseconds == 200);
    assert(s_lock_depth == 0);
    assert(s_dependency_depth == 0);
    assert(pthread_mutex_lock(&s_gate) == 0);
    ++s_completed_cycles;
    assert(pthread_cond_broadcast(&s_changed) == 0);
    take_permit();
    assert(pthread_mutex_unlock(&s_gate) == 0);
}

void ryz_system_services_platform_report(
    ryz_system_services_stage_t stage, esp_err_t error, uint32_t suppressed)
{
    assert(s_lock_depth == 0);
    assert(stage < RYZ_SYSTEM_STAGE_COUNT);
    ++s_reports[stage].count;
    s_reports[stage].error = error;
    s_reports[stage].suppressed = suppressed;
}

static void dependency_enter(ryz_system_services_stage_t stage)
{
    assert(!pthread_equal(pthread_self(), s_main));
    assert(pthread_equal(pthread_self(), s_worker));
    assert(s_lock_depth == 0);
    assert(s_dependency_depth == 0);
    ++s_dependency_depth;
    ++s_calls[stage];
    /* A real public read must complete even while a dependency executes. This
     * catches holding the service snapshot lock across external calls. */
    ryz_system_services_snapshot_t snapshot;
    assert(ryz_system_services_get_snapshot(&snapshot) == ESP_OK);
}

static esp_err_t dependency_exit(ryz_system_services_stage_t stage)
{
    assert(s_dependency_depth == 1);
    --s_dependency_depth;
    return s_errors[stage];
}

#define INIT_FAKE(function, stage)               \
    esp_err_t function(void)                    \
    {                                          \
        dependency_enter(stage);               \
        return dependency_exit(stage);         \
    }

INIT_FAKE(ryz_provisioning_init, RYZ_SYSTEM_PROVISIONING_INIT)
esp_err_t ryz_provisioning_start(void)
{
    dependency_enter(RYZ_SYSTEM_PROVISIONING_START);
    s_now += s_start_elapsed_us;
    return dependency_exit(RYZ_SYSTEM_PROVISIONING_START);
}
INIT_FAKE(ryz_time_init, RYZ_SYSTEM_TIME_INIT)
INIT_FAKE(ryz_ota_init, RYZ_SYSTEM_OTA_INIT)

esp_err_t ryz_provisioning_get_snapshot(ryz_provisioning_snapshot_t *out)
{
    dependency_enter(RYZ_SYSTEM_NETWORK_SNAPSHOT);
    assert(pthread_mutex_lock(&s_gate) == 0);
    s_network_snapshot_entered = true;
    assert(pthread_cond_broadcast(&s_changed) == 0);
    while (s_block_network_snapshot && !s_release_network_snapshot) timed_wait();
    assert(pthread_mutex_unlock(&s_gate) == 0);
    *out = s_network; /* Deliberately dirty even on error: owner must discard. */
    return dependency_exit(RYZ_SYSTEM_NETWORK_SNAPSHOT);
}

esp_err_t ryz_time_get_snapshot(ryz_time_snapshot_t *out)
{
    dependency_enter(RYZ_SYSTEM_TIME_SNAPSHOT);
    *out = s_time;
    return dependency_exit(RYZ_SYSTEM_TIME_SNAPSHOT);
}

esp_err_t ryz_time_set_network_ready(bool ready)
{
    dependency_enter(RYZ_SYSTEM_TIME_NETWORK);
    assert(s_time_network_count < 128);
    s_time_network_arguments[s_time_network_count++] = ready;
    if (s_errors[RYZ_SYSTEM_TIME_NETWORK] == ESP_OK) s_time.network_ready = ready;
    return dependency_exit(RYZ_SYSTEM_TIME_NETWORK);
}

esp_err_t ryz_ota_set_prerequisites(bool network, bool clock)
{
    dependency_enter(RYZ_SYSTEM_OTA_PREREQUISITES);
    assert(s_prerequisite_count < 128);
    s_prerequisites[s_prerequisite_count].network = network;
    s_prerequisites[s_prerequisite_count].clock = clock;
    ++s_prerequisite_count;
    return dependency_exit(RYZ_SYSTEM_OTA_PREREQUISITES);
}

esp_err_t ryz_provisioning_request_reprovision(void)
{
    dependency_enter(RYZ_SYSTEM_REPROVISION);
    assert(s_prerequisite_count > 0);
    assert(!s_prerequisites[s_prerequisite_count - 1].network);
    assert(pthread_mutex_lock(&s_gate) == 0);
    s_reprovision_entered = true;
    assert(pthread_cond_broadcast(&s_changed) == 0);
    while (s_block_reprovision && !s_release_reprovision) timed_wait();
    assert(pthread_mutex_unlock(&s_gate) == 0);
    if (s_errors[RYZ_SYSTEM_REPROVISION] == ESP_OK) {
        s_network.state = RYZ_PROVISIONING_AP_READY;
        ++s_network.revision;
    }
    return dependency_exit(RYZ_SYSTEM_REPROVISION);
}

esp_err_t ryz_provisioning_set_enabled(bool enabled)
{
    dependency_enter(RYZ_SYSTEM_NETWORK_CONTROL);
    assert(atomic_load(&s_ota_held));
    assert(!s_ota.worker_active && s_ota.cleanup_confirmed);
    /* An OTA whose initialization failed cannot admit a worker. The owner
     * still latches its future hold, but must not call uninitialized APIs. */
    if (s_errors[RYZ_SYSTEM_OTA_INIT] == ESP_OK)
        assert(s_prerequisite_count && !s_prerequisites[s_prerequisite_count - 1].network);
    ++s_network_control_calls;
    if (s_errors[RYZ_SYSTEM_NETWORK_CONTROL] == ESP_OK) {
        s_network.enabled = enabled;
        s_network.state = enabled ? RYZ_PROVISIONING_CONNECTING : RYZ_PROVISIONING_OFF;
        s_network.stop_confirmed = !enabled;
        s_network.ipv4[0] = '\0';
        ++s_network.revision;
    }
    return dependency_exit(RYZ_SYSTEM_NETWORK_CONTROL);
}

esp_err_t ryz_provisioning_open_portal(void)
{
    dependency_enter(RYZ_SYSTEM_NETWORK_CONTROL);
    assert(atomic_load(&s_ota_held));
    assert(!s_ota.worker_active && s_ota.cleanup_confirmed);
    assert(s_prerequisite_count && !s_prerequisites[s_prerequisite_count - 1].network);
    ++s_open_portal_calls;
    assert(pthread_mutex_lock(&s_gate) == 0);
    s_open_portal_entered = true;
    assert(pthread_cond_broadcast(&s_changed) == 0);
    while (s_block_open_portal && !s_release_open_portal) timed_wait();
    assert(pthread_mutex_unlock(&s_gate) == 0);
    if (s_errors[RYZ_SYSTEM_NETWORK_CONTROL] == ESP_OK) {
        s_network.enabled = true;
        s_network.state = RYZ_PROVISIONING_AP_READY;
        s_network.portal_active = true;
        s_network.stop_confirmed = false;
        s_network.ipv4[0] = '\0';
        ++s_network.revision;
    }
    return dependency_exit(RYZ_SYSTEM_NETWORK_CONTROL);
}

esp_err_t ryz_provisioning_boot_fallback(void)
{
    dependency_enter(RYZ_SYSTEM_BOOT_NETWORK);
    assert(atomic_load(&s_ota_held));
    assert(!s_ota.worker_active && s_ota.cleanup_confirmed);
    assert(s_prerequisite_count && !s_prerequisites[s_prerequisite_count - 1].network);
    ++s_boot_fallback_calls;
    s_network.boot_restore_pending = false;
    uint32_t id = 123;
    assert(ryz_system_services_request_network(RYZ_SYSTEM_NETWORK_OFF, &id) == ESP_ERR_INVALID_STATE && !id);
    assert(pthread_mutex_lock(&s_gate) == 0);
    s_boot_fallback_entered = true;
    assert(pthread_cond_broadcast(&s_changed) == 0);
    while (s_block_boot_fallback && !s_release_boot_fallback) timed_wait();
    assert(pthread_mutex_unlock(&s_gate) == 0);
    if (s_boot_fallback_error == ESP_OK) {
        s_network.state = s_boot_became_online ? RYZ_PROVISIONING_ONLINE : RYZ_PROVISIONING_AP_READY;
        s_network.portal_active = !s_boot_became_online;
        strcpy(s_network.portal_ip, s_boot_became_online ? "" : "192.0.2.1");
        strcpy(s_network.ipv4, s_boot_became_online ? "192.0.2.3" : "");
        s_network.last_error = ESP_OK;
    } else {
        s_network.state = RYZ_PROVISIONING_FAILED;
        s_network.last_error = s_boot_fallback_error;
    }
    ++s_network.revision;
    (void)dependency_exit(RYZ_SYSTEM_BOOT_NETWORK);
    return s_boot_fallback_error;
}

static void begin_cycle(int64_t now)
{
    assert(pthread_mutex_lock(&s_gate) == 0);
    assert(s_permits == 0);
    s_now = now;
    ++s_permits;
    assert(pthread_cond_broadcast(&s_changed) == 0);
    assert(pthread_mutex_unlock(&s_gate) == 0);
}

static void await_cycles(unsigned target)
{
    assert(pthread_mutex_lock(&s_gate) == 0);
    while (s_completed_cycles < target) timed_wait();
    assert(s_completed_cycles == target);
    assert(pthread_mutex_unlock(&s_gate) == 0);
}

static void cycle(int64_t now)
{
    unsigned target = s_completed_cycles + 1;
    begin_cycle(now);
    await_cycles(target);
}

static ryz_system_services_snapshot_t snapshot(void)
{
    ryz_system_services_snapshot_t value;
    assert(ryz_system_services_get_snapshot(&value) == ESP_OK);
    return value;
}

static void assert_prerequisites(bool network, bool clock)
{
    assert(s_prerequisite_count != 0);
    assert(s_prerequisites[s_prerequisite_count - 1].network == network);
    assert(s_prerequisites[s_prerequisite_count - 1].clock == clock);
}

static void startup_case(void)
{
    ryz_system_services_snapshot_t value;
    uint32_t operation = 123;
    assert(ryz_system_services_get_snapshot(NULL) == ESP_ERR_INVALID_ARG);
    assert(ryz_system_services_get_snapshot(&value) == ESP_ERR_INVALID_STATE);
    assert(ryz_system_services_request_reprovision(NULL) == ESP_ERR_INVALID_ARG);
    assert(ryz_system_services_request_reprovision(&operation) == ESP_ERR_INVALID_STATE);
    assert(operation == 0);
    s_platform_init_error = ESP_ERR_NO_MEM;
    assert(ryz_system_services_start() == ESP_ERR_NO_MEM);
    assert(s_platform_start_calls == 0);
    assert(s_platform_deinit_calls == 0);
    s_platform_init_error = ESP_OK;
    s_platform_start_error = ESP_FAIL;
    assert(ryz_system_services_start() == ESP_FAIL);
    assert(s_platform_deinit_calls == 1);
    assert(ryz_system_services_get_snapshot(&value) == ESP_ERR_INVALID_STATE);
    s_platform_start_error = ESP_OK;
    assert(ryz_system_services_start() == ESP_OK);
    assert(ryz_system_services_start() == ESP_OK);
    assert(s_platform_init_calls == 3);
    assert(s_platform_start_calls == 2);
    assert(snapshot().cycles == 0);
    assert(ryz_system_services_request_reprovision(&operation) == ESP_ERR_INVALID_STATE);
    cycle(0);
    assert(snapshot().cycles == 1);
    assert(ryz_system_services_start() == ESP_OK);
    assert(s_platform_start_calls == 2);
}

static void independent_case(void)
{
    assert(ryz_system_services_start() == ESP_OK);
    cycle(0);
    assert_prerequisites(true, false);
    s_time.clock_valid = true;
    s_time.state = RYZ_TIME_SYNCED;
    ++s_time.revision;
    cycle(200000);
    assert_prerequisites(true, true);
    cycle(400000);
    ryz_system_services_snapshot_t value = snapshot();
    assert(value.cycles == 3 && value.updated_at_us == 400000);
    assert(value.network.revision == 7 && value.time.clock_valid);
    assert(s_calls[RYZ_SYSTEM_NETWORK_SNAPSHOT] == 3);
    assert(s_calls[RYZ_SYSTEM_TIME_SNAPSHOT] == 3);
    s_network.state = RYZ_PROVISIONING_CONNECTING;
    cycle(600000);
    assert_prerequisites(false, true);
    assert(!s_time.network_ready);
}

static void *start_concurrently(void *unused)
{
    (void)unused;
    assert(ryz_system_services_start() == ESP_OK);
    return NULL;
}

static void concurrent_start_case(void)
{
    pthread_t starter;
    s_block_platform_start = true;
    assert(pthread_create(&starter, NULL, start_concurrently, NULL) == 0);
    assert(pthread_mutex_lock(&s_gate) == 0);
    while (!s_platform_start_entered) timed_wait();
    assert(pthread_mutex_unlock(&s_gate) == 0);
    assert(ryz_system_services_start() == ESP_ERR_INVALID_STATE);
    ryz_system_services_snapshot_t value;
    assert(ryz_system_services_get_snapshot(&value) == ESP_ERR_INVALID_STATE);
    uint32_t operation = 42;
    assert(ryz_system_services_request_reprovision(&operation) == ESP_ERR_INVALID_STATE);
    assert(operation == 0);
    assert(pthread_mutex_lock(&s_gate) == 0);
    s_release_platform_start = true;
    assert(pthread_cond_broadcast(&s_changed) == 0);
    assert(pthread_mutex_unlock(&s_gate) == 0);
    assert(pthread_join(starter, NULL) == 0);
    assert(s_platform_start_calls == 1);
    assert(ryz_system_services_start() == ESP_OK);
    cycle(0);
    assert(snapshot().cycles == 1);
}

static void fail_closed_case(void)
{
    s_time.clock_valid = true;
    assert(ryz_system_services_start() == ESP_OK);
    cycle(0);
    assert_prerequisites(true, true);
    s_errors[RYZ_SYSTEM_NETWORK_SNAPSHOT] = ESP_FAIL;
    cycle(200000);
    assert_prerequisites(false, true);
    ryz_system_services_snapshot_t value = snapshot();
    assert(!value.network_valid);
    assert(value.network.state == RYZ_PROVISIONING_FAILED);
    assert(value.network.last_error == ESP_FAIL);
    assert(value.network.ipv4[0] == '\0' && value.network.ap_password[0] == '\0');
    s_errors[RYZ_SYSTEM_NETWORK_SNAPSHOT] = ESP_OK;
    s_errors[RYZ_SYSTEM_TIME_SNAPSHOT] = ESP_ERR_TIMEOUT;
    cycle(400000);
    assert_prerequisites(true, false);
    value = snapshot();
    assert(value.network_valid && !value.time_valid);
    assert(!value.time.clock_valid && value.time.last_sync_unix == 0);
    assert(value.errors[RYZ_SYSTEM_TIME_SNAPSHOT] == ESP_ERR_TIMEOUT);
    s_errors[RYZ_SYSTEM_TIME_SNAPSHOT] = ESP_OK;
    cycle(600000);
    assert_prerequisites(true, true);
    assert(snapshot().errors[RYZ_SYSTEM_TIME_SNAPSHOT] == ESP_OK);
}

static void ntp_backoff_case(void)
{
    s_errors[RYZ_SYSTEM_TIME_NETWORK] = ESP_FAIL;
    assert(ryz_system_services_start() == ESP_OK);
    cycle(0);
    cycle(4999999);
    assert(s_time_network_count == 1);
    cycle(5000000);
    assert(s_time_network_count == 2);
    s_network.state = RYZ_PROVISIONING_FAILED;
    cycle(5200000);
    cycle(5400000);
    assert(s_time_network_count == 4);
    assert(!s_time_network_arguments[2] && !s_time_network_arguments[3]);
    s_errors[RYZ_SYSTEM_TIME_NETWORK] = ESP_OK;
    cycle(5600000);
    s_network.state = RYZ_PROVISIONING_ONLINE;
    cycle(5800000);
    assert(s_time_network_count == 6 && s_time_network_arguments[5]);
    assert(snapshot().errors[RYZ_SYSTEM_TIME_NETWORK] == ESP_OK);
}

static void initial_retry_case(void)
{
    s_errors[RYZ_SYSTEM_PROVISIONING_INIT] = ESP_FAIL;
    s_errors[RYZ_SYSTEM_TIME_INIT] = ESP_FAIL;
    s_errors[RYZ_SYSTEM_OTA_INIT] = ESP_FAIL;
    assert(ryz_system_services_start() == ESP_OK);
    cycle(0);
    assert(!snapshot().provisioning_ready && !snapshot().time_ready);
    assert(!snapshot().ota_ready && !snapshot().network_valid);
    assert(snapshot().boot_network_settled && snapshot().boot_network_error == ESP_FAIL);
    memset(s_errors, 0, sizeof(s_errors));
    s_errors[RYZ_SYSTEM_PROVISIONING_START] = ESP_FAIL;
    cycle(4999999);
    assert(s_calls[RYZ_SYSTEM_PROVISIONING_INIT] == 1);
    assert(s_calls[RYZ_SYSTEM_PROVISIONING_START] == 0);
    cycle(5000000);
    assert(s_calls[RYZ_SYSTEM_PROVISIONING_INIT] == 2);
    assert(s_calls[RYZ_SYSTEM_TIME_INIT] == 2 && s_calls[RYZ_SYSTEM_OTA_INIT] == 2);
    assert(!snapshot().provisioning_started);
    s_errors[RYZ_SYSTEM_PROVISIONING_START] = ESP_OK;
    cycle(9999999);
    assert(s_calls[RYZ_SYSTEM_PROVISIONING_START] == 0);
    cycle(10000000);
    assert(!snapshot().provisioning_started);
    assert(s_calls[RYZ_SYSTEM_PROVISIONING_START] == 0);
    s_network.state = RYZ_PROVISIONING_FAILED;
    cycle(15000000);
    cycle(20000000);
    assert(s_calls[RYZ_SYSTEM_PROVISIONING_INIT] == 2);
    assert(s_calls[RYZ_SYSTEM_PROVISIONING_START] == 0);
    assert_prerequisites(false, false);
}

static void reprovision_case(void)
{
    uint32_t first = 0, second = 0;
    assert(ryz_system_services_start() == ESP_OK);
    cycle(0);
    s_block_reprovision = true;
    s_block_network_snapshot = true;
    s_network_snapshot_entered = false;
    s_errors[RYZ_SYSTEM_REPROVISION] = ESP_FAIL;
    assert(ryz_system_services_request_reprovision(&first) == ESP_OK);
    assert(first != 0 && s_calls[RYZ_SYSTEM_REPROVISION] == 0);
    assert(ryz_system_services_request_reprovision(&second) == ESP_ERR_INVALID_STATE);
    assert(second == 0);
    assert(snapshot().reprovision_completed == 0);
    begin_cycle(200000);
    assert(pthread_mutex_lock(&s_gate) == 0);
    while (!s_reprovision_entered) timed_wait();
    assert(pthread_mutex_unlock(&s_gate) == 0);
    ryz_system_services_snapshot_t value = snapshot();
    assert(value.reprovision_requested == first && value.reprovision_completed == 0);
    assert(ryz_system_services_request_reprovision(&second) == ESP_ERR_INVALID_STATE);
    assert(pthread_mutex_lock(&s_gate) == 0);
    s_release_reprovision = true;
    assert(pthread_cond_broadcast(&s_changed) == 0);
    while (!s_network_snapshot_entered) timed_wait();
    assert(pthread_mutex_unlock(&s_gate) == 0);
    /* The operation returned, but its resulting network sample has not. No
     * observer may see completed paired with the old cycle or reuse its slot. */
    value = snapshot();
    assert(value.cycles == 1 && value.reprovision_completed == 0);
    assert(value.reprovision_requested == first);
    assert(ryz_system_services_request_reprovision(&second) == ESP_ERR_INVALID_STATE);
    assert(pthread_mutex_lock(&s_gate) == 0);
    s_release_network_snapshot = true;
    assert(pthread_cond_broadcast(&s_changed) == 0);
    assert(pthread_mutex_unlock(&s_gate) == 0);
    await_cycles(2);
    value = snapshot();
    assert(value.reprovision_completed == first && value.reprovision_result == ESP_FAIL);
    assert(s_calls[RYZ_SYSTEM_REPROVISION] == 1);
    assert(s_reports[RYZ_SYSTEM_REPROVISION].count == 1);
    s_errors[RYZ_SYSTEM_REPROVISION] = ESP_OK;
    assert(ryz_system_services_request_reprovision(&second) == ESP_OK);
    assert(second != 0 && second != first);
    value = snapshot();
    assert(value.reprovision_requested == second && value.reprovision_completed == first);
    assert(value.reprovision_result == ESP_FAIL);
    cycle(400000);
    value = snapshot();
    assert(value.reprovision_completed == second && value.reprovision_result == ESP_OK);
    assert(s_calls[RYZ_SYSTEM_REPROVISION] == 2);
    assert(value.network.state == RYZ_PROVISIONING_AP_READY);
    assert_prerequisites(false, false);
}

static void network_pending_case(void)
{
    assert(ryz_system_services_start() == ESP_OK);
    cycle(0);
    s_ota.worker_active = true;
    s_ota.cleanup_confirmed = false;
    uint32_t first, other;
    assert(ryz_system_services_request_network(RYZ_SYSTEM_NETWORK_OFF, &first) == ESP_OK);
    assert(atomic_load(&s_ota_held) && s_network_control_calls == 0);
    assert(snapshot().network_operation.requested == first);
    assert(snapshot().network_operation.completed == 0);
    assert(ryz_system_services_request_reprovision(&other) == ESP_ERR_INVALID_STATE && !other);
    cycle(200000);
    assert(snapshot().network_operation.phase == RYZ_SYSTEM_NETWORK_WAITING_OTA);
    assert(snapshot().network.state == RYZ_PROVISIONING_ONLINE);
    assert(s_network_control_calls == 0 && atomic_load(&s_ota_held));
    s_ota.state = RYZ_OTA_FAILED; /* Business failure still is not release. */
    cycle(400000);
    assert(s_network_control_calls == 0);
    s_ota.worker_active = false;
    s_ota.cleanup_confirmed = true;
    s_check_release_reentry = true;
    cycle(600000);
    assert(snapshot().network_operation.completed == first);
    assert(snapshot().network_operation.result == ESP_OK);
    assert(snapshot().network.state == RYZ_PROVISIONING_OFF);
    assert(s_network_control_calls == 1 && !atomic_load(&s_ota_held));
    cycle(6000000);
    assert(s_calls[RYZ_SYSTEM_PROVISIONING_START] == 1);
    assert(snapshot().network.state == RYZ_PROVISIONING_OFF);
    assert(ryz_system_services_request_network(RYZ_SYSTEM_NETWORK_ON, &other) == ESP_OK);
    assert(other > first && snapshot().network_operation.completed == first);
    cycle(6200000);
    assert(s_network_control_calls == 2);
    assert(snapshot().network.state == RYZ_PROVISIONING_CONNECTING);
    assert(snapshot().network_operation.completed == other);
}

static void network_cleanup_failure_case(void)
{
    assert(ryz_system_services_start() == ESP_OK);
    cycle(0);
    s_ota.cleanup_confirmed = false;
    s_ota.cleanup_error = ESP_FAIL;
    uint32_t id;
    assert(ryz_system_services_request_network(RYZ_SYSTEM_NETWORK_OFF, &id) == ESP_OK);
    cycle(200000);
    assert(snapshot().network_operation.completed == id);
    assert(snapshot().network_operation.phase == RYZ_SYSTEM_NETWORK_FAILED);
    assert(snapshot().network_operation.result == ESP_FAIL);
    assert(snapshot().network.state == RYZ_PROVISIONING_ONLINE);
    assert(!s_network_control_calls);
    s_ota.cleanup_confirmed = true;
    assert(ryz_system_services_request_network(RYZ_SYSTEM_NETWORK_OFF, &id) == ESP_OK);
    s_errors[RYZ_SYSTEM_NETWORK_CONTROL] = ESP_ERR_TIMEOUT;
    cycle(400000);
    assert(snapshot().network_operation.result == ESP_ERR_TIMEOUT);
    assert(snapshot().network_operation.phase == RYZ_SYSTEM_NETWORK_FAILED);
    assert(snapshot().network.state == RYZ_PROVISIONING_ONLINE);
    cycle(6000000);
    assert(s_network_control_calls == 1); /* No automatic control retry. */
}

static void network_overrides_retry_case(void)
{
    s_errors[RYZ_SYSTEM_PROVISIONING_START] = ESP_FAIL;
    s_errors[RYZ_SYSTEM_OTA_INIT] = ESP_FAIL;
    assert(ryz_system_services_start() == ESP_OK);
    cycle(0);
    assert(!snapshot().provisioning_started);
    uint32_t id;
    assert(ryz_system_services_request_network(RYZ_SYSTEM_NETWORK_OFF, &id) == ESP_OK);
    cycle(200000);
    assert(snapshot().network_operation.completed == id);
    assert(snapshot().network.state == RYZ_PROVISIONING_OFF);
    assert(snapshot().provisioning_started); /* Settled OFF is healthy intent. */
    s_errors[RYZ_SYSTEM_OTA_INIT] = ESP_OK;
    cycle(5000000);
    assert(s_calls[RYZ_SYSTEM_PROVISIONING_START] == 1);
    assert(snapshot().network.state == RYZ_PROVISIONING_OFF);
}

static void network_setup_case(void)
{
    s_network.credentials_stored = true;
    strcpy(s_network.sta_ssid, "saved-network");
    assert(ryz_system_services_start() == ESP_OK);
    cycle(0);
    s_ota.worker_active = true;
    s_ota.cleanup_confirmed = false;
    uint32_t first, other;
    assert(ryz_system_services_request_network(RYZ_SYSTEM_NETWORK_OPEN_AP, &first) == ESP_OK);
    assert(first && atomic_load(&s_ota_held) && !s_open_portal_calls);
    assert(snapshot().network_operation.action == RYZ_SYSTEM_NETWORK_OPEN_AP);
    assert(snapshot().network_operation.phase == RYZ_SYSTEM_NETWORK_QUEUED);
    assert(ryz_system_services_request_network(RYZ_SYSTEM_NETWORK_ON, &other) == ESP_ERR_INVALID_STATE);
    assert(!other);
    cycle(200000);
    assert(snapshot().network_operation.phase == RYZ_SYSTEM_NETWORK_WAITING_OTA);
    assert(snapshot().network.state == RYZ_PROVISIONING_ONLINE && !s_open_portal_calls);
    s_ota.state = RYZ_OTA_FAILED;
    cycle(400000);
    assert(!s_open_portal_calls); /* A business failure is not SDK cleanup. */
    s_ota.worker_active = false;
    s_ota.cleanup_confirmed = true;
    s_block_open_portal = true;
    begin_cycle(600000);
    assert(pthread_mutex_lock(&s_gate) == 0);
    while (!s_open_portal_entered) timed_wait();
    assert(pthread_mutex_unlock(&s_gate) == 0);
    ryz_system_services_snapshot_t during = snapshot();
    assert(during.network_operation.phase == RYZ_SYSTEM_NETWORK_APPLYING);
    assert(during.network_operation.requested == first && !during.network_operation.completed);
    assert(during.network.state == RYZ_PROVISIONING_ONLINE);
    assert(ryz_system_services_request_reprovision(&other) == ESP_ERR_INVALID_STATE && !other);
    assert(ryz_system_services_request_network(RYZ_SYSTEM_NETWORK_OFF, &other) == ESP_ERR_INVALID_STATE && !other);
    assert(pthread_mutex_lock(&s_gate) == 0);
    s_release_open_portal = true;
    assert(pthread_cond_broadcast(&s_changed) == 0);
    assert(pthread_mutex_unlock(&s_gate) == 0);
    await_cycles(4);
    ryz_system_services_snapshot_t done = snapshot();
    assert(done.network_operation.completed == first && done.network_operation.result == ESP_OK);
    assert(done.network_operation.completed_action == RYZ_SYSTEM_NETWORK_OPEN_AP);
    assert(done.network_operation.phase == RYZ_SYSTEM_NETWORK_DONE);
    assert(done.network.state == RYZ_PROVISIONING_AP_READY && done.network.portal_active);
    assert(done.network.credentials_stored && !strcmp(done.network.sta_ssid, "saved-network"));
    assert(!done.reprovision_requested && !done.reprovision_completed);
    assert(s_open_portal_calls == 1 && !s_network_control_calls && !s_calls[RYZ_SYSTEM_REPROVISION]);
    assert(!atomic_load(&s_ota_held));
    cycle(6000000);
    assert(s_open_portal_calls == 1 && s_calls[RYZ_SYSTEM_PROVISIONING_START] == 1);
}

static void network_setup_failures_case(void)
{
    uint32_t id = 99;
    assert(ryz_system_services_request_network(RYZ_SYSTEM_NETWORK_OPEN_AP, &id) == ESP_ERR_INVALID_STATE);
    assert(!id);
    assert(ryz_system_services_start() == ESP_OK);
    cycle(0);
    const ryz_system_network_action_t invalid[] = {
        RYZ_SYSTEM_NETWORK_NONE, (ryz_system_network_action_t)-1, (ryz_system_network_action_t)99,
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        id = 99;
        assert(ryz_system_services_request_network(invalid[i], &id) == ESP_ERR_INVALID_ARG && !id);
    }
    assert(!atomic_load(&s_ota_held) && !snapshot().network_operation.requested);
    assert(ryz_system_services_request_network(RYZ_SYSTEM_NETWORK_OPEN_AP, NULL) == ESP_ERR_INVALID_ARG);
    s_ota.cleanup_confirmed = false;
    s_ota.cleanup_error = ESP_FAIL;
    assert(ryz_system_services_request_network(RYZ_SYSTEM_NETWORK_OPEN_AP, &id) == ESP_OK);
    cycle(200000);
    assert(snapshot().network_operation.completed == id);
    assert(snapshot().network_operation.completed_action == RYZ_SYSTEM_NETWORK_OPEN_AP);
    assert(snapshot().network_operation.phase == RYZ_SYSTEM_NETWORK_FAILED);
    assert(snapshot().network_operation.result == ESP_FAIL && !s_open_portal_calls);
    assert(snapshot().network.state == RYZ_PROVISIONING_ONLINE && !atomic_load(&s_ota_held));
    s_ota.cleanup_confirmed = true;
    s_errors[RYZ_SYSTEM_NETWORK_CONTROL] = ESP_ERR_TIMEOUT;
    assert(ryz_system_services_request_network(RYZ_SYSTEM_NETWORK_OPEN_AP, &id) == ESP_OK);
    cycle(400000);
    assert(snapshot().network_operation.result == ESP_ERR_TIMEOUT && s_open_portal_calls == 1);
    assert(snapshot().network.state == RYZ_PROVISIONING_ONLINE && !snapshot().network.portal_active);
    cycle(6000000);
    assert(s_open_portal_calls == 1 && !s_network_control_calls && !s_calls[RYZ_SYSTEM_REPROVISION]);
    s_errors[RYZ_SYSTEM_NETWORK_CONTROL] = ESP_OK;
    assert(ryz_system_services_request_network(RYZ_SYSTEM_NETWORK_OPEN_AP, &id) == ESP_OK);
    cycle(6200000);
    assert(snapshot().network_operation.completed == id && snapshot().network_operation.result == ESP_OK);
    assert(snapshot().network.state == RYZ_PROVISIONING_AP_READY && s_open_portal_calls == 2);
}

static void network_completion_state_case(void)
{
    assert(ryz_system_services_start() == ESP_OK);
    cycle(0);
    assert(!snapshot().network_operation.completed_network_valid);
    s_ota.cleanup_confirmed = false;
    s_ota.cleanup_error = ESP_FAIL;
    uint32_t first, second, third;
    assert(ryz_system_services_request_network(RYZ_SYSTEM_NETWORK_OPEN_AP, &first) == ESP_OK);
    cycle(200000);
    ryz_system_services_snapshot_t done = snapshot();
    assert(done.network_operation.completed == first && done.network_operation.result == ESP_FAIL);
    assert(done.network_operation.completed_network_valid);
    assert(done.network_operation.completed_network_state == RYZ_PROVISIONING_ONLINE);

    /* A later genuine STA failure changes live state, not the historical
     * network observation belonging to the rejected OPEN_AP operation. */
    s_network.state = RYZ_PROVISIONING_FAILED;
    ++s_network.revision;
    cycle(400000);
    done = snapshot();
    assert(done.network.state == RYZ_PROVISIONING_FAILED);
    assert(done.network_operation.completed == first && done.network_operation.completed_network_valid);
    assert(done.network_operation.completed_network_state == RYZ_PROVISIONING_ONLINE);
    s_ota.cleanup_confirmed = true;
    assert(ryz_system_services_request_network(RYZ_SYSTEM_NETWORK_ON, &second) == ESP_OK);
    ryz_system_services_snapshot_t queued = snapshot();
    assert(queued.network_operation.requested == second && queued.network_operation.completed == first);
    assert(queued.network_operation.completed_action == RYZ_SYSTEM_NETWORK_OPEN_AP);
    assert(queued.network_operation.result == ESP_FAIL && queued.network_operation.completed_network_valid);
    assert(queued.network_operation.completed_network_state == RYZ_PROVISIONING_ONLINE);

    s_block_network_snapshot = true;
    s_network_snapshot_entered = false;
    begin_cycle(600000);
    assert(pthread_mutex_lock(&s_gate) == 0);
    while (!s_network_snapshot_entered) timed_wait();
    assert(pthread_mutex_unlock(&s_gate) == 0);
    /* Native ON already returned, but the post-action sample is blocked.
     * The public completion and its metadata must still belong to first. */
    ryz_system_services_snapshot_t during = snapshot();
    assert(during.network_operation.completed == first);
    assert(during.network_operation.completed_network_valid);
    assert(during.network_operation.completed_network_state == RYZ_PROVISIONING_ONLINE);
    assert(during.network.state == RYZ_PROVISIONING_FAILED);
    assert(pthread_mutex_lock(&s_gate) == 0);
    s_release_network_snapshot = true;
    assert(pthread_cond_broadcast(&s_changed) == 0);
    assert(pthread_mutex_unlock(&s_gate) == 0);
    await_cycles(4);
    done = snapshot();
    assert(done.network_operation.completed == second && done.network_operation.result == ESP_OK);
    assert(done.network.state == RYZ_PROVISIONING_CONNECTING);
    assert(done.network_operation.completed_network_valid);
    assert(done.network_operation.completed_network_state == RYZ_PROVISIONING_CONNECTING);
    s_network.state = RYZ_PROVISIONING_FAILED;
    cycle(800000);
    assert(snapshot().network_operation.completed_network_state == RYZ_PROVISIONING_CONNECTING);
    s_errors[RYZ_SYSTEM_NETWORK_CONTROL] = ESP_ERR_TIMEOUT;
    assert(ryz_system_services_request_network(RYZ_SYSTEM_NETWORK_OPEN_AP, &third) == ESP_OK);
    cycle(1000000);
    done = snapshot();
    assert(done.network_operation.completed == third && done.network_operation.result == ESP_ERR_TIMEOUT);
    assert(done.network_operation.completed_network_valid);
    assert(done.network_operation.completed_network_state == RYZ_PROVISIONING_FAILED);
}

static void network_completion_invalid_sample_case(void)
{
    assert(ryz_system_services_start() == ESP_OK);
    cycle(0);
    uint32_t first, second, third;
    assert(ryz_system_services_request_network(RYZ_SYSTEM_NETWORK_OPEN_AP, &first) == ESP_OK);
    cycle(200000);
    assert(snapshot().network_operation.completed_network_valid);
    assert(snapshot().network_operation.completed_network_state == RYZ_PROVISIONING_AP_READY);
    s_errors[RYZ_SYSTEM_NETWORK_SNAPSHOT] = ESP_FAIL;
    assert(ryz_system_services_request_network(RYZ_SYSTEM_NETWORK_OFF, &second) == ESP_OK);
    assert(snapshot().network_operation.completed == first);
    assert(snapshot().network_operation.completed_network_state == RYZ_PROVISIONING_AP_READY);
    cycle(400000);
    ryz_system_services_snapshot_t done = snapshot();
    assert(done.network_operation.completed == second && done.network_operation.result == ESP_OK);
    assert(!done.network_valid && !done.network_operation.completed_network_valid);
    assert(done.network_operation.completed_network_state == RYZ_PROVISIONING_UNCONFIGURED);
    s_errors[RYZ_SYSTEM_NETWORK_SNAPSHOT] = ESP_OK;
    cycle(600000);
    done = snapshot();
    assert(done.network_valid && done.network.state == RYZ_PROVISIONING_OFF);
    assert(!done.network_operation.completed_network_valid); /* Never backfill history. */
    assert(ryz_system_services_request_network(RYZ_SYSTEM_NETWORK_OPEN_AP, &third) == ESP_OK);
    assert(snapshot().network_operation.completed == second);
    assert(!snapshot().network_operation.completed_network_valid);
    cycle(800000);
    done = snapshot();
    assert(done.network_operation.completed == third && done.network_operation.completed_network_valid);
    assert(done.network_operation.completed_network_state == RYZ_PROVISIONING_AP_READY);
}

static void logging_case(void)
{
    s_errors[RYZ_SYSTEM_NETWORK_SNAPSHOT] = ESP_ERR_TIMEOUT;
    s_errors[RYZ_SYSTEM_TIME_SNAPSHOT] = ESP_FAIL;
    assert(ryz_system_services_start() == ESP_OK);
    for (unsigned i = 0; i < 25; ++i) cycle((int64_t)i * 200000);
    assert(s_reports[RYZ_SYSTEM_NETWORK_SNAPSHOT].count == 1);
    assert(s_reports[RYZ_SYSTEM_TIME_SNAPSHOT].count == 1);
    cycle(5000000);
    assert(s_reports[RYZ_SYSTEM_NETWORK_SNAPSHOT].count == 2);
    assert(s_reports[RYZ_SYSTEM_NETWORK_SNAPSHOT].suppressed == 24);
    assert(s_reports[RYZ_SYSTEM_NETWORK_SNAPSHOT].error == ESP_ERR_TIMEOUT);
    assert(s_reports[RYZ_SYSTEM_TIME_SNAPSHOT].count == 2);
    assert(s_reports[RYZ_SYSTEM_TIME_SNAPSHOT].suppressed == 24);
}

static void metrics_case(void)
{
    assert(ryz_system_services_start()==ESP_OK);
    assert(!s_temperature_calls && !s_idle_calls);
    assert(snapshot().metrics.temperature_error==ESP_ERR_NOT_FINISHED);
    cycle(0);
    ryz_system_metrics_snapshot_t m=snapshot().metrics;
    assert(m.temperature_valid && m.temperature_deci_c==367 && m.temperature_error==ESP_OK);
    assert(!m.load_valid && !m.load_permille && m.load_error==ESP_ERR_NOT_FINISHED);
    assert(m.sampled_at_us==0 && s_temperature_calls==1 && s_idle_calls==1);
    for(unsigned i=0;i<20;++i) (void)snapshot();
    cycle(999999);
    assert(s_temperature_calls==1 && s_idle_calls==1 && snapshot().metrics.sampled_at_us==0);
    s_idle[0]=1000000;
    cycle(1000000);
    m=snapshot().metrics;
    assert(m.load_valid && m.load_permille==500 && m.load_error==ESP_OK && m.sampled_at_us==1000000);
    s_idle[0]=2000000; s_idle[1]=1000000;
    cycle(2000000); assert(snapshot().metrics.load_permille==0);
    cycle(3000000); assert(snapshot().metrics.load_permille==1000);
    assert(s_temperature_calls==4 && s_idle_calls==4 && s_platform_start_calls==1);
    assert(snapshot().provisioning_ready && snapshot().provisioning_started && snapshot().ota_ready);
}

static void metrics_errors_case(void)
{
    assert(ryz_system_services_start()==ESP_OK);
    cycle(1000000);
    s_temperature=90; s_temperature_error=ESP_FAIL;
    s_idle[0]=s_idle[1]=500000;
    cycle(2000000);
    ryz_system_metrics_snapshot_t m=snapshot().metrics;
    assert(!m.temperature_valid && !m.temperature_deci_c && m.temperature_error==ESP_FAIL);
    assert(m.load_valid && m.load_permille==500);
    s_temperature_error=ESP_OK; s_temperature=NAN; s_idle_error=ESP_ERR_TIMEOUT;
    cycle(3000000); m=snapshot().metrics;
    assert(!m.temperature_valid && m.temperature_error==ESP_ERR_INVALID_STATE);
    assert(!m.load_valid && !m.load_permille && m.load_error==ESP_ERR_TIMEOUT);
    s_temperature=-3.25f; s_idle_error=ESP_OK;
    cycle(4000000); m=snapshot().metrics;
    assert(m.temperature_valid && m.temperature_deci_c==-33);
    assert(!m.load_valid && m.load_error==ESP_ERR_NOT_FINISHED);
    s_idle[0]=s_idle[1]=0;
    cycle(5000000); m=snapshot().metrics;
    assert(!m.load_valid && !m.load_permille && m.load_error==ESP_ERR_INVALID_STATE);
    cycle(6000000); assert(snapshot().metrics.load_valid && snapshot().metrics.load_permille==1000);
    cycle(5500000); m=snapshot().metrics;
    assert(!m.load_valid && m.load_error==ESP_ERR_INVALID_STATE);
    s_idle[0]=s_idle[1]=1000000;
    cycle(6500000); assert(snapshot().metrics.load_valid && !snapshot().metrics.load_permille);
    s_idle[0]=3000000;
    cycle(7500000); assert(!snapshot().metrics.load_valid);
    s_idle[0]=3500000; s_idle[1]=1500000;
    cycle(8500000); assert(snapshot().metrics.load_valid && snapshot().metrics.load_permille==500);
    s_idle[0]=10000000;
    cycle(9500000); assert(!snapshot().metrics.load_valid);
    unsigned calls=s_temperature_calls;
    cycle(-1); m=snapshot().metrics;
    assert(!m.temperature_valid && !m.load_valid && !m.sampled_at_us && s_temperature_calls==calls);
    s_idle[0]=s_idle[1]=0;
    cycle(10500000); assert(snapshot().metrics.load_error==ESP_ERR_NOT_FINISHED);
    /* Beyond the SDK U32 wrap boundary: real 64-bit values, not modulo-U32. */
    cycle(INT64_C(4294000000));
    s_idle[0]=INT64_C(4294500000); s_idle[1]=INT64_C(4294500000);
    cycle(INT64_C(4295000000)); assert(!snapshot().metrics.load_valid); /* Impossible jump. */
    s_idle[0]+=500000; s_idle[1]+=500000;
    cycle(INT64_C(4296000000));
    assert(snapshot().metrics.load_valid && snapshot().metrics.load_permille==500);
    cycle(INT64_MAX); assert(!snapshot().metrics.load_valid);
    assert(snapshot().provisioning_ready && snapshot().provisioning_started && snapshot().ota_ready);
}

static void metrics_blocked_case(void)
{
    assert(ryz_system_services_start()==ESP_OK);
    cycle(0);
    s_block_temperature=true; s_temperature_entered=false;
    begin_cycle(1000000);
    assert(pthread_mutex_lock(&s_gate)==0);
    while(!s_temperature_entered) timed_wait();
    assert(pthread_mutex_unlock(&s_gate)==0);
    ryz_system_services_snapshot_t old=snapshot();
    assert(old.cycles==1 && old.metrics.sampled_at_us==0);
    assert(old.metrics.temperature_valid); /* Old age retained, not refreshed. */
    assert(pthread_mutex_lock(&s_gate)==0);
    s_release_temperature=true;
    assert(pthread_cond_broadcast(&s_changed)==0);
    assert(pthread_mutex_unlock(&s_gate)==0);
    await_cycles(2);
    assert(snapshot().metrics.sampled_at_us==1000000 && s_platform_start_calls==1);
}

static void metrics_skew_case(void)
{
    assert(ryz_system_services_start()==ESP_OK);
    s_idle_sample_offset[1]=200000;
    cycle(1000000); /* Local endpoints 1.0s and 1.2s. */
    assert(!snapshot().metrics.load_valid);
    s_idle[0]=500000; s_idle[1]=600000;
    s_idle_sample_offset[1]=400000;
    cycle(2000000); /* Intervals 1.0s and 1.2s: both 50% busy. */
    assert(snapshot().metrics.load_valid && snapshot().metrics.load_permille==500);
    assert(snapshot().metrics.sampled_at_us==2000000);
    s_idle[0]+=1000000; s_idle[1]+=1400000;
    s_idle_sample_offset[1]=800000;
    cycle(3000000); /* 1.4s idle on core 1 is valid for its own interval. */
    assert(snapshot().metrics.load_valid && !snapshot().metrics.load_permille);
    s_idle_error=ESP_ERR_INVALID_STATE;
    cycle(4000000);
    assert(!snapshot().metrics.load_valid && !snapshot().metrics.load_permille);
    s_idle_error=ESP_OK;
    cycle(5000000); /* Any IPC failure revokes both baselines. */
    assert(snapshot().metrics.load_error==ESP_ERR_NOT_FINISHED);
    cycle(6000000);
    assert(snapshot().metrics.load_valid && snapshot().metrics.load_permille==1000);
    assert(s_platform_start_calls==1);
}

static void saved_connecting(void)
{
    s_network.state = RYZ_PROVISIONING_CONNECTING;
    s_network.boot_restore_pending = true;
    s_network.credentials_stored = true;
    s_network.ipv4[0] = '\0';
    strcpy(s_network.sta_ssid, "saved-network");
}

static void boot_timeout_case(void)
{
    saved_connecting();
    assert(ryz_system_services_start() == ESP_OK);
    assert(!snapshot().boot_network_settled);
    cycle(0);
    assert(!snapshot().boot_network_settled && !s_boot_fallback_calls);
    assert(snapshot().boot_network_deadline_us == 10000000);
    cycle(9999999);
    assert(!snapshot().boot_network_settled && !s_boot_fallback_calls);
    cycle(10000000);
    assert(snapshot().boot_network_settled && snapshot().boot_network_error == ESP_OK);
    assert(snapshot().network.state == RYZ_PROVISIONING_AP_READY);
    assert(snapshot().network.credentials_stored && !strcmp(snapshot().network.sta_ssid, "saved-network"));
    assert(!strcmp(snapshot().network.ap_password, "host-fixture"));
    assert(s_boot_fallback_calls == 1 && !s_open_portal_calls && !s_network_control_calls);
    assert(!atomic_load(&s_ota_held));
    s_network.state = RYZ_PROVISIONING_FAILED;
    cycle(20000000);
    assert(s_boot_fallback_calls == 1 && s_calls[RYZ_SYSTEM_PROVISIONING_START] == 1);
}

static void boot_start_clock_case(void)
{
    saved_connecting();
    s_start_elapsed_us = 3000000;
    assert(ryz_system_services_start() == ESP_OK);
    cycle(0);
    assert(snapshot().boot_network_deadline_us == 13000000);
    cycle(12999999);
    assert(!snapshot().boot_network_settled && !s_boot_fallback_calls);
    cycle(13000000);
    assert(snapshot().boot_network_settled && s_boot_fallback_calls == 1);
}

static void boot_terminal_case(const char *kind)
{
    if (!strcmp(kind, "boot_off")) {
        s_network.enabled = false; s_network.stop_confirmed = true;
        s_network.state = RYZ_PROVISIONING_OFF; s_network.ipv4[0] = '\0';
    } else if (!strcmp(kind, "boot_ap")) {
        s_network.state = RYZ_PROVISIONING_AP_READY; s_network.portal_active = true;
        strcpy(s_network.portal_ip, "192.0.2.1"); s_network.ipv4[0] = '\0';
    }
    const ryz_provisioning_state_t expected = s_network.state;
    assert(ryz_system_services_start() == ESP_OK);
    cycle(0);
    assert(snapshot().boot_network_settled && snapshot().boot_network_error == ESP_OK);
    cycle(10000000);
    assert(snapshot().network.state == expected && !s_boot_fallback_calls && !s_network_control_calls);
    s_network.state = RYZ_PROVISIONING_FAILED;
    cycle(20000000);
    assert(!s_boot_fallback_calls && s_calls[RYZ_SYSTEM_PROVISIONING_START] == 1);
}

static void boot_failure_case(const char *kind)
{
    saved_connecting();
    if (!strcmp(kind, "boot_sync_failure")) {
        s_errors[RYZ_SYSTEM_PROVISIONING_START] = ESP_FAIL;
        s_network.state = RYZ_PROVISIONING_FAILED;
        s_network.last_error = ESP_FAIL;
    }
    if (!strcmp(kind, "boot_snapshot_failure")) s_errors[RYZ_SYSTEM_NETWORK_SNAPSHOT] = ESP_FAIL;
    if (!strcmp(kind, "boot_fallback_failure")) s_boot_fallback_error = ESP_ERR_TIMEOUT;
    assert(ryz_system_services_start() == ESP_OK);
    cycle(0);
    if (!strcmp(kind, "boot_snapshot_failure")) {
        assert(snapshot().boot_network_settled && snapshot().boot_network_error == ESP_FAIL);
        s_errors[RYZ_SYSTEM_NETWORK_SNAPSHOT] = ESP_OK;
        cycle(20000000);
        assert(!s_boot_fallback_calls);
    } else {
        if (strcmp(kind, "boot_sync_failure")) {
            s_network.state = RYZ_PROVISIONING_FAILED;
            s_network.last_error = ESP_FAIL;
            cycle(200000);
        }
        assert(snapshot().boot_network_settled && s_boot_fallback_calls == 1);
        assert(snapshot().boot_network_error == s_boot_fallback_error);
        assert(!atomic_load(&s_ota_held));
        cycle(20000000);
        assert(s_boot_fallback_calls == 1);
    }
    assert(s_calls[RYZ_SYSTEM_PROVISIONING_START] == 1);
}

static void boot_late_online_case(void)
{
    saved_connecting();
    assert(ryz_system_services_start() == ESP_OK);
    cycle(0);
    s_boot_became_online = true;
    cycle(10000000);
    assert(snapshot().boot_network_settled && snapshot().network.state == RYZ_PROVISIONING_ONLINE);
    assert(s_boot_fallback_calls == 1 && !s_open_portal_calls);
    assert_prerequisites(true, false);
}

static void boot_ota_case(const char *kind)
{
    saved_connecting();
    assert(ryz_system_services_start() == ESP_OK);
    cycle(0);
    s_ota.worker_active = true; s_ota.cleanup_confirmed = false;
    cycle(10000000);
    assert(!snapshot().boot_network_settled && atomic_load(&s_ota_held) && !s_boot_fallback_calls);
    if (!strcmp(kind, "boot_explicit_off")) {
        uint32_t id;
        assert(ryz_system_services_request_network(RYZ_SYSTEM_NETWORK_OFF, &id) == ESP_OK);
        cycle(10200000);
        assert(snapshot().boot_network_settled && atomic_load(&s_ota_held));
        assert(!s_boot_fallback_calls && !s_network_control_calls);
        s_ota.worker_active = false; s_ota.cleanup_confirmed = true;
        cycle(10400000);
        assert(snapshot().network.state == RYZ_PROVISIONING_OFF && !atomic_load(&s_ota_held));
        assert(s_network_control_calls == 1 && !s_boot_fallback_calls);
    } else {
        s_ota.worker_active = false;
        s_ota.cleanup_confirmed = strcmp(kind, "boot_cleanup_failure") != 0;
        s_ota.cleanup_error = ESP_FAIL;
        cycle(10200000);
        assert(snapshot().boot_network_settled && !atomic_load(&s_ota_held));
        assert(s_boot_fallback_calls == (s_ota.cleanup_confirmed ? 1U : 0U));
        assert(snapshot().boot_network_error == (s_ota.cleanup_confirmed ? ESP_OK : ESP_FAIL));
    }
}

static void boot_fallback_blocked_case(void)
{
    saved_connecting();
    assert(ryz_system_services_start() == ESP_OK);
    cycle(0);
    s_block_boot_fallback = true;
    begin_cycle(10000000);
    assert(pthread_mutex_lock(&s_gate) == 0);
    while (!s_boot_fallback_entered) timed_wait();
    assert(pthread_mutex_unlock(&s_gate) == 0);
    assert(!snapshot().boot_network_settled && snapshot().cycles == 1);
    uint32_t id;
    assert(ryz_system_services_request_network(RYZ_SYSTEM_NETWORK_OFF, &id) == ESP_ERR_INVALID_STATE && !id);
    assert(pthread_mutex_lock(&s_gate) == 0);
    s_release_boot_fallback = true;
    assert(pthread_cond_broadcast(&s_changed) == 0);
    assert(pthread_mutex_unlock(&s_gate) == 0);
    await_cycles(2);
    assert(snapshot().boot_network_settled);
}

static void boot_portal_submit_case(void)
{
    saved_connecting();
    s_network.boot_restore_pending = false; /* AP was ready before HTTP submit. */
    assert(ryz_system_services_start() == ESP_OK);
    cycle(0);
    assert(snapshot().boot_network_settled && snapshot().boot_network_error == ESP_OK);
    cycle(10000000);
    assert(snapshot().network.state == RYZ_PROVISIONING_CONNECTING && !s_boot_fallback_calls);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    s_main = pthread_self();
    s_network.state = RYZ_PROVISIONING_ONLINE;
    s_network.enabled = true;
    s_network.revision = 7;
    strcpy(s_network.ipv4, "192.0.2.3");
    strcpy(s_network.ap_password, "host-fixture");
    s_time.last_sync_unix = RYZ_TIME_MIN_VALID_UNIX_SECONDS + 100;
    if (strcmp(argv[1], "startup") == 0) startup_case();
    else if (strcmp(argv[1], "concurrent_start") == 0) concurrent_start_case();
    else if (strcmp(argv[1], "independent") == 0) independent_case();
    else if (strcmp(argv[1], "fail_closed") == 0) fail_closed_case();
    else if (strcmp(argv[1], "ntp_backoff") == 0) ntp_backoff_case();
    else if (strcmp(argv[1], "initial_retry") == 0) initial_retry_case();
    else if (strcmp(argv[1], "reprovision") == 0) reprovision_case();
    else if (strcmp(argv[1], "logging") == 0) logging_case();
    else if (strcmp(argv[1], "network_pending") == 0) network_pending_case();
    else if (strcmp(argv[1], "network_cleanup_failure") == 0) network_cleanup_failure_case();
    else if (strcmp(argv[1], "network_overrides_retry") == 0) network_overrides_retry_case();
    else if (strcmp(argv[1], "network_setup") == 0) network_setup_case();
    else if (strcmp(argv[1], "network_setup_failures") == 0) network_setup_failures_case();
    else if (strcmp(argv[1], "network_completion_state") == 0) network_completion_state_case();
    else if (strcmp(argv[1], "network_completion_invalid_sample") == 0) network_completion_invalid_sample_case();
    else if (strcmp(argv[1], "metrics") == 0) metrics_case();
    else if (strcmp(argv[1], "metrics_errors") == 0) metrics_errors_case();
    else if (strcmp(argv[1], "metrics_blocked") == 0) metrics_blocked_case();
    else if (strcmp(argv[1], "metrics_skew") == 0) metrics_skew_case();
    else if (strcmp(argv[1], "boot_timeout") == 0) boot_timeout_case();
    else if (strcmp(argv[1], "boot_start_clock") == 0) boot_start_clock_case();
    else if (strcmp(argv[1], "boot_off") == 0 || strcmp(argv[1], "boot_ap") == 0 ||
             strcmp(argv[1], "boot_online") == 0) boot_terminal_case(argv[1]);
    else if (strcmp(argv[1], "boot_failed") == 0 || strcmp(argv[1], "boot_sync_failure") == 0 ||
             strcmp(argv[1], "boot_snapshot_failure") == 0 || strcmp(argv[1], "boot_fallback_failure") == 0)
        boot_failure_case(argv[1]);
    else if (strcmp(argv[1], "boot_late_online") == 0) boot_late_online_case();
    else if (strcmp(argv[1], "boot_ota") == 0 || strcmp(argv[1], "boot_cleanup_failure") == 0 ||
             strcmp(argv[1], "boot_explicit_off") == 0) boot_ota_case(argv[1]);
    else if (strcmp(argv[1], "boot_fallback_blocked") == 0) boot_fallback_blocked_case();
    else if (strcmp(argv[1], "boot_portal_submit") == 0) boot_portal_submit_case();
    else abort();
    assert(s_created);
    assert(pthread_mutex_lock(&s_gate) == 0);
    s_stopping = true;
    assert(pthread_cond_broadcast(&s_changed) == 0);
    assert(pthread_mutex_unlock(&s_gate) == 0);
    assert(pthread_join(s_worker, NULL) == 0);
    printf("SYSTEM_SERVICES_PASS %s\n", argv[1]);
    return 0;
}
