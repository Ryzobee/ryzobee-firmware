#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "ryz_rgb_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Trusted C control of the single board LED on GPIO45, raw RGB8, no gamma or
 * brightness scaling. Black is an actual frame, not disabling a peripheral.
 * Lazily starts one CPU1-pinned worker; no GPIO/RMT calls in this caller. Active work is
 * rejected, never overwritten; boot-local IDs never wrap. Success only admits
 * a request. No automatic boot animation or assumed startup color. */
esp_err_t ryz_rgb_submit(ryz_rgb_color_t color, uint32_t *out_id);

/* Cleanup-only, accepted asynchronously for the current color request token.
 * Supersedes QUEUED without sending; SENDING finishes in the same worker before
 * cleanup. Never transmits black or changes last-completed color policy.
 * CLEANED + !resources_held + cleanup_error==OK confirms resource release.
 * FAILED cleanup retains resources/error for an explicit same-token retry.
 * Repeated CLEANING/CLEANED requests are idempotent. Old tokens reject newer
 * requests. Cleanup allocates no ID, so remains usable at ID exhaustion. */
esp_err_t ryz_rgb_cleanup(uint32_t request_id);

/* Cached only, never allocates or starts hardware. Before successful worker
 * creation returns INVALID_STATE and clears out. Check phase AND output_known;
 * there is no physical LED readback. */
esp_err_t ryz_rgb_get_snapshot(ryz_rgb_snapshot_t *out);

/* Generic exclusive board-LED lease. open is software-only: no worker,
 * GPIO write or assumed previous color. Tokens never wrap. A healthy parked
 * native allocation can be reused by the same driver worker, but another
 * lease or an active/failed-retained native operation rejects admission.
 * Legacy submit/cleanup are refused while a lease exists. */
esp_err_t ryz_rgb_lease_open(uint32_t *out_lease);
esp_err_t ryz_rgb_lease_submit(uint32_t lease, ryz_rgb_color_t color, uint32_t *out_id);
/* Snapshot contains only this lease's last completed color and request.
 * Before first write it is IDLE/output_known=false, not adopted history.
 * COMPLETED+output_known means frame transmission, never optical feedback. */
esp_err_t ryz_rgb_lease_snapshot(uint32_t lease, ryz_rgb_snapshot_t *out);
/* One nonblocking cleanup progress step. NOT_FINISHED means worker pending;
 * OK only after this lease is retired and owned resources are released.
 * An unwritten lease does not tear down an older controller's parked handle.
 * No black frame is transmitted. Failure retains ownership. An explicit
 * trusted retry resets the failed close before another close progress step. */
esp_err_t ryz_rgb_lease_close(uint32_t lease);
esp_err_t ryz_rgb_lease_retry_close(uint32_t lease);

#ifdef __cplusplus
}
#endif
