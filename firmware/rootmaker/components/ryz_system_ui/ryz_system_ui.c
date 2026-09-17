#include "ryz_system_ui.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "ryz_qr_encoder.h"

#if defined(ESP_PLATFORM) || defined(RYZ_SYSTEM_UI_LVGL)
#define RYZ_UI_NATIVE_V5 1
#include "ryz_v5_render.h"
#include "ryz_v5_apps.h"
#include "ryz_v5_status.h"
#include "ryz_v5_password.h"
#include "ryz_v5_fault.h"
#include "ryz_v5_display.h"
#include "esp_timer.h"
#endif

_Static_assert(RYZ_DISPLAY_WIDTH == 240 && RYZ_DISPLAY_HEIGHT == 240,
               "ryz_system_ui requires a 240x240 display");

enum {
    COLOR_BLACK = 0x0000,
    COLOR_CANVAS = 0x1082,
    COLOR_PANEL = 0x18C3,
    COLOR_BORDER = 0x39C7,
    COLOR_MUTED = 0x9D13,
    COLOR_WHITE = 0xFFFF,
    COLOR_RYZOBEE_ORANGE = 0xFB40,
};

enum {
    TOP_BAR_HEIGHT = 32,
    CONTENT_HEIGHT = 164,
    CARD_X = 8,
    CARD_WIDTH = 224,
    CARD_HEIGHT = 72,
    NETWORK_CARD_Y = TOP_BAR_HEIGHT,
    HOME_NETWORK_X = 64,
    HOME_NETWORK_Y = 32,
    HOME_NETWORK_WIDTH = 168,
    HOME_NETWORK_HEIGHT = 77,
    BACK_X = 0,
    BACK_Y = 0,
    BACK_WIDTH = 44,
    BACK_HEIGHT = 44,
    BACK_VISUAL_WIDTH = 24,
    NAV_Y = TOP_BAR_HEIGHT + CONTENT_HEIGHT,
    NAV_HEIGHT = 44,
    NAV_TAB_WIDTH = 80,
    STORAGE_USAGE_X = 122,
    STORAGE_USAGE_Y = 158,
    STORAGE_USAGE_WIDTH = 110,
    STORAGE_USAGE_HEIGHT = 16,
    STORAGE_TRACK_X = 8,
    STORAGE_TRACK_Y = 181,
    STORAGE_TRACK_WIDTH = 224,
    STORAGE_TRACK_HEIGHT = 6,
    QR_REGION_X = 78,
    QR_REGION_Y = 58,
    QR_REGION_SIZE = 132,
    QR_QUIET_ZONE_MODULES = 4,
    WIFI_QR_PAYLOAD_CAPACITY = 224,
    APP_TILE_WIDTH = 104,
    APP_TILE_HEIGHT = 64,
    SETTINGS_ROW_HEIGHT = 48,
    FIGMA_SCREEN_BYTES = RYZ_DISPLAY_WIDTH * RYZ_DISPLAY_HEIGHT * 2,
};

_Static_assert(TOP_BAR_HEIGHT + CONTENT_HEIGHT + NAV_HEIGHT ==
                   RYZ_DISPLAY_HEIGHT,
               "top bar, content and navigation must fill the screen");
_Static_assert(NAV_HEIGHT >= 44,
               "bottom navigation must meet the minimum touch target");
_Static_assert(BACK_WIDTH >= 44 && BACK_HEIGHT >= 44,
               "back navigation must meet the minimum touch target");

typedef enum {
    TARGET_NONE,
    TARGET_HOME,
    TARGET_APPS,
    TARGET_SETTINGS,
    TARGET_HOME_NETWORK,
    TARGET_BACK,
    TARGET_SETTINGS_NETWORK,
    TARGET_REPROVISION,
    TARGET_SETTINGS_BLE,
    TARGET_SETTINGS_SCRIPTS,
    TARGET_SETTINGS_VERSION,
    TARGET_FAULT_HOME,
    TARGET_FAULT_NEXT,
    TARGET_SETTINGS_DISPLAY,
    TARGET_NETWORK_BASE = 100,
    TARGET_BLE_BASE = 200,
    TARGET_VERSION_BASE = 300,
    TARGET_SCRIPTS_BASE = 400,
} target_t;

static ryz_ui_navigation_t s_navigation;
/* Existing route enum starts at zero, navigation identifiers at one. */
#define s_route ryz_system_ui_current_route()
static target_t s_pressed_target = TARGET_NONE;
static ryz_system_ui_action_t s_pending_action = RYZ_SYSTEM_UI_ACTION_NONE;
static ryz_ui_nav_token_t s_pressed_page;
static ryz_ui_nav_token_t s_action_page;
static bool s_wait_for_release = true;
static bool s_presented;
static bool s_contact_active;
static uint16_t s_capture_x, s_capture_y;
static uint32_t s_render_revision;
static ryz_home_selection_t s_home_selection;

ryz_home_selection_t ryz_system_ui_home_selection(void)
{ return s_home_selection; }

#ifdef RYZ_UI_NATIVE_V5
static uint16_t s_tap_down_x, s_tap_down_y;
/* TP direction codes are unrotated: veto a click, never infer navigation. */
static bool release_is_swipe(const ryz_touch_sample_t *sample)
{ return sample->event==RYZ_TOUCH_UP && sample->gesture>=1 && sample->gesture<=4; }
static uint32_t s_version_pressed_attempt, s_version_presented_attempt;
static bool s_version_presented_restart;
/* BLE authorization belongs to the frame the finger actually saw. The
 * service cache may change without a navigation generation change. */
static ryz_v5_ble_model_t s_ble_presented, s_ble_pressed;
static bool s_ble_presented_valid, s_ble_pressed_valid;
static bool s_ble_confirmation_valid, s_ble_command_pending;
static bool s_ble_modal_submitted;
static uint32_t s_ble_confirmation_operation, s_ble_pair_operation;
static bool s_ble_pair_requested;
static bool s_ble_pair_cancelled;
static uint32_t s_ble_pair_baseline;
static ryz_v5_ble_model_t s_ble_submitted;
/* V5 top-level paging owns only horizontal gestures. The APPS controller
 * retains its vertical list stream; a drag can never become a late tap. */
static struct {
    bool active, moved;
    unsigned axis; /* 0 undecided, 1 horizontal, 2 vertical */
    int x, y, last_x, last_y;
    ryz_ui_nav_token_t page;
} s_swipe;
static struct { bool active, moved; unsigned axis; int x,y,last_y; ryz_ui_nav_token_t page; } s_info_drag;

static bool version_restart_ready(const ryz_v5_version_model_t *m)
{ return m->image_staged && m->attempt_id && m->restart_ready && !m->restart_pending; }

static bool ble_route(ryz_system_ui_route_t route)
{ return route>=RYZ_SYSTEM_UI_ROUTE_BLE_PAIRED && route<=RYZ_SYSTEM_UI_ROUTE_BLE_FORGET; }

static bool ble_target(target_t target)
{ return target>TARGET_BLE_BASE && target<TARGET_VERSION_BASE; }

static bool ble_comparison_action(target_t target)
{ return target==(target_t)(TARGET_BLE_BASE+RYZ_V5_BLE_ACTION_CONFIRM_CODE) ||
         target==(target_t)(TARGET_BLE_BASE+RYZ_V5_BLE_ACTION_REJECT_CODE); }

static bool ble_same_identity(const ryz_v5_ble_model_t *a,const ryz_v5_ble_model_t *b,
                              target_t target)
{
    if(a->operation_id!=b->operation_id || a->unavailable!=b->unavailable ||
       a->state!=b->state || a->enabled!=b->enabled || a->bonded!=b->bonded ||
       a->checking_state!=b->checking_state || a->pairing_active!=b->pairing_active)
        return false;
    if(!ble_comparison_action(target)) return true;
    return a->compare_pending && b->compare_pending &&
        a->state==RYZ_V5_BLE_STATE_VERIFY_CODE && a->operation_id &&
        a->remaining_valid && b->remaining_valid &&
        a->remaining_seconds>0 && b->remaining_seconds>0 &&
        a->compare_value<=999999 && a->compare_value==b->compare_value;
}

static void ble_reset_input(void)
{ s_ble_presented_valid=false; s_ble_pressed_valid=false; }

static void ble_leave_flow(void)
{
    ble_reset_input();
    s_ble_confirmation_valid=false;
    s_ble_modal_submitted=false;
    s_ble_command_pending=false;
    s_ble_pair_operation=0;
    s_ble_pair_requested=false;
    s_ble_pair_cancelled=false;
}

static ryz_system_ui_route_t ble_landing(const ryz_v5_ble_model_t *m)
{
    if(m->unavailable || !m->enabled) return RYZ_SYSTEM_UI_ROUTE_BLE_OFF;
    if(m->pairing_active || m->compare_pending ||
       m->state==RYZ_V5_BLE_STATE_VERIFY_CODE ||
       m->state==RYZ_V5_BLE_STATE_COMMITTING ||
       m->state==RYZ_V5_BLE_STATE_PAIR_TIMEOUT ||
       m->state==RYZ_V5_BLE_STATE_PAIR_FAILED)
        return RYZ_SYSTEM_UI_ROUTE_BLE_PAIRING;
    return m->bonded?RYZ_SYSTEM_UI_ROUTE_BLE_PAIRED:RYZ_SYSTEM_UI_ROUTE_BLE_EMPTY;
}

static void ble_recheck_capture(const ryz_v5_ble_model_t *current)
{
    if(ble_target(s_pressed_target) && (!s_ble_pressed_valid ||
       !ble_same_identity(&s_ble_pressed,current,s_pressed_target))) {
        s_pressed_target=TARGET_NONE;
        s_ble_pressed_valid=false;
    }
}

static ryz_v5_scripts_binding_t s_scripts_binding;
static ryz_v5_scripts_intent_t s_scripts_pressed;
static uint64_t s_scripts_contact_generation, s_scripts_presented_generation;
static uint32_t s_scripts_contact_revision, s_scripts_presented_revision;
static uint16_t s_scripts_down_x, s_scripts_down_y;
static int s_scripts_last_y;
static unsigned s_scripts_axis;
static bool s_scripts_cancelled, s_scripts_scroll_origin;

static bool scripts_route(void)
{ return s_route>=RYZ_SYSTEM_UI_ROUTE_SCRIPTS_SETTINGS && s_route<=RYZ_SYSTEM_UI_ROUTE_SCRIPTS_DELETE_ALL; }

static void scripts_reset_input(void)
{
    s_scripts_pressed=(ryz_v5_scripts_intent_t){0};
    s_scripts_cancelled=true;
    s_scripts_scroll_origin=false;
    s_scripts_axis=0;
    s_scripts_presented_generation=0;
    s_scripts_presented_revision=0;
    ryz_v5_scripts_admission_error(0,ESP_OK);
}

void ryz_v5_scripts_bind(const ryz_v5_scripts_binding_t *binding)
{ s_scripts_binding=binding?*binding:(ryz_v5_scripts_binding_t){0}; scripts_reset_input(); }
#endif

#ifndef RYZ_UI_NATIVE_V5
#ifdef ESP_PLATFORM
extern const uint8_t home_v5_start[] asm("_binary_home_v5_rgb565_start");
extern const uint8_t apps_v5_start[] asm("_binary_apps_v5_rgb565_start");
extern const uint8_t settings_v5_start[] asm("_binary_settings_v5_rgb565_start");
extern const uint8_t ap_setup_v5_start[] asm("_binary_ap_setup_v5_rgb565_start");

static const uint8_t *figma_screen_asset(ryz_system_ui_route_t route)
{
    if (route == RYZ_SYSTEM_UI_ROUTE_APPS) return apps_v5_start;
    if (route == RYZ_SYSTEM_UI_ROUTE_SETTINGS) return settings_v5_start;
    if (route == RYZ_SYSTEM_UI_ROUTE_AP_SETUP) return ap_setup_v5_start;
    return home_v5_start;
}

static esp_err_t draw_figma_screen(ryz_system_ui_route_t route)
{
    return ryz_display_blit_rgb565_be(
        0, 0, RYZ_DISPLAY_WIDTH, RYZ_DISPLAY_HEIGHT,
        figma_screen_asset(route), FIGMA_SCREEN_BYTES);
}
#endif

static esp_err_t draw_text(int x, int y, const char *text, uint16_t color,
                           int scale)
{
    return ryz_display_text(x, y, text, strlen(text), color, scale);
}

