/* Real visibility policy + real status cache. These typed link substitutes
 * must never run: no LVGL rendering or physical timing is claimed here. */
#include "ryz_v5_render_dirty.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define UNUSED(value) ((void)(value))
#define NO_DRAW() assert(!"visibility policy called a renderer")
esp_err_t ryz_v5_network_draw(lv_obj_t *root, const ryz_v5_network_model_t *model)
{ UNUSED(root); UNUSED(model); NO_DRAW(); return ESP_FAIL; }
esp_err_t ryz_v5_ble_draw(lv_obj_t *root, const ryz_v5_ble_model_t *model)
{ UNUSED(root); UNUSED(model); NO_DRAW(); return ESP_FAIL; }
esp_err_t ryz_v5_version_draw(lv_obj_t *root, const ryz_v5_version_model_t *model)
{ UNUSED(root); UNUSED(model); NO_DRAW(); return ESP_FAIL; }
esp_err_t ryz_v5_scripts_draw(lv_obj_t *root, const ryz_v5_scripts_snapshot_t *model)
{ UNUSED(root); UNUSED(model); NO_DRAW(); return ESP_FAIL; }
int ryz_v5_network_info_height(const ryz_v5_network_model_t *model)
{ UNUSED(model); NO_DRAW(); return 0; }
int ryz_v5_ble_info_height(const ryz_v5_ble_model_t *model)
{ UNUSED(model); NO_DRAW(); return 0; }
int ryz_v5_version_info_height(const ryz_v5_version_model_t *model)
{ UNUSED(model); NO_DRAW(); return 0; }
int ryz_v5_version_available_height(const ryz_v5_version_model_t *model)
{ UNUSED(model); NO_DRAW(); return 0; }
const char *ryz_v5_network_title(ryz_v5_network_page_t page)
{ UNUSED(page); NO_DRAW(); return NULL; }
const char *ryz_v5_ble_title(const ryz_v5_ble_model_t *model)
{ UNUSED(model); NO_DRAW(); return NULL; }
const char *ryz_v5_version_title(ryz_v5_version_page_t page)
{ UNUSED(page); NO_DRAW(); return NULL; }
const char *ryz_v5_scripts_title(const ryz_v5_scripts_snapshot_t *model)
{ UNUSED(model); NO_DRAW(); return NULL; }
ryz_v5_network_action_t ryz_v5_network_hit(const ryz_v5_network_model_t *model, uint16_t x, uint16_t y)
{ UNUSED(model); UNUSED(x); UNUSED(y); NO_DRAW(); return RYZ_V5_NETWORK_ACTION_NONE; }
ryz_v5_ble_action_t ryz_v5_ble_hit(const ryz_v5_ble_model_t *model, int x, int y)
{ UNUSED(model); UNUSED(x); UNUSED(y); NO_DRAW(); return RYZ_V5_BLE_ACTION_NONE; }
ryz_v5_version_action_t ryz_v5_version_hit(const ryz_v5_version_model_t *model, uint16_t x, uint16_t y)
{ UNUSED(model); UNUSED(x); UNUSED(y); NO_DRAW(); return RYZ_V5_VERSION_ACTION_NONE; }
bool ryz_v5_scripts_hit(const ryz_v5_scripts_snapshot_t *model, uint16_t x, uint16_t y, ryz_v5_scripts_intent_t *out)
{ UNUSED(model); UNUSED(x); UNUSED(y); UNUSED(out); NO_DRAW(); return false; }

static void uptime(void)
{
    ryz_v5_status_t before = {0}, after = {0}, observed;
    strcpy(before.version.uptime, "0:00:01");
    after = before;
    strcpy(after.version.uptime, "0:00:02");
    (void)ryz_v5_status_set(&before);
    assert(!ryz_v5_status_publish_visible(RYZ_SYSTEM_UI_ROUTE_SETTINGS, &after));
    ryz_v5_status_get(&observed);
    assert(!strcmp(observed.version.uptime, "0:00:02"));
    assert(!ryz_v5_status_visible_changed(RYZ_SYSTEM_UI_ROUTE_APPS, &before, &after));
    assert(ryz_v5_status_visible_changed(RYZ_SYSTEM_UI_ROUTE_VERSION_INFO, &before, &after));
    assert(!ryz_v5_status_visible_changed(RYZ_SYSTEM_UI_ROUTE_VERSION_CURRENT, &before, &after));
}

