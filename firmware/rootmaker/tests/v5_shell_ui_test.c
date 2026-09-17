/* Real native system shell + status cache + navigation + LVGL/FreeType/QR.
 * Only immutable service views, physical samples and panel/OS are controlled.
 * This is not Workbench scheduling, networking, BLE, Store or device evidence. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ryz_system_ui.h"
#include "ryz_v5_status.h"
#include "ryz_v5_apps.h"
#include "ryz_v5_render.h"
#include "ryz_v5_assets.h"
#include "ryz_v5_widgets.h"
#include "ryz_v5_fault.h"
#include "ryz_v5_render_dirty.h"
#include "ryz_font.h"
#include "ryz_display_host.h"
#include "ryz_pixels_host.h"

#ifdef NDEBUG
#error "Native shell checks require active assertions"
#endif

static ryz_system_ui_snapshot_t system_view;
static ryz_v5_status_t service_view;
static unsigned groups;

static void passed(const char *name)
{ ++groups; printf("PASS %s\n", name); }

static bool has_label(lv_obj_t *object, const char *text)
{
    if (lv_obj_check_type(object, &lv_label_class) &&
        !strcmp(lv_label_get_text(object), text)) return true;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
        if (has_label(lv_obj_get_child(object, (int32_t)i), text)) return true;
    return false;
}

static void assert_release_label_fits(lv_obj_t *object, const char *text)
{
    if (lv_obj_check_type(object, &lv_label_class) &&
        !strcmp(lv_label_get_text(object), text)) {
        const lv_font_t *font = lv_obj_get_style_text_font(object, 0);
        for (const char *ch = text; *ch; ++ch) {
            lv_font_glyph_dsc_t glyph;
            assert(lv_font_get_glyph_dsc(font, &glyph, (unsigned char)*ch,
                                         (unsigned char)ch[1]));
            assert(!glyph.is_placeholder);
        }
        lv_point_t size;
        lv_text_get_size(&size, text, font,
            lv_obj_get_style_text_letter_space(object, 0),
            lv_obj_get_style_text_line_space(object, 0), LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        assert(size.x <= lv_obj_get_width(object));
        assert(size.y <= lv_obj_get_height(object));
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
        assert_release_label_fits(lv_obj_get_child(object, (int32_t)i), text);
}

static ryz_system_ui_action_t sample(ryz_touch_event_t event, bool positioned,
                                     unsigned x, unsigned y)
{
    const ryz_touch_sample_t value = {
        .event = event, .pressed = event == RYZ_TOUCH_DOWN || event == RYZ_TOUCH_MOVE,
        .has_position = positioned, .x = (uint16_t)x, .y = (uint16_t)y,
    };
    ryz_system_ui_action_t action = RYZ_SYSTEM_UI_ACTION_OTA_UPDATE;
    assert(ryz_system_ui_process_sample(&value, ESP_OK, &system_view, &action) == ESP_OK);
    assert(ryz_system_ui_take_action() == RYZ_SYSTEM_UI_ACTION_NONE);
    return action;
}

static void released(void)
{ assert(sample(RYZ_TOUCH_NONE, false, 0, 0) == RYZ_SYSTEM_UI_ACTION_NONE); }

static ryz_system_ui_action_t tap(unsigned x, unsigned y)
{
    assert(sample(RYZ_TOUCH_DOWN, true, x, y) == RYZ_SYSTEM_UI_ACTION_NONE);
    return sample(RYZ_TOUCH_UP, false, 0, 0);
}

static void publish(void)
{
    service_view.network.system = system_view;
    (void)ryz_v5_status_set(&service_view);
}

static void fresh(void)
{
    ryz_system_ui_reset();
    ryz_v5_apps_bind(NULL);
    ryz_v5_ble_bind_services(NULL);
    ryz_host_display_reset();
    memset(&system_view, 0, sizeof(system_view));
    memset(&service_view, 0, sizeof(service_view));
    service_view.ble.unavailable = true;
    publish();
    assert(ryz_system_ui_render(&system_view) == ESP_OK);
    released();
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_HOME);
    assert(ryz_host_display_shows() > 0 && ryz_host_display_rows() >= 240);
}

static void root_routes(void)
{
    fresh();
    const ryz_ui_nav_token_t old_home = ryz_system_ui_page_token();
    assert(tap(120, 220) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_APPS);
    assert(has_label(lv_screen_active(), "LUA FILES"));
    assert(has_label(lv_screen_active(), "RYZOBEE"));
    assert(!has_label(lv_screen_active(), "LUA TOOL"));
    const size_t apps_shows = ryz_host_display_shows();
    assert(tap(20, 16) == RYZ_SYSTEM_UI_ACTION_NONE); /* Brand is not Back. */
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_APPS);
    assert(ryz_host_display_shows() == apps_shows);
    assert(tap(70, 54) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(has_label(lv_screen_active(), "LUA FILES"));
    assert(tap(20, 16) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(has_label(lv_screen_active(), "LUA FILES"));
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_APPS);
    assert(tap(70, 54) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(tap(200, 220) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_SETTINGS);
    const ryz_ui_nav_token_t settings = ryz_system_ui_page_token();
    assert(tap(120, 145) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_DISPLAY);
    assert(has_label(lv_screen_active(),"DISPLAY"));
    /* Frozen V5 CHILD header has only the back glyph, no obsolete left rail. */
    assert(ryz_host_display_pixel(1,16)==0);
    assert(tap(20,16)==RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_SETTINGS);
    assert(ryz_system_ui_page_token().generation!=settings.generation);
    assert(ryz_system_ui_navigate(old_home, RYZ_SYSTEM_UI_ROUTE_AP_SETUP) == ESP_ERR_INVALID_STATE);
    assert(tap(40, 220) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_HOME);
    assert(tap(120, 220) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(has_label(lv_screen_active(), "LUA FILES"));
    assert(!has_label(lv_screen_active(), "LUA TOOL"));
    passed("native APPS stream root / Lua child / Back / reentry; DISPLAY entry; stale route token rejected");
}

static void release_version_routes(void)
{
    fresh();
    strcpy(system_view.firmware_version, RYZ_HOST_FIRMWARE_VERSION);
    strcpy(service_view.version.running, RYZ_HOST_FIRMWARE_VERSION);
    strcpy(service_view.version.build, "HOST FIXTURE");
    publish();
    assert(tap(200, 220) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_SETTINGS);
    assert(has_label(lv_screen_active(), RYZ_HOST_FIRMWARE_VERSION));
    assert_release_label_fits(lv_screen_active(), RYZ_HOST_FIRMWARE_VERSION);
    assert(tap(120, 180) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_VERSION_CURRENT);
    assert(has_label(lv_screen_active(), "V" RYZ_HOST_FIRMWARE_VERSION));
    assert_release_label_fits(lv_screen_active(), "V" RYZ_HOST_FIRMWARE_VERSION);
    assert(tap(120, 90) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_VERSION_INFO);
    assert(has_label(lv_screen_active(), "V" RYZ_HOST_FIRMWARE_VERSION));
    assert(has_label(lv_screen_active(), "HOST FIXTURE"));
    assert_release_label_fits(lv_screen_active(), "V" RYZ_HOST_FIRMWARE_VERSION);
    passed("release version follows PROJECT_VER across Settings, Version and Info; static glyphs fit");
}

/* This is the trusted asynchronous Owner seam, not a simulated network
 * request. Capture its token before accepting/publishing the new state. */
static void owner_page(ryz_system_ui_route_t route)
{
    assert(ryz_system_ui_navigate(ryz_system_ui_page_token(), route) == ESP_OK);
    assert(ryz_system_ui_render(&system_view) == ESP_OK);
    released();
}

static lv_obj_t *image_for(lv_obj_t *root, const lv_image_dsc_t *asset)
{
    if (lv_obj_check_type(root, &lv_image_class) && lv_image_get_src(root) == asset)
        return root;
    lv_obj_t *found = NULL;
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *child = image_for(lv_obj_get_child(root, (int32_t)i), asset);
        if (child) { assert(!found); found = child; }
    }
    return found;
}

static void assert_header_icons(bool connected)
{
    /* 2026-09-06 live Figma TOP/CHILD: 16px Wi-Fi/BLE, battery stroke
     * bounds preserved. Positions are rounded once, never runtime-scaled. */
    const struct {
        const lv_image_dsc_t *asset;
        int x, y, w, h;
        uint32_t color;
    } icons[] = {
        {&ryz_v5_asset_wifi_status, 136, 8, 16, 16, connected ? V5_BODY : V5_DISABLED},
        {&ryz_v5_asset_ble_status, 154, 8, 16, 16, V5_DISABLED},
        {&ryz_v5_asset_battery_outline, 172, 10, 18, 12, V5_DISABLED},
        {&ryz_v5_asset_battery_terminal, 191, 14, 2, 5, V5_DISABLED},
    };
    for (unsigned i = 0; i < sizeof(icons) / sizeof(icons[0]); ++i) {
        lv_obj_t *object = image_for(lv_screen_active(), icons[i].asset);
        assert(object);
        lv_area_t box;
        lv_obj_get_coords(object, &box);
        assert(box.x1 == icons[i].x && box.y1 == icons[i].y);
        assert(lv_area_get_width(&box) == icons[i].w);
        assert(lv_area_get_height(&box) == icons[i].h);
        assert(icons[i].asset->header.cf == LV_COLOR_FORMAT_A8);
        assert(icons[i].asset->header.w == icons[i].w);
        assert(icons[i].asset->header.h == icons[i].h);
        assert(lv_color_eq(lv_obj_get_style_image_recolor(object, 0),
                           lv_color_hex(icons[i].color)));
        assert(box.y1 >= 8 && box.y2 < 24 && box.x2 < 196); /* Time starts at 196. */
        if (i) assert(icons[i-1].x + icons[i-1].w < box.x1);
    }
    /* Unknown power status must not copy the design's example 75% fill. */
    for (unsigned y = 13; y < 19; ++y)
        for (unsigned x = 175; x < 186; ++x)
            assert(ryz_host_display_pixel(x, y) == 0);
}

static void shared_header_icons(void)
{
    fresh();
    assert_header_icons(false);
    lv_obj_t *home_icon = image_for(lv_screen_active(), &ryz_v5_asset_home);
    assert(home_icon && ryz_v5_asset_home.header.cf == LV_COLOR_FORMAT_A8);
    assert(lv_color_eq(lv_obj_get_style_image_recolor(home_icon, 0), lv_color_hex(V5_ORANGE)));
    assert(has_label(lv_screen_active(), "--:--"));
    system_view.connected = true;
    strcpy(system_view.time_hhmm, "18:43");
    publish();
    assert(ryz_system_ui_render(&system_view) == ESP_OK);
    assert_header_icons(true);
    assert(has_label(lv_screen_active(), "18:43"));
    assert(tap(200, 220) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert_header_icons(true);
    /* Focus changes color, not the underlying bitmap resource. */
    home_icon = image_for(lv_screen_active(), &ryz_v5_asset_home);
    lv_obj_t *settings_icon = image_for(lv_screen_active(), &ryz_v5_asset_settings);
    assert(home_icon && settings_icon);
    assert(lv_color_eq(lv_obj_get_style_image_recolor(home_icon, 0), lv_color_hex(V5_MUTED)));
    assert(lv_color_eq(lv_obj_get_style_image_recolor(settings_icon, 0), lv_color_hex(V5_ORANGE)));
    system_view.connected = false;
    publish();
    owner_page(RYZ_SYSTEM_UI_ROUTE_WIFI_OFF);
    assert_header_icons(false);
    assert(image_for(lv_screen_active(), &ryz_v5_asset_back));
    passed("shared V5 header and focus recolor: same A8 assets, exact geometry, state and unknown battery");
}

static void quiet_menu_sample(ryz_touch_event_t event, bool positioned,
                              unsigned x, unsigned y)
{
    static uint16_t pixels[240 * 240];
    memcpy(pixels, ryz_host_display_frame(), sizeof(pixels));
    const size_t shows = ryz_host_display_shows();
    const size_t blits = ryz_host_display_blits();
    const uint32_t revision = ryz_system_ui_render_revision();
    const ryz_ui_nav_token_t page = ryz_system_ui_page_token();
    assert(sample(event, positioned, x, y) == RYZ_SYSTEM_UI_ACTION_NONE);
    const bool identical = !memcmp(pixels, ryz_host_display_frame(), sizeof(pixels));
    if (!identical || ryz_host_display_shows() != shows ||
        ryz_host_display_blits() != blits) {
        fprintf(stderr, "MENU_SAMPLE event=%d pixels_equal=%d shows_delta=%zu blits_delta=%zu\n",
                event, identical, ryz_host_display_shows() - shows,
                ryz_host_display_blits() - blits);
    }
    assert(identical);
    assert(ryz_host_display_shows() == shows);
    assert(ryz_host_display_blits() == blits);
    assert(ryz_system_ui_render_revision() == revision);
    assert(ryz_system_ui_page_token().generation == page.generation);
}

static void ordinary_menu_capture_is_not_a_visual_change(void)
{
    static const struct {
        ryz_system_ui_route_t from, to;
        unsigned x, y;
    } cases[] = {
        {RYZ_SYSTEM_UI_ROUTE_HOME, RYZ_SYSTEM_UI_ROUTE_APPS, 120, 220},
        {RYZ_SYSTEM_UI_ROUTE_HOME, RYZ_SYSTEM_UI_ROUTE_SETTINGS, 200, 220},
        {RYZ_SYSTEM_UI_ROUTE_HOME, RYZ_SYSTEM_UI_ROUTE_HOME, 40, 220},
        {RYZ_SYSTEM_UI_ROUTE_HOME, RYZ_SYSTEM_UI_ROUTE_HOME, 40, 134},
        {RYZ_SYSTEM_UI_ROUTE_SETTINGS, RYZ_SYSTEM_UI_ROUTE_HOME, 40, 220},
        {RYZ_SYSTEM_UI_ROUTE_SETTINGS, RYZ_SYSTEM_UI_ROUTE_APPS, 120, 220},
        {RYZ_SYSTEM_UI_ROUTE_SETTINGS, RYZ_SYSTEM_UI_ROUTE_SETTINGS, 200, 220},
        {RYZ_SYSTEM_UI_ROUTE_SETTINGS, RYZ_SYSTEM_UI_ROUTE_WIFI_NO_NETWORK, 100, 48},
        {RYZ_SYSTEM_UI_ROUTE_SETTINGS, RYZ_SYSTEM_UI_ROUTE_BLE_OFF, 100, 81},
        {RYZ_SYSTEM_UI_ROUTE_SETTINGS, RYZ_SYSTEM_UI_ROUTE_SCRIPTS_SETTINGS, 100, 114},
        {RYZ_SYSTEM_UI_ROUTE_SETTINGS, RYZ_SYSTEM_UI_ROUTE_VERSION_CURRENT, 120, 180},
    };
    fresh();
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        owner_page(cases[i].from);
        quiet_menu_sample(RYZ_TOUCH_DOWN, true, cases[i].x, cases[i].y);
        quiet_menu_sample(RYZ_TOUCH_NONE, false, 0, 0); /* Cancel without UP. */
        quiet_menu_sample(RYZ_TOUCH_UP, false, 0, 0);   /* Cannot activate later. */

        quiet_menu_sample(RYZ_TOUCH_DOWN, true, cases[i].x, cases[i].y);
        quiet_menu_sample(RYZ_TOUCH_MOVE, true, cases[i].x, cases[i].y);
        quiet_menu_sample(RYZ_TOUCH_MOVE, true, 0, 120); /* Leave every target. */
        quiet_menu_sample(RYZ_TOUCH_MOVE, true, cases[i].x, cases[i].y);
        quiet_menu_sample(RYZ_TOUCH_UP, false, 0, 0);
        assert(ryz_system_ui_current_route() == cases[i].from);

        quiet_menu_sample(RYZ_TOUCH_DOWN, true, cases[i].x, cases[i].y);
        const size_t shows = ryz_host_display_shows();
        const size_t blits = ryz_host_display_blits();
        const uint32_t revision = ryz_system_ui_render_revision();
        assert(sample(RYZ_TOUCH_UP, false, 0, 0) == RYZ_SYSTEM_UI_ACTION_NONE);
        assert(ryz_system_ui_current_route() == cases[i].to);
        const bool readout=cases[i].from==RYZ_SYSTEM_UI_ROUTE_HOME && cases[i].y==134;
        const bool unchanged=cases[i].from==cases[i].to;
        assert(ryz_host_display_shows() == shows + (unchanged?0:1));
        assert(unchanged ? ryz_host_display_blits()==blits : ryz_host_display_blits()>blits);
        assert(ryz_system_ui_render_revision() == (uint32_t)(revision + (readout?0:1)));
    }
    passed("ordinary menus only render on accepted UP; HOME scalar readouts never trigger a repaint");
}

static void open_wifi_from_settings(void)
{
    assert(tap(200, 220) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_SETTINGS);
    assert(tap(100, 48) == RYZ_SYSTEM_UI_ACTION_NONE);
}

static void assert_home_selection(ryz_home_selection_t expected)
{
    const ryz_home_selection_t actual = ryz_system_ui_home_selection();
    assert(actual.metric == expected.metric);
    assert(actual.generation == expected.generation);
    assert(actual.selected_at_us == expected.selected_at_us);
}


static void apps_stream_does_not_capture_a_navigation_move(void)
{
    fresh();
    assert(tap(120, 220) == RYZ_SYSTEM_UI_ACTION_NONE);
    quiet_menu_sample(RYZ_TOUCH_DOWN, true, 40, 220);
    quiet_menu_sample(RYZ_TOUCH_MOVE, true, 40, 50);
    quiet_menu_sample(RYZ_TOUCH_UP, false, 0, 0);
    assert(has_label(lv_screen_active(), "LUA FILES"));
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_APPS);
    assert(tap(200, 220) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_SETTINGS);
    passed("a navigation DOWN moving into LUA TOOL neither opens it nor steals the shell UP");
}

