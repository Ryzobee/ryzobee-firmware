#pragma once

#include "ryz_workbench_apps.h"
#include "workbench_write_guard.h"

typedef struct {
    bool accepted;
    bool uncertain;
    esp_err_t error;
    char message[128];
    char job_id[32];
} ryz_workbench_apps_launch_result_t;

/* Owner-only: consumes malloc source on every result; no filesystem reads.
 * Production uses the existing complete-ACK-before-enqueue job_start path. */
typedef void (*ryz_workbench_apps_launch_fn)(
    void *context, const char *name, char *source,
    ryz_workbench_apps_launch_result_t *result);

/* Boot once before Owner/RX start, no allocations or filesystem work. */
esp_err_t ryz_workbench_apps_init(const char *boot_id);
/* Sole UI Owner. active excludes modal/foreign/Lua ownership. Opens the
 * existing reader, consumes intents/prepared results, and copies a snapshot.
 * Initial read uses STORE_PAGE_MAX; PAGE lets the renderer choose its limit.
 * A failed reader request is explicit, never retried every UI tick. */
void ryz_workbench_apps_owner_tick(bool active, ryz_ui_nav_token_t page,
                                  ryz_workbench_apps_launch_fn launch,
                                  void *context);
/* Sole existing RX task: one bounded Store preparation or confirmed CAS
 * deletion. Never invoke on UI Owner. runtime_busy is sampled from the actual
 * job owner, but is not a write reservation. Confirmed deletion passes guard
 * to the shared file RPC, which alone acquires/releases the common writer
 * lease. Missing/denied guards reject deletion; source preparation does not
 * acquire a writer lease and Job launch still repeats final admission.
 * Before RX accepts DELETE, closed/stale view or a newer pending intent can
 * reject it. After that point navigation is not cancellation: retain outcome.
 * No filesystem call holds the Controller or job lock. */
void ryz_workbench_apps_rx_tick(bool runtime_busy,
                               const ryz_workbench_write_guard_t *guard);
