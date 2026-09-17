#include "ryz_v5_render_dirty.h"

#include <string.h>

#define CHANGED(member) (memcmp(&before->member, &after->member, sizeof(before->member)) != 0)

/* Preserve all present/future fields except the one explicitly invisible
 * member. Unlike copying large models onto the UI stack this only reads. */
static bool changed_except(const void *before, const void *after, size_t size,
                            size_t offset, size_t length)
{
    const unsigned char *left = before, *right = after;
    return memcmp(left, right, offset) != 0 ||
        memcmp(left + offset + length, right + offset + length,
               size - offset - length) != 0;
}

bool ryz_v5_system_visible_changed(ryz_system_ui_route_t route,
    const ryz_system_ui_snapshot_t *before,
    const ryz_system_ui_snapshot_t *after)
{
    if (!before || !after || route < 0 || route >= RYZ_SYSTEM_UI_ROUTE_COUNT)
        return true;
    if(route==RYZ_SYSTEM_UI_ROUTE_SCRIPT_FAULT)
        return CHANGED(ap_active) || CHANGED(ap_ssid) || CHANGED(ap_password) || CHANGED(portal_ip);
    /* Shared header Wi-Fi/HH:MM; BLE is compared in the status model below.
     * Battery remains unavailable, not inferred from unrelated metrics. */
    if (CHANGED(connected) || CHANGED(time_hhmm)) return true;
    if (route == RYZ_SYSTEM_UI_ROUTE_HOME)
        /* Fixed WLAN plot plus all three live scalar readouts; FPS is hidden. */
        return CHANGED(home_selection.metric) || CHANGED(home_selection.generation) ||
            CHANGED(cpu_temperature_valid) || CHANGED(cpu_temperature_c) ||
            CHANGED(cpu_load_valid) || CHANGED(cpu_load_percent) ||
            CHANGED(psram_usage_valid) || CHANGED(psram_used_percent) ||
            CHANGED(home_value_valid) || CHANGED(home_value) ||
            CHANGED(home_history_count) || CHANGED(home_history) ||
            CHANGED(ap_active) || CHANGED(rssi_valid) || CHANGED(rssi_dbm) ||
            CHANGED(storage_ready) || CHANGED(storage_total_bytes) || CHANGED(storage_used_bytes);
    if (route == RYZ_SYSTEM_UI_ROUTE_SETTINGS)
        return CHANGED(state) || CHANGED(firmware_version);
    if (route == RYZ_SYSTEM_UI_ROUTE_APPS) return false;
    if (route == RYZ_SYSTEM_UI_ROUTE_DISPLAY) return false;
    if (route >= RYZ_SYSTEM_UI_ROUTE_AP_SETUP && route <= RYZ_SYSTEM_UI_ROUTE_WIFI_NO_NETWORK) {
        /* All network pages conservatively retain link/admission and AP QR
         * identity changes, even when only one of those pages displays them.
         * CPU/FPS/RSSI/traffic/storage are not drawn by the network renderer. */
        return CHANGED(configured) || CHANGED(ap_active) || CHANGED(state) ||
            CHANGED(ap_ssid) || CHANGED(ap_password) || CHANGED(portal_ip);
    }
    if (route >= RYZ_SYSTEM_UI_ROUTE_BLE_PAIRED && route <= RYZ_SYSTEM_UI_ROUTE_VERSION_FAILED)
        return false; /* Their content is compared in the corresponding model. */
    return true;
}

bool ryz_v5_status_visible_changed(ryz_system_ui_route_t route,
    const ryz_v5_status_t *before, const ryz_v5_status_t *after)
{
    if (!before || !after || route < 0 || route >= RYZ_SYSTEM_UI_ROUTE_COUNT)
        return true;
    if(route==RYZ_SYSTEM_UI_ROUTE_SCRIPT_FAULT) return false;
    bool before_link=!before->ble.unavailable && before->ble.enabled && before->ble.bonded && before->ble.linked;
    bool after_link=!after->ble.unavailable && after->ble.enabled && after->ble.bonded && after->ble.linked;
    if(before_link!=after_link) return true;
    if(route==RYZ_SYSTEM_UI_ROUTE_SETTINGS &&
       (before->ble.unavailable!=after->ble.unavailable || before->ble.enabled!=after->ble.enabled)) return true;
    if(route==RYZ_SYSTEM_UI_ROUTE_DISPLAY) return false;
    if (route == RYZ_SYSTEM_UI_ROUTE_HOME || route == RYZ_SYSTEM_UI_ROUTE_APPS ||
        route == RYZ_SYSTEM_UI_ROUTE_SETTINGS) return false;
    if (route >= RYZ_SYSTEM_UI_ROUTE_AP_SETUP && route <= RYZ_SYSTEM_UI_ROUTE_WIFI_NO_NETWORK)
        return changed_except(&before->network, &after->network, sizeof(before->network),
                   offsetof(ryz_v5_network_model_t, system), sizeof(before->network.system)) ||
            ryz_v5_system_visible_changed(route, &before->network.system, &after->network.system);
    if (route >= RYZ_SYSTEM_UI_ROUTE_BLE_PAIRED && route <= RYZ_SYSTEM_UI_ROUTE_BLE_FORGET)
        return memcmp(&before->ble, &after->ble, sizeof(before->ble)) != 0;
    if (route >= RYZ_SYSTEM_UI_ROUTE_SCRIPTS_SETTINGS && route <= RYZ_SYSTEM_UI_ROUTE_SCRIPTS_DELETE_ALL)
        return memcmp(&before->scripts, &after->scripts, sizeof(before->scripts)) != 0;
    if (route >= RYZ_SYSTEM_UI_ROUTE_VERSION_CURRENT && route <= RYZ_SYSTEM_UI_ROUTE_VERSION_FAILED) {
        if (route == RYZ_SYSTEM_UI_ROUTE_VERSION_INFO)
            return memcmp(&before->version, &after->version, sizeof(before->version)) != 0;
        /* Only Version Info draws uptime. Other OTA fields remain conservative
         * to preserve operation, failure and attempt-bound input eligibility. */
        return changed_except(&before->version, &after->version, sizeof(before->version),
            offsetof(ryz_v5_version_model_t, uptime), sizeof(before->version.uptime));
    }
    return true;
}
