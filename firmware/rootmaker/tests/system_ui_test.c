#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "display.h"
#include "display_font.h"
#include "ryz_qr_encoder.h"
#include "ryz_system_ui.h"

enum {
    MAX_RECTS = 2048,
    MAX_TEXTS = 256,
    MAX_TEXT_LENGTH = 31,
};

typedef struct {
    int x;
    int y;
    int width;
    int height;
    uint16_t color;
} rect_call_t;

typedef struct {
    int x;
    int y;
    char text[MAX_TEXT_LENGTH + 1];
    uint16_t color;
    int scale;
} text_call_t;

static int s_clear_count;
static uint16_t s_clear_color;
static rect_call_t s_rects[MAX_RECTS];
static size_t s_rect_count;
static text_call_t s_texts[MAX_TEXTS];
static size_t s_text_count;
static int s_show_count;
static esp_err_t s_show_error;
static int s_qr_encode_count;
static int s_qr_side;
static char s_qr_payload[256];

static void display_spy_reset(void)
{
    s_clear_count = 0;
    s_clear_color = 0;
    s_rect_count = 0;
    s_text_count = 0;
    s_show_count = 0;
    s_show_error = ESP_OK;
    s_qr_encode_count = 0;
    s_qr_side = 21;
    s_qr_payload[0] = '\0';
}

static bool fake_qr_module(const void *matrix, int x, int y)
{
    (void)matrix;
    return (x == 0 && y == 0) ||
           (x == s_qr_side - 1 && y == s_qr_side - 1);
}

esp_err_t ryz_qr_encode_text(const char *text,
                             ryz_qr_matrix_consumer_t consume,
                             void *context)
{
    assert(text != NULL);
    assert(consume != NULL);
    ++s_qr_encode_count;
    const size_t length = strlen(text);
    assert(length < sizeof(s_qr_payload));
    memcpy(s_qr_payload, text, length + 1);
    return consume(context, s_qr_side, fake_qr_module, NULL);
}

static bool text_was_drawn(const char *expected)
{
    for (size_t i = 0; i < s_text_count; ++i) {
        if (strcmp(s_texts[i].text, expected) == 0) {
            return true;
        }
    }
    return false;
}

static bool color_was_drawn(uint16_t expected)
{
    if (s_clear_count > 0 && s_clear_color == expected) {
        return true;
    }
    for (size_t i = 0; i < s_rect_count; ++i) {
        if (s_rects[i].color == expected) {
            return true;
        }
    }
    for (size_t i = 0; i < s_text_count; ++i) {
        if (s_texts[i].color == expected) {
            return true;
        }
    }
    return false;
}

static bool rect_was_drawn(int x, int y, int width, int height,
                           uint16_t color)
{
    for (size_t i = 0; i < s_rect_count; ++i) {
        const rect_call_t *call = &s_rects[i];
        if (call->x == x && call->y == y && call->width == width &&
            call->height == height && call->color == color) {
            return true;
        }
    }
    return false;
}

static ryz_touch_sample_t touch(ryz_touch_event_t event, bool has_position,
                                uint16_t x, uint16_t y)
{
    return (ryz_touch_sample_t) {
        .pressed = event != RYZ_TOUCH_UP,
        .has_position = has_position,
        .x = x,
        .y = y,
        .fingers = event == RYZ_TOUCH_UP ? 0 : 1,
        .event = event,
    };
}

esp_err_t ryz_display_clear(uint16_t color)
{
    ++s_clear_count;
    s_clear_color = color;
    return ESP_OK;
}

esp_err_t ryz_display_rect(int x, int y, int width, int height, uint16_t color)
{
    assert(x >= 0);
    assert(y >= 0);
    assert(width > 0);
    assert(height > 0);
    assert(x + width <= RYZ_DISPLAY_WIDTH);
    assert(y + height <= RYZ_DISPLAY_HEIGHT);
    assert(s_rect_count < MAX_RECTS);
    s_rects[s_rect_count++] = (rect_call_t) {
        .x = x,
        .y = y,
        .width = width,
        .height = height,
        .color = color,
    };
    return ESP_OK;
}

esp_err_t ryz_display_text(int x, int y, const char *text, size_t length,
                           uint16_t color, int scale)
{
    assert(text != NULL);
    assert(length <= MAX_TEXT_LENGTH);
    assert(scale >= 1 && scale <= 6);
    assert(x >= 0);
    assert(y >= 0);
    const size_t pixel_width = length == 0 ? 0 :
                               ((length * 6) - 1) * (size_t)scale;
    assert((size_t)x + pixel_width <= RYZ_DISPLAY_WIDTH);
    assert(y + (7 * scale) <= RYZ_DISPLAY_HEIGHT);
    /* Keep this spy honest: every visible UI string must pass the exact
     * production 5x7 font lookup instead of being accepted by the host only. */
    for (size_t index = 0; index < length; ++index) {
        assert(display_glyph(text[index]) != NULL);
    }
    assert(s_text_count < MAX_TEXTS);
    text_call_t *call = &s_texts[s_text_count++];
    call->x = x;
    call->y = y;
    memcpy(call->text, text, length);
    call->text[length] = '\0';
    call->color = color;
    call->scale = scale;
    return ESP_OK;
}

esp_err_t ryz_display_show(void)
{
    return ryz_display_show_checked(NULL, NULL);
}

esp_err_t ryz_display_show_checked(bool (*cancelled)(void *), void *context)
{
    return ryz_display_show_checked_status(cancelled, context, NULL);
}

esp_err_t ryz_display_show_checked_status(
    bool (*cancelled)(void *), void *context, bool *cancelled_out)
{
    if (cancelled_out) *cancelled_out = false;
    ++s_show_count;
    if (cancelled && cancelled(context)) {
        if (cancelled_out) *cancelled_out = true;
        return ESP_ERR_TIMEOUT;
    }
    return s_show_error;
}

