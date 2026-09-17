#include "ryz_lvgl_perf.h"

/* 9.5.0 exposes show/hide/dump, but no public subject/FPS accessor. Keep this
 * single version-bound dependency here, never in the UI or managed library.
 * Upgrade deliberately: confirm observer lifetime and reset-after-publish. */
#include "src/display/lv_display_private.h"

#if LVGL_VERSION_MAJOR != 9 || LVGL_VERSION_MINOR != 5 || LVGL_VERSION_PATCH != 0
#error "Review the LVGL sysmon adapter before changing the locked LVGL version"
#endif
#if !LV_USE_SYSMON || !LV_USE_PERF_MONITOR || !LV_USE_OBSERVER
#error "Ryzobee FPS requires LVGL sysmon, performance monitor and observer"
#endif
#if LV_USE_PERF_MONITOR_LOG_MODE
#error "Ryzobee hides the sysmon label; do not enable unsolicited perf logs"
#endif

#define FPS_SAMPLE_MS 1000U
static uint32_t reported_at_ms;
static uint16_t cached_fps;
static bool cached_valid;

static void published(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer;
    const lv_sysmon_perf_info_t *info = lv_subject_get_pointer(subject);
    /* The official calculation is valid only during this callback. Sysmon
     * zeros its working structure immediately after notifying observers. */
    cached_valid = info && info->calculated.fps <= UINT16_MAX;
    cached_fps = cached_valid ? (uint16_t)info->calculated.fps : 0;
}

esp_err_t ryz_lvgl_perf_init(lv_display_t *display)
{
    cached_valid = false;
    cached_fps = 0;
    /* lv_display_create has already installed the official monitor. Hide it
     * before any flush so it cannot cover the V5 page or the boot splash. */
    if (!display->perf_label || !display->perf_sysmon_backend.timer)
        return ESP_ERR_NO_MEM;
    lv_sysmon_hide_performance(display);
    lv_sysmon_performance_pause(display);
    /* Report explicitly from the existing owner poll, not the default 300 ms
     * timer. C pages can use lv_refr_now without running lv_timer_handler. */
    lv_sysmon_performance_dump(display);
    reported_at_ms = lv_tick_get();
    if (!lv_subject_add_observer(&display->perf_sysmon_backend.subject,
                                 published, NULL)) return ESP_ERR_NO_MEM;
    /* add_observer immediately delivers the current (baseline) value. */
    cached_valid = false;
    cached_fps = 0;
    return ESP_OK;
}

bool ryz_lvgl_perf_read(lv_display_t *display, uint16_t *fps)
{
    if (lv_tick_elaps(reported_at_ms) >= FPS_SAMPLE_MS) {
        lv_sysmon_performance_dump(display);
        reported_at_ms = lv_tick_get();
    }
    *fps = cached_fps;
    return cached_valid;
}
