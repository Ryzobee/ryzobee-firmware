#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "ryz_net_traffic.h"

/* Only the IDF/RTOS boundary is replaced. All binding, epoch and counter
 * behavior comes from the production translation unit. */
struct esp_netif_obj { unsigned fixture_id; };
static esp_netif_t sta = {1};
static esp_netif_t other_netif = {2};
static const uint8_t bssid_a[6] = {2, 3, 4, 5, 6, 7};
static const uint8_t bssid_b[6] = {2, 3, 4, 5, 6, 8};
static _Thread_local unsigned critical_depth;
static _Thread_local unsigned real_calls;
static _Thread_local esp_err_t real_result;
static _Thread_local void (*real_hook)(void);
static _Thread_local bool real_frees_buffers;
enum { TX, TX_WRAP, RX };
static _Thread_local struct {
    int kind;
    esp_netif_t *netif;
    uintptr_t data, extra;
    size_t length;
} last_real;

void net_traffic_test_enter(portMUX_TYPE *mux)
{
    assert(!critical_depth);
    assert(pthread_mutex_lock(mux) == 0);
    ++critical_depth;
}
void net_traffic_test_exit(portMUX_TYPE *mux)
{
    assert(critical_depth == 1);
    --critical_depth;
    assert(pthread_mutex_unlock(mux) == 0);
}

static esp_err_t real_io(int kind, esp_netif_t *netif, void *data,
                         size_t length, void *extra)
{
    assert(!critical_depth);
    last_real.kind = kind;
    last_real.netif = netif;
    last_real.data = (uintptr_t)data;
    last_real.extra = (uintptr_t)extra;
    last_real.length = length;
    ++real_calls;
    if (real_hook) real_hook();
    if (real_frees_buffers) {
        free(data);
        free(extra);
    }
    return real_result;
}
esp_err_t __real_esp_netif_transmit(esp_netif_t *n, void *d, size_t l)
{ return real_io(TX, n, d, l, NULL); }
esp_err_t __real_esp_netif_transmit_wrap(esp_netif_t *n, void *d, size_t l, void *p)
{ return real_io(TX_WRAP, n, d, l, p); }
esp_err_t __real_esp_netif_receive(esp_netif_t *n, void *d, size_t l, void *b)
{ return real_io(RX, n, d, l, b); }

esp_err_t __wrap_esp_netif_transmit(esp_netif_t *, void *, size_t);
esp_err_t __wrap_esp_netif_transmit_wrap(esp_netif_t *, void *, size_t, void *);
esp_err_t __wrap_esp_netif_receive(esp_netif_t *, void *, size_t, void *);

static esp_err_t io(int kind, esp_netif_t *netif, void *data,
                    size_t length, void *extra)
{
    if (kind == TX) return __wrap_esp_netif_transmit(netif, data, length);
    if (kind == TX_WRAP) return __wrap_esp_netif_transmit_wrap(netif, data, length, extra);
    assert(kind == RX);
    return __wrap_esp_netif_receive(netif, data, length, extra);
}

static ryz_net_traffic_snapshot_t observe(esp_netif_t *netif, const uint8_t *bssid)
{
    ryz_net_traffic_snapshot_t value;
    assert(ryz_net_traffic_observe(netif, bssid, &value) == ESP_OK && value.valid);
    return value;
}

static void assert_cleared(const ryz_net_traffic_snapshot_t *out)
{
    assert(!out->valid && !out->epoch && !out->rx_bytes && !out->tx_bytes);
}

static void test_first_observation_and_totals(void)
{
    ryz_net_traffic_snapshot_t first, current;
    assert(ryz_net_traffic_observe(&sta, bssid_a, &first) == ESP_OK);
    assert(first.valid && first.epoch == 1 && !first.rx_bytes && !first.tx_bytes);
    char data[] = "untouched";
    int owner_buffer = 9;
    assert(__wrap_esp_netif_transmit(&sta, data, 41) == ESP_OK);
    assert(__wrap_esp_netif_transmit_wrap(&sta, data, 59, &owner_buffer) == ESP_OK);
    assert(__wrap_esp_netif_receive(&sta, data, 77, &owner_buffer) == ESP_OK);
    assert(ryz_net_traffic_observe(&sta, bssid_a, &current) == ESP_OK);
    assert(current.valid && current.epoch == first.epoch);
    assert(current.tx_bytes == 100 && current.rx_bytes == 77 && real_calls == 3);
    assert(!strcmp(data, "untouched") && owner_buffer == 9);
}