static bool cancel_render(void *context)
{
    bool *called = context;
    *called = true;
    return true;
}

static void ready_ui(const ryz_system_ui_snapshot_t *snapshot)
{
    ryz_system_ui_reset();
    display_spy_reset();
    assert(ryz_system_ui_render(snapshot) == ESP_OK);
    const ryz_touch_sample_t released = {.event = RYZ_TOUCH_NONE};
    ryz_system_ui_action_t action = RYZ_SYSTEM_UI_ACTION_REPROVISION;
    assert(ryz_system_ui_process_sample(&released, ESP_OK, snapshot, &action) == ESP_OK);
    assert(action == RYZ_SYSTEM_UI_ACTION_NONE);
    display_spy_reset();
}

static void test_checked_render_forwards_cancellation(void)
{
    const ryz_system_ui_network_snapshot_t network = {0};
    bool cancellation_checked = false;
    bool cancelled_out = false;

    display_spy_reset();
    ryz_system_ui_reset();
    assert(ryz_system_ui_render_checked(
               &network, cancel_render, &cancellation_checked,
               &cancelled_out) ==
           ESP_ERR_TIMEOUT);
    assert(cancellation_checked);
    assert(cancelled_out);
    assert(s_show_count == 1);
}

static void test_boots_to_home_and_renders(void)
{
    const ryz_system_ui_snapshot_t snapshot = {
        .configured = false,
        .connected = false,
        .ap_active = false,
        .storage_ready = true,
        .storage_total_bytes = 1000,
        .storage_used_bytes = 610,
    };

    display_spy_reset();
    ryz_system_ui_reset();

    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_HOME);
    assert(ryz_system_ui_render(&snapshot) == ESP_OK);
    assert(s_clear_count == 1);
    assert(s_show_count == 1);
    assert(text_was_drawn("RYZOBEE"));
    assert(text_was_drawn("HOME"));
    assert(text_was_drawn("APPS"));
    assert(text_was_drawn("SET"));
    assert(text_was_drawn("NETWORK"));
    assert(text_was_drawn("NOT SET"));
    assert(text_was_drawn("STORAGE"));
    assert(text_was_drawn("61% USED"));
    assert(rect_was_drawn(8, 32, 224, 72, 0x18C3));
    assert(rect_was_drawn(8, 181, 224, 6, 0x39C7));
    assert(rect_was_drawn(8, 181, 137, 6, 0xFB40));
    assert(rect_was_drawn(0, 238, 80, 2, 0xFB40));
    assert(color_was_drawn(0xFB40));
    assert(color_was_drawn(0x0000));
}

static void test_unavailable_storage_renders_unknown_usage(void)
{
    const ryz_system_ui_snapshot_t snapshot = {0};

    display_spy_reset();
    ryz_system_ui_reset();
    assert(ryz_system_ui_render(&snapshot) == ESP_OK);
    assert(text_was_drawn("--% USED"));
    assert(rect_was_drawn(8, 181, 224, 6, 0x39C7));
}

static void test_apps_tab_highlights_then_activates(void)
{
    const ryz_system_ui_network_snapshot_t network = {0};
    ryz_touch_sample_t sample = touch(RYZ_TOUCH_DOWN, true, 100, 220);

    ready_ui(&network);
    display_spy_reset();
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_HOME);
    assert(s_show_count == 1);
    assert(rect_was_drawn(80, 238, 80, 2, 0xFB40));

    display_spy_reset();
    sample = touch(RYZ_TOUCH_UP, true, 100, 220);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_APPS);
    assert(s_show_count == 1);
    assert(text_was_drawn("LUA TOOL"));
    assert(text_was_drawn("EMPTY"));
    assert(rect_was_drawn(8, 40, 104, 64, 0x18C3));
    assert(rect_was_drawn(80, 238, 80, 2, 0xFB40));
}

static void test_settings_tab_activates_settings_page(void)
{
    const ryz_system_ui_network_snapshot_t network = {0};
    ryz_touch_sample_t sample = touch(RYZ_TOUCH_DOWN, true, 200, 220);

    ready_ui(&network);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    display_spy_reset();
    sample = touch(RYZ_TOUCH_UP, true, 200, 220);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);

    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_SETTINGS);
    assert(s_show_count == 1);
    assert(text_was_drawn("SETTINGS"));
    assert(text_was_drawn("NETWORK"));
    assert(text_was_drawn("DISPLAY"));
    assert(text_was_drawn("REPROVISION"));
    assert(rect_was_drawn(160, 238, 80, 2, 0xFB40));
}

static void test_storage_region_is_not_an_ap_setup_shortcut(void)
{
    const ryz_system_ui_network_snapshot_t network = {0};
    ryz_touch_sample_t sample = touch(RYZ_TOUCH_DOWN, true, 100, 170);

    ready_ui(&network);
    display_spy_reset();
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_HOME);
    assert(s_show_count == 0);

    sample = touch(RYZ_TOUCH_UP, true, 100, 170);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_HOME);
    assert(s_show_count == 0);
}

static void test_home_wlan_chart_opens_ap_setup(void)
{
    const ryz_system_ui_network_snapshot_t network = {0};
    ryz_touch_sample_t sample = touch(RYZ_TOUCH_DOWN, true, 100, 70);

    ready_ui(&network);
    display_spy_reset();
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_HOME);
    assert(s_show_count == 1);

    display_spy_reset();
    sample = touch(RYZ_TOUCH_UP, true, 100, 70);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_AP_SETUP);
    assert(s_show_count == 1);
    assert(text_was_drawn("AP SETUP"));
}

