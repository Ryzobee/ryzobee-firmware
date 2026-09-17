#pragma once

#include "ryz_rgb.h"

/* Internal single-worker Interface, never call from UI/RPC or concurrently.
 * At most one RMT transaction. Failed completion keeps payload/resources alive
 * until successful quiesce/reclaim; subsequent writes try bounded recovery.
 * No caller buffer is retained. Success requires the entire black/color frame
 * to complete, then successful channel disable. Failure means output unknown.
 * Channel/encoder allocations (including partial init) are kept for retry or
 * explicit cleanup, never freed under a possibly active transmitter.
 * All operations belong to the same CPU1-pinned worker task. */
esp_err_t ryz_rgb_driver_write(ryz_rgb_color_t color);

typedef struct {
    bool resources_held;
    esp_err_t cleanup_error;
} ryz_rgb_driver_status_t;

/* Stop/drain, then release encoder/channel. No frame, allocation or retry loop.
 * On failure retain every remaining handle; the same worker may retry. */
esp_err_t ryz_rgb_driver_cleanup(void);
/* Worker-only cached observation, no RMT/GPIO calls. */
ryz_rgb_driver_status_t ryz_rgb_driver_get_status(void);