static void network_flow(void)
{
    fresh();
    service_view.network.enabled = true;
    service_view.network.can_setup = true;
    publish();
    open_wifi_from_settings();
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_WIFI_NO_NETWORK);
    const ryz_ui_nav_token_t setup_page = ryz_system_ui_page_token();
    assert(sample(RYZ_TOUCH_DOWN,true,20,172)==RYZ_SYSTEM_UI_ACTION_NONE);
    assert(sample(RYZ_TOUCH_MOVE,true,50,172)==RYZ_SYSTEM_UI_ACTION_NONE);
    assert(sample(RYZ_TOUCH_UP,false,0,0)==RYZ_SYSTEM_UI_ACTION_NONE);
    assert(tap(20,172) == RYZ_SYSTEM_UI_ACTION_REPROVISION);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_WIFI_NO_NETWORK);
    assert(ryz_system_ui_page_token().generation == setup_page.generation);
    /* UI intent alone must not claim that an AP is up or that STA joined. */
    assert(!system_view.connected && !system_view.ap_active);
    system_view.state = RYZ_SYSTEM_UI_NETWORK_AP_STARTING;
    publish();
    owner_page(RYZ_SYSTEM_UI_ROUTE_AP_SETUP);
    assert(ryz_system_ui_tick(&system_view) == ESP_OK);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_AP_SETUP);
    system_view.state = RYZ_SYSTEM_UI_NETWORK_AP_READY;
    system_view.ap_active = true;
    strcpy(system_view.ap_ssid, "SHELL-FIXTURE");
    strcpy(system_view.ap_password, "fixture-password");
    strcpy(system_view.portal_ip, "192.168.4.1");
    publish();
    assert(ryz_system_ui_render(&system_view) == ESP_OK); /* Real managed QR. */
    assert(ryz_system_ui_tick(&system_view) == ESP_OK);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_AP_SETUP);
    system_view.state = RYZ_SYSTEM_UI_NETWORK_CREDENTIALS_RECEIVED;
    publish();
    assert(ryz_system_ui_tick(&system_view) == ESP_OK);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_WIFI_JOINING);
    system_view.state = RYZ_SYSTEM_UI_NETWORK_CONNECTING;
    publish();
    assert(ryz_system_ui_tick(&system_view) == ESP_OK);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_WIFI_JOINING);
    system_view.state = RYZ_SYSTEM_UI_NETWORK_ONLINE;
    system_view.connected = true;
    strcpy(service_view.network.ipv4, "192.0.2.4");
    publish();
    assert(ryz_system_ui_tick(&system_view) == ESP_OK);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_WIFI_READY);
    released();
    assert(tap(140, 196) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_HOME);
    assert(tap(200, 220) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(tap(100, 48) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_WIFI_CONNECTED);
    assert(tap(100, 123) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_WIFI_INFO);
    assert(tap(20, 16) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_WIFI_CONNECTED);
    assert(tap(20, 16) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_SETTINGS);
    passed("Wi-Fi intent != success; AP / JOIN / READY follow published state; Info Back preserves stack");
}