static void test_transparent_unbound_other_and_failures(void)
{
    unsigned expected_calls = 0;
    char bytes[9] = "sentinel";
    int extra = 7;
    /* Not bound, AP/other netif and even invalid arguments are never consumed
     * or rejected by the telemetry wrapper; only the actual driver decides. */
    for (int kind = TX; kind <= RX; ++kind) {
        assert(io(kind, &sta, bytes, 9, &extra) == ESP_OK);
        ++expected_calls;
    }
    ryz_net_traffic_snapshot_t initial = observe(&sta, bssid_a);
    assert(!initial.rx_bytes && !initial.tx_bytes);
    const esp_err_t errors[] = {ESP_FAIL, ESP_ERR_TIMEOUT, ESP_ERR_INVALID_ARG, 0x7788};
    for (int kind = TX; kind <= RX; ++kind) {
        real_result = ESP_OK;
        assert(io(kind, &other_netif, bytes, 91, &extra) == ESP_OK);
        ++expected_calls;
        for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
            real_result = errors[i];
            assert(io(kind, &sta, bytes, 79, &extra) == errors[i]);
            assert(last_real.kind == kind && last_real.netif == &sta);
            assert(last_real.data == (uintptr_t)bytes && last_real.length == 79);
            assert(last_real.extra == (kind == TX ? 0 : (uintptr_t)&extra));
            ++expected_calls;
        }
        assert(io(kind, NULL, NULL, 0, NULL) == 0x7788);
        ++expected_calls;
        assert(!last_real.netif && !last_real.data && !last_real.length && !last_real.extra);
    }
    ryz_net_traffic_snapshot_t current = observe(&sta, bssid_a);
    assert(current.epoch == initial.epoch && !current.rx_bytes && !current.tx_bytes);
    assert(real_calls == expected_calls && extra == 7 && !strcmp(bytes, "sentinel"));
}

static void test_invalid_arguments_do_not_rebind(void)
{
    ryz_net_traffic_snapshot_t initial = observe(&sta, bssid_a), out;
    assert(io(TX, &sta, NULL, 29, NULL) == ESP_OK);
    memset(&out, 0xA5, sizeof(out));
    assert(ryz_net_traffic_observe(NULL, bssid_a, &out) == ESP_ERR_INVALID_ARG);
    assert_cleared(&out);
    memset(&out, 0xA5, sizeof(out));
    assert(ryz_net_traffic_observe(&sta, NULL, &out) == ESP_ERR_INVALID_ARG);
    assert_cleared(&out);
    assert(ryz_net_traffic_observe(&other_netif, bssid_b, NULL) == ESP_ERR_INVALID_ARG);
    out = observe(&sta, bssid_a);
    assert(out.epoch == initial.epoch && out.tx_bytes == 29);
}

static void test_identity_changes_and_revoke(void)
{
    ryz_net_traffic_revoke();
    ryz_net_traffic_revoke();
    ryz_net_traffic_snapshot_t current = observe(&sta, bssid_a);
    assert(current.epoch == 1);
    assert(io(RX, &sta, NULL, 12, NULL) == ESP_OK);
    current = observe(&sta, bssid_b);
    assert(current.epoch == 2 && !current.rx_bytes && !current.tx_bytes);
    assert(io(TX, &sta, NULL, 30, NULL) == ESP_OK);
    current = observe(&other_netif, bssid_b);
    assert(current.epoch == 3 && !current.rx_bytes && !current.tx_bytes);
    assert(io(TX, &sta, NULL, 99, NULL) == ESP_OK);
    assert(observe(&other_netif, bssid_b).tx_bytes == 0);
    ryz_net_traffic_revoke();
    assert(io(RX, &other_netif, NULL, 120, NULL) == ESP_OK);
    current = observe(&other_netif, bssid_b);
    assert(current.epoch == 4 && !current.rx_bytes && !current.tx_bytes);
}

