#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "fake_platform.h"
#include "ryz_time.h"

static const int64_t epoch = RYZ_TIME_MIN_VALID_UNIX_SECONDS;

static ryz_time_snapshot_t snapshot(void)
{
    ryz_time_snapshot_t value;
    assert(ryz_time_get_snapshot(&value) == ESP_OK);
    return value;
}

static void expect_time(int64_t seconds)
{
    ryz_time_utc_sample_t sample = {.valid = false, .unix_seconds = -1};
    ryz_time_snapshot_t before = snapshot();
    unsigned starts = fake_time_platform_start_count();
    assert(ryz_time_sample_utc(&sample) == ESP_OK);
    assert(sample.valid && sample.unix_seconds == seconds);
    ryz_time_snapshot_t after = snapshot();
    assert(memcmp(&before, &after, sizeof(before)) == 0);
    assert(fake_time_platform_start_count() == starts);
}

static void expect_unknown(esp_err_t error)
{
    ryz_time_utc_sample_t sample = {.valid = true, .unix_seconds = INT64_MAX};
    unsigned starts = fake_time_platform_start_count();
    assert(ryz_time_sample_utc(&sample) == error);
    assert(!sample.valid && sample.unix_seconds == 0);
    assert(fake_time_platform_start_count() == starts);
}

static void calibrate(int64_t seconds, uint32_t usec, int64_t mono)
{
    fake_time_platform_set_now_us(mono);
    fake_time_platform_emit_precise_sync(seconds, usec, mono);
}

static void precision_offline_timeout(void)
{
    calibrate(epoch, 999999, 1000000);
    expect_time(epoch);
    fake_time_platform_set_now_us(1000001);
    expect_time(epoch + 1);
    fake_time_platform_set_now_us(2000001);
    expect_time(epoch + 2);
    assert(snapshot().last_sync_unix == epoch);
    assert(ryz_time_set_network_ready(false) == ESP_OK);
    fake_time_platform_set_now_us(5000000);
    expect_time(epoch + 4);
    assert(ryz_time_set_network_ready(true) == ESP_OK);
    fake_time_platform_set_now_us(65000000);
    assert(ryz_time_set_network_ready(true) == ESP_ERR_TIMEOUT);
    assert(snapshot().state == RYZ_TIME_FAILED && snapshot().clock_valid);
    expect_time(epoch + 64);
    assert(ryz_time_set_network_ready(false) == ESP_OK);
    expect_time(epoch + 64);
}

static void invalid_observations(void)
{
    const struct { int64_t seconds; uint32_t usec; int64_t mono; } invalid[] = {
        {0, 0, 1000000},
        {RYZ_TIME_MIN_VALID_UNIX_SECONDS - 1, 0, 1000000},
        {INT64_MIN, 0, 1000000},
        {RYZ_TIME_MIN_VALID_UNIX_SECONDS, 1000000, 1000000},
        {RYZ_TIME_MIN_VALID_UNIX_SECONDS, UINT32_MAX, 1000000},
        {RYZ_TIME_MIN_VALID_UNIX_SECONDS, 0, -1},
        {RYZ_TIME_MIN_VALID_UNIX_SECONDS, 0, 1000001},
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        calibrate(epoch, 0, 1000000);
        expect_time(epoch);
        fake_time_platform_emit_precise_sync(invalid[i].seconds, invalid[i].usec, invalid[i].mono);
        assert(!snapshot().clock_valid && snapshot().last_sync_unix == 0);
        expect_unknown(ESP_OK);
        fake_time_platform_set_now_us(2000000);
        expect_unknown(ESP_OK);
    }
    calibrate(epoch + 1, 0, 2000000);
    expect_time(epoch + 1);
}