static void modal_and_unavailable(void)
{
    fresh();
    assert(tap(200, 220) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(tap(100, 81) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_BLE_OFF);
    assert(has_label(lv_screen_active(), "BLE UNAVAILABLE"));
    assert(tap(213, 46) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_BLE_OFF);
    const ryz_ui_nav_token_t underlying = ryz_system_ui_page_token();
    owner_page(RYZ_SYSTEM_UI_ROUTE_BLE_FORGET);
    const ryz_ui_nav_token_t modal = ryz_system_ui_page_token();
    assert(modal.modal);
    assert(ryz_system_ui_navigate(underlying, RYZ_SYSTEM_UI_ROUTE_HOME) == ESP_ERR_INVALID_STATE);
    assert(ryz_system_ui_navigate(modal, RYZ_SYSTEM_UI_ROUTE_WIFI_INFO) == ESP_ERR_INVALID_STATE);
    assert(tap(40, 230) == RYZ_SYSTEM_UI_ACTION_NONE); /* No background Home nav. */
    assert(tap(213, 46) == RYZ_SYSTEM_UI_ACTION_NONE); /* No underlying toggle. */
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_BLE_FORGET);
    assert(ryz_system_ui_page_token().generation == modal.generation);
    assert(tap(20, 16) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_BLE_OFF);
    assert(!ryz_system_ui_page_token().modal);
    assert(ryz_system_ui_navigate(modal, RYZ_SYSTEM_UI_ROUTE_BLE_SUCCESS) == ESP_ERR_INVALID_STATE);
    assert(tap(20, 16) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_SETTINGS);
    assert(tap(100, 114) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_SCRIPTS_SETTINGS);
    assert(tap(120, 145) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(tap(120, 179) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_SCRIPTS_SETTINGS);
    owner_page(RYZ_SYSTEM_UI_ROUTE_SCRIPTS_SAVED);
    assert(has_label(lv_screen_active(), "NOT CONFIRMED"));
    assert(!has_label(lv_screen_active(), "BOOT SCRIPT SET"));
    assert(tap(150, 198) == RYZ_SYSTEM_UI_ACTION_NONE);
    ryz_v5_status_t observed;
    ryz_v5_status_get(&observed);
    assert(observed.ble.unavailable && !observed.ble.bonded && !observed.ble.pairing_active);
    assert(!observed.scripts.boot_known && observed.scripts.result == RYZ_V5_SCRIPTS_IDLE);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_SCRIPTS_SAVED);
    passed("modal intercept / Back / late-token rejection; unavailable BLE and Scripts never mutate or claim success");
}

static struct {
    unsigned calls;
    ryz_v5_ble_action_t action;
    uint32_t operation, value;
    esp_err_t result;
} ble_commands;

static esp_err_t ble_submit_spy(void *context,ryz_v5_ble_action_t action,
                               uint32_t operation,uint32_t value)
{
    assert(context==&ble_commands);
    ++ble_commands.calls;
    ble_commands.action=action;
    ble_commands.operation=operation;
    ble_commands.value=value;
    return ble_commands.result;
}

