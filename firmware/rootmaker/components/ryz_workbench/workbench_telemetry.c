#include "workbench_telemetry.h"

#include <string.h>

#define SAMPLE_MAX_AGE_US UINT64_C(2500000)
#define TRAFFIC_MIN_INTERVAL_US UINT64_C(500000)
#define METRIC_INTERVAL_US UINT64_C(1000000)

static void publish_home(const ryz_workbench_telemetry_t *window,
                          ryz_system_ui_snapshot_t *ui)
{
    ui->home_selection = window->selection;
    ui->home_value_valid = window->history_count != 0;
    ui->home_value = window->history_count ? window->history[window->history_count-1U] : 0;
    ui->home_history_count = window->history_count;
    memcpy(ui->home_history, window->history, sizeof(ui->home_history));
}

static void clear_history(ryz_workbench_telemetry_t *window)
{
    window->history_count = 0;
    memset(window->history, 0, sizeof(window->history));
    window->metric_observed = false;
}

static void append_history(ryz_workbench_telemetry_t *window, int64_t value)
{
    if (window->history_count == RYZ_SYSTEM_UI_TRAFFIC_SAMPLES) {
        memmove(window->history, window->history + 1,
                (RYZ_SYSTEM_UI_TRAFFIC_SAMPLES - 1U) * sizeof(window->history[0]));
        --window->history_count;
    }
    window->history[window->history_count++] = value;
}

bool ryz_workbench_telemetry_select(ryz_workbench_telemetry_t *window,
    ryz_home_selection_t selection, ryz_system_ui_snapshot_t *ui)
{
    if (!window || !ui || selection.metric < RYZ_HOME_METRIC_NET ||
        selection.metric >= RYZ_HOME_METRIC_COUNT) return false;
    bool changed = window->selection.metric != selection.metric ||
        window->selection.generation != selection.generation;
    if (changed) {
        memset(window, 0, sizeof(*window));
        window->selection = selection;
        ui->cpu_temperature_valid = ui->cpu_load_valid = ui->psram_usage_valid = false;
        ui->cpu_temperature_c = ui->cpu_load_percent = ui->psram_used_percent = 0;
    }
    publish_home(window, ui);
    return changed;
}

static void observe_metric(ryz_workbench_telemetry_t *window, bool valid,
                             int64_t value, uint64_t at, uint64_t now_us)
{
    if (!valid || at < window->selection.selected_at_us || now_us < at ||
        now_us - at > SAMPLE_MAX_AGE_US) {
        clear_history(window);
        return;
    }
    if (window->metric_observed &&
        (now_us < window->metric_observed_now_us || at < window->metric_observed_us ||
         at - window->metric_observed_us > SAMPLE_MAX_AGE_US ||
         (at == window->metric_observed_us && value != window->metric_observed_value)))
        clear_history(window);
    bool duplicate = window->metric_observed && at == window->metric_observed_us;
    bool record = !window->metric_observed ||
        (!duplicate && at - window->metric_recorded_us >= METRIC_INTERVAL_US);
    window->metric_observed = true;
    window->metric_observed_us = at;
    window->metric_observed_now_us = now_us;
    window->metric_observed_value = value;
    if (record) {
        append_history(window, value);
        window->metric_recorded_us = at;
    }
}

void ryz_workbench_telemetry_psram(ryz_workbench_telemetry_t *window,
    uint64_t now_us, size_t total_bytes, size_t free_bytes,
    ryz_system_ui_snapshot_t *ui)
{
    if (!window || !ui) return;
    if (window->selection.metric != RYZ_HOME_METRIC_PSRAM &&
        window->selection.metric != RYZ_HOME_METRIC_NET) {
        ui->psram_usage_valid = false;
        ui->psram_used_percent = 0;
        return;
    }
    bool valid = total_bytes && free_bytes <= total_bytes;
#if SIZE_MAX > UINT64_MAX / 101
    valid = valid && total_bytes <= UINT64_MAX / 101;
#endif
    if (!valid || !window->psram_sampled || now_us < window->psram_sampled_at_us ||
        now_us - window->psram_sampled_at_us >= UINT64_C(1000000)) {
        window->psram_sampled = true;
        window->psram_sampled_at_us = now_us;
        window->psram_valid = valid;
        window->psram_used_percent = valid ?
            (uint8_t)(((uint64_t)(total_bytes-free_bytes)*100+total_bytes/2)/total_bytes) : 0;
    }
    ui->psram_usage_valid = window->psram_valid;
    ui->psram_used_percent = window->psram_used_percent;
    /* Fixed HOME samples memory independently, without replacing WLAN history. */
    if (window->selection.metric == RYZ_HOME_METRIC_PSRAM) {
        observe_metric(window, window->psram_valid, window->psram_used_percent,
            window->psram_sampled_at_us, now_us);
        publish_home(window, ui);
    }
}