static void test_ap_ready_renders_escaped_wifi_qr_with_quiet_zone(void)
{
    const ryz_system_ui_network_snapshot_t network = {
        .configured = false,
        .connected = false,
        .ap_active = true,
        .state = RYZ_SYSTEM_UI_NETWORK_AP_READY,
        .ap_ssid = "RYZOBEE-A1B2",
        .ap_password = "p:a\\ss,wo;rd",
        .portal_ip = "192.168.4.1",
    };
    ryz_touch_sample_t sample = touch(RYZ_TOUCH_DOWN, true, 100, 70);

    ready_ui(&network);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    display_spy_reset();
    sample = touch(RYZ_TOUCH_UP, false, 0, 0);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);

    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_AP_SETUP);
    assert(s_qr_encode_count == 1);
    assert(strcmp(s_qr_payload,
                  "WIFI:T:WPA;S:RYZOBEE-A1B2;P:p\\:a\\\\ss\\,wo\\;rd;;") == 0);
    assert(text_was_drawn("RYZOBEE-A1B2"));
    assert(text_was_drawn("192.168.4.1"));
    /* A 21-module fake plus a four-module quiet zone is centered at scale 4. */
    assert(rect_was_drawn(78, 58, 132, 132, 0x1082));
    assert(rect_was_drawn(102, 82, 4, 4, 0xFFFF));
    assert(rect_was_drawn(182, 162, 4, 4, 0xFFFF));
    assert(!text_was_drawn("HOME"));
}

static void test_largest_supported_qr_version_fits_display(void)
{
    const ryz_system_ui_network_snapshot_t network = {
        .ap_active = true,
        .state = RYZ_SYSTEM_UI_NETWORK_AP_READY,
        .ap_ssid = "RYZOBEE-ABCD",
        .ap_password = "A1B2C3D4E5F6",
    };
    ryz_touch_sample_t sample = touch(RYZ_TOUCH_DOWN, true, 100, 70);

    ready_ui(&network);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    display_spy_reset();
    s_qr_side = 57;
    sample = touch(RYZ_TOUCH_UP, false, 0, 0);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);

    /* Version 10 is 57 modules. Four quiet modules per side fit at 2 px. */
    assert(rect_was_drawn(78, 58, 132, 132, 0x1082));
    assert(rect_was_drawn(87, 67, 2, 2, 0xFFFF));
    assert(rect_was_drawn(199, 179, 2, 2, 0xFFFF));
}

static void test_ap_setup_reports_connecting_and_failed_states(void)
{
    ryz_system_ui_network_snapshot_t network = {0};
    ryz_touch_sample_t sample = touch(RYZ_TOUCH_DOWN, true, 100, 70);

    ready_ui(&network);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    sample = touch(RYZ_TOUCH_UP, false, 0, 0);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);

    network.configured = true;
    network.state = RYZ_SYSTEM_UI_NETWORK_CONNECTING;
    display_spy_reset();
    assert(ryz_system_ui_render(&network) == ESP_OK);
    assert(text_was_drawn("CONNECTING"));

    network.state = RYZ_SYSTEM_UI_NETWORK_FAILED;
    display_spy_reset();
    assert(ryz_system_ui_render(&network) == ESP_OK);
    assert(text_was_drawn("SETUP FAILED"));

    /* Retained AP fields after a failed stop are not proof of reachability. */
    network.ap_active = true;
    strcpy(network.ap_ssid, "RYZOBEE-RETAINED");
    strcpy(network.ap_password, "retained-test-only");
    const ryz_system_ui_network_state_t stopped_states[] = {
        RYZ_SYSTEM_UI_NETWORK_STOPPING, RYZ_SYSTEM_UI_NETWORK_OFF,
        RYZ_SYSTEM_UI_NETWORK_FAILED,
    };
    const char *labels[] = {"STOPPING WI-FI", "WI-FI OFF", "SETUP FAILED"};
    for (size_t i = 0; i < 3; ++i) {
        network.state = stopped_states[i];
        display_spy_reset();
        assert(ryz_system_ui_render(&network) == ESP_OK);
        assert(text_was_drawn(labels[i]));
        assert(s_qr_encode_count == 0);
        assert(!text_was_drawn("AP READY"));
    }
}

static void test_ap_setup_rejects_unterminated_portal_ip(void)
{
    ryz_system_ui_network_snapshot_t network = {0};
    ryz_touch_sample_t sample = touch(RYZ_TOUCH_DOWN, true, 100, 70);

    ready_ui(&network);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    sample = touch(RYZ_TOUCH_UP, false, 0, 0);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);

    network.ap_active = true;
    strcpy(network.ap_ssid, "RYZOBEE-TEST");
    strcpy(network.ap_password, "A1B2C3D4E5F6");
    memset(network.portal_ip, '1', sizeof(network.portal_ip));
    display_spy_reset();
    assert(ryz_system_ui_render(&network) == ESP_ERR_INVALID_ARG);
    assert(s_show_count == 0);
}

static void test_secondary_page_back_highlights_then_returns_home(void)
{
    const ryz_system_ui_network_snapshot_t network = {0};
    ryz_touch_sample_t sample = touch(RYZ_TOUCH_DOWN, true, 100, 70);

    ready_ui(&network);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    sample = touch(RYZ_TOUCH_UP, true, 100, 70);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_AP_SETUP);

    display_spy_reset();
    /* The visible glyph is compact, but the logical hit area is 44 x 44. */
    sample = touch(RYZ_TOUCH_DOWN, true, 40, 40);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_AP_SETUP);
    assert(s_show_count == 1);
    assert(rect_was_drawn(0, 0, 24, 32, 0xFB40));

    display_spy_reset();
    sample = touch(RYZ_TOUCH_UP, true, 40, 40);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_HOME);
    assert(s_show_count == 1);
    assert(text_was_drawn("NOT SET"));
}