static void bind_ble_spy(void)
{
    memset(&ble_commands,0,sizeof(ble_commands));
    const ryz_v5_ble_binding_t binding={.submit=ble_submit_spy,.context=&ble_commands};
    ryz_v5_ble_bind_services(&binding);
}

static void ble_numeric_identity(void)
{
    fresh(); bind_ble_spy();
    service_view.ble=(ryz_v5_ble_model_t){.page=RYZ_V5_BLE_PAIRING,
        .state=RYZ_V5_BLE_STATE_VERIFY_CODE,.enabled=true,.bond_known=true,
        .pairing_active=true,.operation_id=17,.compare_pending=true,.compare_value=12345,
        .remaining_valid=true,.remaining_seconds=20,.pairing_window_seconds=30};
    publish(); owner_page(RYZ_SYSTEM_UI_ROUTE_BLE_PAIRING);
    assert(has_label(lv_screen_active(),"012345"));
    sample(RYZ_TOUCH_DOWN,true,165,188);
    service_view.ble.compare_value=54321; publish(); /* Cache changed; no new frame. */
    sample(RYZ_TOUCH_UP,false,0,0);
    assert(!ble_commands.calls);
    assert(ryz_system_ui_tick(&system_view)==ESP_OK);
    released();
    sample(RYZ_TOUCH_DOWN,true,165,188);
    service_view.ble.compare_value=111111; publish();
    assert(ryz_system_ui_render(&system_view)==ESP_OK);
    service_view.ble.compare_value=54321; publish();
    assert(ryz_system_ui_render(&system_view)==ESP_OK);
    sample(RYZ_TOUCH_UP,false,0,0);
    assert(!ble_commands.calls); /* A lost capability never rearms on restoration. */
    sample(RYZ_TOUCH_DOWN,true,165,188);
    ++service_view.ble.operation_id; publish();
    sample(RYZ_TOUCH_UP,false,0,0);
    assert(!ble_commands.calls);
    assert(ryz_system_ui_render(&system_view)==ESP_OK);
    sample(RYZ_TOUCH_DOWN,true,165,188);
    service_view.ble.compare_pending=false; publish();
    assert(ryz_system_ui_tick(&system_view)==ESP_OK);
    service_view.ble.compare_pending=true; publish();
    assert(ryz_system_ui_render(&system_view)==ESP_OK);
    sample(RYZ_TOUCH_UP,false,0,0);
    assert(!ble_commands.calls);
    sample(RYZ_TOUCH_DOWN,true,165,188);
    service_view.ble.remaining_seconds=0; publish();
    assert(ryz_system_ui_tick(&system_view)==ESP_OK);
    service_view.ble.remaining_seconds=10; publish();
    assert(ryz_system_ui_render(&system_view)==ESP_OK);
    sample(RYZ_TOUCH_UP,false,0,0);
    assert(!ble_commands.calls);
    sample(RYZ_TOUCH_DOWN,true,165,188);
    owner_page(RYZ_SYSTEM_UI_ROUTE_HOME);
    sample(RYZ_TOUCH_UP,false,0,0);
    assert(!ble_commands.calls);
    owner_page(RYZ_SYSTEM_UI_ROUTE_BLE_PAIRING);
    sample(RYZ_TOUCH_DOWN,true,165,188);
    /* A no-change refresh now correctly transfers nothing. Change visible
     * content so this test still injects a real in-flight flush failure. */
    --service_view.ble.remaining_seconds; publish();
    ryz_host_display_fail_show(ESP_FAIL,20);
    assert(ryz_system_ui_render(&system_view)==ESP_FAIL);
    ryz_host_display_fail_show(ESP_OK,0);
    assert(ryz_system_ui_render(&system_view)==ESP_OK);
    sample(RYZ_TOUCH_UP,false,0,0);
    assert(!ble_commands.calls);
    sample(RYZ_TOUCH_DOWN,true,165,188);
    sample(RYZ_TOUCH_MOVE,true,60,188);
    sample(RYZ_TOUCH_UP,true,165,188);
    assert(!ble_commands.calls);
    sample(RYZ_TOUCH_DOWN,true,165,188);
    ryz_system_ui_action_t action=RYZ_SYSTEM_UI_ACTION_NONE;
    assert(ryz_system_ui_process_sample(NULL,ESP_FAIL,&system_view,&action)==ESP_FAIL);
    assert(ryz_system_ui_render(&system_view)==ESP_OK);
    sample(RYZ_TOUCH_UP,false,0,0);
    assert(!ble_commands.calls);
    /* The old physical UP only releases the input gate. A new DOWN is required. */
    ble_commands.result=ESP_ERR_INVALID_STATE;
    tap(165,188);
    assert(ble_commands.calls==1 && ble_commands.action==RYZ_V5_BLE_ACTION_CONFIRM_CODE &&
           ble_commands.operation==18 && ble_commands.value==54321);
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_BLE_PAIRING);
    ble_commands.result=ESP_OK;
    tap(165,188);
    assert(ble_commands.calls==2 && ble_commands.value==54321);
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_BLE_PAIRING);
    tap(165,188);
    assert(ble_commands.calls==2); /* Admitted command has not completed yet. */
    /* Reject carries the exact same displayed identity and never navigates
     * optimistically to a terminal state. */
    const ryz_v5_ble_model_t comparison=service_view.ble;
    fresh(); bind_ble_spy(); service_view.ble=comparison;
    publish(); owner_page(RYZ_SYSTEM_UI_ROUTE_BLE_PAIRING);
    tap(40,188);
    assert(ble_commands.calls==1 && ble_commands.action==RYZ_V5_BLE_ACTION_REJECT_CODE &&
           ble_commands.operation==18 && ble_commands.value==54321);
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_BLE_PAIRING);
    passed("BLE numeric approval binds rendered DOWN operation/code; changes, move, read errors revoke; admission != success");
}

static void ble_confirmation_flow(void)
{
    fresh(); bind_ble_spy();
    service_view.ble=(ryz_v5_ble_model_t){.page=RYZ_V5_BLE_PAIRED,.enabled=true,
        .state=RYZ_V5_BLE_STATE_SAVED_WAITING,
        .bond_known=true,.bonded=true,.operation_id=30,.peer_name="PEER A"};
    publish();
    tap(200,220); tap(100,81);
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_BLE_PAIRED);
    tap(100,160);
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_BLE_REPLACE);
    assert(ryz_system_ui_page_token().modal && !ble_commands.calls);
    assert(ryz_system_ui_tick(&system_view)==ESP_OK);
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_BLE_REPLACE);
    tap(40,190);
    assert(!ble_commands.calls && ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_BLE_PAIRED);
    tap(100,190);
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_BLE_FORGET);
    sample(RYZ_TOUCH_DOWN,true,160,190);
    service_view.ble.operation_id=31; strcpy(service_view.ble.peer_name,"PEER B"); publish();
    sample(RYZ_TOUCH_UP,false,0,0);
    assert(!ble_commands.calls);
    assert(ryz_system_ui_render(&system_view)==ESP_OK);
    tap(160,190);
    assert(!ble_commands.calls); /* Must reopen confirmation for the new identity. */
    tap(40,190); tap(100,190);
    ble_commands.result=ESP_FAIL;
    tap(160,190);
    assert(ble_commands.calls==1 && ble_commands.action==RYZ_V5_BLE_ACTION_CONFIRM_FORGET &&
           ble_commands.operation==31 && ryz_system_ui_page_token().modal);
    ble_commands.result=ESP_OK;
    tap(160,190);
    assert(ble_commands.calls==2 && ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_BLE_FORGET);
    service_view.ble.operation_id=32;
    service_view.ble.state=RYZ_V5_BLE_STATE_FORGETTING;
    service_view.ble.checking_state=true; publish();
    assert(ryz_system_ui_tick(&system_view)==ESP_OK);
    tap(20,16); tap(160,190);
    assert(ble_commands.calls==2 && ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_BLE_FORGET);
    service_view.ble.state=RYZ_V5_BLE_STATE_LEGACY;
    service_view.ble.checking_state=false; service_view.ble.bonded=false;
    service_view.ble.page=RYZ_V5_BLE_EMPTY; publish();
    assert(ryz_system_ui_tick(&system_view)==ESP_OK);
    assert(!ryz_system_ui_page_token().modal &&
           ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_BLE_EMPTY);
    passed("BLE replace/forget require modal confirmation; peer change rejects old question; async delete dismisses only on result");
}