static bool fresh(uint64_t sampled_at_us, uint64_t now_us)
{
    return now_us >= sampled_at_us && now_us - sampled_at_us <= SAMPLE_MAX_AGE_US;
}

static void traffic_baseline(ryz_workbench_telemetry_t *telemetry,
                              const ryz_provisioning_link_info_t *link,
                              uint64_t now_us)
{
    ryz_workbench_traffic_window_t *window = &telemetry->traffic;
    clear_history(telemetry);
    memset(window, 0, sizeof(*window));
    if (!link) return;
    window->baseline_valid = true;
    window->epoch = link->traffic_epoch;
    window->baseline_us = window->observed_us = link->sampled_at_us;
    window->baseline_rx = window->observed_rx = link->traffic_rx_bytes;
    window->baseline_tx = window->observed_tx = link->traffic_tx_bytes;
    window->observed_now_us = now_us;
}

static void observe_traffic(ryz_workbench_telemetry_t *telemetry,
                             const ryz_provisioning_link_info_t *link,
                             uint64_t now_us)
{
    ryz_workbench_traffic_window_t *window = &telemetry->traffic;
    const uint64_t at = link->sampled_at_us;
    const uint64_t rx = link->traffic_rx_bytes, tx = link->traffic_tx_bytes;
    if (!window->baseline_valid || window->epoch != link->traffic_epoch ||
        now_us < window->observed_now_us || at < window->observed_us ||
        rx < window->observed_rx || tx < window->observed_tx ||
        (at == window->observed_us &&
         (rx != window->observed_rx || tx != window->observed_tx))) {
        traffic_baseline(telemetry, link, now_us);
        return;
    }
    window->observed_now_us = now_us;
    if (at == window->observed_us) return;
    const uint64_t elapsed = at - window->baseline_us;
    window->observed_us = at;
    window->observed_rx = rx;
    window->observed_tx = tx;
    if (elapsed > SAMPLE_MAX_AGE_US) {
        traffic_baseline(telemetry, link, now_us);
        return;
    }
    if (elapsed < TRAFFIC_MIN_INTERVAL_US) return;
    const uint64_t delta_rx = rx - window->baseline_rx;
    const uint64_t delta_tx = tx - window->baseline_tx;
    if (delta_rx > UINT64_MAX - delta_tx ||
        delta_rx + delta_tx > UINT64_MAX / UINT64_C(8000)) {
        traffic_baseline(telemetry, link, now_us);
        return;
    }
    const uint64_t scaled = (delta_rx + delta_tx) * UINT64_C(8000);
    uint64_t rate = scaled / elapsed;
    if (scaled % elapsed >= elapsed / 2 + elapsed % 2) ++rate;
    if (rate > UINT32_MAX) {
        traffic_baseline(telemetry, link, now_us);
        return;
    }
    append_history(telemetry, (int64_t)rate);
    window->baseline_us = at;
    window->baseline_rx = rx;
    window->baseline_tx = tx;
}

static void update_traffic(ryz_workbench_telemetry_t *window,
                            const ryz_system_services_snapshot_t *services,
                            uint64_t now_us, ryz_system_ui_snapshot_t *ui)
{
    const ryz_provisioning_link_info_t *link = services ? &services->network.link : NULL;
    if (!services || !services->network_valid || !services->network.enabled ||
        services->network.state != RYZ_PROVISIONING_ONLINE || !ui->connected ||
        !link->traffic_valid || !link->traffic_epoch || !fresh(link->sampled_at_us, now_us) ||
        link->sampled_at_us < window->selection.selected_at_us)
        traffic_baseline(window, NULL, now_us);
    else
        observe_traffic(window, link, now_us);
    publish_home(window, ui);
}