static void test_move_out_cancels_even_if_pointer_returns(void)
{
    const ryz_system_ui_network_snapshot_t network = {0};
    ryz_touch_sample_t sample = touch(RYZ_TOUCH_DOWN, true, 100, 220);

    ready_ui(&network);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);

    display_spy_reset();
    sample = touch(RYZ_TOUCH_MOVE, true, 100, 180);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_HOME);
    assert(s_show_count == 1);
    assert(rect_was_drawn(0, 238, 80, 2, 0xFB40));
    assert(!rect_was_drawn(80, 238, 80, 2, 0xFB40));

    display_spy_reset();
    sample = touch(RYZ_TOUCH_MOVE, true, 100, 220);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    sample = touch(RYZ_TOUCH_UP, true, 100, 220);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_HOME);
    assert(s_show_count == 0);
}

static void test_positionless_up_uses_last_in_bounds_sample(void)
{
    const ryz_system_ui_network_snapshot_t network = {0};
    ryz_touch_sample_t sample = touch(RYZ_TOUCH_DOWN, true, 100, 220);

    ready_ui(&network);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    sample = touch(RYZ_TOUCH_UP, true, 100, 220);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_APPS);

    sample = touch(RYZ_TOUCH_DOWN, true, 40, 220);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    display_spy_reset();
    sample = touch(RYZ_TOUCH_UP, false, 0, 0);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_HOME);
    assert(s_show_count == 1);
    assert(text_was_drawn("NOT SET"));
    assert(rect_was_drawn(0, 238, 80, 2, 0xFB40));
}

static void test_up_outside_cancels_and_clears_highlight(void)
{
    const ryz_system_ui_network_snapshot_t network = {0};
    ryz_touch_sample_t sample = touch(RYZ_TOUCH_DOWN, true, 100, 220);

    ready_ui(&network);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    display_spy_reset();
    sample = touch(RYZ_TOUCH_UP, true, 100, 180);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);

    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_HOME);
    assert(s_show_count == 1);
    assert(rect_was_drawn(0, 238, 80, 2, 0xFB40));
    assert(!rect_was_drawn(80, 238, 80, 2, 0xFB40));
}

static void test_configured_network_card_opens_network_status(void)
{
    const ryz_system_ui_network_snapshot_t network = {
        .configured = true,
        .connected = false,
        .ap_active = false,
    };
    ryz_touch_sample_t sample = touch(RYZ_TOUCH_DOWN, true, 100, 70);

    ready_ui(&network);
    display_spy_reset();
    assert(ryz_system_ui_render(&network) == ESP_OK);
    assert(text_was_drawn("NOT CONNECTED"));

    display_spy_reset();
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    assert(s_show_count == 1);
    display_spy_reset();
    sample = touch(RYZ_TOUCH_UP, true, 100, 70);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_AP_SETUP);
    assert(s_show_count == 1);
    assert(text_was_drawn("AP SETUP"));
}

static void test_home_reports_connecting_and_failed_network_states(void)
{
    ryz_system_ui_network_snapshot_t network = {
        .configured = true,
        .state = RYZ_SYSTEM_UI_NETWORK_CONNECTING,
    };

    ryz_system_ui_reset();
    display_spy_reset();
    assert(ryz_system_ui_render(&network) == ESP_OK);
    assert(text_was_drawn("CONNECTING..."));
    assert(text_was_drawn("STORAGE"));

    network.state = RYZ_SYSTEM_UI_NETWORK_FAILED;
    display_spy_reset();
    assert(ryz_system_ui_render(&network) == ESP_OK);
    assert(text_was_drawn("CONNECTION FAILED"));
    assert(text_was_drawn("STORAGE"));

    network.connected = true; /* Even stale flags cannot override explicit OFF. */
    network.state = RYZ_SYSTEM_UI_NETWORK_OFF;
    display_spy_reset();
    assert(ryz_system_ui_render(&network) == ESP_OK);
    assert(text_was_drawn("WI-FI OFF"));
    assert(!text_was_drawn("ONLINE"));
    network.state = RYZ_SYSTEM_UI_NETWORK_STOPPING;
    display_spy_reset();
    assert(ryz_system_ui_render(&network) == ESP_OK);
    assert(text_was_drawn("STOPPING WI-FI"));
    assert(!text_was_drawn("ONLINE"));
}

static void test_apps_and_settings_use_bottom_navigation_to_return_home(void)
{
    const ryz_system_ui_network_snapshot_t network = {0};
    const uint16_t tab_x[] = {100, 200};

    for (size_t i = 0; i < sizeof(tab_x) / sizeof(tab_x[0]); ++i) {
        ready_ui(&network);
        display_spy_reset();
        ryz_touch_sample_t sample = touch(RYZ_TOUCH_DOWN, true, tab_x[i], 220);
        assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
        sample = touch(RYZ_TOUCH_UP, false, 0, 0);
        assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
        assert(ryz_system_ui_current_route() != RYZ_SYSTEM_UI_ROUTE_HOME);

        sample = touch(RYZ_TOUCH_DOWN, true, 30, 16);
        assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
        sample = touch(RYZ_TOUCH_UP, false, 0, 0);
        assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
        assert(ryz_system_ui_current_route() != RYZ_SYSTEM_UI_ROUTE_HOME);

        sample = touch(RYZ_TOUCH_DOWN, true, 40, 220);
        assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
        sample = touch(RYZ_TOUCH_UP, false, 0, 0);
        assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
        assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_HOME);
    }
}

static void test_disabled_lua_tile_does_not_activate(void)
{
    const ryz_system_ui_network_snapshot_t network = {0};
    ryz_touch_sample_t sample = touch(RYZ_TOUCH_DOWN, true, 100, 220);

    ready_ui(&network);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    sample = touch(RYZ_TOUCH_UP, false, 0, 0);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_APPS);

    display_spy_reset();
    sample = touch(RYZ_TOUCH_DOWN, true, 50, 70);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    sample = touch(RYZ_TOUCH_UP, true, 50, 70);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_APPS);
    assert(s_show_count == 0);
}