static void ble_snapshot_navigation(void)
{
    fresh(); bind_ble_spy();
    service_view.ble=(ryz_v5_ble_model_t){.page=RYZ_V5_BLE_OFF,.bond_known=true,.operation_id=1};
    publish(); tap(200,220); tap(100,81);
    tap(213,46);
    assert(ble_commands.calls==1 && ble_commands.action==RYZ_V5_BLE_ACTION_ENABLE);
    assert(ryz_system_ui_tick(&system_view)==ESP_OK &&
           ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_BLE_OFF);
    service_view.ble.enabled=true; service_view.ble.operation_id=2;
    service_view.ble.page=RYZ_V5_BLE_EMPTY; publish();
    assert(ryz_system_ui_tick(&system_view)==ESP_OK &&
           ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_BLE_EMPTY);
    released();
    sample(RYZ_TOUCH_DOWN,true,20,164);
    sample(RYZ_TOUCH_MOVE,true,50,164);
    sample(RYZ_TOUCH_UP,false,0,0);
    assert(ble_commands.calls==1);
    tap(20,164);
    assert(ble_commands.calls==2 && ble_commands.action==RYZ_V5_BLE_ACTION_PAIR);
    assert(ryz_system_ui_tick(&system_view)==ESP_OK &&
           ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_BLE_EMPTY);
    service_view.ble.operation_id=3; service_view.ble.page=RYZ_V5_BLE_PAIRING;
    service_view.ble.pairing_active=true; service_view.ble.remaining_valid=true;
    service_view.ble.remaining_seconds=24; service_view.ble.pairing_window_seconds=30; publish();
    assert(ryz_system_ui_tick(&system_view)==ESP_OK &&
           ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_BLE_PAIRING);
    released();
    service_view.ble.state=RYZ_V5_BLE_STATE_VERIFY_CODE;
    service_view.ble.compare_pending=true; service_view.ble.compare_value=9876; publish();
    assert(ryz_system_ui_tick(&system_view)==ESP_OK);
    tap(165,188);
    assert(ble_commands.calls==3 && ble_commands.value==9876);
    service_view.ble.compare_pending=false;
    service_view.ble.state=RYZ_V5_BLE_STATE_COMMITTING; publish();
    assert(ryz_system_ui_tick(&system_view)==ESP_OK);
    tap(20,16); tap(165,188);
    assert(ble_commands.calls==3);
    service_view.ble.pairing_active=false; service_view.ble.bonded=true;
    service_view.ble.state=RYZ_V5_BLE_STATE_LEGACY;
    service_view.ble.page=RYZ_V5_BLE_SUCCESS; publish();
    assert(ryz_system_ui_tick(&system_view)==ESP_OK &&
           ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_BLE_SUCCESS);
    released(); tap(120,196);
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_BLE_PAIRED);
    assert(ryz_system_ui_tick(&system_view)==ESP_OK &&
           ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_BLE_PAIRED);
    tap(213,46);
    assert(ble_commands.calls==4 && ble_commands.action==RYZ_V5_BLE_ACTION_DISABLE);
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_BLE_PAIRED);
    service_view.ble.enabled=false; service_view.ble.page=RYZ_V5_BLE_OFF;
    service_view.ble.operation_id=4; publish();
    assert(ryz_system_ui_tick(&system_view)==ESP_OK &&
           ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_BLE_OFF && service_view.ble.bonded);
    owner_page(RYZ_SYSTEM_UI_ROUTE_HOME);
    service_view.ble.operation_id=80; service_view.ble.enabled=true;
    service_view.ble.page=RYZ_V5_BLE_SUCCESS; publish();
    assert(ryz_system_ui_tick(&system_view)==ESP_OK &&
           ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_HOME);
    /* Reentry with a previously saved peer is an overview, not a new Success. */
    tap(200,220); tap(100,81);
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_BLE_PAIRED);
    service_view.ble.page=RYZ_V5_BLE_PAIRING; service_view.ble.operation_id=81;
    service_view.ble.bonded=false; service_view.ble.pairing_active=true; publish();
    owner_page(RYZ_SYSTEM_UI_ROUTE_BLE_PAIRING);
    assert(ryz_system_ui_tick(&system_view)==ESP_OK);
    tap(35,228);
    assert(ble_commands.action==RYZ_V5_BLE_ACTION_CANCEL_PAIR && ble_commands.operation==81);
    unsigned calls=ble_commands.calls;
    service_view.ble.pairing_active=false; service_view.ble.bonded=true;
    service_view.ble.page=RYZ_V5_BLE_SUCCESS; publish();
    assert(ryz_system_ui_tick(&system_view)==ESP_OK);
    assert(ryz_system_ui_current_route()!=RYZ_SYSTEM_UI_ROUTE_BLE_SUCCESS && ble_commands.calls==calls);
    passed("BLE ON/pair/commit/success follow snapshots; DONE consumes Success; cancellation and leaving flow reject late Success");
}

static void capabilities_change(void)
{
    fresh();
    system_view.state = RYZ_SYSTEM_UI_NETWORK_OFF;
    publish();
    open_wifi_from_settings();
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_WIFI_OFF);
    const ryz_ui_nav_token_t page = ryz_system_ui_page_token();
    assert(sample(RYZ_TOUCH_DOWN, true, 210, 45) == RYZ_SYSTEM_UI_ACTION_NONE);
    service_view.network.busy = true;
    publish();
    assert(ryz_system_ui_render(&system_view) == ESP_OK);
    assert(ryz_system_ui_page_token().generation == page.generation);
    assert(sample(RYZ_TOUCH_UP, false, 0, 0) == RYZ_SYSTEM_UI_ACTION_NONE);
    service_view.network.busy = false;
    publish();
    assert(sample(RYZ_TOUCH_DOWN, true, 210, 45) == RYZ_SYSTEM_UI_ACTION_NONE);
    service_view.network.enabled = true; /* Same geometry now means OFF, not ON. */
    publish();
    assert(ryz_system_ui_render(&system_view) == ESP_OK);
    assert(sample(RYZ_TOUCH_UP, false, 0, 0) == RYZ_SYSTEM_UI_ACTION_NONE);
    service_view.network.enabled = false;
    publish();
    assert(tap(210, 45) == RYZ_SYSTEM_UI_ACTION_NETWORK_ON);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_WIFI_OFF);
    passed("positionless UP revalidates current same-page capability and action identity");
}

static bool cancel_now(void *context)
{ ++*(unsigned *)context; return true; }

