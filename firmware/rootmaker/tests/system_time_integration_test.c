#define _POSIX_C_SOURCE 200809L

/* Keep the real public time declarations visible. Only the old fixture's
 * substitute definitions are renamed; production owner/time objects are
 * compiled separately without these macros. */
#include "ryz_time.h"
#define ryz_time_init fixture_time_init
#define ryz_time_get_snapshot fixture_time_get_snapshot
#define ryz_time_set_network_ready fixture_time_set_network_ready
#define main system_services_fixture_main
#include "system_services_test.c"
#undef main
#undef ryz_time_set_network_ready
#undef ryz_time_get_snapshot
#undef ryz_time_init

#include "ryz_time_platform.h"

/* Reuse only the controlled pthread owner and its other dependency Adapters.
 * This translation unit supplies the time platform, never a time state machine.
 * OTA observations below prove prerequisite propagation, not SDK admission. */
static pthread_mutex_t time_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local unsigned time_lock_depth;
static ryz_time_platform_sync_sink_t time_sink;
static unsigned time_init_calls, time_start_calls, time_now_calls;
static int64_t time_started_at[8];

void ryz_time_platform_snapshot_lock(void)
{
    assert(time_lock_depth == 0);
    assert(pthread_mutex_lock(&time_mutex) == 0);
    ++time_lock_depth;
}

bool ryz_time_platform_snapshot_try_lock(void)
{
    assert(time_lock_depth == 0);
    if (pthread_mutex_trylock(&time_mutex) != 0) return false;
    ++time_lock_depth;
    return true;
}

void ryz_time_platform_snapshot_unlock(void)
{
    assert(time_lock_depth == 1);
    --time_lock_depth;
    assert(pthread_mutex_unlock(&time_mutex) == 0);
}

static void assert_time_platform_owner(void)
{
    assert(pthread_equal(pthread_self(), s_worker));
    assert(s_lock_depth == 0 && time_lock_depth == 0);
    ryz_system_services_snapshot_t value;
    assert(ryz_system_services_get_snapshot(&value) == ESP_OK);
}

esp_err_t ryz_time_platform_init(ryz_time_platform_sync_sink_t sink)
{
    assert_time_platform_owner();
    assert(sink && !time_sink);
    time_sink = sink;
    ++time_init_calls;
    return ESP_OK;
}

esp_err_t ryz_time_platform_start(void)
{
    assert_time_platform_owner();
    assert(time_start_calls < sizeof(time_started_at) / sizeof(time_started_at[0]));
    time_started_at[time_start_calls++] = s_now;
    return ESP_OK; /* Deliberately no automatic SNTP response. */
}

int64_t ryz_time_platform_now_us(void)
{
    /* Fixtures change s_now only while the owner is at its condition-variable
     * barrier. No wall-clock sleep stands in for the 60-second NTP window. */
    ++time_now_calls;
    return s_now;
}

static ryz_time_snapshot_t actual_time(void)
{
    const unsigned starts = time_start_calls, samples = time_now_calls;
    ryz_time_snapshot_t value;
    assert(ryz_time_get_snapshot(&value) == ESP_OK);
    assert(time_start_calls == starts && time_now_calls == samples);
    return value;
}

static void start_online(void)
{
    s_main = pthread_self();
    s_network.state = RYZ_PROVISIONING_ONLINE;
    s_network.enabled = true;
    s_network.revision = 7;
    strcpy(s_network.ipv4, "192.0.2.3");
    assert(ryz_system_services_start() == ESP_OK);
    cycle(INT64_C(1000000));
    assert(time_init_calls == 1 && time_start_calls == 1);
    assert(time_started_at[0] == INT64_C(1000000));
    assert(snapshot().time_valid && snapshot().time.state == RYZ_TIME_SYNCING);
    assert(snapshot().boot_network_settled && snapshot().boot_network_error == ESP_OK);
    assert(!snapshot().time.clock_valid); /* Boot does not wait for the NTP result. */
    assert(actual_time().network_ready && !actual_time().clock_valid);
    assert_prerequisites(true, false);
}

