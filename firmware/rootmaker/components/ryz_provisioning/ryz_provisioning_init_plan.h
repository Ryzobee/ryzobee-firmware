#pragma once

#include <stddef.h>
#include <stdint.h>

/** Resource order is acquisition order; rollback walks it in reverse. */
typedef enum {
    RYZ_PROVISIONING_INIT_EVENT_LOOP = 0,
    RYZ_PROVISIONING_INIT_AP_NETIF,
    RYZ_PROVISIONING_INIT_STA_NETIF,
    RYZ_PROVISIONING_INIT_WIFI,
    RYZ_PROVISIONING_INIT_NETWORK_MUTEX,
    RYZ_PROVISIONING_INIT_CONNECT_QUEUE,
    RYZ_PROVISIONING_INIT_CONNECT_TASK,
    RYZ_PROVISIONING_INIT_WIFI_HANDLER,
    RYZ_PROVISIONING_INIT_IP_HANDLER,
    RYZ_PROVISIONING_INIT_RESOURCE_COUNT,
} ryz_provisioning_init_resource_t;

typedef struct {
    uint32_t owned;
} ryz_provisioning_init_plan_t;

static inline void ryz_provisioning_init_acquire(
    ryz_provisioning_init_plan_t *plan,
    ryz_provisioning_init_resource_t resource)
{
    if (plan == NULL || resource < 0 ||
        resource >= RYZ_PROVISIONING_INIT_RESOURCE_COUNT) {
        return;
    }
    plan->owned |= 1U << (uint32_t)resource;
}

static inline size_t ryz_provisioning_init_take_rollback_plan(
    ryz_provisioning_init_plan_t *plan,
    ryz_provisioning_init_resource_t *out_resources,
    size_t capacity)
{
    if (plan == NULL || out_resources == NULL) {
        return 0;
    }

    size_t count = 0;
    for (int resource = RYZ_PROVISIONING_INIT_RESOURCE_COUNT - 1;
         resource >= 0;
         --resource) {
        if ((plan->owned & (1U << (uint32_t)resource)) != 0U &&
            count < capacity) {
            out_resources[count++] =
                (ryz_provisioning_init_resource_t)resource;
        }
    }
    plan->owned = 0;
    return count;
}