static void failures_revoke_input(void)
{
    fresh();
    system_view.state = RYZ_SYSTEM_UI_NETWORK_OFF;
    publish();
    open_wifi_from_settings();
    assert(sample(RYZ_TOUCH_DOWN, true, 210, 45) == RYZ_SYSTEM_UI_ACTION_NONE);
    const ryz_ui_nav_token_t pressed_page = ryz_system_ui_page_token();
    const uint32_t failed_revision = ryz_system_ui_render_revision();
    ryz_host_display_fail_show(ESP_FAIL, 32);
    bool cancelled = true;
    assert(ryz_system_ui_render_checked(&system_view, NULL, NULL, &cancelled) == ESP_FAIL);
    assert(!cancelled && ryz_system_ui_page_token().generation != pressed_page.generation);
    assert(ryz_system_ui_render_revision() == failed_revision);
    ryz_system_ui_action_t action = RYZ_SYSTEM_UI_ACTION_NETWORK_ON;
    assert(ryz_system_ui_process_sample(NULL, ESP_ERR_TIMEOUT, &system_view, &action) == ESP_ERR_TIMEOUT);
    assert(action == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_render(&system_view) == ESP_OK);
    assert(sample(RYZ_TOUCH_DOWN, true, 210, 45) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(sample(RYZ_TOUCH_MOVE, true, 210, 45) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(sample(RYZ_TOUCH_UP, false, 0, 0) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(tap(210, 45) == RYZ_SYSTEM_UI_ACTION_NETWORK_ON);
    assert(sample(RYZ_TOUCH_DOWN, true, 210, 45) == RYZ_SYSTEM_UI_ACTION_NONE);
    unsigned callbacks = 0;
    cancelled = false;
    const uint32_t cancelled_revision = ryz_system_ui_render_revision();
    assert(ryz_system_ui_render_checked(&system_view, cancel_now, &callbacks, &cancelled) == ESP_ERR_TIMEOUT);
    assert(cancelled && callbacks > 0);
    assert(ryz_system_ui_render_revision() == cancelled_revision);
    assert(ryz_system_ui_take_action() == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_render(&system_view) == ESP_OK);
    assert(sample(RYZ_TOUCH_DOWN, true, 210, 45) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(sample(RYZ_TOUCH_MOVE, true, 210, 45) == RYZ_SYSTEM_UI_ACTION_NONE);
    released(); /* Actual successful NONE release is allowed after TP error. */
    assert(tap(210, 45) == RYZ_SYSTEM_UI_ACTION_NETWORK_ON);
    passed("driver failure / read failure / true cancellation revoke capture until physical release");
}

static void revision_survives_reset(void)
{
    fresh();
    const uint32_t revision = ryz_system_ui_render_revision();
    assert(ryz_system_ui_reset_checked() == ESP_OK);
    assert(ryz_system_ui_render_revision() == revision);
    assert(ryz_system_ui_render(&system_view) == ESP_OK);
    assert(ryz_system_ui_render_revision() == (uint32_t)(revision + 1U));
    passed("successful presentation revision survives reset and advances only after the next complete render");
}

static void staged_fixture(uint32_t attempt)
{
    service_view.version = (ryz_v5_version_model_t){
        .image_staged = true, .restart_ready = true, .attempt_id = attempt,
        .ab_slots = true,
    };
    strcpy(service_view.version.running, "0.8.0");
    strcpy(service_view.version.candidate, "0.9.0");
    publish();
}

static void restart_routes(void)
{
    fresh();
    assert(tap(200, 220) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(tap(120, 180) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_VERSION_CURRENT);
    assert(tap(20, 16) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_SETTINGS);
    staged_fixture(17);
    assert(tap(120, 180) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_VERSION_REBOOT);
    assert(has_label(lv_screen_active(), "IMAGE VERIFIED"));
    assert(tap(40, 202) == RYZ_SYSTEM_UI_ACTION_NONE); /* LATER is not a restart. */
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_SETTINGS);
    assert(tap(120, 180) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_VERSION_REBOOT);
    service_view.version.image_staged = false;
    publish();
    assert(ryz_system_ui_tick(&system_view) == ESP_OK);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_VERSION_CURRENT);
    assert(!has_label(lv_screen_active(), "IMAGE VERIFIED"));
    owner_page(RYZ_SYSTEM_UI_ROUTE_VERSION_DOWNLOADING);
    assert(ryz_system_ui_tick(&system_view) == ESP_OK);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_VERSION_DOWNLOADING);
    staged_fixture(18);
    assert(ryz_system_ui_tick(&system_view) == ESP_OK);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_VERSION_REBOOT);
    released();
    assert(tap(20, 16) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_SETTINGS);

    const ryz_system_ui_route_t unchanged[] = {
        RYZ_SYSTEM_UI_ROUTE_HOME, RYZ_SYSTEM_UI_ROUTE_SETTINGS,
        RYZ_SYSTEM_UI_ROUTE_VERSION_CURRENT, RYZ_SYSTEM_UI_ROUTE_VERSION_INFO,
        RYZ_SYSTEM_UI_ROUTE_VERSION_AVAILABLE, RYZ_SYSTEM_UI_ROUTE_VERSION_FAILED,
        RYZ_SYSTEM_UI_ROUTE_SCRIPTS_SETTINGS,
    };
    for (size_t i = 0; i < sizeof(unchanged) / sizeof(unchanged[0]); ++i) {
        owner_page(unchanged[i]);
        const ryz_ui_nav_token_t token = ryz_system_ui_page_token();
        assert(ryz_system_ui_tick(&system_view) == ESP_OK);
        assert(ryz_system_ui_current_route() == unchanged[i]);
        assert(ryz_system_ui_page_token().generation == token.generation);
    }
    passed("staged Version entry / LATER / download completion / stage loss preserve real route ownership");
}

static void restart_identity(void)
{
    /* A page route token alone cannot authorize a different staged image. */
    for (unsigned change = 0; change < 5; ++change) {
        fresh();
        staged_fixture(31);
        owner_page(RYZ_SYSTEM_UI_ROUTE_VERSION_REBOOT);
        const ryz_ui_nav_token_t page = ryz_system_ui_page_token();
        assert(sample(RYZ_TOUCH_DOWN, true, 160, 202) == RYZ_SYSTEM_UI_ACTION_NONE);
        if (change == 0 || change == 1) ++service_view.version.attempt_id;
        if (change == 2) service_view.version.restart_ready = false;
        if (change == 3) service_view.version.restart_pending = true;
        if (change == 4) service_view.version.image_staged = false;
        publish();
        if (change != 0) assert(ryz_system_ui_render(&system_view) == ESP_OK);
        assert(ryz_system_ui_page_token().generation == page.generation);
        assert(sample(RYZ_TOUCH_UP, false, 0, 0) == RYZ_SYSTEM_UI_ACTION_NONE);
        service_view.version.image_staged = true;
        service_view.version.restart_ready = true;
        service_view.version.restart_pending = false;
        publish();
        assert(ryz_system_ui_render(&system_view) == ESP_OK);
        assert(tap(160, 202) == RYZ_SYSTEM_UI_ACTION_OTA_RESTART);
        assert(sample(RYZ_TOUCH_UP, false, 0, 0) == RYZ_SYSTEM_UI_ACTION_NONE);
    }

    fresh();
    staged_fixture(41);
    owner_page(RYZ_SYSTEM_UI_ROUTE_VERSION_REBOOT);
    ++service_view.version.attempt_id;
    publish(); /* New metadata, but the panel still shows attempt 41. */
    assert(sample(RYZ_TOUCH_DOWN, true, 160, 202) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(sample(RYZ_TOUCH_UP, false, 0, 0) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_render(&system_view) == ESP_OK);
    assert(tap(160, 202) == RYZ_SYSTEM_UI_ACTION_OTA_RESTART);
    service_view.version.attempt_id = 0;
    publish();
    assert(ryz_system_ui_render(&system_view) == ESP_OK);
    assert(tap(160, 202) == RYZ_SYSTEM_UI_ACTION_NONE);
    passed("restart requires the displayed nonzero attempt; changed identity/readiness/pending cannot survive UP");
}

static void scripts_restart_stays_disabled(void)
{
    fresh();
    staged_fixture(57); /* OTA permission must not authorize the Scripts button. */
    service_view.scripts = (ryz_v5_scripts_snapshot_t){
        .page = RYZ_V5_SCRIPTS_SAVED, .generation = 19,
        .operation = RYZ_V5_SCRIPTS_OP_BOOT, .result = RYZ_V5_SCRIPTS_COMMITTED,
        .can_restart = false,
    };
    strcpy(service_view.scripts.result_name, "demo.lua");
    publish();
    owner_page(RYZ_SYSTEM_UI_ROUTE_SCRIPTS_SAVED);
    assert(has_label(lv_screen_active(), "BOOT SCRIPT SET"));
    assert(tap(160, 198) == RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_SCRIPTS_SAVED);
    ryz_v5_status_t observed;
    ryz_v5_status_get(&observed);
    assert(observed.scripts.error == ESP_OK && !observed.scripts.can_restart);
    passed("a committed Scripts fixture still has no restart permission from the OTA view");
}

static lv_obj_t *find_text(lv_obj_t *root,const char *text)
{
    if(lv_obj_check_type(root,&lv_label_class) && !strcmp(lv_label_get_text(root),text)) return root;
    for(uint32_t i=0;i<lv_obj_get_child_count(root);++i) {
        lv_obj_t *found=find_text(lv_obj_get_child(root,(int32_t)i),text);
        if(found) return found;
    }
    return NULL;
}

static void swipe_and_info_scroll(void)
{
    fresh();
    sample(RYZ_TOUCH_DOWN,true,190,80);
    sample(RYZ_TOUCH_MOVE,true,105,84);
    sample(RYZ_TOUCH_UP,true,105,84);
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_APPS);
    sample(RYZ_TOUCH_DOWN,true,190,80);
    sample(RYZ_TOUCH_MOVE,true,90,84);
    sample(RYZ_TOUCH_UP,true,90,84);
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_SETTINGS);
    /* Vertical capture never turns into a late horizontal swipe or a tap. */
    sample(RYZ_TOUCH_DOWN,true,190,80);
    sample(RYZ_TOUCH_MOVE,true,190,110);
    sample(RYZ_TOUCH_MOVE,true,100,115);
    sample(RYZ_TOUCH_UP,true,100,115);
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_SETTINGS);
    service_view.version=(ryz_v5_version_model_t){.running="1.2.3",.build="Sep 13 2026 23:42:13",
        .idf="5.5.4",.slot="ota_0",.uptime="100:00:00",.chip="ESP32-S3 M101"};
    publish(); owner_page(RYZ_SYSTEM_UI_ROUTE_VERSION_INFO);
    lv_obj_t *chip=find_text(lv_screen_active(),"ESP32-S3 M101"); assert(chip);
    lv_area_t before,after; lv_obj_get_coords(chip,&before);
    sample(RYZ_TOUCH_DOWN,true,180,170);
    sample(RYZ_TOUCH_MOVE,true,180,90);
    sample(RYZ_TOUCH_MOVE,true,180,60);
    sample(RYZ_TOUCH_UP,false,0,0);
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_VERSION_INFO);
    chip=find_text(lv_screen_active(),"ESP32-S3 M101"); lv_obj_get_coords(chip,&after);
    assert(after.y1<before.y1 && after.y1>=58 && after.y2<190);
    lv_obj_t *button=find_text(lv_screen_active(),"SHOW DEVICE ID");
    lv_obj_get_coords(button,&after); assert(after.y1>=194 && after.y2<236);
    /* View-only offsets never alter the service cache or force periodic dirties. */
    assert(!ryz_v5_status_publish_visible(RYZ_SYSTEM_UI_ROUTE_VERSION_INFO,&service_view));
    passed("non-cycling root swipes, locked axes, real Info scroll and fixed footer");
}