static void off_boot_case(void)
{
    s_main = pthread_self();
    s_network.state = RYZ_PROVISIONING_OFF;
    s_network.enabled = false;
    s_network.stop_confirmed = true;
    assert(ryz_system_services_start() == ESP_OK);
    cycle(0);
    const ryz_system_services_snapshot_t first = snapshot();
    assert(first.cycles == 1 && first.boot_network_settled && first.boot_network_error == ESP_OK);
    assert(first.time_valid && first.time.state == RYZ_TIME_WAITING_NETWORK);
    assert(time_init_calls == 1 && !time_start_calls);
    assert(!actual_time().network_ready && !actual_time().clock_valid);
    cycle(INT64_C(10000000));
    cycle(INT64_C(60000000));
    assert(snapshot().boot_network_settled && snapshot().boot_network_error == ESP_OK);
    assert(snapshot().time.state == RYZ_TIME_WAITING_NETWORK && !time_start_calls);
    assert(s_calls[RYZ_SYSTEM_PROVISIONING_START] == 1);
    assert_prerequisites(false, false);
}

static void timeout_retry_case(void)
{
    start_online();
    cycle(INT64_C(60999999));
    assert(snapshot().time.state == RYZ_TIME_SYNCING && time_start_calls == 1);
    cycle(INT64_C(61000000));
    ryz_system_services_snapshot_t failed = snapshot();
    assert(failed.time_valid && failed.time.state == RYZ_TIME_FAILED);
    assert(failed.time.last_error == ESP_ERR_TIMEOUT);
    assert(failed.errors[RYZ_SYSTEM_TIME_NETWORK] == ESP_ERR_TIMEOUT);
    assert(!failed.time.clock_valid && failed.time.last_sync_unix == 0);
    assert(failed.boot_network_settled && failed.boot_network_error == ESP_OK);
    assert(time_start_calls == 1); /* No same-cycle implicit restart. */
    assert_prerequisites(true, false);
    cycle(INT64_C(61200000));
    cycle(INT64_C(65999999));
    assert(snapshot().time.state == RYZ_TIME_FAILED && time_start_calls == 1);
    cycle(INT64_C(66000000));
    assert(snapshot().time.state == RYZ_TIME_SYNCING && time_start_calls == 2);
    assert(time_started_at[1] == INT64_C(66000000));
    assert(snapshot().errors[RYZ_SYSTEM_TIME_NETWORK] == ESP_OK);
    assert(snapshot().network.revision == 7);
    assert_prerequisites(true, false);
}

static void offline_backoff_case(void)
{
    start_online();
    cycle(INT64_C(61000000));
    assert(snapshot().time.state == RYZ_TIME_FAILED && time_start_calls == 1);
    s_network.state = RYZ_PROVISIONING_OFF;
    s_network.enabled = false;
    ++s_network.revision;
    cycle(INT64_C(61200000));
    assert(snapshot().time.state == RYZ_TIME_WAITING_NETWORK);
    assert(!snapshot().time.network_ready && !actual_time().network_ready);
    assert(snapshot().errors[RYZ_SYSTEM_TIME_NETWORK] == ESP_OK);
    assert(time_start_calls == 1);
    assert(snapshot().boot_network_settled && snapshot().boot_network_error == ESP_OK);
    assert(s_calls[RYZ_SYSTEM_PROVISIONING_START] == 1);
    assert_prerequisites(false, false);
    s_network.state = RYZ_PROVISIONING_ONLINE;
    s_network.enabled = true;
    ++s_network.revision;
    cycle(INT64_C(61400000));
    assert(snapshot().time.state == RYZ_TIME_SYNCING && time_start_calls == 2);
    assert(time_started_at[1] == INT64_C(61400000));
    assert_prerequisites(true, false);
    cycle(INT64_C(66000000));
    cycle(INT64_C(121399999));
    assert(snapshot().time.state == RYZ_TIME_SYNCING && time_start_calls == 2);
    cycle(INT64_C(121400000));
    assert(snapshot().time.state == RYZ_TIME_FAILED);
    assert(snapshot().errors[RYZ_SYSTEM_TIME_NETWORK] == ESP_ERR_TIMEOUT);
    assert(time_start_calls == 2); /* Reconnect started its own full window. */
    assert(snapshot().boot_network_settled && snapshot().boot_network_error == ESP_OK);
    assert(s_calls[RYZ_SYSTEM_PROVISIONING_START] == 1);
}

