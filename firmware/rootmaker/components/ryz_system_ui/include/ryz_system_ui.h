#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "touch_protocol.h"
#include "ryz_ui_navigation.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RYZ_SYSTEM_UI_SSID_MAX_LENGTH 32
#define RYZ_SYSTEM_UI_PASSWORD_MAX_LENGTH 63
#define RYZ_SYSTEM_UI_IPV4_MAX_LENGTH 15

typedef enum {
    RYZ_SYSTEM_UI_ROUTE_HOME,
    RYZ_SYSTEM_UI_ROUTE_APPS,
    RYZ_SYSTEM_UI_ROUTE_SETTINGS,
    RYZ_SYSTEM_UI_ROUTE_AP_SETUP,
    RYZ_SYSTEM_UI_ROUTE_WIFI_CONNECTED,
    RYZ_SYSTEM_UI_ROUTE_WIFI_OFF,
    RYZ_SYSTEM_UI_ROUTE_WIFI_INFO,
    RYZ_SYSTEM_UI_ROUTE_WIFI_JOINING,
    RYZ_SYSTEM_UI_ROUTE_WIFI_FAILED,
    RYZ_SYSTEM_UI_ROUTE_WIFI_READY,
    RYZ_SYSTEM_UI_ROUTE_WIFI_SERVER,
    RYZ_SYSTEM_UI_ROUTE_WIFI_NO_NETWORK,
    RYZ_SYSTEM_UI_ROUTE_BLE_PAIRED,
    RYZ_SYSTEM_UI_ROUTE_BLE_OFF,
    RYZ_SYSTEM_UI_ROUTE_BLE_EMPTY,
    RYZ_SYSTEM_UI_ROUTE_BLE_PAIRING,
    RYZ_SYSTEM_UI_ROUTE_BLE_REPLACE,
    RYZ_SYSTEM_UI_ROUTE_BLE_SUCCESS,
    RYZ_SYSTEM_UI_ROUTE_BLE_INFO,
    RYZ_SYSTEM_UI_ROUTE_BLE_FORGET,
    RYZ_SYSTEM_UI_ROUTE_SCRIPTS_SETTINGS,
    RYZ_SYSTEM_UI_ROUTE_SCRIPTS_PICKER,
    RYZ_SYSTEM_UI_ROUTE_SCRIPTS_SAVED,
    RYZ_SYSTEM_UI_ROUTE_SCRIPTS_DELETE_ALL,
    RYZ_SYSTEM_UI_ROUTE_VERSION_CURRENT,
    RYZ_SYSTEM_UI_ROUTE_VERSION_INFO,
    RYZ_SYSTEM_UI_ROUTE_VERSION_AVAILABLE,
    RYZ_SYSTEM_UI_ROUTE_VERSION_DOWNLOADING,
    RYZ_SYSTEM_UI_ROUTE_VERSION_REBOOT,
    RYZ_SYSTEM_UI_ROUTE_VERSION_FAILED,
    RYZ_SYSTEM_UI_ROUTE_SCRIPT_FAULT,
    RYZ_SYSTEM_UI_ROUTE_DISPLAY,
    RYZ_SYSTEM_UI_ROUTE_COUNT,
} ryz_system_ui_route_t;

typedef enum {
    RYZ_SYSTEM_UI_NETWORK_UNCONFIGURED = 0,
    RYZ_SYSTEM_UI_NETWORK_AP_STARTING,
    RYZ_SYSTEM_UI_NETWORK_AP_READY,
    RYZ_SYSTEM_UI_NETWORK_CREDENTIALS_RECEIVED,
    RYZ_SYSTEM_UI_NETWORK_CONNECTING,
    RYZ_SYSTEM_UI_NETWORK_ONLINE,
    RYZ_SYSTEM_UI_NETWORK_FAILED,
    RYZ_SYSTEM_UI_NETWORK_STOPPING,
    RYZ_SYSTEM_UI_NETWORK_OFF,
} ryz_system_ui_network_state_t;

typedef enum {
    RYZ_SYSTEM_UI_ACTION_NONE = 0,
    RYZ_SYSTEM_UI_ACTION_REPROVISION, /* Legacy name: non-destructive OPEN_AP. */
    RYZ_SYSTEM_UI_ACTION_NETWORK_ON,
    RYZ_SYSTEM_UI_ACTION_NETWORK_OFF,
    RYZ_SYSTEM_UI_ACTION_NETWORK_CANCEL,
    RYZ_SYSTEM_UI_ACTION_NETWORK_RETRY,
    RYZ_SYSTEM_UI_ACTION_OTA_CHECK,
    RYZ_SYSTEM_UI_ACTION_OTA_UPDATE,
    RYZ_SYSTEM_UI_ACTION_OTA_CANCEL,
    RYZ_SYSTEM_UI_ACTION_OTA_RESTART,
    RYZ_SYSTEM_UI_ACTION_OTA_RETRY,
} ryz_system_ui_action_t;

#define RYZ_SYSTEM_UI_TRAFFIC_SAMPLES 24

typedef enum {
    RYZ_HOME_METRIC_NET = 0,
    RYZ_HOME_METRIC_TEMP,
    RYZ_HOME_METRIC_CPU,
    RYZ_HOME_METRIC_PSRAM,
    RYZ_HOME_METRIC_COUNT,
} ryz_home_metric_t;

/* Volatile UI-owner selection. A new generation starts a fresh recording,
 * including when returning to a previously selected metric. */
typedef struct {
    ryz_home_metric_t metric;
    uint32_t generation;
    uint64_t selected_at_us;
} ryz_home_selection_t;

/* The system owner supplies this immutable view. The UI never starts, stops,
 * or persists networking/storage itself. ap_password is trusted local display
 * data only: callers must not expose this snapshot through logs or RPC. The
 * storage fields describe the mounted scripts filesystem, not physical flash
 * usage and not Lua VM memory. */