static void swipe_cancellation_contract(void)
{
    fresh();
    owner_page(RYZ_SYSTEM_UI_ROUTE_WIFI_CONNECTED);
    /* A drag can remain inside a wide action row the whole time. */
    sample(RYZ_TOUCH_DOWN,true,80,120);
    sample(RYZ_TOUCH_MOVE,true,130,120);
    sample(RYZ_TOUCH_UP,false,0,0);
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_WIFI_CONNECTED);
    assert(tap(80,120)==RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_WIFI_INFO);

    for(unsigned gesture=1;gesture<=4;++gesture) {
        owner_page(RYZ_SYSTEM_UI_ROUTE_WIFI_CONNECTED);
        sample(RYZ_TOUCH_DOWN,true,80,120);
        const ryz_touch_sample_t up={.event=RYZ_TOUCH_UP,.gesture=gesture};
        ryz_system_ui_action_t action;
        assert(ryz_system_ui_process_sample(&up,ESP_OK,&system_view,&action)==ESP_OK);
        assert(action==RYZ_SYSTEM_UI_ACTION_NONE);
        assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_WIFI_CONNECTED);
    }
    service_view.version=(ryz_v5_version_model_t){.running="1.2.3",.build="Sep 13 2026 23:42:13",
        .idf="5.5.4",.slot="ota_0",.uptime="100:00:00",.chip="ESP32-S3 M101"};
    publish(); owner_page(RYZ_SYSTEM_UI_ROUTE_VERSION_INFO);
    lv_area_t before,after;
    lv_obj_get_coords(find_text(lv_screen_active(),"ESP32-S3 M101"),&before);
    sample(RYZ_TOUCH_DOWN,true,180,150);
    sample(RYZ_TOUCH_MOVE,true,180,130);
    sample(RYZ_TOUCH_MOVE,true,180,149);
    lv_obj_get_coords(find_text(lv_screen_active(),"ESP32-S3 M101"),&after);
    assert(after.y1==before.y1-1); /* Locked scroll remains continuous near DOWN. */
    sample(RYZ_TOUCH_UP,false,0,0);
    passed("wide-row and sparse-gesture drags never click; Info reverse drag follows to origin");
}

/* A populated list is essential: an empty Apps view cannot expose an
 * accidentally submitted SELECT after horizontal navigation. */
static ryz_v5_apps_snapshot_t swipe_apps;
static unsigned swipe_selects;
static esp_err_t swipe_apps_get(void *context, ryz_v5_apps_snapshot_t *out)
{
    (void)context;
    swipe_apps.token.page=ryz_system_ui_page_token();
    swipe_apps.open=ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_APPS;
    *out=swipe_apps;
    return ESP_OK;
}
static esp_err_t swipe_apps_submit(void *context, const ryz_v5_apps_intent_t *intent)
{
    (void)context;
    if(intent->action==RYZ_V5_APPS_SELECT) {
        ++swipe_selects;
        const ryz_script_store_entry_t entry=swipe_apps.reader.page.entries[intent->index];
        ++swipe_apps.token.reader_request_id;
        swipe_apps.reader=(ryz_apps_snapshot_t){
            .request_id=swipe_apps.token.reader_request_id,
            .completed_id=swipe_apps.token.reader_request_id,
            .view=RYZ_APPS_VIEW_DETAIL,.state=RYZ_APPS_READY,
            .store_ready=true,.store_status_valid=true};
        swipe_apps.reader.detail.file.entry=entry;
        swipe_apps.reader.detail.source_valid=true;
        swipe_apps.reader.detail.revision=1;
    }
    return ESP_OK;
}
static void populated_apps(void)
{
    fresh();
    swipe_apps=(ryz_v5_apps_snapshot_t){.open=true,.page_limit=RYZ_SCRIPT_STORE_PAGE_MAX,
        .token={.reader_request_id=1}};
    swipe_apps.reader=(ryz_apps_snapshot_t){.request_id=1,.completed_id=1,
        .view=RYZ_APPS_VIEW_CATALOG,.state=RYZ_APPS_READY,
        .store_ready=true,.store_status_valid=true};
    swipe_apps.reader.page.revision=1;
    swipe_apps.reader.page.count=swipe_apps.reader.page.total=8;
    for(unsigned i=0;i<8;++i) {
        snprintf(swipe_apps.reader.page.entries[i].name,
            sizeof(swipe_apps.reader.page.entries[i].name),"swipe%u.lua",i);
        swipe_apps.reader.page.entries[i].bytes=100;
    }
    swipe_selects=0;
    const ryz_v5_apps_binding_t binding={.get=swipe_apps_get,.submit=swipe_apps_submit};
    ryz_v5_apps_bind(&binding);
    owner_page(RYZ_SYSTEM_UI_ROUTE_APPS);
    assert(ryz_system_ui_tick(&system_view)==ESP_OK);
    released();
    assert(ryz_v5_apps_has_navigation() && has_label(lv_screen_active(),"swipe0.lua"));
}
static void apps_swipe_no_select(void)
{
    populated_apps();
    tap(120,70);
    assert(swipe_selects==1); /* Positive control: this exact row is actionable. */
    assert(has_label(lv_screen_active(),"APP INFO"));
    populated_apps();
    const uint32_t before_down=ryz_system_ui_render_revision();
    sample(RYZ_TOUCH_DOWN,true,120,70);
    assert(ryz_system_ui_render_revision()==before_down);
    sample(RYZ_TOUCH_UP,false,0,0);
    assert(swipe_selects==1);
    static const struct { int dx,dy; bool move,positioned_up; } paths[]={
        {-90,0,true,false},{90,0,true,false},
        {-25,0,true,false},{25,0,true,false},
        {-90,0,false,true},{90,0,false,true},
        {-12,9,true,false},{12,9,true,false},
        {-10,7,true,false},{10,7,true,false},
    };
    for(unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);++i) {
        populated_apps();
        sample(RYZ_TOUCH_DOWN,true,120,70);
        if(paths[i].move) sample(RYZ_TOUCH_MOVE,true,120+paths[i].dx,70+paths[i].dy);
        sample(RYZ_TOUCH_UP,paths[i].positioned_up,120+paths[i].dx,70+paths[i].dy);
        assert(swipe_selects==0);
        assert(!has_label(lv_screen_active(),"APP INFO"));
        const ryz_system_ui_route_t destination=paths[i].dx<=-48 ? RYZ_SYSTEM_UI_ROUTE_SETTINGS :
            paths[i].dx>=48 ? RYZ_SYSTEM_UI_ROUTE_HOME : RYZ_SYSTEM_UI_ROUTE_APPS;
        assert(ryz_system_ui_current_route()==destination);
    }
    /* A polling backend may miss the in-between coordinates while the
     * panel is rendering. These are injected traces, not a device capture.
     * Raw direction is unrotated, so it vetoes a tap but cannot guess a page. */
    for(unsigned gesture=1;gesture<=4;++gesture) {
        populated_apps();
        sample(RYZ_TOUCH_DOWN,true,120,70);
        ryz_touch_sample_t up={.event=RYZ_TOUCH_UP,.gesture=(uint8_t)gesture};
        ryz_system_ui_action_t action=RYZ_SYSTEM_UI_ACTION_NONE;
        assert(ryz_system_ui_process_sample(&up,ESP_OK,&system_view,&action)==ESP_OK);
        assert(swipe_selects==0 && !has_label(lv_screen_active(),"APP INFO"));
        assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_APPS);
        /* The physical release also releases capture; the next real tap works. */
        tap(120,70);
        assert(swipe_selects==1 && has_label(lv_screen_active(),"APP INFO"));
    }
    passed("populated APPS horizontal gestures never submit script SELECT");
}