void ryz_workbench_telemetry_update(
    ryz_workbench_telemetry_t *window,
    const ryz_system_services_snapshot_t *services,
    uint64_t now_us, const uint16_t *lvgl_fps,
    ryz_system_ui_snapshot_t *ui, ryz_v5_network_model_t *network)
{
    if (!window || !ui || !network) return;
    if (window->selection.metric == RYZ_HOME_METRIC_NET)
        update_traffic(window, services, now_us, ui);
    ui->rssi_valid = false;
    ui->rssi_dbm = 0;
    ui->cpu_temperature_valid = false;
    ui->cpu_temperature_c = 0;
    ui->cpu_load_valid = false;
    ui->cpu_load_percent = 0;
    if (window->selection.metric != RYZ_HOME_METRIC_PSRAM &&
        window->selection.metric != RYZ_HOME_METRIC_NET) {
        ui->psram_usage_valid = false;
        ui->psram_used_percent = 0;
    }
    network->mac_valid = network->ipv6_valid = false;
    memset(network->mac, 0, sizeof(network->mac));
    memset(network->ipv6, 0, sizeof(network->ipv6));
    if (services) {
        const ryz_provisioning_link_info_t *link = &services->network.link;
        const bool online = services->network_valid && services->network.enabled &&
            services->network.state == RYZ_PROVISIONING_ONLINE &&
            ui->connected && fresh(link->sampled_at_us, now_us);
        if (online) {
            ui->rssi_valid = link->rssi_valid;
            ui->rssi_dbm = link->rssi_valid ? link->rssi_dbm : 0;
            network->mac_valid = link->mac_valid &&
                link->mac[0] && memchr(link->mac, 0, sizeof(link->mac));
            network->ipv6_valid = link->ipv6_valid &&
                link->ipv6[0] && memchr(link->ipv6, 0, sizeof(link->ipv6));
            if (network->mac_valid) memcpy(network->mac, link->mac, sizeof(network->mac));
            if (network->ipv6_valid) memcpy(network->ipv6, link->ipv6, sizeof(network->ipv6));
        }
        const ryz_system_metrics_snapshot_t *metrics = &services->metrics;
        if (fresh(metrics->sampled_at_us, now_us) &&
            metrics->sampled_at_us >= window->selection.selected_at_us) {
            ui->cpu_temperature_valid = (window->selection.metric == RYZ_HOME_METRIC_NET ||
                window->selection.metric == RYZ_HOME_METRIC_TEMP) &&
                metrics->temperature_valid;
            if (ui->cpu_temperature_valid) {
                const int deci = metrics->temperature_deci_c;
                ui->cpu_temperature_c = (int16_t)(deci < 0 ? -((-deci + 5) / 10) : (deci + 5) / 10);
            }
            ui->cpu_load_valid = (window->selection.metric == RYZ_HOME_METRIC_NET ||
                window->selection.metric == RYZ_HOME_METRIC_CPU) &&
                metrics->load_valid && metrics->load_permille <= 1000;
            if (ui->cpu_load_valid)
                ui->cpu_load_percent = (uint8_t)((metrics->load_permille + 5) / 10);
        }
    }
    ui->display_fps_valid = lvgl_fps != NULL;
    ui->display_fps = lvgl_fps ? *lvgl_fps : 0;
    if (window->selection.metric == RYZ_HOME_METRIC_TEMP ||
        window->selection.metric == RYZ_HOME_METRIC_CPU) {
        bool temperature = window->selection.metric == RYZ_HOME_METRIC_TEMP;
        observe_metric(window, temperature ? ui->cpu_temperature_valid : ui->cpu_load_valid,
            temperature ? ui->cpu_temperature_c : ui->cpu_load_percent,
            services ? services->metrics.sampled_at_us : 0, now_us);
    }
    publish_home(window, ui);
}
