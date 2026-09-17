#pragma once
#include "lvgl.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    RYZ_V5_VERSION_CURRENT, RYZ_V5_VERSION_INFO, RYZ_V5_VERSION_AVAILABLE,
    RYZ_V5_VERSION_DOWNLOADING, RYZ_V5_VERSION_REBOOT, RYZ_V5_VERSION_FAILED,
} ryz_v5_version_page_t;
typedef enum {
    RYZ_V5_VERSION_ACTION_NONE, RYZ_V5_VERSION_ACTION_INFO,
    RYZ_V5_VERSION_ACTION_CHECK, RYZ_V5_VERSION_ACTION_DEVICE_ID,
    RYZ_V5_VERSION_ACTION_LATER, RYZ_V5_VERSION_ACTION_UPDATE,
    RYZ_V5_VERSION_ACTION_CANCEL, RYZ_V5_VERSION_ACTION_RESTART,
    RYZ_V5_VERSION_ACTION_RETRY,
} ryz_v5_version_action_t;

/* Owner-only, copyable data; no URLs, handles or fabricated update metadata.
 * The view does not infer newest/healthy/power from a version or connection.
 * Action readiness is supplied by the corresponding real service admission. */
typedef struct {
    ryz_v5_version_page_t page;
    int info_scroll;
    char running[32], candidate[32], updated[24], channel[24];
    char build[32], idf[32], slot[32], uptime[24], chip[32], device_id[65];
    bool check_ready, checked, up_to_date, checking, healthy_known, healthy;
    bool wifi_ready, power_known, power_ready, ab_slots;
    bool update_ready, cancel_ready, restart_ready, retry_ready;
    bool verifying, cancelling, cleanup_pending, show_device_id;
    /* Exact staged-image identity, not the running firmware version. */
    bool image_staged, restart_pending;
    uint32_t attempt_id;
    uint32_t downloaded_bytes, total_bytes;
    esp_err_t error;
    esp_err_t restart_error; /* Admission failure does not invalidate the image. */
    char failure_title[40], failure_body[160];
    /* Optional bounded, non-secret safety description supplied by the Owner.
     * Empty/unterminated data uses the built-in safety text; this is not a
     * release-notes service. info_scroll is view-only for INFO and AVAILABLE. */
    char update_description[160];
} ryz_v5_version_model_t;
int ryz_v5_version_info_height(const ryz_v5_version_model_t *model);
/* Full wrapped static-font content height; AVAILABLE clips it to 192 x 48. */
int ryz_v5_version_available_height(const ryz_v5_version_model_t *model);

esp_err_t ryz_v5_version_draw(lv_obj_t *root, const ryz_v5_version_model_t *model);
ryz_v5_version_action_t ryz_v5_version_hit(
    const ryz_v5_version_model_t *model, uint16_t x, uint16_t y);
const char *ryz_v5_version_title(ryz_v5_version_page_t page);