static pthread_mutex_t driver_gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t driver_changed = PTHREAD_COND_INITIALIZER;
static bool driver_entered, driver_release;
static void blocked_driver(void)
{
    assert(pthread_mutex_lock(&driver_gate) == 0);
    driver_entered = true;
    assert(pthread_cond_broadcast(&driver_changed) == 0);
    while (!driver_release) assert(pthread_cond_wait(&driver_changed, &driver_gate) == 0);
    assert(pthread_mutex_unlock(&driver_gate) == 0);
}
static void *blocked_io(void *argument)
{
    int kind = *(const int *)argument;
    real_hook = blocked_driver;
    assert(io(kind, &sta, NULL, 987, NULL) == ESP_OK);
    assert(real_calls == 1);
    return NULL;
}

static void test_inflight_old_epoch_is_not_credited(void)
{
    for (int kind = TX; kind <= RX; ++kind) {
        for (int same_identity = 0; same_identity <= 1; ++same_identity) {
            ryz_net_traffic_revoke();
            ryz_net_traffic_snapshot_t before = observe(&sta, bssid_a);
            assert(pthread_mutex_lock(&driver_gate) == 0);
            driver_entered = false;
            driver_release = false;
            assert(pthread_mutex_unlock(&driver_gate) == 0);
            pthread_t worker;
            assert(pthread_create(&worker, NULL, blocked_io, &kind) == 0);
            assert(pthread_mutex_lock(&driver_gate) == 0);
            while (!driver_entered) assert(pthread_cond_wait(&driver_changed, &driver_gate) == 0);
            assert(pthread_mutex_unlock(&driver_gate) == 0);

            const uint8_t *next_bssid = same_identity ? bssid_a : bssid_b;
            if (same_identity) ryz_net_traffic_revoke();
            ryz_net_traffic_snapshot_t next = observe(&sta, next_bssid);
            assert(next.epoch > before.epoch && !next.tx_bytes && !next.rx_bytes);
            /* The real SDK call is still blocked: no module lock spans it. */
            assert(io(TX, &sta, NULL, 17, NULL) == ESP_OK);
            assert(io(RX, &sta, NULL, 19, NULL) == ESP_OK);
            assert(pthread_mutex_lock(&driver_gate) == 0);
            driver_release = true;
            assert(pthread_cond_broadcast(&driver_changed) == 0);
            assert(pthread_mutex_unlock(&driver_gate) == 0);
            assert(pthread_join(worker, NULL) == 0);
            next = observe(&sta, next_bssid);
            assert(next.tx_bytes == 17 && next.rx_bytes == 19);
        }
    }
}

static void rebind_during_real_call(void)
{
    ryz_net_traffic_revoke();
    (void)observe(&sta, bssid_a);
}
static void test_synchronous_reentry_and_consumed_buffers(void)
{
    ryz_net_traffic_snapshot_t initial = observe(&sta, bssid_a);
    real_hook = rebind_during_real_call;
    assert(io(TX_WRAP, &sta, NULL, 900, NULL) == ESP_OK);
    real_hook = NULL;
    ryz_net_traffic_snapshot_t current = observe(&sta, bssid_a);
    assert(current.epoch > initial.epoch && !current.tx_bytes);
    /* RX may consume both buffers inside __real. ASan ensures no late read. */
    void *payload = malloc(23), *driver_buffer = malloc(1);
    assert(payload && driver_buffer);
    real_frees_buffers = true;
    assert(io(RX, &sta, payload, 23, driver_buffer) == ESP_OK);
    real_frees_buffers = false;
    assert(observe(&sta, bssid_a).rx_bytes == 23);
}