static void home_metrics(void)
{
    ryz_system_ui_snapshot_t before = {0}, after = {0};
    after.psram_usage_valid = true;
    after.psram_used_percent = 42;
    assert(ryz_v5_system_visible_changed(RYZ_SYSTEM_UI_ROUTE_HOME, &before, &after));
    for (int route = RYZ_SYSTEM_UI_ROUTE_APPS; route < RYZ_SYSTEM_UI_ROUTE_COUNT; ++route)
        assert(!ryz_v5_system_visible_changed((ryz_system_ui_route_t)route, &before, &after));
    after.cpu_temperature_valid = after.cpu_load_valid = after.display_fps_valid = true;
    after.cpu_temperature_c = 42;
    after.cpu_load_percent = 19;
    after.display_fps = 4;
    assert(ryz_v5_system_visible_changed(RYZ_SYSTEM_UI_ROUTE_HOME, &before, &after));
    after.home_value_valid = after.rssi_valid = true;
    after.home_value = 1024;
    after.home_history_count = 1;
    after.home_history[0] = 1024;
    after.rssi_dbm = -45;
    after.storage_ready = true;
    after.storage_total_bytes = 1000;
    after.storage_used_bytes = 100;
    assert(ryz_v5_system_visible_changed(RYZ_SYSTEM_UI_ROUTE_HOME, &before, &after));
    for (int route = RYZ_SYSTEM_UI_ROUTE_APPS; route < RYZ_SYSTEM_UI_ROUTE_COUNT; ++route)
        assert(!ryz_v5_system_visible_changed((ryz_system_ui_route_t)route, &before, &after));
    ryz_v5_status_t first = {0}, second = {0}, observed;
    first.network.system = before;
    second.network.system = after;
    (void)ryz_v5_status_set(&first);
    assert(!ryz_v5_status_publish_visible(RYZ_SYSTEM_UI_ROUTE_WIFI_INFO, &second));
    ryz_v5_status_get(&observed);
    assert(observed.network.system.cpu_temperature_c == 42);
    assert(observed.network.system.home_history_count == 1);
    before=after;
    after.home_selection.metric=RYZ_HOME_METRIC_CPU;
    assert(ryz_v5_system_visible_changed(RYZ_SYSTEM_UI_ROUTE_HOME,&before,&after));
    before=after;
    ++after.home_selection.generation;
    assert(ryz_v5_system_visible_changed(RYZ_SYSTEM_UI_ROUTE_HOME,&before,&after));
}

static void header_and_settings(void)
{
    const ryz_system_ui_snapshot_t before = {0};
    ryz_system_ui_snapshot_t after;
    for (int route = 0; route < RYZ_SYSTEM_UI_ROUTE_COUNT; ++route) {
        if(route==RYZ_SYSTEM_UI_ROUTE_SCRIPT_FAULT) continue; /* No global header. */
        after = before;
        after.connected = true;
        assert(ryz_v5_system_visible_changed((ryz_system_ui_route_t)route, &before, &after));
        after = before;
        strcpy(after.time_hhmm, "12:34");
        assert(ryz_v5_system_visible_changed((ryz_system_ui_route_t)route, &before, &after));
    }
    after = before;
    after.state = RYZ_SYSTEM_UI_NETWORK_OFF;
    assert(ryz_v5_system_visible_changed(RYZ_SYSTEM_UI_ROUTE_SETTINGS, &before, &after));
    after = before;
    strcpy(after.firmware_version, "0.9.1");
    assert(ryz_v5_system_visible_changed(RYZ_SYSTEM_UI_ROUTE_SETTINGS, &before, &after));
    assert(!ryz_v5_system_visible_changed(RYZ_SYSTEM_UI_ROUTE_APPS, &before, &after));
}