static void test_settings_network_row_opens_ap_setup(void)
{
    const ryz_system_ui_network_snapshot_t network = {
        .configured = true,
        .state = RYZ_SYSTEM_UI_NETWORK_ONLINE,
    };
    ryz_touch_sample_t sample = touch(RYZ_TOUCH_DOWN, true, 200, 220);

    ready_ui(&network);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    sample = touch(RYZ_TOUCH_UP, false, 0, 0);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_SETTINGS);

    display_spy_reset();
    sample = touch(RYZ_TOUCH_DOWN, true, 100, 55);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_SETTINGS);
    assert(rect_was_drawn(8, 32, 224, 48, 0xFB40));

    display_spy_reset();
    sample = touch(RYZ_TOUCH_UP, false, 0, 0);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_AP_SETUP);
    assert(text_was_drawn("AP SETUP"));
    assert(!text_was_drawn("HOME"));
}

static void test_settings_reprovision_action_is_armed_on_down_and_taken_once(void)
{
    const ryz_system_ui_network_snapshot_t network = {
        .configured = true,
        .state = RYZ_SYSTEM_UI_NETWORK_FAILED,
    };
    ryz_touch_sample_t sample = touch(RYZ_TOUCH_DOWN, true, 200, 220);

    ready_ui(&network);
    assert(ryz_system_ui_take_action() == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    sample = touch(RYZ_TOUCH_UP, false, 0, 0);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_SETTINGS);

    display_spy_reset();
    sample = touch(RYZ_TOUCH_DOWN, true, 100, 160);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    assert(ryz_system_ui_take_action() == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(rect_was_drawn(8, 136, 224, 48, 0xFB40));

    sample = touch(RYZ_TOUCH_UP, false, 0, 0);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    assert(ryz_system_ui_take_action() == RYZ_SYSTEM_UI_ACTION_REPROVISION);
    assert(ryz_system_ui_take_action() == RYZ_SYSTEM_UI_ACTION_NONE);
}

static void test_settings_reprovision_move_out_cancels_action(void)
{
    const ryz_system_ui_network_snapshot_t network = {.configured = true};
    ryz_touch_sample_t sample = touch(RYZ_TOUCH_DOWN, true, 200, 220);

    ready_ui(&network);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    sample = touch(RYZ_TOUCH_UP, false, 0, 0);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);

    sample = touch(RYZ_TOUCH_DOWN, true, 100, 160);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    sample = touch(RYZ_TOUCH_MOVE, true, 100, 110);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    sample = touch(RYZ_TOUCH_MOVE, true, 100, 160);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    sample = touch(RYZ_TOUCH_UP, true, 100, 160);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);

    assert(ryz_system_ui_take_action() == RYZ_SYSTEM_UI_ACTION_NONE);
}

static void test_failed_reprovision_release_cannot_deliver_action(void)
{
    const ryz_system_ui_snapshot_t network = {.configured = true};
    ready_ui(&network);
    display_spy_reset();
    ryz_touch_sample_t sample = touch(RYZ_TOUCH_DOWN, true, 200, 220);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    sample = touch(RYZ_TOUCH_UP, false, 0, 0);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_SETTINGS);

    sample = touch(RYZ_TOUCH_DOWN, true, 100, 160);
    assert(ryz_system_ui_handle_touch(&sample, &network) == ESP_OK);
    assert(ryz_system_ui_take_action() == RYZ_SYSTEM_UI_ACTION_NONE);

    display_spy_reset();
    s_show_error = ESP_ERR_TIMEOUT;
    sample = touch(RYZ_TOUCH_UP, false, 0, 0);
    /* Match the current Workbench public-call order: handle, take, then error
     * reset. Resetting global state cannot retract an already-delivered enum;
     * the UI must not deliver an action for this failed input/render cycle. */
    const esp_err_t error = ryz_system_ui_handle_touch(&sample, &network);
    const ryz_system_ui_action_t action = ryz_system_ui_take_action();
    assert(error == ESP_ERR_TIMEOUT && s_show_count == 1);
    if (error != ESP_OK) ryz_system_ui_reset();
    assert(ryz_system_ui_take_action() == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(action == RYZ_SYSTEM_UI_ACTION_NONE);
    display_spy_reset();
}

static void test_network_becoming_configured_opens_current_status(void)
{
    const ryz_system_ui_network_snapshot_t unconfigured = {0};
    const ryz_system_ui_network_snapshot_t configured = {
        .configured = true,
    };
    ryz_touch_sample_t sample = touch(RYZ_TOUCH_DOWN, true, 100, 70);

    ready_ui(&unconfigured);
    assert(ryz_system_ui_handle_touch(&sample, &unconfigured) == ESP_OK);
    display_spy_reset();
    sample = touch(RYZ_TOUCH_UP, false, 0, 0);
    assert(ryz_system_ui_handle_touch(&sample, &configured) == ESP_OK);

    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_AP_SETUP);
    assert(s_show_count == 1);
    assert(text_was_drawn("AP SETUP"));
}

static ryz_system_ui_action_t process_ok(
    const ryz_touch_sample_t *sample, const ryz_system_ui_snapshot_t *snapshot)
{
    ryz_system_ui_action_t action = RYZ_SYSTEM_UI_ACTION_REPROVISION;
    assert(ryz_system_ui_process_sample(sample, ESP_OK, snapshot, &action) == ESP_OK);
    return action;
}

