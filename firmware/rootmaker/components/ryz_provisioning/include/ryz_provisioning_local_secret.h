#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/** Trusted local C UI only. Never expose these calls or their result through
 * RPC, Lua, HTTP, ordinary snapshots, logging, or persistent UI models. */
typedef struct {
    bool available;
    uint64_t epoch;
} ryz_provisioning_local_secret_t;

/** Copy metadata only, without waiting, allocating, reading NVS or querying
 * Wi-Fi/netif. An unavailable initialized connection returns OK + {false,0};
 * contention returns TIMEOUT. All failures clear a non-NULL output.
 * Capture the epoch on a new physical press, never renew it during that hold. */
esp_err_t ryz_provisioning_local_secret_try_get(ryz_provisioning_local_secret_t *out);

/** Copy only the current STA configuration's password for the exact captured
 * nonzero epoch. Requires a validated observation no older than 2.5 seconds.
 * Empty password is a successful open network, not an unavailable secret.
 * A valid output buffer is cleared over its entire capacity before any check;
 * errors leave it cleared (INVALID_ARG, INVALID_SIZE, INVALID_STATE, TIMEOUT).
 * On every release, navigation, failed check or cancellation the caller must
 * erase the buffer and its rendered copies. Never silently retry a failed hold.
 * Epochs are independent of telemetry revisions and never reused during boot. */
esp_err_t ryz_provisioning_local_secret_try_copy(
    uint64_t expected_epoch, char *out, size_t cap);