static void fault_home_only(void)
{
    fresh();
    ryz_v5_fault_model_t model={.code="LUA-S01",.summary="Syntax error",.line=-1};
    ryz_v5_fault_set(&model); owner_page(RYZ_SYSTEM_UI_ROUTE_SCRIPT_FAULT);
    assert(has_label(lv_screen_active(),"LINK UNAVAILABLE"));
    sample(RYZ_TOUCH_DOWN,true,190,80);
    sample(RYZ_TOUCH_MOVE,true,80,82);
    sample(RYZ_TOUCH_UP,true,80,82);
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_SCRIPT_FAULT);
    assert(tap(16,16)==RYZ_SYSTEM_UI_ACTION_NONE); /* No hidden Back. */
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_SCRIPT_FAULT);
    assert(tap(120,218)==RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_HOME);
    system_view.ap_active=true;
    strcpy(system_view.ap_ssid,"DEMO-AP"); strcpy(system_view.ap_password,"DEMO12345");
    strcpy(system_view.portal_ip,"192.168.4.1");
    ryz_v5_fault_set(&model);
    assert(ryz_v5_fault_link("/f/12345678?cap=0123456789abcdef0123456789abcdef"));
    owner_page(RYZ_SYSTEM_UI_ROUTE_SCRIPT_FAULT);
    assert(has_label(lv_screen_active(),"JOIN DEVICE\nWI-FI"));
    assert(ryz_v5_fault_needs_cleanup());
    assert(tap(160,218)==RYZ_SYSTEM_UI_ACTION_NONE);
    assert(has_label(lv_screen_active(),"SCAN FOR\nDIAGNOSTICS"));
    assert(tap(160,218)==RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_HOME && !ryz_v5_fault_needs_cleanup());
    passed("fault has no hidden header or root swipe; explicit Home leaves without retry");
}

static void home_fixed_dashboard(void)
{
    fresh();
    const ryz_home_selection_t before=ryz_system_ui_home_selection();
    const unsigned xs[]={8,40,79,80,116,155,156,195,231};
    const unsigned ys[]={112,134,155};
    for(unsigned x=0;x<sizeof(xs)/sizeof(*xs);++x)
        for(unsigned y=0;y<sizeof(ys)/sizeof(*ys);++y) {
            assert(tap(xs[x],ys[y])==RYZ_SYSTEM_UI_ACTION_NONE);
            assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_HOME);
            assert_home_selection(before);
        }
    system_view.cpu_temperature_valid=system_view.cpu_load_valid=system_view.psram_usage_valid=true;
    system_view.cpu_temperature_c=48;
    system_view.cpu_load_percent=23;
    system_view.psram_used_percent=42;
    system_view.home_selection=before;
    publish();
    assert(ryz_system_ui_render(&system_view)==ESP_OK);
    assert(has_label(lv_screen_active(),"48 \xc2\xb0" "C"));
    assert(has_label(lv_screen_active(),"23%"));
    assert(has_label(lv_screen_active(),"42%"));
    assert(tap(145,70)==RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route()!=RYZ_SYSTEM_UI_ROUTE_HOME);
    assert_home_selection(before);
    fresh();
    assert(sample(RYZ_TOUCH_DOWN,true,190,134)==RYZ_SYSTEM_UI_ACTION_NONE);
    assert(sample(RYZ_TOUCH_MOVE,true,90,134)==RYZ_SYSTEM_UI_ACTION_NONE);
    assert(sample(RYZ_TOUCH_UP,true,90,134)==RYZ_SYSTEM_UI_ACTION_NONE);
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_APPS);
    passed("HOME fixed WLAN graph, inert scalar cards, simultaneous values, Wi-Fi entry and swipe navigation");
}

int main(int argc, char **argv)
{
    assert(argc == 1 || argc == 2);
    assert(ryz_font_init() == ESP_OK);
    if (argc == 1 || !strcmp(argv[1], "home")) home_fixed_dashboard();
    if (argc == 1 || !strcmp(argv[1], "routes")) root_routes();
    if (argc == 1 || !strcmp(argv[1], "release-version")) release_version_routes();
    if (argc == 1 || !strcmp(argv[1], "header")) shared_header_icons();
    if (argc == 1 || !strcmp(argv[1], "menu-capture")) ordinary_menu_capture_is_not_a_visual_change();
    if (argc == 1 || !strcmp(argv[1], "apps-capture")) apps_stream_does_not_capture_a_navigation_move();
    if (argc == 1 || !strcmp(argv[1], "network")) network_flow();
    if (argc == 1 || !strcmp(argv[1], "modal")) modal_and_unavailable();
    if (argc == 1 || !strcmp(argv[1], "ble-numeric")) ble_numeric_identity();
    if (argc == 1 || !strcmp(argv[1], "ble-confirmation")) ble_confirmation_flow();
    if (argc == 1 || !strcmp(argv[1], "ble-navigation")) ble_snapshot_navigation();
    if (argc == 1 || !strcmp(argv[1], "capabilities")) capabilities_change();
    if (argc == 1 || !strcmp(argv[1], "failures")) failures_revoke_input();
    if (argc == 1 || !strcmp(argv[1], "render-revision")) revision_survives_reset();
    if (argc == 1 || !strcmp(argv[1], "restart-routes")) restart_routes();
    if (argc == 1 || !strcmp(argv[1], "restart-identity")) restart_identity();
    if (argc == 1 || !strcmp(argv[1], "scripts-restart")) scripts_restart_stays_disabled();
    if (argc == 1 || !strcmp(argv[1], "swipe-info")) swipe_and_info_scroll();
    if (argc == 1 || !strcmp(argv[1], "swipe-cancel")) swipe_cancellation_contract();
    if (argc == 1 || !strcmp(argv[1], "apps-swipe")) apps_swipe_no_select();
    if (argc == 1 || !strcmp(argv[1], "fault")) fault_home_only();
    assert(groups);
    ryz_system_ui_reset();
    assert(ryz_v5_release() == ESP_OK);
    printf("V5_SHELL_UI_PASS %u groups; service-view and panel adapters only\n", groups);
    return 0;
}
