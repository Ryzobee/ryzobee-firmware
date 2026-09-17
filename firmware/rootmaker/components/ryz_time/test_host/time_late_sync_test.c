#include <assert.h>
#include <stdio.h>

#include "fake_platform.h"
#include "ryz_time.h"

static ryz_time_snapshot_t snapshot(void)
{
    ryz_time_snapshot_t value;
    assert(ryz_time_get_snapshot(&value) == ESP_OK);
    return value;
}

int main(void)
{
    fake_time_platform_reset();
    assert(ryz_time_init() == ESP_OK);
    assert(ryz_time_set_network_ready(true) == ESP_OK);
    fake_time_platform_set_now_us(INT64_C(60000000));
    assert(ryz_time_set_network_ready(true) == ESP_ERR_TIMEOUT);

    /* Timeout first, then the SDK reports its new global clock. */
    fake_time_platform_emit_sync(RYZ_TIME_MIN_VALID_UNIX_SECONDS);
    assert(snapshot().state == RYZ_TIME_SYNCED && snapshot().clock_valid);
    assert(ryz_time_set_network_ready(true) == ESP_OK);
    assert(fake_time_platform_start_count() == 1);

    fake_time_platform_set_now_us(INT64_C(70000000));
    assert(ryz_time_set_network_ready(false) == ESP_OK);
    assert(snapshot().clock_valid);
    fake_time_platform_set_now_us(INT64_C(80000000));
    assert(ryz_time_set_network_ready(true) == ESP_OK);
    fake_time_platform_set_now_us(INT64_C(139999999));
    assert(ryz_time_set_network_ready(true) == ESP_OK);
    fake_time_platform_set_now_us(INT64_C(140000000));
    assert(ryz_time_set_network_ready(true) == ESP_ERR_TIMEOUT);
    assert(snapshot().state == RYZ_TIME_FAILED && snapshot().clock_valid);
    assert(snapshot().last_sync_unix == RYZ_TIME_MIN_VALID_UNIX_SECONDS);

    assert(ryz_time_set_network_ready(false) == ESP_OK);
    fake_time_platform_emit_sync(RYZ_TIME_MIN_VALID_UNIX_SECONDS + 60);
    assert(snapshot().state == RYZ_TIME_WAITING_NETWORK);
    assert(!snapshot().network_ready && snapshot().clock_valid);
    assert(snapshot().last_sync_unix == RYZ_TIME_MIN_VALID_UNIX_SECONDS + 60);

    fake_time_platform_set_now_us(INT64_C(149000000));
    assert(ryz_time_set_network_ready(true) == ESP_OK);
    fake_time_platform_set_now_us(INT64_C(209000000));
    /* Callback first, then a deadline observation must not undo the sync. */
    fake_time_platform_emit_sync(RYZ_TIME_MIN_VALID_UNIX_SECONDS + 120);
    assert(ryz_time_set_network_ready(true) == ESP_OK);
    assert(snapshot().state == RYZ_TIME_SYNCED && fake_time_platform_start_count() == 3);

    fake_time_platform_emit_sync(RYZ_TIME_MIN_VALID_UNIX_SECONDS - 1);
    assert(!snapshot().clock_valid && snapshot().last_sync_unix == 0);
    assert(ryz_time_set_network_ready(true) == ESP_ERR_INVALID_RESPONSE);
    assert(fake_time_platform_start_count() == 3);
    /* The caller, not a reader or hidden timer, decides when to retry. */
    assert(ryz_time_set_network_ready(true) == ESP_OK);
    assert(snapshot().state == RYZ_TIME_SYNCING && fake_time_platform_start_count() == 4);

    assert(ryz_time_set_network_ready(false) == ESP_OK);
    fake_time_platform_set_now_us(INT64_MAX - INT64_C(60000000));
    assert(ryz_time_set_network_ready(true) == ESP_OK);
    fake_time_platform_set_now_us(INT64_MAX - 1);
    assert(ryz_time_set_network_ready(true) == ESP_OK);
    fake_time_platform_set_now_us(INT64_MAX);
    assert(ryz_time_set_network_ready(true) == ESP_ERR_TIMEOUT);
    assert(fake_time_platform_start_count() == 5);
    puts("RYZ_TIME_LATE_SYNC_PASS");
    return 0;
}