static void open_settings(const ryz_system_ui_snapshot_t *snapshot)
{
    ready_ui(snapshot);
    ryz_touch_sample_t sample = touch(RYZ_TOUCH_DOWN, true, 200, 220);
    assert(process_ok(&sample, snapshot) == RYZ_SYSTEM_UI_ACTION_NONE);
    sample = touch(RYZ_TOUCH_UP, false, 0, 0);
    assert(process_ok(&sample, snapshot) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_SETTINGS);
    display_spy_reset();
}

static void test_physical_error_ignores_sample_and_guards_held_finger(void)
{
    const ryz_system_ui_snapshot_t snapshot = {.configured = true};
    open_settings(&snapshot);
    ryz_touch_sample_t sample = touch(RYZ_TOUCH_DOWN, true, 100, 160);
    assert(process_ok(&sample, &snapshot) == RYZ_SYSTEM_UI_ACTION_NONE);
    const ryz_ui_nav_token_t before = ryz_system_ui_page_token();
    ryz_system_ui_action_t action = RYZ_SYSTEM_UI_ACTION_REPROVISION;
    /* A failed BSP read owns no valid sample; dereferencing this poison
     * pointer would fail under ASan instead of silently accepting zeros. */
    assert(ryz_system_ui_process_sample(
        (const ryz_touch_sample_t *)(uintptr_t)1, ESP_ERR_TIMEOUT,
        &snapshot, &action) == ESP_ERR_TIMEOUT);
    assert(action == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_take_action() == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_SETTINGS);
    assert(ryz_system_ui_page_token().generation > before.generation);

    display_spy_reset();
    assert(ryz_system_ui_render(&snapshot) == ESP_OK);
    display_spy_reset();
    sample = touch(RYZ_TOUCH_DOWN, true, 100, 160);
    assert(process_ok(&sample, &snapshot) == RYZ_SYSTEM_UI_ACTION_NONE);
    sample = touch(RYZ_TOUCH_MOVE, true, 100, 160);
    assert(process_ok(&sample, &snapshot) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(s_show_count == 0);
    sample = (ryz_touch_sample_t){.event = RYZ_TOUCH_NONE};
    assert(process_ok(&sample, &snapshot) == RYZ_SYSTEM_UI_ACTION_NONE);
    sample = touch(RYZ_TOUCH_DOWN, true, 100, 160);
    assert(process_ok(&sample, &snapshot) == RYZ_SYSTEM_UI_ACTION_NONE);
    sample = touch(RYZ_TOUCH_UP, false, 0, 0);
    assert(process_ok(&sample, &snapshot) == RYZ_SYSTEM_UI_ACTION_REPROVISION);
    assert(ryz_system_ui_take_action() == RYZ_SYSTEM_UI_ACTION_NONE);
}

static void test_physical_error_clears_preexisting_pending_action(void)
{
    const ryz_system_ui_snapshot_t snapshot = {.configured = true};
    open_settings(&snapshot);
    ryz_touch_sample_t sample = touch(RYZ_TOUCH_DOWN, true, 100, 160);
    assert(ryz_system_ui_handle_touch(&sample, &snapshot) == ESP_OK);
    sample = touch(RYZ_TOUCH_UP, false, 0, 0);
    assert(ryz_system_ui_handle_touch(&sample, &snapshot) == ESP_OK);
    ryz_system_ui_action_t action = RYZ_SYSTEM_UI_ACTION_REPROVISION;
    assert(ryz_system_ui_process_sample(NULL, ESP_ERR_INVALID_STATE,
                                         &snapshot, &action) == ESP_ERR_INVALID_STATE);
    assert(action == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_take_action() == RYZ_SYSTEM_UI_ACTION_NONE);
}

static void test_reset_and_async_navigation_revoke_old_tokens(void)
{
    const ryz_system_ui_snapshot_t snapshot = {0};
    ready_ui(&snapshot);
    const ryz_ui_nav_token_t home = ryz_system_ui_page_token();
    assert(ryz_system_ui_navigate(home, RYZ_SYSTEM_UI_ROUTE_APPS) == ESP_OK);
    const ryz_ui_nav_token_t apps = ryz_system_ui_page_token();
    assert(apps.generation > home.generation);
    assert(ryz_system_ui_navigate(home, RYZ_SYSTEM_UI_ROUTE_SETTINGS) == ESP_ERR_INVALID_STATE);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_APPS);
    assert(ryz_system_ui_page_token().generation == apps.generation);

    uint64_t generation = apps.generation;
    for (unsigned i = 0; i < 32; ++i) {
        ryz_system_ui_reset();
        assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_HOME);
        assert(ryz_system_ui_page_token().generation > generation);
        generation = ryz_system_ui_page_token().generation;
        assert(ryz_system_ui_navigate(apps, RYZ_SYSTEM_UI_ROUTE_SETTINGS) == ESP_ERR_INVALID_STATE);
    }
    assert(ryz_system_ui_navigate(ryz_system_ui_page_token(),
                                 (ryz_system_ui_route_t)99) == ESP_ERR_INVALID_ARG);
    assert(ryz_system_ui_page_token().generation == generation);
}

static void test_down_then_external_navigation_does_not_click_new_page(void)
{
    const ryz_system_ui_snapshot_t snapshot = {.configured = true};
    ready_ui(&snapshot);
    ryz_touch_sample_t sample = touch(RYZ_TOUCH_DOWN, true, 100, 70);
    assert(process_ok(&sample, &snapshot) == RYZ_SYSTEM_UI_ACTION_NONE);
    const ryz_ui_nav_token_t home = ryz_system_ui_page_token();
    assert(ryz_system_ui_navigate(home, RYZ_SYSTEM_UI_ROUTE_SETTINGS) == ESP_OK);
    assert(ryz_system_ui_render(&snapshot) == ESP_OK);
    sample = touch(RYZ_TOUCH_UP, true, 100, 160);
    assert(process_ok(&sample, &snapshot) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_SETTINGS);
    sample = touch(RYZ_TOUCH_DOWN, true, 100, 160);
    assert(process_ok(&sample, &snapshot) == RYZ_SYSTEM_UI_ACTION_NONE);
    sample = touch(RYZ_TOUCH_UP, false, 0, 0);
    assert(process_ok(&sample, &snapshot) == RYZ_SYSTEM_UI_ACTION_REPROVISION);
}

