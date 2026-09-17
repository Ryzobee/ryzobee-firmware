#include <assert.h>
#include <stdio.h>

#include "fake_platform.h"
#include "ryz_time.h"

int main(void)
{
    fake_time_platform_reset();
    assert(ryz_time_init() == ESP_OK);
    fake_time_platform_set_start_result(ESP_FAIL);
    fake_time_platform_set_start_sync(true, RYZ_TIME_MIN_VALID_UNIX_SECONDS);
    assert(ryz_time_set_network_ready(true) == ESP_OK);
    ryz_time_snapshot_t snapshot;
    assert(ryz_time_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RYZ_TIME_SYNCED && snapshot.clock_valid);
    assert(snapshot.last_error == ESP_OK);
    assert(snapshot.last_sync_unix == RYZ_TIME_MIN_VALID_UNIX_SECONDS);

    assert(ryz_time_set_network_ready(false) == ESP_OK);
    fake_time_platform_set_start_sync(true, RYZ_TIME_MIN_VALID_UNIX_SECONDS - 1);
    assert(ryz_time_set_network_ready(true) == ESP_ERR_INVALID_RESPONSE);
    assert(ryz_time_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RYZ_TIME_FAILED && !snapshot.clock_valid);
    assert(snapshot.last_error == ESP_ERR_INVALID_RESPONSE);
    assert(fake_time_platform_start_count() == 2);
    /* The invalid callback was already returned above; do not report it a
     * second time instead of making the owner's deliberate retry. */
    fake_time_platform_set_start_result(ESP_OK);
    assert(ryz_time_set_network_ready(true) == ESP_OK);
    assert(fake_time_platform_start_count() == 3);

    assert(ryz_time_set_network_ready(false) == ESP_OK);
    fake_time_platform_set_start_sync(true, RYZ_TIME_MIN_VALID_UNIX_SECONDS - 1);
    assert(ryz_time_set_network_ready(true) == ESP_OK);
    /* If start succeeded, the asynchronous invalid clock still needs one
     * report so the owner can wait before its next start. */
    assert(ryz_time_set_network_ready(true) == ESP_ERR_INVALID_RESPONSE);
    assert(fake_time_platform_start_count() == 4);
    assert(ryz_time_set_network_ready(true) == ESP_OK);
    assert(fake_time_platform_start_count() == 5);
    puts("RYZ_TIME_START_CALLBACK_PASS");
    return 0;
}