typedef struct {
    bool configured;
    bool connected;
    bool ap_active;
    ryz_system_ui_network_state_t state;
    char ap_ssid[RYZ_SYSTEM_UI_SSID_MAX_LENGTH + 1];
    char ap_password[RYZ_SYSTEM_UI_PASSWORD_MAX_LENGTH + 1];
    char portal_ip[RYZ_SYSTEM_UI_IPV4_MAX_LENGTH + 1];
    bool storage_ready;
    size_t storage_total_bytes;
    size_t storage_used_bytes;
    /* Trusted C clock/version fields, empty until actually observed. */
    char time_hhmm[6];
    char firmware_version[32];
    /* Quantized current observations; never a sensor/SDK call in a renderer.
     * Trusted owner revokes validity when the source sample is stale. */
    bool rssi_valid;
    int8_t rssi_dbm;
    /* Exactly one HOME recording: NET decimal kbit/s, TEMP signed Celsius,
     * CPU/PSRAM percent. Slots are oldest-to-newest, not zero-padded samples.
     * int64_t covers both negative temperatures and the uint32_t NET range. */
    ryz_home_selection_t home_selection;
    bool home_value_valid;
    int64_t home_value;
    uint8_t home_history_count;
    int64_t home_history[RYZ_SYSTEM_UI_TRAFFIC_SAMPLES];
    /* Compatibility mirrors for the selected metric only; all unselected
     * mirrors are invalid/zero. Native HOME renders the recording above. */
    bool cpu_temperature_valid;
    int16_t cpu_temperature_c;
    bool cpu_load_valid;
    uint8_t cpu_load_percent;
    bool display_fps_valid;
    uint16_t display_fps;
    /* Physical PSRAM usage, not Lua's configured heap budget or Flash. */
    bool psram_usage_valid;
    uint8_t psram_used_percent;
} ryz_system_ui_snapshot_t;

/* Source compatibility for callers written before storage joined the system
 * status view. New code should use ryz_system_ui_snapshot_t. */
typedef ryz_system_ui_snapshot_t ryz_system_ui_network_snapshot_t;

/* One physical display has one UI owner. These calls intentionally expose no
 * renderer or navigation state; call them from the same execution context.
 * reset normally changes state without drawing; after sensitive content it
 * additionally performs mandatory checked black-frame cleanup. Call the
 * checked variant before untrusted handoff. render is the explicit refresh
 * seam for a changed network snapshot. */
void ryz_system_ui_reset(void);
/* Same reset, with checked sensitive-display cleanup. The Owner must require
 * ESP_OK before dispatching an untrusted application after system UI. */
esp_err_t ryz_system_ui_reset_checked(void);
ryz_system_ui_route_t ryz_system_ui_current_route(void);
/* Render/ownership/input failures revoke the current interaction generation
 * without changing the route. Clear capture and pending intent, then require
 * a successful physical released sample (UP or NONE), not an invented UP. */
void ryz_system_ui_invalidate_input(void);
ryz_ui_nav_token_t ryz_system_ui_page_token(void);
/* Owner-only count of fully successful page presentations, including their
 * navigation/input gate. Unsigned wrap is intentional: compare nearby values
 * or subtract as uint32_t. Reset, failure/cancellation and independent privacy
 * cleanup do not advance or reset it. This is not the panel's FPS counter. */
uint32_t ryz_system_ui_render_revision(void);
/* Owner-only; telemetry reads this before mapping the next service snapshot.
 * Rendering never changes the selection or collects observations. */
ryz_home_selection_t ryz_system_ui_home_selection(void);
/* Owner-only asynchronous navigation seam. Capture expected when issuing the
 * operation, not when its callback arrives. Only existing rendered routes are
 * accepted; new V5 pages/modal visuals are added with their feature stages. */
esp_err_t ryz_system_ui_navigate(ryz_ui_nav_token_t expected,
                                ryz_system_ui_route_t route);
/* Return and clear the pending UI intent. Callers should consume it after each
 * touch sample. Rendering and touch handling never perform networking. */
ryz_system_ui_action_t ryz_system_ui_take_action(void);
esp_err_t ryz_system_ui_render(
    const ryz_system_ui_snapshot_t *snapshot);
/* Owner's periodic UI step, including Apps cache refresh and 1.2s holds.
 * No storage, Lua execution or hardware-tool operation runs in this call. */
esp_err_t ryz_system_ui_tick(const ryz_system_ui_snapshot_t *snapshot);
/* Render with a display-transfer cancellation check. The callback runs
 * between stripes; ESP_ERR_TIMEOUT means either it requested cancellation or
 * the display driver itself timed out, so callers should track that intent. */
esp_err_t ryz_system_ui_render_checked(
    const ryz_system_ui_snapshot_t *snapshot,
    bool (*cancelled)(void *), void *context, bool *cancelled_out);
/* Touch handling redraws only when visible press/navigation state changes.
 * A positionless UP uses the last still-armed in-bounds target, matching the
 * CST816 protocol; leaving that target permanently cancels the gesture. */
esp_err_t ryz_system_ui_handle_touch(
    const ryz_touch_sample_t *sample,
    const ryz_system_ui_snapshot_t *snapshot);
/* Production physical-input seam. Non-OK read_status ignores sample (which
 * may be uninitialized); errors never deliver a UI intent. Owner must call
 * this for failures too. It does not execute any network/storage operation. */
esp_err_t ryz_system_ui_process_sample(
    const ryz_touch_sample_t *sample, esp_err_t read_status,
    const ryz_system_ui_snapshot_t *snapshot,
    ryz_system_ui_action_t *action_out);

#ifdef __cplusplus
}
#endif
