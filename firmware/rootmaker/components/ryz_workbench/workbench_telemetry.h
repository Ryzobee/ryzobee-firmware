#pragma once

#include "ryz_system_services.h"
#include "ryz_system_ui.h"
#include "ryz_v5_network.h"

/* UI-owner-only windows. Zero-initialize; no handles, polling, allocations or
 * driver calls. History contains only completed measured intervals. */
typedef struct {
    bool baseline_valid;
    uint32_t epoch;
    uint64_t baseline_us, baseline_rx, baseline_tx;
    uint64_t observed_us, observed_rx, observed_tx, observed_now_us;
} ryz_workbench_traffic_window_t;

typedef struct {
    ryz_home_selection_t selection;
    uint8_t history_count;
    int64_t history[RYZ_SYSTEM_UI_TRAFFIC_SAMPLES];
    ryz_workbench_traffic_window_t traffic;
    bool metric_observed;
    uint64_t metric_observed_us, metric_recorded_us, metric_observed_now_us;
    int64_t metric_observed_value;
    bool psram_sampled, psram_valid;
    uint64_t psram_sampled_at_us;
    uint8_t psram_used_percent;
} ryz_workbench_telemetry_t;

/* The selected metric owns the only recording. A new metric or generation
 * discards history, traffic baseline and cached PSRAM immediately. Repeating
 * the same selection preserves its samples; returning later never restores
 * an older recording. Called by the UI owner before update/psram. */
bool ryz_workbench_telemetry_select(ryz_workbench_telemetry_t *window,
    ryz_home_selection_t selection, ryz_system_ui_snapshot_t *ui);

/* Read-only memory observations, one-second publication, rounded usage.
 * Zero total or free > total is unknown, never a plausible percentage. */
void ryz_workbench_telemetry_psram(ryz_workbench_telemetry_t *window,
    uint64_t now_us, size_t total_bytes, size_t free_bytes,
    ryz_system_ui_snapshot_t *ui);

/* Map real service observations into the existing V5 fields. NULL services
 * revokes network/CPU observations. Future or >2.5s-old samples are unknown.
 * FPS is copied from the official LVGL result cached by ryz_lvgl_get_fps;
 * NULL means unavailable. Its one-second window belongs to the LVGL adapter,
 * not the network sampler. No counter conversion or second averaging here.
 * WLAN is combined RX+TX decimal kbit/s, nearest-integer rounded, from fresh
 * monotonic counter intervals of 0.5..2.5s. First/restarted samples are unknown;
 * duplicates do not append history. Disconnection, invalid observations,
 * epoch/clock/counter resets or unrepresentable deltas clear the entire WLAN
 * window. Sub-0.5s observations retain the calculation baseline. */
void ryz_workbench_telemetry_update(
    ryz_workbench_telemetry_t *window,
    const ryz_system_services_snapshot_t *services,
    uint64_t now_us, const uint16_t *lvgl_fps,
    ryz_system_ui_snapshot_t *ui, ryz_v5_network_model_t *network);