static void network_changes(void)
{
    const ryz_v5_status_t before = {0};
    ryz_v5_status_t after;
    /* These all affect network content, error state, privacy or admission. A
     * conservative redraw on another network page is intentionally allowed. */
#define NETWORK_CHANGE(change) do { \
    after = before; change; \
    for (int route = RYZ_SYSTEM_UI_ROUTE_AP_SETUP; route <= RYZ_SYSTEM_UI_ROUTE_WIFI_NO_NETWORK; ++route) \
        assert(ryz_v5_status_visible_changed((ryz_system_ui_route_t)route, &before, &after)); \
    assert(!ryz_v5_status_visible_changed(RYZ_SYSTEM_UI_ROUTE_SETTINGS, &before, &after)); \
    assert(!ryz_v5_status_visible_changed(RYZ_SYSTEM_UI_ROUTE_APPS, &before, &after)); \
} while (0)
    NETWORK_CHANGE(after.network.enabled = true);
    NETWORK_CHANGE(after.network.busy = true);
    NETWORK_CHANGE(after.network.setup_pending = true);
    NETWORK_CHANGE(after.network.setup_failed = true);
    NETWORK_CHANGE(after.network.can_cancel = true);
    NETWORK_CHANGE(after.network.can_retry = true);
    NETWORK_CHANGE(after.network.can_setup = true);
    NETWORK_CHANGE(after.network.initialization_error = ESP_FAIL);
    NETWORK_CHANGE(after.network.operation_error = ESP_FAIL);
    NETWORK_CHANGE(strcpy(after.network.operation_label, "OFF FAILED"));
    NETWORK_CHANGE(after.network.error = ESP_FAIL);
    NETWORK_CHANGE(after.network.failure_detail = 7);
    NETWORK_CHANGE(strcpy(after.network.sta_ssid, "fixture-ssid"));
    NETWORK_CHANGE(strcpy(after.network.ipv4, "192.0.2.2"));
    NETWORK_CHANGE(after.network.mac_valid = true);
    NETWORK_CHANGE(after.network.ipv6_valid = true);
    NETWORK_CHANGE(strcpy(after.network.mac, "02:00:00:00:00:01"));
    NETWORK_CHANGE(strcpy(after.network.ipv6, "2001:db8::1"));
    NETWORK_CHANGE(after.network.elapsed_valid = true);
    NETWORK_CHANGE(after.network.elapsed_ms = 1000);
    NETWORK_CHANGE(after.network.system.configured = true);
    NETWORK_CHANGE(after.network.system.connected = true);
    NETWORK_CHANGE(after.network.system.ap_active = true);
    NETWORK_CHANGE(after.network.system.state = RYZ_SYSTEM_UI_NETWORK_OFF);
    NETWORK_CHANGE(strcpy(after.network.system.ap_ssid, "fixture-ap"));
    NETWORK_CHANGE(strcpy(after.network.system.ap_password, "fixture-only"));
    NETWORK_CHANGE(strcpy(after.network.system.portal_ip, "192.0.2.1"));
    NETWORK_CHANGE(strcpy(after.network.system.time_hhmm, "12:34"));
#undef NETWORK_CHANGE
}

static void ble_changes(void)
{
    const ryz_v5_status_t before = {0};
    ryz_v5_status_t after;
#define BLE_CHANGE(change) do { \
    after = before; change; \
    for (int route = RYZ_SYSTEM_UI_ROUTE_BLE_PAIRED; route <= RYZ_SYSTEM_UI_ROUTE_BLE_FORGET; ++route) \
        assert(ryz_v5_status_visible_changed((ryz_system_ui_route_t)route, &before, &after)); \
    assert(ryz_v5_status_visible_changed(RYZ_SYSTEM_UI_ROUTE_SETTINGS, &before, &after) == \
        (before.ble.unavailable!=after.ble.unavailable || before.ble.enabled!=after.ble.enabled)); \
    assert(!ryz_v5_status_visible_changed(RYZ_SYSTEM_UI_ROUTE_AP_SETUP, &before, &after)); \
} while (0)
    BLE_CHANGE(after.ble.unavailable = true);
    BLE_CHANGE(after.ble.enabled = true);
    BLE_CHANGE(after.ble.bonded = true);
    BLE_CHANGE(after.ble.linked = true);
    BLE_CHANGE(after.ble.pairing_active = true);
    BLE_CHANGE(after.ble.remaining_seconds = 17);
    BLE_CHANGE(after.ble.remaining_valid = true);
    BLE_CHANGE(after.ble.reconnect_enabled = true);
    BLE_CHANGE(after.ble.compare_pending = true);
    BLE_CHANGE(after.ble.compare_value = 123456);
    BLE_CHANGE(after.ble.state = RYZ_V5_BLE_STATE_VERIFY_CODE);
    BLE_CHANGE(after.ble.state = RYZ_V5_BLE_STATE_COMMITTING);
    BLE_CHANGE(after.ble.state = RYZ_V5_BLE_STATE_STOPPING);
    BLE_CHANGE(strcpy(after.ble.peer_name, "fixture-peer"));
#undef BLE_CHANGE
    after=before; after.ble.enabled=after.ble.bonded=after.ble.linked=true;
    for(int route=0;route<RYZ_SYSTEM_UI_ROUTE_COUNT;++route)
        assert(ryz_v5_status_visible_changed((ryz_system_ui_route_t)route,&before,&after)==
            (route!=RYZ_SYSTEM_UI_ROUTE_SCRIPT_FAULT));
}