static esp_err_t draw_centered_bounded_text(int y, const char *text,
                                             size_t max_length,
                                             uint16_t color, int scale)
{
    size_t length = 0;
    while (length <= max_length && text[length] != '\0') {
        ++length;
    }
    if (length > max_length) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t pixel_width = length == 0 ? 0 :
                               ((length * 6U) - 1U) * (size_t)scale;
    if (pixel_width > RYZ_DISPLAY_WIDTH) {
        return ESP_ERR_INVALID_ARG;
    }
    return ryz_display_text(
        (RYZ_DISPLAY_WIDTH - (int)pixel_width) / 2,
        y, text, length, color, scale);
}

static esp_err_t draw_left_bounded_text(int x, int y, int width,
                                        const char *text, size_t max_length,
                                        uint16_t color, int scale)
{
    size_t length = 0;
    while (length <= max_length && text[length] != '\0') {
        ++length;
    }
    if (length > max_length || width < 1 || scale < 1) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t visible_capacity =
        ((size_t)width + 1U) / (6U * (size_t)scale);
    if (length > visible_capacity) length = visible_capacity;
    return ryz_display_text(x, y, text, length, color, scale);
}

static esp_err_t draw_outlined_panel(int x, int y, int width, int height,
                                     uint16_t fill, uint16_t border)
{
    esp_err_t error = ryz_display_rect(x, y, width, height, fill);
    if (error != ESP_OK) return error;
    error = ryz_display_rect(x, y, width, 1, border);
    if (error != ESP_OK) return error;
    error = ryz_display_rect(x, y + height - 1, width, 1, border);
    if (error != ESP_OK) return error;
    error = ryz_display_rect(x, y, 1, height, border);
    if (error != ESP_OK) return error;
    return ryz_display_rect(x + width - 1, y, 1, height, border);
}

#ifndef ESP_PLATFORM
static esp_err_t draw_top_bar(void)
{
    const char *title = "RYZOBEE";
    const char *code = "SYS";
    int title_x = 10;
    if (s_route == RYZ_SYSTEM_UI_ROUTE_APPS) {
        title = "APPS";
        code = "02";
    } else if (s_route == RYZ_SYSTEM_UI_ROUTE_SETTINGS) {
        title = "SETTINGS";
        code = "03";
    } else if (s_route == RYZ_SYSTEM_UI_ROUTE_AP_SETUP) {
        title = "AP SETUP";
        code = "AP";
        title_x = 44;
        if (s_pressed_target == TARGET_BACK) {
            esp_err_t error = ryz_display_rect(
                BACK_X, BACK_Y, BACK_VISUAL_WIDTH, TOP_BAR_HEIGHT,
                COLOR_RYZOBEE_ORANGE);
            if (error != ESP_OK) return error;
            error = draw_text(8, 9, "<", COLOR_BLACK, 2);
            if (error != ESP_OK) return error;
        } else {
            esp_err_t error = draw_text(8, 9, "<", COLOR_RYZOBEE_ORANGE, 2);
            if (error != ESP_OK) return error;
        }
    } else {
        esp_err_t error = ryz_display_rect(
            2, 8, 2, 16, COLOR_RYZOBEE_ORANGE);
        if (error != ESP_OK) return error;
    }

    esp_err_t error = draw_text(title_x, 9, title, COLOR_WHITE, 2);
    if (error != ESP_OK) return error;
    const size_t code_length = strlen(code);
    const int code_width = (int)(((code_length * 6U) - 1U));
    return draw_text(RYZ_DISPLAY_WIDTH - 8 - code_width, 12, code,
                     COLOR_RYZOBEE_ORANGE, 1);
}

static esp_err_t draw_status_card(int y, const char *title,
                                  const char *detail, bool pressed,
                                  bool accent_detail)
{
    const uint16_t fill = pressed ? COLOR_RYZOBEE_ORANGE : COLOR_PANEL;
    const uint16_t border = pressed ? COLOR_RYZOBEE_ORANGE : COLOR_BORDER;
    const uint16_t primary = pressed ? COLOR_BLACK : COLOR_WHITE;
    const uint16_t secondary = pressed ? COLOR_BLACK :
                               accent_detail ? COLOR_RYZOBEE_ORANGE :
                                               COLOR_MUTED;
    const uint16_t accent = pressed ? COLOR_BLACK : COLOR_RYZOBEE_ORANGE;
    esp_err_t error = draw_outlined_panel(
        CARD_X, y, CARD_WIDTH, CARD_HEIGHT, fill, border);
    if (error != ESP_OK) return error;
    error = ryz_display_rect(CARD_X + 8, y + 12, 4, 48, accent);
    if (error != ESP_OK) return error;
    error = draw_text(CARD_X + 22, y + 18, title, primary, 2);
    if (error != ESP_OK) return error;
    error = draw_text(CARD_X + 22, y + 49, detail, secondary, 1);
    if (error != ESP_OK) return error;
    return draw_text(CARD_X + CARD_WIDTH - 22, y + 25, ">", accent, 2);
}
#endif

static unsigned storage_used_percent(const ryz_system_ui_snapshot_t *snapshot)
{
    if (!snapshot->storage_ready || snapshot->storage_total_bytes == 0) {
        return 0;
    }
    if (snapshot->storage_used_bytes >= snapshot->storage_total_bytes) {
        return 100;
    }
    const uint64_t numerator =
        ((uint64_t)snapshot->storage_used_bytes * 100U) +
        (snapshot->storage_total_bytes / 2U);
    return (unsigned)(numerator / snapshot->storage_total_bytes);
}

static esp_err_t draw_home_storage(const ryz_system_ui_snapshot_t *snapshot)
{
    char usage[16];
    const unsigned percent = storage_used_percent(snapshot);
    if (!snapshot->storage_ready || snapshot->storage_total_bytes == 0) {
        snprintf(usage, sizeof(usage), "--%% USED");
    } else {
        snprintf(usage, sizeof(usage), "%u%% USED", percent);
    }

    esp_err_t error = ryz_display_rect(
        STORAGE_USAGE_X, STORAGE_USAGE_Y, STORAGE_USAGE_WIDTH,
        STORAGE_USAGE_HEIGHT, COLOR_CANVAS);
    if (error != ESP_OK) return error;
    const size_t usage_length = strlen(usage);
    const int usage_width = (int)((usage_length * 6U) - 1U);
    error = draw_text(
        STORAGE_USAGE_X + STORAGE_USAGE_WIDTH - usage_width,
        STORAGE_USAGE_Y + 3, usage, COLOR_MUTED, 1);
    if (error != ESP_OK) return error;

    error = ryz_display_rect(
        STORAGE_TRACK_X, STORAGE_TRACK_Y, STORAGE_TRACK_WIDTH,
        STORAGE_TRACK_HEIGHT, COLOR_BORDER);
    if (error != ESP_OK) return error;
    const int fill_width =
        (int)(((STORAGE_TRACK_WIDTH * percent) + 50U) / 100U);
    if (fill_width > 0) {
        error = ryz_display_rect(
            STORAGE_TRACK_X, STORAGE_TRACK_Y, fill_width,
            STORAGE_TRACK_HEIGHT, COLOR_RYZOBEE_ORANGE);
        if (error != ESP_OK) return error;
    }
    const int segments[] = {64, 120, 176};
    for (size_t index = 0;
         index < sizeof(segments) / sizeof(segments[0]); ++index) {
        error = ryz_display_rect(
            segments[index], STORAGE_TRACK_Y, 1, STORAGE_TRACK_HEIGHT,
            COLOR_CANVAS);
        if (error != ESP_OK) return error;
    }
    return ESP_OK;
}

#ifndef ESP_PLATFORM
static esp_err_t draw_home(const ryz_system_ui_snapshot_t *snapshot)
{
    const char *network_detail = "NOT SET";
    if (snapshot->state == RYZ_SYSTEM_UI_NETWORK_OFF) {
        network_detail = "WI-FI OFF";
    } else if (snapshot->state == RYZ_SYSTEM_UI_NETWORK_STOPPING) {
        network_detail = "STOPPING WI-FI";
    } else if (snapshot->connected ||
        snapshot->state == RYZ_SYSTEM_UI_NETWORK_ONLINE) {
        network_detail = "ONLINE";
    } else if (snapshot->state == RYZ_SYSTEM_UI_NETWORK_FAILED) {
        network_detail = "CONNECTION FAILED";
    } else if (snapshot->state == RYZ_SYSTEM_UI_NETWORK_CONNECTING) {
        network_detail = "CONNECTING...";
    } else if (snapshot->state ==
               RYZ_SYSTEM_UI_NETWORK_CREDENTIALS_RECEIVED) {
        network_detail = "SAVED";
    } else if (snapshot->configured) {
        network_detail = "NOT CONNECTED";
    }

    esp_err_t error = draw_status_card(
        NETWORK_CARD_Y, "NETWORK", network_detail, false, false);
    if (error != ESP_OK) return error;
    error = draw_text(26, STORAGE_USAGE_Y + 3, "STORAGE", COLOR_WHITE, 1);
    if (error != ESP_OK) return error;
    return draw_home_storage(snapshot);
}
#endif