static void test_process_sample_render_failure_never_delivers_and_requires_release(void)
{
    const ryz_system_ui_snapshot_t snapshot = {.configured = true};
    open_settings(&snapshot);
    ryz_touch_sample_t sample = touch(RYZ_TOUCH_DOWN, true, 100, 160);
    assert(process_ok(&sample, &snapshot) == RYZ_SYSTEM_UI_ACTION_NONE);
    s_show_error = ESP_ERR_TIMEOUT;
    sample = touch(RYZ_TOUCH_UP, false, 0, 0);
    ryz_system_ui_action_t action = RYZ_SYSTEM_UI_ACTION_REPROVISION;
    assert(ryz_system_ui_process_sample(&sample, ESP_OK, &snapshot, &action) == ESP_ERR_TIMEOUT);
    assert(action == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_take_action() == RYZ_SYSTEM_UI_ACTION_NONE);
    display_spy_reset();
    assert(ryz_system_ui_render(&snapshot) == ESP_OK);
    display_spy_reset();
    sample = touch(RYZ_TOUCH_DOWN, true, 100, 160);
    assert(process_ok(&sample, &snapshot) == RYZ_SYSTEM_UI_ACTION_NONE);
    sample = touch(RYZ_TOUCH_MOVE, true, 100, 160);
    assert(process_ok(&sample, &snapshot) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(s_show_count == 0);
    sample = (ryz_touch_sample_t){.event = RYZ_TOUCH_NONE};
    assert(process_ok(&sample, &snapshot) == RYZ_SYSTEM_UI_ACTION_NONE);
    sample = touch(RYZ_TOUCH_DOWN, true, 100, 160);
    assert(process_ok(&sample, &snapshot) == RYZ_SYSTEM_UI_ACTION_NONE);
    sample = touch(RYZ_TOUCH_UP, false, 0, 0);
    assert(process_ok(&sample, &snapshot) == RYZ_SYSTEM_UI_ACTION_REPROVISION);
}

static void test_none_release_clears_capture_without_click(void)
{
    const ryz_system_ui_snapshot_t snapshot = {0};
    ready_ui(&snapshot);
    ryz_touch_sample_t sample = touch(RYZ_TOUCH_DOWN, true, 100, 220);
    assert(process_ok(&sample, &snapshot) == RYZ_SYSTEM_UI_ACTION_NONE);
    display_spy_reset();
    sample = (ryz_touch_sample_t){.event = RYZ_TOUCH_NONE};
    assert(process_ok(&sample, &snapshot) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(s_show_count == 1);
    sample = touch(RYZ_TOUCH_UP, true, 100, 220);
    assert(process_ok(&sample, &snapshot) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_HOME);
}

static void test_duplicate_down_cannot_retarget_contact(void)
{
    const ryz_system_ui_snapshot_t snapshot = {0};
    for (unsigned variant = 0; variant < 3; ++variant) {
        ready_ui(&snapshot);
        /* Cover a target, an empty region, and a target cancelled by MOVE. */
        ryz_touch_sample_t sample = touch(RYZ_TOUCH_DOWN, true, 100,
                                          variant == 1 ? 150 : 220);
        assert(process_ok(&sample, &snapshot) == RYZ_SYSTEM_UI_ACTION_NONE);
        if (variant == 2) {
            sample = touch(RYZ_TOUCH_MOVE, true, 100, 150);
            assert(process_ok(&sample, &snapshot) == RYZ_SYSTEM_UI_ACTION_NONE);
        }
        sample = touch(RYZ_TOUCH_DOWN, true, 200, 220);
        assert(process_ok(&sample, &snapshot) == RYZ_SYSTEM_UI_ACTION_NONE);
        sample = touch(RYZ_TOUCH_UP, false, 0, 0);
        assert(process_ok(&sample, &snapshot) == RYZ_SYSTEM_UI_ACTION_NONE);
        assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_HOME);
        sample = touch(RYZ_TOUCH_DOWN, true, 200, 220);
        assert(process_ok(&sample, &snapshot) == RYZ_SYSTEM_UI_ACTION_NONE);
        sample = touch(RYZ_TOUCH_UP, false, 0, 0);
        assert(process_ok(&sample, &snapshot) == RYZ_SYSTEM_UI_ACTION_NONE);
        assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_SETTINGS);
    }
}

static void test_unpresented_page_and_invalid_samples_do_not_arm(void)
{
    const ryz_system_ui_snapshot_t snapshot = {0};
    ryz_system_ui_reset();
    display_spy_reset();
    ryz_touch_sample_t sample = touch(RYZ_TOUCH_DOWN, true, 200, 220);
    assert(process_ok(&sample, &snapshot) == RYZ_SYSTEM_UI_ACTION_NONE);
    sample = touch(RYZ_TOUCH_UP, false, 0, 0);
    assert(process_ok(&sample, &snapshot) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_HOME);
    assert(s_show_count == 0);
    ready_ui(&snapshot);
    sample = touch(RYZ_TOUCH_DOWN, true, 100, 220);
    assert(process_ok(&sample, &snapshot) == RYZ_SYSTEM_UI_ACTION_NONE);
    sample = (ryz_touch_sample_t){.event = RYZ_TOUCH_DOWN, .pressed = false};
    ryz_system_ui_action_t action = RYZ_SYSTEM_UI_ACTION_REPROVISION;
    assert(ryz_system_ui_process_sample(&sample, ESP_OK, &snapshot, &action) == ESP_ERR_INVALID_ARG);
    assert(action == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_take_action() == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_process_sample(NULL, ESP_OK, &snapshot, &action) == ESP_ERR_INVALID_ARG);
    assert(action == RYZ_SYSTEM_UI_ACTION_NONE);
}