static void late_sync_case(void)
{
    start_online();
    cycle(INT64_C(61000000));
    assert(snapshot().time.state == RYZ_TIME_FAILED && time_start_calls == 1);
    /* A sync callback reports the wall clock actually applied by the platform,
     * even after our waiting window ended. No artificial attempt token gates it. */
    assert(time_sink);
    time_sink(INT64_C(1735689600), 0, s_now);
    ryz_time_snapshot_t synced = actual_time();
    assert(synced.state == RYZ_TIME_SYNCED && synced.clock_valid);
    assert(synced.last_error == ESP_OK && synced.last_sync_unix == INT64_C(1735689600));
    cycle(INT64_C(61200000));
    ryz_system_services_snapshot_t published = snapshot();
    assert(published.network.revision == 7 && published.time_valid);
    assert(published.time.state == RYZ_TIME_SYNCED && published.time.clock_valid);
    assert(published.time.last_sync_unix == INT64_C(1735689600));
    assert(time_start_calls == 1);
    assert_prerequisites(true, true);
    cycle(INT64_C(66000000));
    assert(snapshot().time.state == RYZ_TIME_SYNCED && time_start_calls == 1);
    assert(snapshot().errors[RYZ_SYSTEM_TIME_NETWORK] == ESP_OK);
    s_network.state = RYZ_PROVISIONING_OFF;
    s_network.enabled = false;
    ++s_network.revision;
    cycle(INT64_C(66200000));
    assert(snapshot().time.state == RYZ_TIME_WAITING_NETWORK);
    assert(!snapshot().time.network_ready && snapshot().time.clock_valid);
    assert(snapshot().time.last_sync_unix == INT64_C(1735689600));
    assert_prerequisites(false, true); /* Retained clock is not network access. */
}

static void invalid_sync_backoff_case(void)
{
    start_online();
    assert(time_sink);
    time_sink(INT64_C(1735689600), 0, s_now);
    cycle(INT64_C(1200000));
    assert(snapshot().time.clock_valid && time_start_calls == 1);
    assert_prerequisites(true, true);
    time_sink(0, 0, s_now);
    ryz_time_snapshot_t invalid = actual_time();
    assert(invalid.state == RYZ_TIME_FAILED && invalid.last_error == ESP_ERR_INVALID_RESPONSE);
    assert(!invalid.clock_valid && invalid.last_sync_unix == 0);
    cycle(INT64_C(1400000));
    ryz_system_services_snapshot_t failed = snapshot();
    assert(failed.time_valid && failed.time.state == RYZ_TIME_FAILED);
    assert(failed.time.last_error == ESP_ERR_INVALID_RESPONSE);
    assert(!failed.time.clock_valid && failed.time.last_sync_unix == 0);
    assert(failed.errors[RYZ_SYSTEM_TIME_NETWORK] == ESP_ERR_INVALID_RESPONSE);
    assert(time_start_calls == 1);
    assert_prerequisites(true, false);
    cycle(INT64_C(1600000));
    cycle(INT64_C(6399999));
    assert(snapshot().time.state == RYZ_TIME_FAILED && time_start_calls == 1);
    assert_prerequisites(true, false);
    cycle(INT64_C(6400000));
    assert(snapshot().time.state == RYZ_TIME_SYNCING && time_start_calls == 2);
    assert(time_started_at[1] == INT64_C(6400000));
    assert(snapshot().errors[RYZ_SYSTEM_TIME_NETWORK] == ESP_OK);
    assert(!snapshot().time.clock_valid);
    time_sink(INT64_C(1735689660), 0, s_now);
    cycle(INT64_C(6600000));
    assert(snapshot().network.revision == 7 && snapshot().time.state == RYZ_TIME_SYNCED);
    assert(snapshot().time.last_sync_unix == INT64_C(1735689660));
    assert(time_start_calls == 2);
    assert_prerequisites(true, true);
}

static void finish_owner(void)
{
    assert(s_created && time_init_calls == 1);
    assert(pthread_mutex_lock(&s_gate) == 0);
    s_stopping = true;
    assert(pthread_cond_broadcast(&s_changed) == 0);
    assert(pthread_mutex_unlock(&s_gate) == 0);
    assert(pthread_join(s_worker, NULL) == 0);
    assert(pthread_mutex_destroy(&time_mutex) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "off_boot") == 0) off_boot_case();
    else if (strcmp(argv[1], "timeout_retry") == 0) timeout_retry_case();
    else if (strcmp(argv[1], "offline_backoff") == 0) offline_backoff_case();
    else if (strcmp(argv[1], "late_sync") == 0) late_sync_case();
    else if (strcmp(argv[1], "invalid_sync_backoff") == 0) invalid_sync_backoff_case();
    else abort();
    finish_owner();
    printf("SYSTEM_TIME_INTEGRATION_PASS %s\n", argv[1]);
    return 0;
}