static void *count_packets(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < 5000; ++i) {
        assert(io(i % 2 ? TX : TX_WRAP, &sta, NULL, 3, NULL) == ESP_OK);
        assert(io(RX, &sta, NULL, 5, NULL) == ESP_OK);
    }
    assert(real_calls == 10000);
    return NULL;
}
static void *observe_packets(void *unused)
{
    (void)unused;
    ryz_net_traffic_snapshot_t previous = observe(&sta, bssid_a);
    for (unsigned i = 0; i < 25000; ++i) {
        ryz_net_traffic_snapshot_t current = observe(&sta, bssid_a);
        assert(current.epoch == previous.epoch);
        assert(current.rx_bytes >= previous.rx_bytes && current.tx_bytes >= previous.tx_bytes);
        previous = current;
    }
    return NULL;
}
static void test_concurrent_counters_and_snapshots(void)
{
    (void)observe(&sta, bssid_a);
    pthread_t workers[4], reader;
    assert(pthread_create(&reader, NULL, observe_packets, NULL) == 0);
    for (unsigned i = 0; i < 4; ++i)
        assert(pthread_create(&workers[i], NULL, count_packets, NULL) == 0);
    for (unsigned i = 0; i < 4; ++i) assert(pthread_join(workers[i], NULL) == 0);
    assert(pthread_join(reader, NULL) == 0);
    ryz_net_traffic_snapshot_t current = observe(&sta, bssid_a);
    assert(current.tx_bytes == 60000 && current.rx_bytes == 100000);
}

static void test_counter_overflow_fails_closed(int kind)
{
    /* macOS SDK boundary accepts a synthetic huge frame length. This reaches
     * production uint64 arithmetic without a backdoor to private counters. */
    _Static_assert(SIZE_MAX == UINT64_MAX, "overflow test requires a 64-bit Host");
    ryz_net_traffic_snapshot_t initial = observe(&sta, bssid_a), out;
    assert(io(kind, &sta, NULL, SIZE_MAX - 1, NULL) == ESP_OK);
    assert(io(kind, &sta, NULL, 1, NULL) == ESP_OK);
    out = observe(&sta, bssid_a);
    assert((kind == RX ? out.rx_bytes : out.tx_bytes) == UINT64_MAX);
    assert(io(kind, &sta, NULL, 1, NULL) == ESP_OK); /* Original result unchanged. */
    memset(&out, 0xA5, sizeof(out));
    assert(ryz_net_traffic_observe(&sta, bssid_a, &out) == ESP_ERR_INVALID_SIZE);
    assert_cleared(&out);
    assert(io(kind, &sta, NULL, 1, NULL) == ESP_OK);
    assert(ryz_net_traffic_observe(&other_netif, bssid_b, &out) == ESP_ERR_INVALID_SIZE);
    assert_cleared(&out);
    assert(real_calls == 4);
    ryz_net_traffic_revoke();
    out = observe(&sta, bssid_a);
    assert(out.epoch > initial.epoch && !out.tx_bytes && !out.rx_bytes);
    assert(io(TX_WRAP, &sta, NULL, 10, NULL) == ESP_OK);
    assert(observe(&sta, bssid_a).tx_bytes == 10);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "totals")) test_first_observation_and_totals();
    else if (!strcmp(argv[1], "transparent")) test_transparent_unbound_other_and_failures();
    else if (!strcmp(argv[1], "arguments")) test_invalid_arguments_do_not_rebind();
    else if (!strcmp(argv[1], "identity")) test_identity_changes_and_revoke();
    else if (!strcmp(argv[1], "inflight")) test_inflight_old_epoch_is_not_credited();
    else if (!strcmp(argv[1], "reentry")) test_synchronous_reentry_and_consumed_buffers();
    else if (!strcmp(argv[1], "concurrent")) test_concurrent_counters_and_snapshots();
    else if (!strcmp(argv[1], "overflow_tx")) test_counter_overflow_fails_closed(TX_WRAP);
    else if (!strcmp(argv[1], "overflow_rx")) test_counter_overflow_fails_closed(RX);
    else assert(!"unknown test case");
    printf("NET_TRAFFIC_PASS %s\n", argv[1]);
    return 0;
}