static void version_changes(void)
{
    const ryz_v5_status_t before = {0};
    ryz_v5_status_t after;
#define VERSION_CHANGE(change) do { \
    after = before; change; \
    for (int route = RYZ_SYSTEM_UI_ROUTE_VERSION_CURRENT; route <= RYZ_SYSTEM_UI_ROUTE_VERSION_FAILED; ++route) \
        assert(ryz_v5_status_visible_changed((ryz_system_ui_route_t)route, &before, &after)); \
    assert(!ryz_v5_status_visible_changed(RYZ_SYSTEM_UI_ROUTE_SETTINGS, &before, &after)); \
    assert(!ryz_v5_status_visible_changed(RYZ_SYSTEM_UI_ROUTE_APPS, &before, &after)); \
} while (0)
    VERSION_CHANGE(after.version.healthy_known = true);
    VERSION_CHANGE(after.version.healthy = true);
    VERSION_CHANGE(after.version.wifi_ready = true);
    VERSION_CHANGE(after.version.check_ready = true);
    VERSION_CHANGE(after.version.checking = true);
    VERSION_CHANGE(after.version.update_ready = true);
    VERSION_CHANGE(after.version.cancel_ready = true);
    VERSION_CHANGE(after.version.retry_ready = true);
    VERSION_CHANGE(after.version.restart_ready = true);
    VERSION_CHANGE(after.version.restart_pending = true);
    VERSION_CHANGE(after.version.image_staged = true);
    VERSION_CHANGE(after.version.attempt_id = 2);
    VERSION_CHANGE(after.version.cleanup_pending = true);
    VERSION_CHANGE(after.version.verifying = true);
    VERSION_CHANGE(after.version.cancelling = true);
    VERSION_CHANGE(after.version.downloaded_bytes = 1024);
    VERSION_CHANGE(after.version.total_bytes = 4096);
    VERSION_CHANGE(after.version.error = ESP_FAIL);
    VERSION_CHANGE(after.version.restart_error = ESP_FAIL);
    VERSION_CHANGE(after.version.show_device_id = true);
    VERSION_CHANGE(strcpy(after.version.chip, "ESP32-S3"));
    VERSION_CHANGE(strcpy(after.version.failure_body, "fixture failure"));
    VERSION_CHANGE(strcpy(after.version.update_description, "fixture safety description"));
    VERSION_CHANGE(strcpy(after.version.candidate, "0.9.1"));
#undef VERSION_CHANGE
    after = before;
    strcpy(after.version.uptime, "0:00:02");
    for (int route = 0; route < RYZ_SYSTEM_UI_ROUTE_COUNT; ++route)
        assert(ryz_v5_status_visible_changed((ryz_system_ui_route_t)route, &before, &after) ==
               (route == RYZ_SYSTEM_UI_ROUTE_VERSION_INFO));
}

static void scripts_changes(void)
{
    const ryz_v5_status_t before = {0};
    ryz_v5_status_t after;
#define SCRIPTS_CHANGE(change) do { \
    after = before; change; \
    for (int route = RYZ_SYSTEM_UI_ROUTE_SCRIPTS_SETTINGS; route <= RYZ_SYSTEM_UI_ROUTE_SCRIPTS_DELETE_ALL; ++route) \
        assert(ryz_v5_status_visible_changed((ryz_system_ui_route_t)route, &before, &after)); \
    assert(!ryz_v5_status_visible_changed(RYZ_SYSTEM_UI_ROUTE_SETTINGS, &before, &after)); \
    assert(!ryz_v5_status_visible_changed(RYZ_SYSTEM_UI_ROUTE_APPS, &before, &after)); \
} while (0)
    SCRIPTS_CHANGE(after.scripts.generation = 1);
    SCRIPTS_CHANGE(after.scripts.catalog.revision = 2);
    SCRIPTS_CHANGE(after.scripts.catalog.total = 3);
    SCRIPTS_CHANGE(after.scripts.boot_known = true);
    SCRIPTS_CHANGE(strcpy(after.scripts.boot_name, "boot.lua"));
    SCRIPTS_CHANGE(after.scripts.can_open_picker = true);
    SCRIPTS_CHANGE(after.scripts.can_change_boot = true);
    SCRIPTS_CHANGE(after.scripts.can_delete_all = true);
    SCRIPTS_CHANGE(after.scripts.can_restart = true);
    SCRIPTS_CHANGE(after.scripts.selected_identity_valid = true);
    SCRIPTS_CHANGE(after.scripts.selected_source_valid = true);
    SCRIPTS_CHANGE(after.scripts.selected_revision = 2);
    SCRIPTS_CHANGE(after.scripts.selected.sha256[0] = 'a');
    SCRIPTS_CHANGE(after.scripts.batch_removed = 1);
    SCRIPTS_CHANGE(after.scripts.batch_complete = true);
    SCRIPTS_CHANGE(after.scripts.result = RYZ_V5_SCRIPTS_UNKNOWN);
    SCRIPTS_CHANGE(after.scripts.recovery_required = true);
    SCRIPTS_CHANGE(after.scripts.cleanup_error = ESP_FAIL);
    SCRIPTS_CHANGE(after.scripts.error = ESP_FAIL);
