#include <assert.h>
#include <stdio.h>

#include "fake_platform.h"
#include "ryz_time.h"

int main(void)
{
    fake_time_platform_reset();
    ryz_time_snapshot_t snapshot = {0};
    assert(ryz_time_get_snapshot(&snapshot) == ESP_ERR_INVALID_STATE);
    assert(ryz_time_get_snapshot(NULL) == ESP_ERR_INVALID_ARG);

    assert(ryz_time_init() == ESP_OK);
    assert(ryz_time_init() == ESP_OK);
    assert(fake_time_platform_init_count() == 1);
    assert(ryz_time_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RYZ_TIME_WAITING_NETWORK);
    assert(!snapshot.network_ready);
    assert(!snapshot.clock_valid);

    assert(ryz_time_set_network_ready(true) == ESP_OK);
    assert(fake_time_platform_start_count() == 1);
    assert(ryz_time_set_network_ready(true) == ESP_OK);
    assert(fake_time_platform_start_count() == 1);
    assert(ryz_time_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RYZ_TIME_SYNCING);
    assert(snapshot.network_ready);

    fake_time_platform_emit_sync(RYZ_TIME_MIN_VALID_UNIX_SECONDS);
    assert(ryz_time_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RYZ_TIME_SYNCED);
    assert(snapshot.clock_valid);
    assert(snapshot.last_sync_unix == RYZ_TIME_MIN_VALID_UNIX_SECONDS);

    assert(ryz_time_set_network_ready(false) == ESP_OK);
    assert(ryz_time_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RYZ_TIME_WAITING_NETWORK);
    assert(!snapshot.network_ready);
    assert(snapshot.clock_valid);

    assert(ryz_time_set_network_ready(true) == ESP_OK);
    assert(fake_time_platform_start_count() == 2);
    fake_time_platform_emit_sync(RYZ_TIME_MIN_VALID_UNIX_SECONDS + 60);
    assert(ryz_time_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.last_sync_unix == RYZ_TIME_MIN_VALID_UNIX_SECONDS + 60);

    assert(ryz_time_set_network_ready(false) == ESP_OK);
    fake_time_platform_set_start_result(ESP_FAIL);
    assert(ryz_time_set_network_ready(true) == ESP_FAIL);
    assert(ryz_time_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RYZ_TIME_FAILED);
    assert(snapshot.last_error == ESP_FAIL);
    assert(snapshot.clock_valid);

    fake_time_platform_set_start_result(ESP_OK);
    assert(ryz_time_set_network_ready(true) == ESP_OK);
    assert(fake_time_platform_start_count() == 4);
    assert(ryz_time_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RYZ_TIME_SYNCING);
    assert(snapshot.network_ready);

    assert(ryz_time_set_network_ready(false) == ESP_OK);
    fake_time_platform_emit_sync(RYZ_TIME_MIN_VALID_UNIX_SECONDS - 1);
    assert(ryz_time_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RYZ_TIME_FAILED);
    assert(snapshot.last_error == ESP_ERR_INVALID_RESPONSE);
    assert(!snapshot.clock_valid);
    assert(snapshot.last_sync_unix == 0);

    puts("RYZ_TIME_STATE_PASS");
    return 0;
}