typedef struct {
    unsigned calls;
    ryz_system_ui_action_t observed;
    bool navigate;
    ryz_ui_nav_token_t expected;
} render_probe_t;

static bool probe_render_frame(void *context)
{
    render_probe_t *probe = context;
    ++probe->calls;
    probe->observed = ryz_system_ui_take_action();
    assert(probe->observed == RYZ_SYSTEM_UI_ACTION_NONE);
    if (probe->navigate) {
        assert(ryz_system_ui_navigate(probe->expected,
                                     RYZ_SYSTEM_UI_ROUTE_SETTINGS) == ESP_OK);
    }
    return false;
}

static void test_inflight_render_callback_cannot_deliver_pending_action(void)
{
    const ryz_system_ui_snapshot_t snapshot = {.configured = true};
    open_settings(&snapshot);
    ryz_touch_sample_t sample = touch(RYZ_TOUCH_DOWN, true, 100, 160);
    assert(ryz_system_ui_handle_touch(&sample, &snapshot) == ESP_OK);
    sample = touch(RYZ_TOUCH_UP, false, 0, 0);
    assert(ryz_system_ui_handle_touch(&sample, &snapshot) == ESP_OK);
    /* Leave the existing action pending, then try taking it from the trusted
     * display callback before this later frame's transfer has completed. */
    render_probe_t probe = {.observed = RYZ_SYSTEM_UI_ACTION_REPROVISION};
    display_spy_reset();
    assert(ryz_system_ui_render_checked(&snapshot, probe_render_frame,
                                        &probe, NULL) == ESP_OK);
    assert(probe.calls == 1 && probe.observed == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(s_show_count == 1);
}

static void test_navigation_during_render_cannot_present_the_old_generation(void)
{
    const ryz_system_ui_snapshot_t snapshot = {.configured = true};
    ready_ui(&snapshot);
    render_probe_t probe = {
        .observed = RYZ_SYSTEM_UI_ACTION_REPROVISION,
        .navigate = true,
        .expected = ryz_system_ui_page_token(),
    };
    assert(ryz_system_ui_render_checked(&snapshot, probe_render_frame,
                                        &probe, NULL) == ESP_ERR_INVALID_STATE);
    assert(probe.calls == 1);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_SETTINGS);
    assert(ryz_system_ui_page_token().generation > probe.expected.generation);
    display_spy_reset();
    ryz_touch_sample_t sample = touch(RYZ_TOUCH_DOWN, true, 100, 160);
    assert(process_ok(&sample, &snapshot) == RYZ_SYSTEM_UI_ACTION_NONE);
    sample = touch(RYZ_TOUCH_UP, false, 0, 0);
    assert(process_ok(&sample, &snapshot) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(s_show_count == 0);
    assert(ryz_system_ui_render(&snapshot) == ESP_OK);
    sample = (ryz_touch_sample_t){.event = RYZ_TOUCH_NONE};
    assert(process_ok(&sample, &snapshot) == RYZ_SYSTEM_UI_ACTION_NONE);
    sample = touch(RYZ_TOUCH_DOWN, true, 100, 160);
    assert(process_ok(&sample, &snapshot) == RYZ_SYSTEM_UI_ACTION_NONE);
    sample = touch(RYZ_TOUCH_UP, false, 0, 0);
    assert(process_ok(&sample, &snapshot) == RYZ_SYSTEM_UI_ACTION_REPROVISION);
}

int main(void)
{
    test_boots_to_home_and_renders();
    test_unavailable_storage_renders_unknown_usage();
    test_checked_render_forwards_cancellation();
    test_apps_tab_highlights_then_activates();
    test_settings_tab_activates_settings_page();
    test_storage_region_is_not_an_ap_setup_shortcut();
    test_home_wlan_chart_opens_ap_setup();
    test_ap_ready_renders_escaped_wifi_qr_with_quiet_zone();
    test_largest_supported_qr_version_fits_display();
    test_ap_setup_reports_connecting_and_failed_states();
    test_ap_setup_rejects_unterminated_portal_ip();
    test_secondary_page_back_highlights_then_returns_home();
    test_move_out_cancels_even_if_pointer_returns();
    test_positionless_up_uses_last_in_bounds_sample();
    test_up_outside_cancels_and_clears_highlight();
    test_configured_network_card_opens_network_status();
    test_home_reports_connecting_and_failed_network_states();
    test_apps_and_settings_use_bottom_navigation_to_return_home();
    test_disabled_lua_tile_does_not_activate();
    test_settings_network_row_opens_ap_setup();
    test_settings_reprovision_action_is_armed_on_down_and_taken_once();
    test_settings_reprovision_move_out_cancels_action();
    test_failed_reprovision_release_cannot_deliver_action();
    test_network_becoming_configured_opens_current_status();
    test_physical_error_ignores_sample_and_guards_held_finger();
    test_physical_error_clears_preexisting_pending_action();
    test_reset_and_async_navigation_revoke_old_tokens();
    test_down_then_external_navigation_does_not_click_new_page();
    test_process_sample_render_failure_never_delivers_and_requires_release();
    test_none_release_clears_capture_without_click();
    test_duplicate_down_cannot_retarget_contact();
    test_unpresented_page_and_invalid_samples_do_not_arm();
    test_inflight_render_callback_cannot_deliver_pending_action();
    test_navigation_during_render_cannot_present_the_old_generation();
    return 0;
}