#undef SCRIPTS_CHANGE
}

static void publication(void)
{
    ryz_v5_status_t first = {0}, next = {0}, observed;
    (void)ryz_v5_status_set(&first);
    assert(!ryz_v5_status_set(&first));
    strcpy(next.version.uptime, "1:00:00");
    next.ble.pairing_active = true;
    next.scripts.recovery_required = true;
    next.network.operation_error = ESP_FAIL;
    assert(!ryz_v5_status_publish_visible(RYZ_SYSTEM_UI_ROUTE_SETTINGS, &next));
    ryz_v5_status_get(&observed);
    assert(memcmp(&observed, &next, sizeof(next)) == 0);
    assert(!ryz_v5_status_set(&next));
    /* The legacy API continues to report hidden full-state changes. */
    assert(ryz_v5_status_set(&first));
    assert(ryz_v5_status_visible_changed(RYZ_SYSTEM_UI_ROUTE_BLE_PAIRING, &first, &next));
    assert(ryz_v5_status_visible_changed(RYZ_SYSTEM_UI_ROUTE_SCRIPTS_SETTINGS, &first, &next));
    assert(ryz_v5_status_visible_changed(RYZ_SYSTEM_UI_ROUTE_WIFI_OFF, &first, &next));
    assert(ryz_v5_status_publish_visible(RYZ_SYSTEM_UI_ROUTE_WIFI_OFF, &next));
    assert(!ryz_v5_status_publish_visible(RYZ_SYSTEM_UI_ROUTE_WIFI_OFF, &next));
}

static void safe_fallback(void)
{
    const ryz_v5_status_t status = {0};
    const ryz_system_ui_snapshot_t system = {0};
    for (int route = 0; route < RYZ_SYSTEM_UI_ROUTE_COUNT; ++route) {
        assert(!ryz_v5_system_visible_changed((ryz_system_ui_route_t)route, &system, &system));
        assert(!ryz_v5_status_visible_changed((ryz_system_ui_route_t)route, &status, &status));
    }
    assert(ryz_v5_system_visible_changed((ryz_system_ui_route_t)-1, &system, &system));
    assert(ryz_v5_system_visible_changed(RYZ_SYSTEM_UI_ROUTE_COUNT, &system, &system));
    assert(ryz_v5_status_visible_changed((ryz_system_ui_route_t)-1, &status, &status));
    assert(ryz_v5_status_visible_changed(RYZ_SYSTEM_UI_ROUTE_COUNT, &status, &status));
    assert(ryz_v5_system_visible_changed(RYZ_SYSTEM_UI_ROUTE_HOME, NULL, &system));
    assert(ryz_v5_system_visible_changed(RYZ_SYSTEM_UI_ROUTE_HOME, &system, NULL));
    assert(ryz_v5_status_visible_changed(RYZ_SYSTEM_UI_ROUTE_HOME, NULL, &status));
    assert(ryz_v5_status_visible_changed(RYZ_SYSTEM_UI_ROUTE_HOME, &status, NULL));
    (void)ryz_v5_status_set(&status);
    assert(ryz_v5_status_publish_visible(RYZ_SYSTEM_UI_ROUTE_SETTINGS, NULL));
    ryz_v5_status_t observed;
    ryz_v5_status_get(&observed);
    assert(memcmp(&observed, &status, sizeof(status)) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "uptime")) uptime();
    else if (!strcmp(argv[1], "home-metrics")) home_metrics();
    else if (!strcmp(argv[1], "header")) header_and_settings();
    else if (!strcmp(argv[1], "network")) network_changes();
    else if (!strcmp(argv[1], "ble")) ble_changes();
    else if (!strcmp(argv[1], "version")) version_changes();
    else if (!strcmp(argv[1], "scripts")) scripts_changes();
    else if (!strcmp(argv[1], "publication")) publication();
    else if (!strcmp(argv[1], "fallback")) safe_fallback();
    else assert(!"unknown case");
    printf("V5_RENDER_DIRTY_PASS %s\n", argv[1]);
    return 0;
}
