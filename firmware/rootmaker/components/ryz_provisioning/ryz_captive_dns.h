#pragma once

#include "esp_err.h"
#include "esp_netif.h"

typedef struct ryz_captive_dns *ryz_captive_dns_handle_t;

/** A failed start may return a non-NULL cleanup-only handle if socket close
 * failed. The caller must retain it and retry stop before creating a server. */
esp_err_t ryz_captive_dns_start(
    esp_netif_t *ap_netif,
    ryz_captive_dns_handle_t *out_handle);
/**
 * Serial owner only; never call from the DNS task itself. NULL is already
 * stopped. Success consumes the handle. Failure retains it for a later retry:
 * no forced task deletion, socket close or memory free before the task ACK.
 * The ACK wait is bounded to 1000 ms; this is not a bound on lwIP's close call.
 */
esp_err_t ryz_captive_dns_stop(ryz_captive_dns_handle_t handle);