static esp_err_t draw_qr_matrix(void *context, int side_length,
                                ryz_qr_module_reader_t read_module,
                                const void *matrix)
{
    (void)context;
    if (side_length <= 0 || read_module == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const int total_modules = side_length + (2 * QR_QUIET_ZONE_MODULES);
    const int scale = QR_REGION_SIZE / total_modules;
    if (scale < 1) {
        return ESP_ERR_INVALID_ARG;
    }
    const int rendered_size = total_modules * scale;
    const int origin_x = QR_REGION_X + ((QR_REGION_SIZE - rendered_size) / 2);
    const int origin_y = QR_REGION_Y + ((QR_REGION_SIZE - rendered_size) / 2);

    esp_err_t error = ryz_display_rect(QR_REGION_X, QR_REGION_Y,
                                       QR_REGION_SIZE, QR_REGION_SIZE,
                                       COLOR_CANVAS);
    if (error != ESP_OK) {
        return error;
    }
    for (int y = 0; y < side_length; ++y) {
        for (int x = 0; x < side_length; ++x) {
            if (!read_module(matrix, x, y)) {
                continue;
            }
            error = ryz_display_rect(
                origin_x + ((x + QR_QUIET_ZONE_MODULES) * scale),
                origin_y + ((y + QR_QUIET_ZONE_MODULES) * scale),
                scale, scale, COLOR_WHITE);
            if (error != ESP_OK) {
                return error;
            }
        }
    }
    return ESP_OK;
}

static esp_err_t draw_qr_focus(void)
{
    static const int outer[][4] = {
        {74, 54, 22, 2}, {74, 54, 2, 22},
        {192, 54, 22, 2}, {212, 54, 2, 22},
        {74, 192, 22, 2}, {74, 172, 2, 22},
        {192, 192, 22, 2}, {212, 172, 2, 22},
    };
    static const int inner[][4] = {
        {78, 58, 18, 1}, {78, 58, 1, 18},
        {192, 58, 18, 1}, {209, 58, 1, 18},
        {78, 189, 18, 1}, {78, 172, 1, 18},
        {192, 189, 18, 1}, {209, 172, 1, 18},
    };
    for (size_t i = 0; i < sizeof(outer) / sizeof(outer[0]); ++i) {
        esp_err_t error = ryz_display_rect(
            outer[i][0], outer[i][1], outer[i][2], outer[i][3],
            COLOR_RYZOBEE_ORANGE);
        if (error != ESP_OK) return error;
    }
    for (size_t i = 0; i < sizeof(inner) / sizeof(inner[0]); ++i) {
        esp_err_t error = ryz_display_rect(
            inner[i][0], inner[i][1], inner[i][2], inner[i][3],
            COLOR_BORDER);
        if (error != ESP_OK) return error;
    }
    return ESP_OK;
}

static esp_err_t draw_ap_setup(
    const ryz_system_ui_snapshot_t *network)
{
    const char *status = "STARTING AP";
    const char *detail = "PLEASE WAIT";
    const bool portal_ready = network->ap_active &&
        network->state != RYZ_SYSTEM_UI_NETWORK_OFF &&
        network->state != RYZ_SYSTEM_UI_NETWORK_STOPPING &&
        network->state != RYZ_SYSTEM_UI_NETWORK_FAILED;
    if (network->state == RYZ_SYSTEM_UI_NETWORK_OFF) {
        status = "WI-FI OFF";
        detail = "RADIO STOPPED";
    } else if (network->state == RYZ_SYSTEM_UI_NETWORK_STOPPING) {
        status = "STOPPING WI-FI";
        detail = "PLEASE WAIT";
    } else if (portal_ready ||
        network->state == RYZ_SYSTEM_UI_NETWORK_AP_READY) {
        status = "AP READY";
        detail = "SCAN TO CONNECT";
    } else if (network->state == RYZ_SYSTEM_UI_NETWORK_CONNECTING) {
        status = "CONNECTING";
        detail = "JOINING NETWORK";
    } else if (network->state == RYZ_SYSTEM_UI_NETWORK_ONLINE) {
        status = "ONLINE";
        detail = "SYSTEM READY";
    } else if (network->state == RYZ_SYSTEM_UI_NETWORK_FAILED) {
        status = "SETUP FAILED";
        detail = "OPEN SETTINGS";
    } else if (network->state ==
               RYZ_SYSTEM_UI_NETWORK_CREDENTIALS_RECEIVED) {
        status = "CREDENTIALS OK";
        detail = "CONNECTING SOON";
    }
    if (portal_ready && network->ap_ssid[0] != '\0' &&
        network->ap_password[0] != '\0') {
        char payload[WIFI_QR_PAYLOAD_CAPACITY];
        size_t cursor = 0;
        const char *parts[] = {"WIFI:T:WPA;S:", network->ap_ssid,
                               ";P:", network->ap_password, ";;"};
        const bool escaped[] = {false, true, false, true, false};
        for (size_t part = 0; part < sizeof(parts) / sizeof(parts[0]); ++part) {
            const size_t limit = escaped[part] ?
                                     (part == 1 ?
                                          RYZ_SYSTEM_UI_SSID_MAX_LENGTH :
                                          RYZ_SYSTEM_UI_PASSWORD_MAX_LENGTH) :
                                     WIFI_QR_PAYLOAD_CAPACITY - 1;
            size_t length = 0;
            while (length <= limit && parts[part][length] != '\0') {
                ++length;
            }
            if (length > limit) {
                return ESP_ERR_INVALID_ARG;
            }
            for (size_t i = 0; i < length; ++i) {
                const char value = parts[part][i];
                const bool needs_escape = escaped[part] &&
                    (value == '\\' || value == ';' || value == ',' ||
                     value == '"' || value == ':');
                if (cursor + (needs_escape ? 2U : 1U) >= sizeof(payload)) {
                    return ESP_ERR_INVALID_ARG;
                }
                if (needs_escape) {
                    payload[cursor++] = '\\';
                }
                payload[cursor++] = value;
            }
        }
        payload[cursor] = '\0';

        esp_err_t error = ryz_qr_encode_text(
            payload, draw_qr_matrix, NULL);
        if (error != ESP_OK) return error;
        error = draw_qr_focus();
        if (error != ESP_OK) return error;
        error = draw_text(78, 199, "SSID", COLOR_RYZOBEE_ORANGE, 1);
        if (error != ESP_OK) return error;
        error = draw_left_bounded_text(
            116, 199, RYZ_DISPLAY_WIDTH - 116, network->ap_ssid,
            RYZ_SYSTEM_UI_SSID_MAX_LENGTH, COLOR_MUTED, 1);
        if (error != ESP_OK) return error;
        if (network->portal_ip[0] != '\0') {
            error = draw_text(78, 219, "IP", COLOR_MUTED, 1);
            if (error != ESP_OK) return error;
            error = draw_left_bounded_text(
                116, 219, RYZ_DISPLAY_WIDTH - 116, network->portal_ip,
                RYZ_SYSTEM_UI_IPV4_MAX_LENGTH, COLOR_MUTED, 1);
        }
        return error;
    }
    esp_err_t error = draw_outlined_panel(
        QR_REGION_X, QR_REGION_Y, QR_REGION_SIZE, QR_REGION_SIZE,
        COLOR_PANEL, COLOR_BORDER);
    if (error != ESP_OK) return error;
    error = draw_centered_bounded_text(
        105, status, 20, COLOR_WHITE, 1);
    if (error != ESP_OK) return error;
    error = draw_centered_bounded_text(
        125, detail, 20, COLOR_MUTED, 1);
    if (error != ESP_OK) return error;
    return draw_qr_focus();
}

#ifndef ESP_PLATFORM
static esp_err_t draw_app_tile(int x, int y, bool installed, bool disabled)
{
    const uint16_t border = disabled ? COLOR_BORDER :
                            installed ? COLOR_RYZOBEE_ORANGE : COLOR_BORDER;
    const uint16_t primary = disabled ? COLOR_BORDER :
                             installed ? COLOR_WHITE : COLOR_MUTED;
    const uint16_t accent = disabled ? COLOR_BORDER :
                            installed ? COLOR_RYZOBEE_ORANGE : COLOR_MUTED;
    esp_err_t error = draw_outlined_panel(
        x, y, APP_TILE_WIDTH, APP_TILE_HEIGHT,
        installed ? COLOR_PANEL : COLOR_BLACK, border);
    if (error != ESP_OK) return error;
    error = draw_text(x + 9, y + 17,
                      installed ? "APP / 01" : "SLOT / --",
                      accent, 1);
    if (error != ESP_OK) return error;
    return draw_text(x + 9, y + 41,
                     installed ? "LUA TOOL" : "EMPTY",
                     primary, 1);
}

static esp_err_t draw_apps(void)
{
    esp_err_t error = draw_app_tile(8, 40, true, true);
    if (error != ESP_OK) return error;
    error = draw_app_tile(128, 40, false, false);
    if (error != ESP_OK) return error;
    error = draw_app_tile(8, 112, false, false);
    if (error != ESP_OK) return error;
    return draw_app_tile(128, 112, false, true);
}

static esp_err_t draw_action_row(int y, const char *number,
                                 const char *label, bool emphasized,
                                 bool pressed, bool disabled)
{
    const uint16_t fill = pressed ? COLOR_RYZOBEE_ORANGE : COLOR_PANEL;
    const uint16_t border = emphasized && !disabled ? COLOR_RYZOBEE_ORANGE :
                                                     COLOR_BORDER;
    const uint16_t primary = pressed ? COLOR_BLACK :
                             disabled ? COLOR_MUTED :
                             emphasized ? COLOR_RYZOBEE_ORANGE : COLOR_WHITE;
    const uint16_t accent = pressed ? COLOR_BLACK :
                            disabled ? COLOR_MUTED : COLOR_RYZOBEE_ORANGE;
    esp_err_t error = draw_outlined_panel(
        CARD_X, y, CARD_WIDTH, SETTINGS_ROW_HEIGHT, fill, border);
    if (error != ESP_OK) return error;
    error = draw_text(CARD_X + 9, y + 17, number, accent, 1);
    if (error != ESP_OK) return error;
    error = draw_text(CARD_X + 32, y + 16, label, primary, 1);
    if (error != ESP_OK) return error;
    return draw_text(CARD_X + 205, y + 16, ">", accent, 1);
}

static esp_err_t draw_settings(void)
{
    esp_err_t error = draw_action_row(
        32, "01", "NETWORK", false,
        s_pressed_target == TARGET_SETTINGS_NETWORK, false);
    if (error != ESP_OK) return error;
    error = draw_action_row(84, "02", "DISPLAY", false, false, true);
    if (error != ESP_OK) return error;
    return draw_action_row(
        136, "04", "REPROVISION", true,
        s_pressed_target == TARGET_REPROVISION, false);
}

static ryz_system_ui_route_t highlighted_tab(void)
{
    if (s_pressed_target == TARGET_HOME) {
        return RYZ_SYSTEM_UI_ROUTE_HOME;
    }
    if (s_pressed_target == TARGET_APPS) {
        return RYZ_SYSTEM_UI_ROUTE_APPS;
    }
    if (s_pressed_target == TARGET_SETTINGS) {
        return RYZ_SYSTEM_UI_ROUTE_SETTINGS;
    }
    if (s_route == RYZ_SYSTEM_UI_ROUTE_AP_SETUP) {
        return RYZ_SYSTEM_UI_ROUTE_HOME;
    }
    return s_route;
}

static esp_err_t draw_bottom_navigation(void)
{
    const ryz_system_ui_route_t highlighted = highlighted_tab();
    const ryz_system_ui_route_t routes[] = {
        RYZ_SYSTEM_UI_ROUTE_HOME,
        RYZ_SYSTEM_UI_ROUTE_APPS,
        RYZ_SYSTEM_UI_ROUTE_SETTINGS,
    };
    const char *labels[] = {"HOME", "APPS", "SET"};
    esp_err_t error = ryz_display_rect(
        0, NAV_Y, RYZ_DISPLAY_WIDTH, NAV_HEIGHT, COLOR_BLACK);
    if (error != ESP_OK) return error;
    error = ryz_display_rect(0, NAV_Y, RYZ_DISPLAY_WIDTH, 1, COLOR_BORDER);
    if (error != ESP_OK) return error;
    error = ryz_display_rect(NAV_TAB_WIDTH, NAV_Y, 1, NAV_HEIGHT,
                             COLOR_BORDER);
    if (error != ESP_OK) return error;
    error = ryz_display_rect(2 * NAV_TAB_WIDTH, NAV_Y, 1, NAV_HEIGHT,
                             COLOR_BORDER);
    if (error != ESP_OK) return error;
    for (size_t index = 0; index < 3; ++index) {
        const int x = (int)index * NAV_TAB_WIDTH;
        const bool selected = highlighted == routes[index];
        const uint16_t color = selected ? COLOR_WHITE : COLOR_MUTED;
        if (selected) {
            error = ryz_display_rect(
                x, NAV_Y + NAV_HEIGHT - 2, NAV_TAB_WIDTH, 2,
                COLOR_RYZOBEE_ORANGE);
            if (error != ESP_OK) return error;
        }
        const size_t label_length = strlen(labels[index]);
        const int label_width = (int)((label_length * 6U) - 1U);
        error = draw_text(x + ((NAV_TAB_WIDTH - label_width) / 2),
                          NAV_Y + 17, labels[index], color, 1);
        if (error != ESP_OK) return error;
    }
    return ESP_OK;
}
#endif

#endif /* Legacy Host diagnostic renderer, never compiled into native UI. */

void ryz_system_ui_reset(void)
{ (void)ryz_system_ui_reset_checked(); }

esp_err_t ryz_system_ui_reset_checked(void)
{
    esp_err_t error=ESP_OK;
    s_home_selection.metric=RYZ_HOME_METRIC_NET;
    ++s_home_selection.generation;
    s_home_selection.selected_at_us=0;
#ifdef RYZ_UI_NATIVE_V5
    s_home_selection.selected_at_us=(uint64_t)esp_timer_get_time();
    memset(&s_swipe, 0, sizeof(s_swipe));
    memset(&s_info_drag, 0, sizeof(s_info_drag));
    ryz_v5_apps_reset();
    ryz_v5_display_reset();
    s_version_pressed_attempt=s_version_presented_attempt=0;
    s_version_presented_restart=false;
    ble_leave_flow();
    scripts_reset_input();
    error=ryz_v5_release();
#endif
    (void)ryz_ui_nav_reset(&s_navigation, RYZ_SYSTEM_UI_ROUTE_HOME + 1U);
    s_pressed_target = TARGET_NONE;
    s_pending_action = RYZ_SYSTEM_UI_ACTION_NONE;
    s_wait_for_release = true;
    s_presented = false;
    s_contact_active = false;
    return error;
}

ryz_system_ui_route_t ryz_system_ui_current_route(void)
{
    const uint32_t route = ryz_ui_nav_current(&s_navigation).route;
    return route ? (ryz_system_ui_route_t)(route - 1U) :
                   RYZ_SYSTEM_UI_ROUTE_HOME;
}

ryz_ui_nav_token_t ryz_system_ui_page_token(void)
{
    return ryz_ui_nav_current(&s_navigation);
}

uint32_t ryz_system_ui_render_revision(void)
{
    return s_render_revision;
}

void ryz_system_ui_invalidate_input(void)
{
#ifdef RYZ_UI_NATIVE_V5
    ryz_v5_display_cancel_gesture();
    memset(&s_swipe, 0, sizeof(s_swipe));
    memset(&s_info_drag, 0, sizeof(s_info_drag));
    ryz_v5_apps_reset();
    s_version_pressed_attempt=s_version_presented_attempt=0;
    s_version_presented_restart=false;
    ble_reset_input();
    scripts_reset_input();
    (void)ryz_v5_password_revoke();
    if(ryz_v5_needs_cleanup()) (void)ryz_v5_release();
#endif
    (void)ryz_ui_nav_invalidate(&s_navigation);
    s_pressed_target = TARGET_NONE;
    s_pending_action = RYZ_SYSTEM_UI_ACTION_NONE;
    s_wait_for_release = true;
    s_presented = false;
    s_contact_active = false;
}

esp_err_t ryz_system_ui_navigate(ryz_ui_nav_token_t expected,
                                ryz_system_ui_route_t route)
{
#ifdef RYZ_UI_NATIVE_V5
    if (route < RYZ_SYSTEM_UI_ROUTE_HOME || route >= RYZ_SYSTEM_UI_ROUTE_COUNT)
        return ESP_ERR_INVALID_ARG;
    bool modal = route == RYZ_SYSTEM_UI_ROUTE_BLE_REPLACE ||
                 route == RYZ_SYSTEM_UI_ROUTE_BLE_FORGET ||
                 route == RYZ_SYSTEM_UI_ROUTE_SCRIPTS_DELETE_ALL;
    ryz_system_ui_route_t current = s_route;
    /* Opening setup from the menu is a child. Returning there after a
     * failed/retried attempt is a state replacement in that same flow;
     * pushing would make Back revisit FAILED and immediately reopen AP. */
    bool ap_state_transition = route == RYZ_SYSTEM_UI_ROUTE_AP_SETUP &&
        (current == RYZ_SYSTEM_UI_ROUTE_WIFI_FAILED || current == RYZ_SYSTEM_UI_ROUTE_WIFI_JOINING);
    bool child = route == RYZ_SYSTEM_UI_ROUTE_WIFI_INFO ||
                 route == RYZ_SYSTEM_UI_ROUTE_WIFI_SERVER ||
                 (route == RYZ_SYSTEM_UI_ROUTE_AP_SETUP && !ap_state_transition) ||
                 route == RYZ_SYSTEM_UI_ROUTE_BLE_INFO ||
                 route == RYZ_SYSTEM_UI_ROUTE_SCRIPTS_PICKER ||
                 route == RYZ_SYSTEM_UI_ROUTE_VERSION_INFO;
    int family = route <= RYZ_SYSTEM_UI_ROUTE_SETTINGS ? 0 :
        route <= RYZ_SYSTEM_UI_ROUTE_WIFI_NO_NETWORK ? 1 :
        route <= RYZ_SYSTEM_UI_ROUTE_BLE_FORGET ? 2 :
        route <= RYZ_SYSTEM_UI_ROUTE_SCRIPTS_DELETE_ALL ? 3 :
        route <= RYZ_SYSTEM_UI_ROUTE_VERSION_FAILED ? 4 : 5;
    int prior = current <= RYZ_SYSTEM_UI_ROUTE_SETTINGS ? 0 :
        current <= RYZ_SYSTEM_UI_ROUTE_WIFI_NO_NETWORK ? 1 :
        current <= RYZ_SYSTEM_UI_ROUTE_BLE_FORGET ? 2 :
        current <= RYZ_SYSTEM_UI_ROUTE_SCRIPTS_DELETE_ALL ? 3 :
        current <= RYZ_SYSTEM_UI_ROUTE_VERSION_FAILED ? 4 : 5;
    const ryz_ui_nav_command_t command = !family ? RYZ_UI_NAV_ROOT :
        modal ? RYZ_UI_NAV_PRESENT : family == prior && !child ?
            RYZ_UI_NAV_REPLACE : RYZ_UI_NAV_PUSH;
#else
    if (route < RYZ_SYSTEM_UI_ROUTE_HOME ||
        route > RYZ_SYSTEM_UI_ROUTE_AP_SETUP) return ESP_ERR_INVALID_ARG;
    const ryz_ui_nav_command_t command =
        route == RYZ_SYSTEM_UI_ROUTE_AP_SETUP ? RYZ_UI_NAV_PUSH : RYZ_UI_NAV_ROOT;
#endif
    const ryz_ui_nav_result_t result = ryz_ui_nav_apply(
        &s_navigation, expected, command, (uint32_t)route + 1U);
    if (result != RYZ_UI_NAV_OK) return ESP_ERR_INVALID_STATE;
#ifdef RYZ_UI_NATIVE_V5
    ble_reset_input();
    if(!ble_route(route)) ble_leave_flow();
    if(route==RYZ_SYSTEM_UI_ROUTE_BLE_REPLACE || route==RYZ_SYSTEM_UI_ROUTE_BLE_FORGET) {
        ryz_v5_status_t state;
        ryz_v5_status_get(&state);
        s_ble_confirmation_operation=state.ble.operation_id;
        s_ble_confirmation_valid=true;
        s_ble_modal_submitted=false;
    } else s_ble_confirmation_valid=false;
    memset(&s_swipe, 0, sizeof(s_swipe));
    memset(&s_info_drag, 0, sizeof(s_info_drag));
    ryz_v5_detail_scroll_reset(route);
    ryz_v5_display_reset();
    if(route==RYZ_SYSTEM_UI_ROUTE_DISPLAY) (void)ryz_v5_display_tick();
    ryz_v5_apps_reset();
    scripts_reset_input();
    (void)ryz_v5_password_revoke();
    if(ryz_v5_needs_cleanup()) (void)ryz_v5_release();
#endif
    s_pressed_target = TARGET_NONE;
    s_pending_action = RYZ_SYSTEM_UI_ACTION_NONE;
    s_wait_for_release = true;
    s_presented = false;
    s_contact_active = false;
    return ESP_OK;
}

ryz_system_ui_action_t ryz_system_ui_take_action(void)
{
    const ryz_system_ui_action_t action = s_presented &&
        ryz_ui_nav_matches(&s_navigation, s_action_page) ?
        s_pending_action : RYZ_SYSTEM_UI_ACTION_NONE;
    s_pending_action = RYZ_SYSTEM_UI_ACTION_NONE;
    return action;
}

esp_err_t ryz_system_ui_render(
    const ryz_system_ui_snapshot_t *network)
{
    return ryz_system_ui_render_checked(network, NULL, NULL, NULL);
}

static esp_err_t render_page(
    const ryz_system_ui_snapshot_t *network,
    bool (*cancelled)(void *), void *context, bool *cancelled_out)
{
    if (cancelled_out) *cancelled_out = false;
    if (network == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

#ifdef RYZ_UI_NATIVE_V5
    if(s_route==RYZ_SYSTEM_UI_ROUTE_HOME &&
       (network->home_selection.metric!=s_home_selection.metric ||
        network->home_selection.generation!=s_home_selection.generation)) {
        /* Touch is processed before the owner's next telemetry publication.
         * Never render an old recording under the newly selected metric. */
        ryz_system_ui_snapshot_t home=*network;
        home.home_selection=s_home_selection;
        home.home_value_valid=false;
        home.home_value=0;
        home.home_history_count=0;
        memset(home.home_history,0,sizeof(home.home_history));
        return ryz_v5_render(s_route,&home,cancelled,context,cancelled_out);
    }
    return ryz_v5_render(s_route, network, cancelled, context, cancelled_out);
#else

    esp_err_t error = ryz_display_clear(COLOR_BLACK);
    if (error != ESP_OK) {
        return error;
    }
#ifdef ESP_PLATFORM
    error = draw_figma_screen(s_route);
    if (error != ESP_OK) return error;
    if (s_route == RYZ_SYSTEM_UI_ROUTE_HOME) {
        error = draw_home_storage(network);
        if (error != ESP_OK) return error;
    } else if (s_route == RYZ_SYSTEM_UI_ROUTE_AP_SETUP) {
        error = ryz_display_rect(74, 195, 166, 36, COLOR_CANVAS);
        if (error != ESP_OK) return error;
        error = draw_ap_setup(network);
        if (error != ESP_OK) return error;
    }
#else
    error = draw_top_bar();
    if (error != ESP_OK) return error;
    if (s_route == RYZ_SYSTEM_UI_ROUTE_APPS) {
        error = draw_apps();
    } else if (s_route == RYZ_SYSTEM_UI_ROUTE_SETTINGS) {
        error = draw_settings();
    } else if (s_route == RYZ_SYSTEM_UI_ROUTE_AP_SETUP) {
        error = draw_ap_setup(network);
    } else {
        error = draw_home(network);
    }
    if (error != ESP_OK) {
        return error;
    }
    if (s_route != RYZ_SYSTEM_UI_ROUTE_AP_SETUP) {
        error = draw_bottom_navigation();
        if (error != ESP_OK) return error;
    }
#endif
    return ryz_display_show_checked_status(
        cancelled, context, cancelled_out);
#endif
}

esp_err_t ryz_system_ui_render_checked(
    const ryz_system_ui_snapshot_t *network,
    bool (*cancelled)(void *), void *context, bool *cancelled_out)
{
    const ryz_ui_nav_token_t page = ryz_system_ui_page_token();
#ifdef RYZ_UI_NATIVE_V5
    ryz_v5_status_t before;
    ryz_v5_status_get(&before);
#endif
    /* A synchronous transfer callback is not a successful presentation yet. */
    s_presented = false;
    esp_err_t error = render_page(network, cancelled, context, cancelled_out);
    if (error != ESP_OK) {
        ryz_system_ui_invalidate_input();
        return error;
    }
    /* A failed/partially transferred frame is never an interactive frame.
     * This also rejects a render callback which invalidated its generation. */
    s_presented = ryz_ui_nav_matches(&s_navigation, page);
    if(!s_presented) {
        ryz_system_ui_invalidate_input();
        return ESP_ERR_INVALID_STATE;
    }
#ifdef RYZ_UI_NATIVE_V5
    if(ble_route(s_route)) {
        s_ble_presented=before.ble;
        s_ble_presented_valid=true;
        ble_recheck_capture(&before.ble);
    } else s_ble_presented_valid=false;
    s_version_presented_attempt=s_route==RYZ_SYSTEM_UI_ROUTE_VERSION_REBOOT?
        before.version.attempt_id:0;
    s_version_presented_restart=s_route==RYZ_SYSTEM_UI_ROUTE_VERSION_REBOOT &&
        version_restart_ready(&before.version);
    /* A capability loss observed during a contact permanently cancels it;
     * restoring readiness before UP must not revive the old gesture. */
    if(s_pressed_target==(target_t)(TARGET_VERSION_BASE+RYZ_V5_VERSION_ACTION_RESTART) &&
       (!s_version_presented_restart || s_version_pressed_attempt!=s_version_presented_attempt))
        s_pressed_target=TARGET_NONE;
    if(scripts_route()) {
        ryz_v5_scripts_snapshot_t scripts;
        ryz_v5_scripts_route_snapshot(s_route,&scripts);
        s_scripts_presented_generation=scripts.generation;
        s_scripts_presented_revision=scripts.catalog.revision;
    }
#endif
    ++s_render_revision;
    return ESP_OK;
}

static target_t target_at(uint16_t x, uint16_t y,
                          const ryz_system_ui_snapshot_t *network)
{
    (void)network;
    if (x >= RYZ_DISPLAY_WIDTH || y >= RYZ_DISPLAY_HEIGHT) {
        return TARGET_NONE;
    }
#ifdef RYZ_UI_NATIVE_V5
    if(s_route==RYZ_SYSTEM_UI_ROUTE_SCRIPT_FAULT) {
        unsigned action=ryz_v5_fault_hit(x,y);
        return action==1?TARGET_FAULT_HOME:action==2?TARGET_FAULT_NEXT:TARGET_NONE;
    }
    if (s_route >= RYZ_SYSTEM_UI_ROUTE_AP_SETUP) {
        if (x < 44 && y < 32) {
            if(s_route>=RYZ_SYSTEM_UI_ROUTE_BLE_PAIRED && s_route<=RYZ_SYSTEM_UI_ROUTE_BLE_FORGET) {
                unsigned action=ryz_v5_ble_route_hit(s_route,x,y);
                return action?(target_t)(TARGET_BLE_BASE+action):TARGET_NONE;
            }
            return TARGET_BACK;
        }
        unsigned action = 0;
        if (s_route <= RYZ_SYSTEM_UI_ROUTE_WIFI_NO_NETWORK) {
            action = ryz_v5_network_route_hit(s_route,x,y);
            return action ? (target_t)(TARGET_NETWORK_BASE+action) : TARGET_NONE;
        }
        if (s_route <= RYZ_SYSTEM_UI_ROUTE_BLE_FORGET) {
            action = ryz_v5_ble_route_hit(s_route,x,y);
            return action ? (target_t)(TARGET_BLE_BASE+action) : TARGET_NONE;
        }
        if (s_route <= RYZ_SYSTEM_UI_ROUTE_SCRIPTS_DELETE_ALL) {
            return TARGET_NONE; /* Dedicated pointer preserves the full intent. */
        }
        action = ryz_v5_version_route_hit(s_route,x,y);
        return action ? (target_t)(TARGET_VERSION_BASE+action) : TARGET_NONE;
    }
    if (s_route == RYZ_SYSTEM_UI_ROUTE_APPS) {
        if (x < 44 && y < 32 && ryz_v5_apps_has_back()) return TARGET_BACK;
        if (!ryz_v5_apps_has_navigation()) return TARGET_NONE;
    }
#endif
    if (s_route == RYZ_SYSTEM_UI_ROUTE_AP_SETUP && x < BACK_WIDTH &&
        y < BACK_HEIGHT) {
        return TARGET_BACK;
    }
    if (s_route != RYZ_SYSTEM_UI_ROUTE_AP_SETUP && y >= NAV_Y) {
        if (x < NAV_TAB_WIDTH) {
            return TARGET_HOME;
        }
        if (x < 2 * NAV_TAB_WIDTH) {
            return TARGET_APPS;
        }
        return TARGET_SETTINGS;
    }
#ifdef RYZ_UI_NATIVE_V5
    if (s_route == RYZ_SYSTEM_UI_ROUTE_SETTINGS && x >= CARD_X &&
        x < CARD_X + CARD_WIDTH && y >= 32 && y < 196) {
        unsigned row=(y-32)/33;
        const target_t targets[]={TARGET_SETTINGS_NETWORK,TARGET_SETTINGS_BLE,
            TARGET_SETTINGS_SCRIPTS,TARGET_SETTINGS_DISPLAY,TARGET_SETTINGS_VERSION};
        return targets[row];
    }
#else
    if (s_route == RYZ_SYSTEM_UI_ROUTE_SETTINGS && x >= CARD_X &&
        x < CARD_X + CARD_WIDTH && y >= 32 &&
        y < 32 + SETTINGS_ROW_HEIGHT) {
        return TARGET_SETTINGS_NETWORK;
    }
    if (s_route == RYZ_SYSTEM_UI_ROUTE_SETTINGS && x >= CARD_X &&
        x < CARD_X + CARD_WIDTH && y >= 136 &&
        y < 136 + SETTINGS_ROW_HEIGHT) {
        return TARGET_REPROVISION;
    }
#endif
    if (s_route == RYZ_SYSTEM_UI_ROUTE_HOME && x >= HOME_NETWORK_X &&
        x < HOME_NETWORK_X + HOME_NETWORK_WIDTH &&
        y >= HOME_NETWORK_Y && y < HOME_NETWORK_Y + HOME_NETWORK_HEIGHT) {
        return TARGET_HOME_NETWORK;
    }
    return TARGET_NONE;
}

#ifdef RYZ_UI_NATIVE_V5
static ryz_system_ui_route_t network_landing(const ryz_system_ui_snapshot_t *s)
{
    if(s->state==RYZ_SYSTEM_UI_NETWORK_OFF) return RYZ_SYSTEM_UI_ROUTE_WIFI_OFF;
    if(s->connected) return RYZ_SYSTEM_UI_ROUTE_WIFI_CONNECTED;
    if(s->state==RYZ_SYSTEM_UI_NETWORK_CONNECTING ||
       s->state==RYZ_SYSTEM_UI_NETWORK_CREDENTIALS_RECEIVED) return RYZ_SYSTEM_UI_ROUTE_WIFI_JOINING;
    if(s->state==RYZ_SYSTEM_UI_NETWORK_FAILED) return RYZ_SYSTEM_UI_ROUTE_WIFI_FAILED;
    return RYZ_SYSTEM_UI_ROUTE_WIFI_NO_NETWORK;
}

static void go_back(void)
{
    ryz_v5_display_reset();
    memset(&s_swipe,0,sizeof(s_swipe));
    memset(&s_info_drag,0,sizeof(s_info_drag));
    scripts_reset_input();
    (void)ryz_v5_password_revoke();
    if(ryz_v5_needs_cleanup()) (void)ryz_v5_release();
    if(ryz_ui_nav_apply(&s_navigation,s_pressed_page,RYZ_UI_NAV_BACK,0)!=RYZ_UI_NAV_OK)
        (void)ryz_ui_nav_reset(&s_navigation,RYZ_SYSTEM_UI_ROUTE_HOME+1U);
    ble_reset_input();
    s_ble_confirmation_valid=false;
    s_ble_modal_submitted=false;
    if(!ble_route(s_route)) ble_leave_flow();
    s_pressed_target=TARGET_NONE;
    s_pending_action=RYZ_SYSTEM_UI_ACTION_NONE;
    s_wait_for_release=true;
    s_presented=false;
    s_contact_active=false;
}

static esp_err_t ble_submit_action(ryz_v5_ble_action_t action)
{
    if(!s_ble_pressed_valid || s_ble_command_pending) return ESP_ERR_INVALID_STATE;
    ryz_v5_status_t state;
    ryz_v5_status_get(&state);
    const target_t target=(target_t)(TARGET_BLE_BASE+action);
    if(!ble_same_identity(&s_ble_pressed,&state.ble,target)) return ESP_ERR_INVALID_STATE;
    if((action==RYZ_V5_BLE_ACTION_CONFIRM_FORGET ||
        action==RYZ_V5_BLE_ACTION_CONFIRM_REPLACE) &&
       (!s_ble_confirmation_valid ||
        s_ble_confirmation_operation!=s_ble_pressed.operation_id)) return ESP_ERR_INVALID_STATE;
    const esp_err_t error=ryz_v5_ble_submit(action,s_ble_pressed.operation_id,
                                          s_ble_pressed.compare_value);
    if(error==ESP_OK) {
        s_ble_command_pending=true;
        s_ble_submitted=state.ble;
        if(action==RYZ_V5_BLE_ACTION_CONFIRM_FORGET ||
           action==RYZ_V5_BLE_ACTION_CONFIRM_REPLACE ||
           action==RYZ_V5_BLE_ACTION_RETRY_FORGET) s_ble_modal_submitted=true;
        if(action==RYZ_V5_BLE_ACTION_PAIR || action==RYZ_V5_BLE_ACTION_RETRY_PAIR ||
           action==RYZ_V5_BLE_ACTION_CONFIRM_REPLACE ||
           (action==RYZ_V5_BLE_ACTION_RETRY_FORGET && state.ble.replace_after_forget)) {
            s_ble_pair_requested=true;
            s_ble_pair_cancelled=false;
            s_ble_pair_baseline=state.ble.operation_id;
            s_ble_pair_operation=0;
        }
        if(action==RYZ_V5_BLE_ACTION_CONFIRM_CODE) {
            s_ble_pair_requested=true;
            s_ble_pair_cancelled=false;
            s_ble_pair_operation=state.ble.operation_id;
        }
        if(action==RYZ_V5_BLE_ACTION_CANCEL_PAIR || action==RYZ_V5_BLE_ACTION_REJECT_CODE ||
           action==RYZ_V5_BLE_ACTION_DISABLE) {
            s_ble_pair_requested=false;
            s_ble_pair_cancelled=true;
            s_ble_pair_operation=0;
        }
    }
    return error;
}

static void ble_activate(ryz_v5_ble_action_t action)
{
    switch(action) {
    case RYZ_V5_BLE_ACTION_INFO:
        (void)ryz_system_ui_navigate(s_pressed_page,RYZ_SYSTEM_UI_ROUTE_BLE_INFO); break;
    case RYZ_V5_BLE_ACTION_REPLACE:
        (void)ryz_system_ui_navigate(s_pressed_page,RYZ_SYSTEM_UI_ROUTE_BLE_REPLACE); break;
    case RYZ_V5_BLE_ACTION_FORGET:
        (void)ryz_system_ui_navigate(s_pressed_page,RYZ_SYSTEM_UI_ROUTE_BLE_FORGET); break;
    case RYZ_V5_BLE_ACTION_DONE: {
        ryz_v5_status_t state;
        ryz_v5_status_get(&state);
        s_ble_pair_requested=false;
        s_ble_pair_operation=0;
        (void)ryz_system_ui_navigate(s_pressed_page,ble_landing(&state.ble));
        break;
    }
    case RYZ_V5_BLE_ACTION_BACK:
        /* Before confirmation, modal Back only dismisses the question. Back
         * from an active pairing first admits cancellation for that identity. */
        if(s_route==RYZ_SYSTEM_UI_ROUTE_BLE_PAIRING &&
           (s_ble_pressed.pairing_active || s_ble_pressed.compare_pending)) {
            if(ble_submit_action(RYZ_V5_BLE_ACTION_CANCEL_PAIR)!=ESP_OK) break;
        }
        go_back(); break;
    case RYZ_V5_BLE_ACTION_NONE: break;
    default:
        (void)ble_submit_action(action);
        break;
    }
}

static void pending(ryz_system_ui_action_t action)
{ s_pending_action=action; s_action_page=s_pressed_page; }

static bool scripts_same_intent(const ryz_v5_scripts_intent_t *a,const ryz_v5_scripts_intent_t *b)
{
    return a->action==b->action && a->generation==b->generation && a->revision==b->revision &&
        a->index==b->index && a->offset==b->offset && a->limit==b->limit && a->scroll_y==b->scroll_y &&
        !strcmp(a->name,b->name) && !strcmp(a->sha256,b->sha256);
}

static ryz_v5_scripts_intent_t scripts_hit(const ryz_v5_scripts_snapshot_t *s,uint16_t x,uint16_t y)
{
    ryz_v5_scripts_intent_t intent={0};
    /* Back remains available while a new page waits for its service snapshot. */
    if(x<44 && y<32) intent.action=RYZ_V5_SCRIPTS_ACTION_BACK;
    else if(s->generation && s->generation==s_scripts_presented_generation &&
            s->catalog.revision==s_scripts_presented_revision)
        (void)ryz_v5_scripts_hit(s,x,y,&intent);
    return intent;
}

static void scripts_activate(const ryz_v5_scripts_intent_t *intent)
{
    switch(intent->action) {
    case RYZ_V5_SCRIPTS_ACTION_NONE: break;
    case RYZ_V5_SCRIPTS_ACTION_BACK: case RYZ_V5_SCRIPTS_ACTION_CANCEL:
    case RYZ_V5_SCRIPTS_ACTION_LATER: go_back(); break;
    case RYZ_V5_SCRIPTS_ACTION_PICKER:
        (void)ryz_system_ui_navigate(s_pressed_page,RYZ_SYSTEM_UI_ROUTE_SCRIPTS_PICKER); break;
    case RYZ_V5_SCRIPTS_ACTION_DELETE_CONFIRM:
        (void)ryz_system_ui_navigate(s_pressed_page,RYZ_SYSTEM_UI_ROUTE_SCRIPTS_DELETE_ALL); break;
    default: {
        esp_err_t error=s_scripts_binding.submit?
            s_scripts_binding.submit(s_scripts_binding.context,intent):ESP_ERR_NOT_SUPPORTED;
        ryz_v5_scripts_admission_error(intent->generation,error);
        break;
    }
    }
}

/* Preserve the full version-bound intent through a gesture. Comparing only
 * ACTION_SELECT would turn a row-to-row drag into an unintended selection. */
static esp_err_t scripts_pointer(const ryz_touch_sample_t *sample,const ryz_system_ui_snapshot_t *network)
{
    ryz_v5_scripts_snapshot_t state;
    ryz_v5_scripts_route_snapshot(s_route,&state);
    if(sample->event==RYZ_TOUCH_DOWN) {
        if(s_contact_active) {
            ryz_system_ui_invalidate_input();
            return ryz_system_ui_render(network);
        }
        s_contact_active=true;
        s_pressed_page=ryz_system_ui_page_token();
        s_scripts_contact_generation=state.generation;
        s_scripts_contact_revision=state.catalog.revision;
        s_scripts_down_x=s_capture_x=sample->x;
        s_scripts_down_y=s_capture_y=sample->y;
        s_scripts_last_y=sample->y;
        s_scripts_axis=0;
        s_scripts_cancelled=!sample->has_position;
        s_scripts_scroll_origin=sample->has_position && state.generation &&
            state.generation==s_scripts_presented_generation && state.catalog.revision==s_scripts_presented_revision &&
            state.page==RYZ_V5_SCRIPTS_PICKER &&
            sample->x>=8 && sample->x<232 && sample->y>=56 && sample->y<190;
        s_scripts_pressed=sample->has_position?scripts_hit(&state,sample->x,sample->y):
            (ryz_v5_scripts_intent_t){0};
        return ESP_OK;
    }
    if(!s_contact_active) return ESP_OK;
    bool same_view=ryz_ui_nav_matches(&s_navigation,s_pressed_page) &&
        state.generation==s_scripts_contact_generation && state.catalog.revision==s_scripts_contact_revision;
    bool same_scroll=ryz_ui_nav_matches(&s_navigation,s_pressed_page) &&
        state.page==RYZ_V5_SCRIPTS_PICKER && state.generation &&
        state.catalog.revision==s_scripts_contact_revision;
    if(!same_view) {
        s_scripts_cancelled=true;
        if(!same_scroll || s_scripts_axis!=2) s_scripts_scroll_origin=false;
    }
    if(sample->event==RYZ_TOUCH_NONE && !sample->pressed) {
        s_contact_active=false;
        s_scripts_pressed=(ryz_v5_scripts_intent_t){0};
        s_scripts_scroll_origin=false;
        return ESP_OK; /* Lost UP is release, never a click. */
    }
    bool scroll_changed=false;
    if(sample->event==RYZ_TOUCH_MOVE || (sample->event==RYZ_TOUCH_NONE && sample->has_position) ||
       (sample->event==RYZ_TOUCH_UP && sample->has_position)) {
        if(!sample->has_position) { s_scripts_cancelled=true; s_scripts_scroll_origin=false; return ESP_OK; }
        int dx=(int)sample->x-s_scripts_down_x, dy=(int)sample->y-s_scripts_down_y;
        int ax=dx<0?-dx:dx, ay=dy<0?-dy:dy;
        s_capture_x=sample->x; s_capture_y=sample->y;
        if((int64_t)dx*dx+(int64_t)dy*dy>=100) {
            s_scripts_cancelled=true;
            if(!s_scripts_axis) {
                if(ax*2>=ay*3) s_scripts_axis=1;
                else if(ay*2>=ax*3) s_scripts_axis=2;
            }
        }
        int delta=s_scripts_last_y-(int)sample->y;
        if(s_scripts_axis==2) s_scripts_last_y=sample->y;
        if(s_scripts_scroll_origin && same_scroll && s_scripts_axis==2) {
            ryz_v5_scripts_intent_t scroll;
            if(ryz_v5_scripts_scroll(&state,delta,&scroll)) {
                scripts_activate(&scroll);
                scroll_changed=true;
            }
        }
        if(s_scripts_axis==1 || sample->x>=240 || sample->y>=240) {
            s_scripts_cancelled=true; s_scripts_scroll_origin=false;
        }
        ryz_v5_scripts_intent_t moved=scripts_hit(&state,sample->x,sample->y);
        if(!scripts_same_intent(&moved,&s_scripts_pressed)) s_scripts_cancelled=true;
        if(sample->event!=RYZ_TOUCH_UP)
            return scroll_changed?ryz_system_ui_render(network):ESP_OK;
    }
    if(sample->event==RYZ_TOUCH_UP) {
        uint16_t x=sample->has_position?sample->x:s_capture_x;
        uint16_t y=sample->has_position?sample->y:s_capture_y;
        int dx=(int)x-s_scripts_down_x,dy=(int)y-s_scripts_down_y;
        ryz_v5_scripts_intent_t released=scripts_hit(&state,x,y), intent=s_scripts_pressed;
        bool activate=same_view && !s_scripts_cancelled && !release_is_swipe(sample) &&
            (int64_t)dx*dx+(int64_t)dy*dy<100 &&
            scripts_same_intent(&released,&intent) && intent.action!=RYZ_V5_SCRIPTS_ACTION_NONE;
        s_contact_active=false; s_scripts_pressed=(ryz_v5_scripts_intent_t){0}; s_scripts_scroll_origin=false;
        if(!activate) return scroll_changed?ryz_system_ui_render(network):ESP_OK;
        scripts_activate(&intent);
        esp_err_t error=ryz_system_ui_render(network);
        if(error==ESP_OK) s_wait_for_release=false;
        return error;
    }
    return ESP_OK;
}
#endif

static void activate_target(
    target_t target,
    const ryz_system_ui_snapshot_t *network)
{
    (void)network;
#ifdef RYZ_UI_NATIVE_V5
    if(target==TARGET_FAULT_HOME) {
        (void)ryz_system_ui_navigate(s_pressed_page,RYZ_SYSTEM_UI_ROUTE_HOME); return;
    }
    if(target==TARGET_FAULT_NEXT) { ryz_v5_fault_next(); return; }
    if(target==TARGET_SETTINGS_DISPLAY) {
        (void)ryz_system_ui_navigate(s_pressed_page,RYZ_SYSTEM_UI_ROUTE_DISPLAY); return;
    }
    /* HOME is a fixed dashboard again. WLAN keeps its original Wi-Fi entry;
     * CPU/load/PSRAM are readouts, not trend selection controls. */
    if(target==TARGET_HOME_NETWORK || target==TARGET_SETTINGS_NETWORK) {
        (void)ryz_system_ui_navigate(s_pressed_page,network_landing(network)); return;
    }
    if(target==TARGET_BACK) { go_back(); return; }
    if(target==TARGET_SETTINGS_BLE) {
        ryz_v5_status_t state;
        ryz_v5_status_get(&state);
        (void)ryz_system_ui_navigate(s_pressed_page,ble_landing(&state.ble)); return;
    }
    if(target==TARGET_SETTINGS_SCRIPTS) {
        (void)ryz_system_ui_navigate(s_pressed_page,RYZ_SYSTEM_UI_ROUTE_SCRIPTS_SETTINGS); return;
    }
    if(target==TARGET_SETTINGS_VERSION) {
        ryz_v5_status_t state;
        ryz_v5_status_get(&state);
        (void)ryz_system_ui_navigate(s_pressed_page,state.version.image_staged?
            RYZ_SYSTEM_UI_ROUTE_VERSION_REBOOT:RYZ_SYSTEM_UI_ROUTE_VERSION_CURRENT); return;
    }
    if(target>TARGET_NETWORK_BASE && target<TARGET_BLE_BASE) {
        switch(target-TARGET_NETWORK_BASE) {
        case RYZ_V5_NETWORK_ACTION_ON: pending(RYZ_SYSTEM_UI_ACTION_NETWORK_ON); break;
        case RYZ_V5_NETWORK_ACTION_OFF: pending(RYZ_SYSTEM_UI_ACTION_NETWORK_OFF); break;
        case RYZ_V5_NETWORK_ACTION_INFO:
            (void)ryz_system_ui_navigate(s_pressed_page,RYZ_SYSTEM_UI_ROUTE_WIFI_INFO); break;
        case RYZ_V5_NETWORK_ACTION_REPROVISION:
            /* Legacy UI enum only: the Owner submits non-destructive OPEN_AP.
             * The separate Info FORGET control never aliases this intent. */
            pending(RYZ_SYSTEM_UI_ACTION_REPROVISION); break;
        case RYZ_V5_NETWORK_ACTION_FILES:
            (void)ryz_system_ui_navigate(s_pressed_page,RYZ_SYSTEM_UI_ROUTE_WIFI_SERVER); break;
        case RYZ_V5_NETWORK_ACTION_BACK: go_back(); break;
        case RYZ_V5_NETWORK_ACTION_HOME:
            (void)ryz_system_ui_navigate(s_pressed_page,RYZ_SYSTEM_UI_ROUTE_HOME); break;
        case RYZ_V5_NETWORK_ACTION_CANCEL: pending(RYZ_SYSTEM_UI_ACTION_NETWORK_CANCEL); break;
        case RYZ_V5_NETWORK_ACTION_RETRY: pending(RYZ_SYSTEM_UI_ACTION_NETWORK_RETRY); break;
        default: break;
        }
        return;
    }
    if(target>TARGET_BLE_BASE && target<TARGET_VERSION_BASE) {
        ble_activate((ryz_v5_ble_action_t)(target-TARGET_BLE_BASE));
        return;
    }
    if(target>TARGET_VERSION_BASE && target<TARGET_SCRIPTS_BASE) {
        switch(target-TARGET_VERSION_BASE) {
        case RYZ_V5_VERSION_ACTION_INFO:
            (void)ryz_system_ui_navigate(s_pressed_page,RYZ_SYSTEM_UI_ROUTE_VERSION_INFO); break;
        case RYZ_V5_VERSION_ACTION_DEVICE_ID: ryz_v5_status_toggle_device_id(); break;
        case RYZ_V5_VERSION_ACTION_LATER:
            (void)ryz_system_ui_navigate(s_pressed_page,RYZ_SYSTEM_UI_ROUTE_SETTINGS); break;
        case RYZ_V5_VERSION_ACTION_CHECK: pending(RYZ_SYSTEM_UI_ACTION_OTA_CHECK); break;
        case RYZ_V5_VERSION_ACTION_UPDATE: pending(RYZ_SYSTEM_UI_ACTION_OTA_UPDATE); break;
        case RYZ_V5_VERSION_ACTION_CANCEL: pending(RYZ_SYSTEM_UI_ACTION_OTA_CANCEL); break;
        case RYZ_V5_VERSION_ACTION_RESTART: pending(RYZ_SYSTEM_UI_ACTION_OTA_RESTART); break;
        case RYZ_V5_VERSION_ACTION_RETRY: pending(RYZ_SYSTEM_UI_ACTION_OTA_RETRY); break;
        default: break;
        }
        return;
    }
#endif
    if (target == TARGET_HOME) {
        (void)ryz_system_ui_navigate(s_pressed_page, RYZ_SYSTEM_UI_ROUTE_HOME);
    } else if (target == TARGET_APPS) {
        (void)ryz_system_ui_navigate(s_pressed_page, RYZ_SYSTEM_UI_ROUTE_APPS);
    } else if (target == TARGET_SETTINGS) {
        (void)ryz_system_ui_navigate(s_pressed_page, RYZ_SYSTEM_UI_ROUTE_SETTINGS);
    } else if (target == TARGET_HOME_NETWORK) {
        (void)ryz_system_ui_navigate(s_pressed_page, RYZ_SYSTEM_UI_ROUTE_AP_SETUP);
    } else if (target == TARGET_SETTINGS_NETWORK) {
        (void)ryz_system_ui_navigate(s_pressed_page, RYZ_SYSTEM_UI_ROUTE_AP_SETUP);
    } else if (target == TARGET_BACK) {
        /* Preserve the existing AP back-to-HOME flow; generic stack back is
         * available for the reviewed V5 detail/modal pages in P2/P3. */
        (void)ryz_system_ui_navigate(s_pressed_page, RYZ_SYSTEM_UI_ROUTE_HOME);
    } else if (target == TARGET_REPROVISION) {
        s_pending_action = RYZ_SYSTEM_UI_ACTION_REPROVISION;
        s_action_page = s_pressed_page;
    }
}

#ifdef RYZ_UI_NATIVE_V5
static bool info_pointer(const ryz_touch_sample_t *sample,
                          const ryz_system_ui_snapshot_t *network,esp_err_t *result)
{
    *result=ESP_OK;
    bool info=s_route==RYZ_SYSTEM_UI_ROUTE_WIFI_INFO || s_route==RYZ_SYSTEM_UI_ROUTE_BLE_INFO ||
        s_route==RYZ_SYSTEM_UI_ROUTE_VERSION_INFO;
    bool description=s_route==RYZ_SYSTEM_UI_ROUTE_VERSION_AVAILABLE;
    if(!info && !description && s_route!=RYZ_SYSTEM_UI_ROUTE_SCRIPT_FAULT) return false;
    if(sample->event==RYZ_TOUCH_DOWN) {
        s_info_drag=(typeof(s_info_drag)){.active=sample->has_position,
            .x=sample->x,.y=sample->y,.last_y=sample->y,.page=ryz_system_ui_page_token()};
        return false;
    }
    if(!s_info_drag.active) return false;
    if(!ryz_ui_nav_matches(&s_navigation,s_info_drag.page)) {
        s_info_drag.active=false;
        return false;
    }
    if(sample->event==RYZ_TOUCH_NONE && !sample->pressed) {
        s_info_drag.active=false;
        return false; /* Lost UP is cancellation, never a final scroll or click. */
    }
    if(sample->has_position) {
        int dx=(int)sample->x-s_info_drag.x,dy=(int)sample->y-s_info_drag.y;
        int ax=dx<0?-dx:dx,ay=dy<0?-dy:dy;
        if((int64_t)dx*dx+(int64_t)dy*dy>=100) {
            s_info_drag.moved=true;
            if(!s_info_drag.axis) {
                if(ax>=10 && ax*2>=ay*3) s_info_drag.axis=1;
                else if(ay>=10 && ay*2>=ax*3) s_info_drag.axis=2;
            }
        }
        if(s_info_drag.moved) {
            s_pressed_target=TARGET_NONE;
            bool hidden=ryz_v5_password_revoke();
            bool origin=description ? s_info_drag.x>=24 && s_info_drag.x<216 &&
                s_info_drag.y>=118 && s_info_drag.y<166 :
                s_info_drag.x>=8 && s_info_drag.x<232 && s_info_drag.y>=58 && s_info_drag.y<190;
            bool changed=(info || description) && s_info_drag.axis==2 && origin &&
                ryz_v5_detail_scroll(s_route,s_info_drag.last_y-(int)sample->y);
            if(changed || hidden) *result=ryz_system_ui_render(network);
        }
        if(s_info_drag.axis==2) s_info_drag.last_y=sample->y;
    } else if(sample->event==RYZ_TOUCH_MOVE) {
        s_info_drag.moved=true;
        s_pressed_target=TARGET_NONE;
        if(ryz_v5_password_revoke()) *result=ryz_system_ui_render(network);
    }
    if(release_is_swipe(sample)) {
        s_info_drag.moved=true;
        s_pressed_target=TARGET_NONE;
        if(ryz_v5_password_revoke()) *result=ryz_system_ui_render(network);
    }
    bool consumed=s_info_drag.moved;
    if(sample->event==RYZ_TOUCH_UP || (sample->event==RYZ_TOUCH_NONE && !sample->pressed)) {
        s_info_drag.active=false;
        if(consumed) s_contact_active=false;
    }
    return consumed;
}

static bool top_level_swipe(const ryz_touch_sample_t *sample,
                            const ryz_system_ui_snapshot_t *network,
                            esp_err_t *result)
{
    *result = ESP_OK;
    const bool top = s_route <= RYZ_SYSTEM_UI_ROUTE_SETTINGS &&
        (s_route != RYZ_SYSTEM_UI_ROUTE_APPS || ryz_v5_apps_has_navigation());
    if (sample->event == RYZ_TOUCH_DOWN) {
        memset(&s_swipe, 0, sizeof(s_swipe));
        if (top && sample->has_position && sample->y >= 32) {
            s_swipe.active = true;
            s_swipe.x = s_swipe.last_x = sample->x;
            s_swipe.y = s_swipe.last_y = sample->y;
            s_swipe.page = ryz_system_ui_page_token();
        }
        return false;
    }
    if (!s_swipe.active) return false;
    if (!top || !ryz_ui_nav_matches(&s_navigation, s_swipe.page)) {
        s_swipe.active = false;
        return false;
    }
    if (sample->event == RYZ_TOUCH_NONE && (!sample->pressed || !sample->has_position)) {
        if (!sample->pressed) {
            s_swipe.active = false;
            s_pressed_target = TARGET_NONE;
        }
        return s_swipe.axis == 1;
    }
    if (sample->has_position) {
        s_swipe.last_x = sample->x;
        s_swipe.last_y = sample->y;
    } else if (sample->event == RYZ_TOUCH_MOVE) {
        s_swipe.moved = true;
        s_pressed_target = TARGET_NONE;
    }
    int dx = s_swipe.last_x - s_swipe.x;
    int dy = s_swipe.last_y - s_swipe.y;
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    if ((int64_t)dx*dx+(int64_t)dy*dy >= 100 || release_is_swipe(sample)) {
        s_swipe.moved = true;
        s_pressed_target = TARGET_NONE;
        if (!s_swipe.axis) {
            if (ax >= 10 && ax * 2 >= ay * 3) s_swipe.axis = 1;
            else if (ay >= 10 && ay * 2 >= ax * 3) s_swipe.axis = 2;
        }
    }
    if (s_swipe.axis == 1) {
        if (s_route == RYZ_SYSTEM_UI_ROUTE_APPS) ryz_v5_apps_cancel_gesture();
        s_pressed_target = TARGET_NONE;
        if (sample->event == RYZ_TOUCH_UP) {
            ryz_system_ui_route_t destination = s_route;
            if (ax >= 48) {
                if (dx < 0 && destination < RYZ_SYSTEM_UI_ROUTE_SETTINGS) ++destination;
                if (dx > 0 && destination > RYZ_SYSTEM_UI_ROUTE_HOME) --destination;
            }
            const ryz_ui_nav_token_t page = s_swipe.page;
            s_swipe.active = false;
            s_contact_active = false;
            if (destination != s_route) {
                *result = ryz_system_ui_navigate(page, destination);
                if (*result == ESP_OK) *result = ryz_system_ui_render(network);
            }
            if (*result == ESP_OK) s_wait_for_release = false;
            if (s_route == RYZ_SYSTEM_UI_ROUTE_APPS)
                (void)ryz_v5_apps_pointer(sample, (uint64_t)esp_timer_get_time() / 1000U);
        }
        return true;
    }
    const bool cancelled_tap = s_swipe.moved && s_route != RYZ_SYSTEM_UI_ROUTE_APPS;
    if (sample->event == RYZ_TOUCH_UP) {
        s_swipe.active = false;
        if (cancelled_tap) s_contact_active = false;
    }
    return cancelled_tap;
}
#endif

esp_err_t ryz_system_ui_handle_touch(
    const ryz_touch_sample_t *sample,
    const ryz_system_ui_snapshot_t *network)
{
    if (sample == NULL || network == NULL) {
        ryz_system_ui_invalidate_input();
        return ESP_ERR_INVALID_ARG;
    }
    if (sample->event < RYZ_TOUCH_NONE || sample->event > RYZ_TOUCH_UP ||
        (sample->event == RYZ_TOUCH_UP && sample->pressed) ||
        ((sample->event == RYZ_TOUCH_DOWN || sample->event == RYZ_TOUCH_MOVE) &&
         !sample->pressed)) {
        ryz_system_ui_invalidate_input();
        return ESP_ERR_INVALID_ARG;
    }
    if (s_wait_for_release || !s_presented) {
        s_pressed_target = TARGET_NONE;
#ifdef RYZ_UI_NATIVE_V5
        (void)ryz_v5_password_revoke();
        if(ryz_v5_password_needs_cleanup() || (!s_presented && ryz_v5_fault_needs_cleanup()))
            (void)ryz_v5_release();
#endif
        /* A valid released NONE also occurs after a TP error. Never require
         * an UP edge which the physical driver may no longer be able to emit. */
        s_wait_for_release = sample->pressed;
#ifdef RYZ_UI_NATIVE_V5
        if (s_route == RYZ_SYSTEM_UI_ROUTE_APPS && !sample->pressed)
            (void)ryz_v5_apps_pointer(sample,
                (uint64_t)esp_timer_get_time() / 1000U);
        if(s_route==RYZ_SYSTEM_UI_ROUTE_DISPLAY && !sample->pressed)
            (void)ryz_v5_display_pointer(sample,NULL);
#endif
        return ESP_OK;
    }
#ifdef RYZ_UI_NATIVE_V5
    esp_err_t gesture_result;
    if(sample->event==RYZ_TOUCH_DOWN &&
       (s_contact_active || s_swipe.active || s_info_drag.active)) {
        ryz_system_ui_invalidate_input();
        return ryz_system_ui_render(network);
    }
    if (info_pointer(sample, network, &gesture_result)) return gesture_result;
    if (top_level_swipe(sample, network, &gesture_result)) return gesture_result;
    if(s_route==RYZ_SYSTEM_UI_ROUTE_DISPLAY) {
        if(sample->event==RYZ_TOUCH_DOWN) s_pressed_page=ryz_system_ui_page_token();
        bool back=false;
        bool redraw=ryz_v5_display_pointer(sample,&back);
        if(back) go_back();
        esp_err_t error=redraw || back?ryz_system_ui_render(network):ESP_OK;
        if(error==ESP_OK && sample->event==RYZ_TOUCH_UP) s_wait_for_release=false;
        return error;
    }
    if(scripts_route()) return scripts_pointer(sample,network);
    if (s_route == RYZ_SYSTEM_UI_ROUTE_APPS &&
        ryz_v5_apps_pointer(sample, (uint64_t)esp_timer_get_time() / 1000U)) {
        return ryz_v5_apps_tick((uint64_t)esp_timer_get_time() / 1000U) ?
            ryz_system_ui_render(network) : ESP_OK;
    }
    /* Wide buttons still contain the finger during a drag. Cancel by the
     * contact's maximum excursion, not only by crossing another hit target.
     * Do this after dedicated sliders/lists so they retain gesture ownership. */
    if(s_contact_active && sample->event!=RYZ_TOUCH_DOWN) {
        int dx=(int)sample->x-s_tap_down_x,dy=(int)sample->y-s_tap_down_y;
        bool back=s_pressed_target==TARGET_BACK && sample->has_position &&
            sample->x<44 && sample->y<32;
        if(release_is_swipe(sample) || (sample->has_position && !back &&
            (int64_t)dx*dx+(int64_t)dy*dy>=100)) {
            s_pressed_target=TARGET_NONE;
            if(ryz_v5_password_revoke()) return ryz_system_ui_render(network);
        }
    }
#endif
    if (sample->event == RYZ_TOUCH_NONE) {
        if (!sample->pressed) s_contact_active = false;
#ifdef RYZ_UI_NATIVE_V5
        if(ryz_v5_password_held() && (!sample->pressed ||
            (sample->has_position && target_at(sample->x,sample->y,network)!=
                (target_t)(TARGET_NETWORK_BASE+RYZ_V5_NETWORK_ACTION_HOLD_PASSWORD)))) {
            (void)ryz_v5_password_revoke();
            s_pressed_target=TARGET_NONE;
            return ryz_system_ui_render(network);
        }
#endif
        if (!sample->pressed && s_pressed_target != TARGET_NONE) {
            s_pressed_target = TARGET_NONE;
#ifndef RYZ_UI_NATIVE_V5
            return ryz_system_ui_render(network);
#endif
        }
        return ESP_OK;
    }
    if (sample->event == RYZ_TOUCH_DOWN) {
        if (s_contact_active) {
            /* Duplicate DOWN cannot replace an in-flight press with another
             * target. Recover only after a physical release. */
            ryz_system_ui_invalidate_input();
            return ryz_system_ui_render(network);
        }
        s_pressed_target = sample->has_position ?
                               target_at(sample->x, sample->y, network) :
                               TARGET_NONE;
        s_capture_x=sample->x;
        s_capture_y=sample->y;
        s_contact_active = true;
        s_pressed_page = ryz_system_ui_page_token();
#ifdef RYZ_UI_NATIVE_V5
        s_tap_down_x=sample->x; s_tap_down_y=sample->y;
        s_ble_pressed_valid=false;
        if(ble_target(s_pressed_target)) {
            ryz_v5_status_t state;
            ryz_v5_status_get(&state);
            s_ble_pressed=s_ble_presented;
            s_ble_pressed_valid=s_ble_presented_valid &&
                ble_same_identity(&s_ble_presented,&state.ble,s_pressed_target);
            if(!s_ble_pressed_valid) s_pressed_target=TARGET_NONE;
        }
        if(s_pressed_target==(target_t)(TARGET_VERSION_BASE+RYZ_V5_VERSION_ACTION_RESTART)) {
            ryz_v5_status_t state;
            ryz_v5_status_get(&state);
            s_version_pressed_attempt=state.version.attempt_id;
            if(!s_version_presented_restart || !version_restart_ready(&state.version) ||
               s_version_pressed_attempt!=s_version_presented_attempt)
                s_pressed_target=TARGET_NONE;
        }
        if(s_pressed_target==(target_t)(TARGET_NETWORK_BASE+RYZ_V5_NETWORK_ACTION_HOLD_PASSWORD)) {
            (void)ryz_v5_password_begin();
            return ryz_system_ui_render(network);
        }
        /* Native ordinary menus have no pressed visual. Capture alone must
         * not rebuild/transfer an identical page. Apps and Scripts use their
         * own pointer paths above; password hold remains a visible change. */
        return ESP_OK;
#else
        return s_pressed_target == TARGET_NONE ? ESP_OK :
                                                 ryz_system_ui_render(network);
#endif
    }
    if (sample->event == RYZ_TOUCH_MOVE) {
        if (s_pressed_target == TARGET_NONE) {
            return ESP_OK;
        }
        const target_t moved_target = sample->has_position ?
                                          target_at(sample->x, sample->y,
                                                    network) : TARGET_NONE;
        if (moved_target != s_pressed_target) {
            s_pressed_target = TARGET_NONE;
#ifdef RYZ_UI_NATIVE_V5
            const bool hidden = ryz_v5_password_revoke();
            return hidden || ryz_v5_password_needs_cleanup() ?
                ryz_system_ui_render(network) : ESP_OK;
#else
            return ryz_system_ui_render(network);
#endif
        }
        s_capture_x=sample->x;
        s_capture_y=sample->y;
        return ESP_OK;
    }
    if (sample->event == RYZ_TOUCH_UP) {
        s_contact_active = false;
#ifdef RYZ_UI_NATIVE_V5
        (void)ryz_v5_password_revoke();
        if(ble_target(s_pressed_target)) {
            ryz_v5_status_t state;
            ryz_v5_status_get(&state);
            ble_recheck_capture(&state.ble);
            if(!s_ble_presented_valid ||
               !ble_same_identity(&s_ble_pressed,&s_ble_presented,s_pressed_target))
                s_pressed_target=TARGET_NONE;
        }
        if(s_pressed_target==(target_t)(TARGET_VERSION_BASE+RYZ_V5_VERSION_ACTION_RESTART)) {
            ryz_v5_status_t state;
            ryz_v5_status_get(&state);
            if(!version_restart_ready(&state.version) ||
               state.version.attempt_id!=s_version_pressed_attempt ||
               state.version.attempt_id!=s_version_presented_attempt)
                s_pressed_target=TARGET_NONE;
        }
#endif
        const target_t pressed_target = s_pressed_target;
        const target_t released_target = sample->has_position ?
                                             target_at(sample->x, sample->y,
                                                       network) :
                                             target_at(s_capture_x,s_capture_y,network);
        const target_t activated = released_target == s_pressed_target &&
            ryz_ui_nav_matches(&s_navigation, s_pressed_page) ?
                                       s_pressed_target : TARGET_NONE;
        s_pressed_target = TARGET_NONE;
        activate_target(activated, network);
        const esp_err_t error = pressed_target == TARGET_NONE ? ESP_OK :
                                                   ryz_system_ui_render(network);
        /* Navigation triggered by this UP has already observed release.
         * Render failure still requires a subsequent successful sample. */
        if (error == ESP_OK) {
            s_wait_for_release = false;
#ifdef RYZ_UI_NATIVE_V5
            if(s_route==RYZ_SYSTEM_UI_ROUTE_APPS)
                (void)ryz_v5_apps_pointer(sample,(uint64_t)esp_timer_get_time()/1000U);
            if(s_route==RYZ_SYSTEM_UI_ROUTE_DISPLAY)
                (void)ryz_v5_display_pointer(sample,NULL);
#endif
        }
        return error;
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t ryz_system_ui_process_sample(
    const ryz_touch_sample_t *sample, esp_err_t read_status,
    const ryz_system_ui_snapshot_t *snapshot,
    ryz_system_ui_action_t *action_out)
{
    if (action_out) *action_out = RYZ_SYSTEM_UI_ACTION_NONE;
    if (read_status != ESP_OK || !action_out) {
        ryz_system_ui_invalidate_input();
        return read_status != ESP_OK ? read_status : ESP_ERR_INVALID_ARG;
    }
    const esp_err_t error = ryz_system_ui_handle_touch(sample, snapshot);
    if (error != ESP_OK) {
        ryz_system_ui_invalidate_input();
        return error;
    }
    *action_out = ryz_system_ui_take_action();
    return ESP_OK;
}

esp_err_t ryz_system_ui_tick(const ryz_system_ui_snapshot_t *snapshot)
{
    if (!snapshot) return ESP_ERR_INVALID_ARG;
#ifdef RYZ_UI_NATIVE_V5
    const ryz_system_ui_route_t current=s_route;
    if(current==RYZ_SYSTEM_UI_ROUTE_DISPLAY && ryz_v5_display_tick())
        return ryz_system_ui_render(snapshot);
    ryz_v5_status_t state;
    ryz_v5_status_get(&state);
    if(ble_route(current)) {
        const ryz_v5_ble_model_t *m=&state.ble;
        ble_recheck_capture(m);
        if(s_ble_command_pending &&
           (!ble_same_identity(&s_ble_submitted,m,TARGET_NONE) ||
            s_ble_submitted.compare_pending!=m->compare_pending ||
            s_ble_submitted.page!=m->page)) s_ble_command_pending=false;
        /* Admitting a command is not a new state. In particular, a replace
         * request can still expose the old saved peer until its owner runs. */
        if(!s_ble_command_pending && !s_ble_pair_cancelled &&
           !m->unavailable && m->operation_id &&
           (current==RYZ_SYSTEM_UI_ROUTE_BLE_PAIRING || s_ble_pair_requested) &&
           (m->pairing_active || m->compare_pending || m->state==RYZ_V5_BLE_STATE_COMMITTING)) {
            s_ble_pair_requested=true;
            s_ble_pair_operation=m->operation_id;
        }
        const bool modal=current==RYZ_SYSTEM_UI_ROUTE_BLE_REPLACE ||
                         current==RYZ_SYSTEM_UI_ROUTE_BLE_FORGET;
        const bool deletion=m->checking_state || m->state==RYZ_V5_BLE_STATE_FORGETTING ||
                            m->state==RYZ_V5_BLE_STATE_FORGET_FAILED;
        ryz_system_ui_route_t destination=current;
        if(current!=RYZ_SYSTEM_UI_ROUTE_BLE_INFO && !s_ble_command_pending &&
           (!modal || (s_ble_modal_submitted && !deletion))) {
            destination=ble_landing(m);
            const bool completed_pair=s_ble_pair_requested && m->operation_id &&
                (s_ble_pair_operation==m->operation_id ||
                 (!s_ble_pair_operation && m->operation_id!=s_ble_pair_baseline));
            if(!m->unavailable && m->enabled && m->bond_known && m->bonded &&
               !m->pairing_active && !m->compare_pending && !m->checking_state &&
               m->page==RYZ_V5_BLE_SUCCESS && completed_pair)
                destination=RYZ_SYSTEM_UI_ROUTE_BLE_SUCCESS;
            else if(current==RYZ_SYSTEM_UI_ROUTE_BLE_SUCCESS &&
                    !m->unavailable && m->enabled && m->bonded)
                destination=current;
        }
        if(destination!=current && s_presented) {
            if(modal) {
                /* Modal confirmation owns its token; retire it before the
                 * service-driven state replacement of the underlying flow. */
                if(ryz_ui_nav_apply(&s_navigation,ryz_system_ui_page_token(),
                                   RYZ_UI_NAV_DISMISS,0)!=RYZ_UI_NAV_OK)
                    return ESP_ERR_INVALID_STATE;
                s_ble_modal_submitted=false;
            }
            esp_err_t error=ryz_system_ui_navigate(ryz_system_ui_page_token(),destination);
            if(error!=ESP_OK) return error;
            return ryz_system_ui_render(snapshot);
        }
        if(s_presented && s_ble_presented_valid &&
           (!ble_same_identity(&s_ble_presented,m,TARGET_NONE) ||
            s_ble_presented.compare_pending!=m->compare_pending ||
            (m->compare_pending && s_ble_presented.compare_value!=m->compare_value) ||
            s_ble_presented.remaining_valid!=m->remaining_valid ||
            (m->remaining_valid && s_ble_presented.remaining_seconds!=m->remaining_seconds)))
            return ryz_system_ui_render(snapshot);
    }
    if(ryz_v5_password_refresh(current==RYZ_SYSTEM_UI_ROUTE_WIFI_INFO &&
        s_presented && snapshot->connected && state.network.enabled && !state.network.busy)) {
        if(!ryz_v5_password_held() &&
           s_pressed_target==(target_t)(TARGET_NETWORK_BASE+RYZ_V5_NETWORK_ACTION_HOLD_PASSWORD))
            s_pressed_target=TARGET_NONE;
        if(s_presented) return ryz_system_ui_render(snapshot);
    }
    ryz_system_ui_route_t next=current;
    if(current==RYZ_SYSTEM_UI_ROUTE_VERSION_DOWNLOADING && state.version.image_staged)
        next=RYZ_SYSTEM_UI_ROUTE_VERSION_REBOOT;
    else if(current==RYZ_SYSTEM_UI_ROUTE_VERSION_REBOOT && !state.version.image_staged)
        next=RYZ_SYSTEM_UI_ROUTE_VERSION_CURRENT;
    if(!state.network.busy && current>=RYZ_SYSTEM_UI_ROUTE_AP_SETUP &&
       current<=RYZ_SYSTEM_UI_ROUTE_WIFI_NO_NETWORK &&
       current!=RYZ_SYSTEM_UI_ROUTE_WIFI_INFO &&
       current!=RYZ_SYSTEM_UI_ROUTE_WIFI_SERVER) {
        /* An accepted setup may still have the old ONLINE snapshot while
         * OTA cleanup runs. Pending commands never drive route success.
         * A failed setup is not a failed STA association (nor RADIO OFF). */
        if(state.network.setup_failed && (current==RYZ_SYSTEM_UI_ROUTE_AP_SETUP ||
           current==RYZ_SYSTEM_UI_ROUTE_WIFI_FAILED)) next=RYZ_SYSTEM_UI_ROUTE_WIFI_FAILED;
        else if(snapshot->state==RYZ_SYSTEM_UI_NETWORK_OFF) next=RYZ_SYSTEM_UI_ROUTE_WIFI_OFF;
        else if(snapshot->state==RYZ_SYSTEM_UI_NETWORK_AP_READY &&
                (current==RYZ_SYSTEM_UI_ROUTE_WIFI_FAILED || current==RYZ_SYSTEM_UI_ROUTE_WIFI_JOINING))
            next=RYZ_SYSTEM_UI_ROUTE_AP_SETUP;
        else if(snapshot->state==RYZ_SYSTEM_UI_NETWORK_CONNECTING ||
                snapshot->state==RYZ_SYSTEM_UI_NETWORK_CREDENTIALS_RECEIVED)
            next=RYZ_SYSTEM_UI_ROUTE_WIFI_JOINING;
        else if(snapshot->state==RYZ_SYSTEM_UI_NETWORK_FAILED) next=RYZ_SYSTEM_UI_ROUTE_WIFI_FAILED;
        else if(snapshot->connected) next=(current==RYZ_SYSTEM_UI_ROUTE_AP_SETUP ||
            current==RYZ_SYSTEM_UI_ROUTE_WIFI_JOINING || current==RYZ_SYSTEM_UI_ROUTE_WIFI_READY)?
            RYZ_SYSTEM_UI_ROUTE_WIFI_READY:RYZ_SYSTEM_UI_ROUTE_WIFI_CONNECTED;
        else if(current!=RYZ_SYSTEM_UI_ROUTE_AP_SETUP &&
                current!=RYZ_SYSTEM_UI_ROUTE_WIFI_NO_NETWORK)
            next=RYZ_SYSTEM_UI_ROUTE_WIFI_NO_NETWORK;
    }
    if(next!=current && s_presented) {
        esp_err_t error=ryz_system_ui_navigate(ryz_system_ui_page_token(),next);
        if(error!=ESP_OK) return error;
        return ryz_system_ui_render(snapshot);
    }
    if (s_route == RYZ_SYSTEM_UI_ROUTE_APPS && s_presented &&
        ryz_v5_apps_tick((uint64_t)esp_timer_get_time() / 1000U))
        return ryz_system_ui_render(snapshot);
#endif
    return ESP_OK;
}
