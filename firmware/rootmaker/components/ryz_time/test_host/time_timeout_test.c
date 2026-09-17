#include <assert.h>
#include <stdio.h>

#include "fake_platform.h"
#include "ryz_time.h"

int main(void)
{
    fake_time_platform_reset();
    assert(ryz_time_init() == ESP_OK);
    assert(ryz_time_set_network_ready(true) == ESP_OK);
    assert(fake_time_platform_start_count() == 1);
    ryz_time_snapshot_t snapshot;
    assert(ryz_time_get_snapshot(&snapshot) == ESP_OK);
    uint32_t syncing_revision = snapshot.revision;

    fake_time_platform_set_now_us(INT64_C(59000000));
    assert(ryz_time_set_network_ready(true) == ESP_OK);
    fake_time_platform_set_now_us(INT64_C(59999999));
    assert(ryz_time_set_network_ready(true) == ESP_OK);
    assert(fake_time_platform_start_count() == 1);

    /* A successful asynchronous start is not an NTP response. The existing
     * system owner republishes network readiness on its regular cycle. */
    fake_time_platform_set_now_us(INT64_C(60000000));
    /* Merely reading, even at the deadline, must not consume the timeout
     * which the system owner needs in order to establish retry backoff. */
    assert(ryz_time_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RYZ_TIME_SYNCING);
    assert(snapshot.revision == syncing_revision);
    assert(ryz_time_set_network_ready(true) == ESP_ERR_TIMEOUT);
    assert(ryz_time_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RYZ_TIME_FAILED);
    assert(snapshot.last_error == ESP_ERR_TIMEOUT);
    assert(!snapshot.clock_valid && snapshot.last_sync_unix == 0);
    assert(snapshot.revision == syncing_revision + 1);
    assert(fake_time_platform_start_count() == 1);
    puts("RYZ_TIME_TIMEOUT_PASS");
    return 0;
}