static void monotonic_regression_and_overflow(void)
{
    calibrate(epoch + 100, 0, 10000000);
    fake_time_platform_set_now_us(20000000);
    expect_time(epoch + 110);
    /* Still newer than the calibration anchor, but older than a prior read. */
    fake_time_platform_set_now_us(19000000);
    ryz_time_snapshot_t before = snapshot();
    expect_unknown(ESP_OK);
    ryz_time_snapshot_t after = snapshot();
    assert(memcmp(&before, &after, sizeof(before)) == 0);
    fake_time_platform_set_now_us(30000000);
    expect_unknown(ESP_OK);
    calibrate(epoch + 200, 0, 30000000);
    expect_time(epoch + 200);
    fake_time_platform_set_now_us(-1);
    expect_unknown(ESP_OK);
    fake_time_platform_set_now_us(40000000);
    expect_unknown(ESP_OK);

    calibrate(INT64_MAX - 1, 999999, 0);
    expect_time(INT64_MAX - 1);
    fake_time_platform_set_now_us(1);
    expect_time(INT64_MAX);
    fake_time_platform_set_now_us(1000001);
    expect_unknown(ESP_OK);
    fake_time_platform_set_now_us(1000000);
    expect_unknown(ESP_OK);
    calibrate(epoch, 999999, 0);
    fake_time_platform_set_now_us(INT64_MAX);
    expect_time(INT64_C(9225076104055));
    calibrate(epoch, 0, INT64_MAX);
    expect_time(epoch);
    fake_time_platform_set_now_us(INT64_MAX - 1);
    expect_unknown(ESP_OK);
    /* A fresh valid correction may move UTC backwards; that is not a
     * regression of the independent monotonic clock. */
    calibrate(epoch, 0, 100000000);
    expect_time(epoch);
}

static void *busy_reader(void *unused)
{
    (void)unused;
    expect_unknown(ESP_ERR_TIMEOUT);
    return NULL;
}

static void mutex_busy_is_nonwaiting(void)
{
    pthread_t reader;
    fake_time_platform_hold_snapshot();
    assert(pthread_create(&reader, NULL, busy_reader, NULL) == 0);
    /* Join while still holding the real mutex: any waiting sample would hit
     * the Host watchdog instead of being mistaken for a nonblocking success. */
    assert(pthread_join(reader, NULL) == 0);
    fake_time_platform_release_snapshot();
    expect_time(epoch);
}

static atomic_bool callbacks_done;

static void *sync_writer(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < 5000; ++i) {
        fake_time_platform_emit_precise_sync(epoch, 900000, 100000000);
        fake_time_platform_emit_precise_sync(epoch + 1000, 100000, 99000000);
    }
    atomic_store(&callbacks_done, true);
    return NULL;
}

static void coherent_callback_read_interleaving(void)
{
    calibrate(epoch, 900000, 100000000);
    unsigned starts = fake_time_platform_start_count();
    pthread_t writer;
    assert(pthread_create(&writer, NULL, sync_writer, NULL) == 0);
    unsigned attempts = 0;
    do {
        ryz_time_utc_sample_t sample = {.valid = true, .unix_seconds = -1};
        esp_err_t error = ryz_time_sample_utc(&sample);
        if (error == ESP_ERR_TIMEOUT) assert(!sample.valid && sample.unix_seconds == 0);
        else {
            assert(error == ESP_OK && sample.valid);
            /* Mixed seconds/anchor pairs would produce epoch+1 or epoch+1000. */
            assert(sample.unix_seconds == epoch || sample.unix_seconds == epoch + 1001);
        }
        ++attempts;
    } while (!atomic_load(&callbacks_done) || attempts < 10000);
    assert(pthread_join(writer, NULL) == 0);
    assert(fake_time_platform_start_count() == starts);
    expect_time(epoch + 1001);
}

int main(void)
{
    fake_time_platform_reset();
    assert(ryz_time_sample_utc(NULL) == ESP_ERR_INVALID_ARG);
    expect_unknown(ESP_ERR_INVALID_STATE);
    assert(ryz_time_init() == ESP_OK);
    expect_unknown(ESP_OK);
    precision_offline_timeout();
    invalid_observations();
    monotonic_regression_and_overflow();
    mutex_busy_is_nonwaiting();
    coherent_callback_read_interleaving();
    puts("RYZ_TIME_UTC_PASS");
    return 0;
}
