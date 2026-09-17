#include "workbench_boot_key.h"

#include <assert.h>
#include <stdio.h>

static void test_threshold_and_rearm(void)
{
    ryz_workbench_boot_key_t state;
    ryz_workbench_boot_key_reset(&state);
    assert(!ryz_workbench_boot_key_update(&state, true, 0));
    assert(!ryz_workbench_boot_key_update(&state, true, 39));
    assert(!ryz_workbench_boot_key_update(&state, true, 40));
    assert(!ryz_workbench_boot_key_update(&state, true, 5039));
    assert(ryz_workbench_boot_key_update(&state, true, 5040));
    assert(!ryz_workbench_boot_key_update(&state, true, 10000));
    assert(!ryz_workbench_boot_key_update(&state, false, 10000));
    assert(!ryz_workbench_boot_key_update(&state, false, 10039));
    assert(!ryz_workbench_boot_key_update(&state, false, 10040));
    assert(!ryz_workbench_boot_key_update(&state, true, 11000));
    assert(!ryz_workbench_boot_key_update(&state, true, 11040));
    assert(!ryz_workbench_boot_key_update(&state, true, 16039));
    assert(ryz_workbench_boot_key_update(&state, true, 16040));
}

static void test_wraparound(void)
{
    ryz_workbench_boot_key_t state;
    ryz_workbench_boot_key_reset(&state);
    const uint32_t start = UINT32_MAX - 20U;
    assert(!ryz_workbench_boot_key_update(&state, true, start));
    assert(!ryz_workbench_boot_key_update(&state, true, start + 40U));
    assert(!ryz_workbench_boot_key_update(&state, true, start + 40U + 4999U));
    assert(ryz_workbench_boot_key_update(&state, true, start + 40U + 5000U));
}

int main(void)
{
    test_threshold_and_rearm();
    test_wraparound();
    puts("WORKBENCH_BOOT_KEY_PASS threshold/rearm/wraparound");
    return 0;
}
