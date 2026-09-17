#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"

/* Trusted C only. These are L2 boundary bytes, not application goodput or
 * successful over-the-air delivery. TX counts frames accepted by the driver;
 * RX counts ESP-NETIF ingress returning ESP_OK. With the audited IDF 5.5.4
 * RECEIVE_REPORT_ERRORS disabled, that RX result does NOT prove lwIP accepted
 * the frame (its input error is hidden by the SDK). No payload is retained. */
typedef struct {
    bool valid;
    uint32_t epoch;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
} ryz_net_traffic_snapshot_t;

/* Call only with the owner's live, verified ONLINE STA identity. This does not
 * validate association or keep the netif alive. Same pointer+BSSID is
 * idempotent; first observation, identity change or observation after revoke
 * starts a zero-byte, nonzero epoch. Epochs never wrap: exhaustion fails closed
 * with INVALID_STATE. A uint64 counter overflow invalidates the binding and
 * returns INVALID_SIZE until explicit revoke. Errors clear a non-NULL out.
 *
 * All state is protected by a very short FreeRTOS critical section. No SDK,
 * heap, logging, callback or blocking operation runs inside it. The owner must
 * revoke before teardown/transition and on observed disconnect or failed live
 * validation; pointer reuse alone is not connection provenance. */
esp_err_t ryz_net_traffic_observe(esp_netif_t *sta, const uint8_t bssid[6],
                                ryz_net_traffic_snapshot_t *out);

/* Revoke the current binding, counters and counter fault, not the monotonic
 * epoch history. In-flight wrappers still delegate normally but cannot credit
 * the revoked epoch, including when the same pointer+BSSID is rebound. This is
 * not a driver drain or netif lifetime barrier. No radio action is performed. */
void ryz_net_traffic_revoke(void);
