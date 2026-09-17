/* Desktop presentation adapter, NOT a second UI implementation. All page
 * drawing, navigation, input capture and fonts come from production C/LVGL.
 * Explicit service fixtures never perform hardware/network/file writes.
 * Factory tools use the production Lua facade with synthetic input. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "ryz_system_ui.h"
#include "ryz_v5_render.h"
#include "ryz_v5_apps.h"
#include "ryz_v5_display.h"
#include "ryz_v5_boot.h"
#include "ryz_v5_status.h"
#include "ryz_lvgl_system.h"
#include "ryz_display_host.h"
#include "esp_timer.h"
#include "factory_demo.h"
#include "workbench_telemetry.h"
#include "include/workbench_boot_key.h"

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static bool running = true, pressed, booting;
static uint32_t boot_ms;
static int scale = 2;
static size_t last_shows = (size_t)-1;
static unsigned blocked_actions;
static ryz_sim_factory_kind_t factory_requested;
static bool test_mode, boot_key_pressed;
static uint64_t boot_key_since;
static uint64_t last_ticks;
static void (*monitor_test_tick)(void);
static ryz_lua_result_t factory_result;
static ryz_system_ui_snapshot_t state;
static ryz_v5_apps_snapshot_t apps;
static ryz_display_settings_snapshot_t display_demo;
static unsigned display_requests;
static ryz_display_settings_value_t display_requested;
static uint8_t display_preview_brightness=50;
static unsigned display_previews;
static ryz_workbench_telemetry_t telemetry_demo;
static ryz_system_services_snapshot_t services_demo;
static uint64_t telemetry_sampled_us;
static size_t psram_demo_free;
static bool telemetry_sampled, home_network_online = true;

/* Only this observation source is simulated. Selection, freshness, rate
 * conversion and the single bounded history are the production mapper. */
static bool telemetry_refresh(void)
{
    const uint64_t now = (uint64_t)esp_timer_get_time();
    const ryz_system_ui_snapshot_t previous = state;
    ryz_v5_status_t status;
    ryz_v5_status_get(&status);
    (void)ryz_workbench_telemetry_select(&telemetry_demo,
        ryz_system_ui_home_selection(), &state);
    if (!telemetry_sampled || now < telemetry_sampled_us ||
        now - telemetry_sampled_us >= UINT64_C(1000000)) {
        const unsigned phase = (unsigned)(now / UINT64_C(1000000));
        const uint64_t elapsed = telemetry_sampled && now >= telemetry_sampled_us ?
            now - telemetry_sampled_us : 0;
        if (state.connected) {
            services_demo.network.link.traffic_rx_bytes +=
                elapsed * (80000U + (phase % 10U) * 20000U) / UINT64_C(1000000);
            services_demo.network.link.traffic_tx_bytes += elapsed / 100U;
        }
        telemetry_sampled = true;
        telemetry_sampled_us = now;
        services_demo.metrics = (ryz_system_metrics_snapshot_t){
            .temperature_valid = true, .temperature_deci_c = (int16_t)(420U + phase % 11U * 8U),
            .load_valid = true, .load_permille = (uint16_t)(170U + phase % 13U * 35U),
            .sampled_at_us = now,
        };
        services_demo.network.link.sampled_at_us = now;
        services_demo.network.link.traffic_epoch = 1;
        services_demo.network.link.traffic_valid = true;
        services_demo.network.link.rssi_valid = true;
        services_demo.network.link.rssi_dbm = -52;
        services_demo.network.link.mac_valid = true;
        strcpy(services_demo.network.link.mac, "02:11:22:33:44:55");
        psram_demo_free = 2U * 1024U * 1024U * (100U - (12U + phase % 17U * 2U)) / 100U;
    }
    services_demo.network_valid = true;
    services_demo.network.enabled = status.network.enabled;
    services_demo.network.state = state.connected ? RYZ_PROVISIONING_ONLINE :
        status.network.enabled ? RYZ_PROVISIONING_AP_READY : RYZ_PROVISIONING_OFF;
    ryz_workbench_telemetry_update(&telemetry_demo, &services_demo, now, NULL,
        &state, &status.network);
    ryz_workbench_telemetry_psram(&telemetry_demo, now, 2U * 1024U * 1024U,
        psram_demo_free, &state);
    status.network.system = state;
    (void)ryz_v5_status_set(&status);
    return memcmp(&previous, &state, sizeof(state)) != 0;
}

static esp_err_t display_get(void *context,ryz_display_settings_snapshot_t *out)
{ (void)context; *out=display_demo; return ESP_OK; }
static esp_err_t display_submit(void *context,uint64_t revision,ryz_display_settings_value_t value)
{
    (void)context;
    if(revision!=display_demo.revision) return ESP_ERR_INVALID_STATE;
    ++display_requests; display_requested=value;
    puts("DEMO: Display draft only; SAVE unsupported, no NVS/panel/touch hardware operation");
    return ESP_ERR_NOT_SUPPORTED;
}
static esp_err_t display_preview(void *context,uint64_t revision,uint8_t brightness)
{
    (void)context;
    if(revision!=display_demo.revision) return ESP_ERR_INVALID_STATE;
    ++display_previews; display_preview_brightness=brightness;
    /* Observe the real UI intent, not the Mac backlight. Pixel readback stays
     * native RGB565; only the window title labels this simulated PWM value. */
    return ESP_OK;
}
static void display_end_preview(void *context)
{ (void)context; display_preview_brightness=display_demo.confirmed.brightness; }

static void require(bool ok, const char *operation)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s: %s\n", operation, SDL_GetError());
        exit(EXIT_FAILURE);
    }
}

static void checked(esp_err_t error)
{
    if (error != ESP_OK) {
        fprintf(stderr, "UI error=%d\n", error);
        exit(EXIT_FAILURE);
    }
}

static void catalog(void)
{
    apps.reader.view = RYZ_APPS_VIEW_CATALOG;
    apps.reader.state = RYZ_APPS_READY;
    apps.reader.store_ready = true;
    apps.reader.store_status_valid = true;
    apps.page_limit = RYZ_SCRIPT_STORE_PAGE_MAX;
    apps.reader.page.offset = 0;
    apps.reader.page.revision = 1;
    apps.reader.page.count = apps.reader.page.total = 5;
    const char *names[] = {"sample_clock.lua", "sample_i2c.lua", "tool_monitor.lua", "tool_i2c.lua", "tool_hardware.lua"};
    for (unsigned i = 0; i < 5; ++i) {
        snprintf(apps.reader.page.entries[i].name,
                 sizeof(apps.reader.page.entries[i].name), "%s", names[i]);
        const ryz_sim_factory_source_t *source=ryz_sim_factory_find(names[i]);
        apps.reader.page.entries[i].bytes = source ? source->bytes : 192;
    }
}

static esp_err_t apps_get(void *context, ryz_v5_apps_snapshot_t *out)
{
    (void)context;
    const ryz_ui_nav_token_t page = ryz_system_ui_page_token();
    if (page.generation != apps.token.page.generation) {
        apps.token.page = page;
        ++apps.token.reader_request_id;
        catalog();
    }
    apps.open = ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_APPS;
    *out = apps;
    return ESP_OK;
}

static esp_err_t apps_submit(void *context, const ryz_v5_apps_intent_t *intent)
{
    (void)context;
    if (intent->expected.page.generation != apps.token.page.generation ||
        intent->expected.reader_request_id != apps.token.reader_request_id)
        return ESP_ERR_INVALID_STATE;
    if (intent->action == RYZ_V5_APPS_RUN || intent->action == RYZ_V5_APPS_DELETE) {
        const ryz_sim_factory_source_t *source=ryz_sim_factory_find(apps.reader.detail.file.entry.name);
        if (intent->action == RYZ_V5_APPS_RUN &&
            apps.reader.view == RYZ_APPS_VIEW_DETAIL && source) {
            /* Queue only; never re-enter Lua/LVGL from native UI callbacks. */
            factory_requested = source->kind;
            return ESP_OK;
        }
        ++blocked_actions;
        printf("DEMO: blocked Apps action=%d; no Lua execution or file write\n", intent->action);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (intent->action == RYZ_V5_APPS_SELECT) {
        if (intent->index >= apps.reader.page.count) return ESP_ERR_INVALID_ARG;
        apps.reader.detail.file.entry = apps.reader.page.entries[intent->index];
        const ryz_sim_factory_source_t *source=ryz_sim_factory_find(apps.reader.detail.file.entry.name);
        if(source) strcpy(apps.reader.detail.file.sha256,source->sha256);
        else {
            memset(apps.reader.detail.file.sha256, 'a', 64);
            apps.reader.detail.file.sha256[64] = '\0';
        }
        apps.reader.detail.revision = 1;
        apps.reader.detail.source_valid = true;
        strcpy(apps.reader.detail.metadata.description, source ?
               "Real factory Lua. SIM input; no device." : "DEMO ONLY. No script is executed.");
        strcpy(apps.reader.detail.metadata.author, "RyzoBee");
        snprintf(apps.reader.detail.metadata.version, sizeof(apps.reader.detail.metadata.version),
                 "%s", source ? source->version : "sample");
        apps.reader.view = RYZ_APPS_VIEW_DETAIL;
    } else if (intent->action == RYZ_V5_APPS_BACK || intent->action == RYZ_V5_APPS_PAGE) {
        catalog();
    } else return ESP_ERR_NOT_SUPPORTED;
    ++apps.token.reader_request_id;
    return ESP_OK;
}

static void fixtures(void)
{
    state = (ryz_system_ui_snapshot_t){
        .configured = true, .connected = true, .state = RYZ_SYSTEM_UI_NETWORK_ONLINE,
        .storage_ready = true, .storage_total_bytes = 1024 * 1024, .storage_used_bytes = 153600,
        .cpu_temperature_valid = true, .cpu_temperature_c = 42,
        .cpu_load_valid = true, .cpu_load_percent = 17,
        .psram_usage_valid = true, .psram_used_percent = 12,
        .time_hhmm = "18:43", .firmware_version = RYZ_HOST_FIRMWARE_VERSION,
        .ap_ssid = "RyzoBee-DEMO", .ap_password = "demo-only-123", .portal_ip = "192.168.4.1",
    };
    ryz_v5_status_t status = {0};
    status.network.enabled = true;
    status.network.can_setup = true;
    status.network.system = state;
    strcpy(status.network.sta_ssid, "DEMO NETWORK");
    strcpy(status.network.ipv4, "192.0.2.10");
    status.ble.unavailable = true;
    status.scripts.generation = 1;
    status.scripts.boot_known = true;
    status.scripts.store_ready = true;
    status.scripts.catalog_state = RYZ_APPS_READY;
    catalog();
    status.scripts.catalog = apps.reader.page;
    strcpy(status.version.running, RYZ_HOST_FIRMWARE_VERSION);
    strcpy(status.version.candidate, "SAMPLE");
    strcpy(status.version.build, "SDL FIXTURE");
    assert(!strcmp(state.firmware_version, RYZ_HOST_FIRMWARE_VERSION));
    assert(!strcmp(status.version.running, RYZ_HOST_FIRMWARE_VERSION));
    (void)ryz_v5_status_set(&status);
    const ryz_v5_apps_binding_t binding = {.get = apps_get, .submit = apps_submit};
    ryz_v5_apps_bind(&binding);
    display_demo=(ryz_display_settings_snapshot_t){.revision=1,.ready=true,
        .phase=RYZ_DISPLAY_SETTINGS_READY,.confirmed={0,4,50},.requested={0,4,50}};
    const ryz_v5_display_binding_t display_binding={.get=display_get,.submit=display_submit,
        .preview=display_preview,.end_preview=display_end_preview};
    ryz_v5_display_bind(&display_binding);
}

static void sample(ryz_touch_event_t event, int x, int y)
{
    if (ryz_sim_factory_active()) {
        const ryz_app_pointer_t pointer = {
            .phase = event == RYZ_TOUCH_DOWN ? RYZ_APP_POINTER_DOWN :
                event == RYZ_TOUCH_MOVE ? RYZ_APP_POINTER_MOVE :
                event == RYZ_TOUCH_UP ? RYZ_APP_POINTER_UP : RYZ_APP_POINTER_NONE,
            .pressed = event == RYZ_TOUCH_DOWN || event == RYZ_TOUCH_MOVE,
            .has_position = event == RYZ_TOUCH_DOWN || event == RYZ_TOUCH_MOVE,
            .x = (uint16_t)x, .y = (uint16_t)y,
            .sampled_ms = (uint32_t)(esp_timer_get_time() / 1000),
        };
        ryz_sim_factory_pointer(&pointer);
        return;
    }
    const ryz_touch_sample_t touch = {
        .event = event, .pressed = event == RYZ_TOUCH_DOWN || event == RYZ_TOUCH_MOVE,
        .has_position = event == RYZ_TOUCH_DOWN || event == RYZ_TOUCH_MOVE,
        .x = (uint16_t)x, .y = (uint16_t)y, .fingers = 1,
    };
    ryz_system_ui_action_t action = RYZ_SYSTEM_UI_ACTION_NONE;
    checked(ryz_system_ui_process_sample(&touch, ESP_OK, &state, &action));
    /* Match the firmware owner boundary: the released tap changes selection
     * immediately, before the next measurement can populate the new curve. */
    (void)ryz_workbench_telemetry_select(&telemetry_demo,
        ryz_system_ui_home_selection(), &state);
    if (action != RYZ_SYSTEM_UI_ACTION_NONE) {
        ++blocked_actions;
        printf("DEMO: service action=%d blocked; no hardware/network operation\n", action);
    }
}

static void title(void)
{
    char text[160];
    if (ryz_sim_factory_active())
        snprintf(text, sizeof(text), "RyzoBee %s | SIMULATED / NO DEVICE | %dx | Esc / B 5s: Home",
                 ryz_sim_factory_source(ryz_sim_factory_kind())->name,scale);
    else if(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_DISPLAY)
        snprintf(text,sizeof(text),"RyzoBee DEMO | %dx | PWM intent: %u%% (no hardware) | built %s %s",
                 scale,display_preview_brightness,__DATE__,__TIME__);
    else snprintf(text, sizeof(text), "RyzoBee DEMO / SIMULATED | %dx | route %d | M: Monitor I: I2C T: Test | built %s %s",
                  scale, ryz_system_ui_current_route(), __DATE__, __TIME__);
    SDL_SetWindowTitle(window, text);
}

static void show(void)
{
    if (last_shows == ryz_host_display_shows()) return;
    require(SDL_UpdateTexture(texture, NULL, ryz_host_display_frame(), 240 * 2) == 0, "texture upload");
    require(SDL_RenderClear(renderer) == 0, "clear");
    require(SDL_RenderCopy(renderer, texture, NULL, NULL) == 0, "render");
    SDL_RenderPresent(renderer);
    last_shows = ryz_host_display_shows();
    title();
}

static void page(ryz_system_ui_route_t route)
{
    pressed = false;
    if (booting) { checked(ryz_v5_boot_end()); booting = false; }
    checked(ryz_system_ui_reset_checked());
    checked(ryz_system_ui_navigate(ryz_system_ui_page_token(), route));
    ryz_v5_status_t status;
    ryz_v5_status_get(&status);
    state.connected = (route == RYZ_SYSTEM_UI_ROUTE_HOME && home_network_online) ||
        route == RYZ_SYSTEM_UI_ROUTE_WIFI_CONNECTED ||
        route == RYZ_SYSTEM_UI_ROUTE_WIFI_INFO || route == RYZ_SYSTEM_UI_ROUTE_WIFI_READY;
    state.ap_active = !state.connected && route != RYZ_SYSTEM_UI_ROUTE_WIFI_OFF;
    state.configured = route != RYZ_SYSTEM_UI_ROUTE_WIFI_NO_NETWORK;
    state.state = route == RYZ_SYSTEM_UI_ROUTE_WIFI_OFF ? RYZ_SYSTEM_UI_NETWORK_OFF :
        route == RYZ_SYSTEM_UI_ROUTE_WIFI_JOINING ? RYZ_SYSTEM_UI_NETWORK_CONNECTING :
        route == RYZ_SYSTEM_UI_ROUTE_WIFI_FAILED ? RYZ_SYSTEM_UI_NETWORK_FAILED :
        state.connected ? RYZ_SYSTEM_UI_NETWORK_ONLINE : RYZ_SYSTEM_UI_NETWORK_AP_READY;
    status.network.enabled = route != RYZ_SYSTEM_UI_ROUTE_WIFI_OFF;
    status.network.system = state;
    status.version.image_staged = route == RYZ_SYSTEM_UI_ROUTE_VERSION_REBOOT;
    if (route >= RYZ_SYSTEM_UI_ROUTE_SCRIPTS_SETTINGS && route <= RYZ_SYSTEM_UI_ROUTE_SCRIPTS_DELETE_ALL) {
        status.scripts.page = (ryz_v5_scripts_page_t)(route - RYZ_SYSTEM_UI_ROUTE_SCRIPTS_SETTINGS);
        ++status.scripts.generation;
    }
    (void)ryz_v5_status_set(&status);
    (void)telemetry_refresh();
    checked(ryz_system_ui_render(&state));
    sample(RYZ_TOUCH_NONE, 0, 0);
}

static void start_boot(void)
{
    if (booting) checked(ryz_v5_boot_end());
    checked(ryz_system_ui_reset_checked());
    checked(ryz_v5_release());
    pressed = false;
    booting = true;
    boot_ms = 0;
    checked(ryz_v5_boot_begin());
}

/* Explicit visual fixture: no radio or persistence. V cycles the three new
 * security states; all drawing still comes from production C/LVGL. */
static void ble_security_demo(unsigned screen)
{
    ryz_v5_status_t status;
    ryz_v5_status_get(&status);
    status.ble=(ryz_v5_ble_model_t){.page=RYZ_V5_BLE_PAIRING,
        .state=screen==1?RYZ_V5_BLE_STATE_COMMITTING:screen==2?RYZ_V5_BLE_STATE_STOPPING:
            RYZ_V5_BLE_STATE_VERIFY_CODE,
        .enabled=true,.bond_known=true,.operation_id=1,.pairing_active=true,
        .compare_pending=screen==0,.compare_value=42719,.remaining_valid=true,
        .remaining_seconds=18,.pairing_window_seconds=30};
    strcpy(status.ble.local_name,"DEMO-RYZOBEE");
    (void)ryz_v5_status_set(&status);
    page(RYZ_SYSTEM_UI_ROUTE_BLE_PAIRING);
    puts("BLE VISUAL DEMO: V cycles confirm/save/stop; commands unavailable; no real pairing");
}

static void advance(uint32_t ms)
{
    ryz_host_display_advance_ms(ms);
    if(ryz_sim_factory_active() && boot_key_pressed &&
       (uint64_t)esp_timer_get_time()/1000U-boot_key_since>=RYZ_WORKBENCH_BOOT_KEY_HOLD_MS) ryz_sim_factory_cancel();
    if (booting) {
        boot_ms += ms;
        if (boot_ms >= 1600) page(RYZ_SYSTEM_UI_ROUTE_HOME);
        else checked(ryz_v5_boot_tick(boot_ms));
    } else if (!ryz_sim_factory_active()) {
        const bool changed = telemetry_refresh();
        if (!pressed) sample(RYZ_TOUCH_NONE, 0, 0);
        if (changed && ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_HOME)
            checked(ryz_system_ui_render(&state));
        checked(ryz_system_ui_tick(&state));
        checked(ryz_lvgl_system_pump(NULL, NULL, NULL));
    }
    show();
}

static void cancel_contact(void)
{
    pressed = false; boot_key_pressed=false;
    if (ryz_sim_factory_active()) {
        const ryz_app_pointer_t interrupted = {.interrupted = true};
        ryz_sim_factory_pointer(&interrupted);
        return;
    }
    if (booting) return;
    ryz_system_ui_invalidate_input();
    /* Invalidation revokes the presentation gate as well as the gesture.
     * Re-present before a fresh released sample can enable interaction. */
    checked(ryz_system_ui_render(&state));
}

static void event(const SDL_Event *e)
{
    if (e->type == SDL_QUIT) { running = false; ryz_sim_factory_cancel(); return; }
    if (e->type == SDL_WINDOWEVENT) {
        if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
            (e->window.event == SDL_WINDOWEVENT_LEAVE && pressed))
            cancel_contact();
        if (e->window.event == SDL_WINDOWEVENT_EXPOSED) last_shows = (size_t)-1;
    }
    if(e->type==SDL_KEYUP && e->key.keysym.sym==SDLK_b) boot_key_pressed=false;
    if (e->type == SDL_KEYDOWN && !e->key.repeat) {
        const SDL_Keycode key = e->key.keysym.sym;
        if (key == SDLK_ESCAPE) {
            if (ryz_sim_factory_active()) ryz_sim_factory_cancel();
            else running = false;
        }
        else if (key >= SDLK_1 && key <= SDLK_3) {
            cancel_contact();
            scale = (int)(key - SDLK_0);
            SDL_SetWindowSize(window, 240 * scale, 240 * scale);
            last_shows = (size_t)-1;
        } else if (ryz_sim_factory_active()) {
            if (key == SDLK_HOME) ryz_sim_factory_cancel();
            else if(key==SDLK_b) { boot_key_pressed=true; boot_key_since=(uint64_t)esp_timer_get_time()/1000U; }
        } else if (key == SDLK_m) factory_requested = RYZ_SIM_FACTORY_MONITOR;
        else if (key == SDLK_i) factory_requested = RYZ_SIM_FACTORY_I2C;
        else if (key == SDLK_t) factory_requested = RYZ_SIM_FACTORY_HARDWARE;
        else if (key == SDLK_n && ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_HOME) {
            home_network_online = !home_network_online;
            state.connected = home_network_online;
            state.ap_active = !home_network_online;
            state.state = home_network_online ? RYZ_SYSTEM_UI_NETWORK_ONLINE : RYZ_SYSTEM_UI_NETWORK_AP_READY;
            ryz_v5_status_t status;
            ryz_v5_status_get(&status);
            status.network.enabled = true; /* Both fixture states keep the demo radio enabled. */
            (void)ryz_v5_status_set(&status);
            (void)telemetry_refresh();
            checked(ryz_system_ui_render(&state));
            puts(home_network_online ? "SIMULATED HOME NET: online; no network operation" :
                                      "SIMULATED HOME NET: offline; no network operation");
        }
        else if (key == SDLK_v) { static unsigned screen; ble_security_demo(screen++%3); }
        else if (key == SDLK_b) start_boot();
        else if (key == SDLK_HOME) page(RYZ_SYSTEM_UI_ROUTE_HOME);
        else if (key == SDLK_RIGHT || key == SDLK_LEFT) {
            int route = (int)ryz_system_ui_current_route() + (key == SDLK_RIGHT ? 1 : -1);
            page((ryz_system_ui_route_t)((route + RYZ_SYSTEM_UI_ROUTE_COUNT) % RYZ_SYSTEM_UI_ROUTE_COUNT));
        }
    }
    if (booting) return;
    if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT) {
        int x = e->button.x / scale, y = e->button.y / scale;
        if (e->button.x < 0 || e->button.y < 0 || x >= 240 || y >= 240) return;
        sample(RYZ_TOUCH_NONE, 0, 0);
        pressed = true;
        sample(RYZ_TOUCH_DOWN, x, y);
    } else if (e->type == SDL_MOUSEMOTION && pressed) {
        int x = e->motion.x / scale, y = e->motion.y / scale;
        if (e->motion.x < 0 || e->motion.y < 0 || x >= 240 || y >= 240) cancel_contact();
        else sample(RYZ_TOUCH_MOVE, x, y);
    } else if (e->type == SDL_MOUSEBUTTONUP && e->button.button == SDL_BUTTON_LEFT && pressed) {
        if (e->button.x < 0 || e->button.y < 0 || e->button.x >= 240 * scale || e->button.y >= 240 * scale)
            cancel_contact();
        else { pressed = false; sample(RYZ_TOUCH_UP, 0, 0); }
    }
}

static void drain(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) event(&e);
}

static void factory_idle(unsigned ms)
{
    drain();
    if (test_mode) {
        advance(ms ? ms : 1);
        if (monitor_test_tick) monitor_test_tick();
    } else {
        if (ms) SDL_Delay(ms);
        uint64_t now = SDL_GetTicks64();
        advance((uint32_t)(now - last_ticks)); last_ticks = now;
    }
}

static void run_factory(void)
{
    if (!factory_requested || !running) return;
    ryz_sim_factory_kind_t kind=factory_requested;
    factory_requested = RYZ_SIM_FACTORY_NONE; pressed = false; boot_key_pressed=false;
    if (booting) { checked(ryz_v5_boot_end()); booting = false; }
    checked(ryz_system_ui_reset_checked()); /* Release native display lease. */
    last_ticks = SDL_GetTicks64();
    ryz_sim_factory_run(kind,factory_idle, show, &factory_result);
    pressed = false; boot_key_pressed=false; last_ticks = SDL_GetTicks64();
    if (running) { page(RYZ_SYSTEM_UI_ROUTE_HOME); show(); }
}

static void click(int x, int y)
{
    SDL_Event e = {.type = SDL_MOUSEBUTTONDOWN};
    e.button.button = SDL_BUTTON_LEFT;
    e.button.x = x * scale; e.button.y = y * scale;
    require(SDL_PushEvent(&e) == 1, "push down");
    drain(); advance(50);
    e.type = SDL_MOUSEBUTTONUP;
    require(SDL_PushEvent(&e) == 1, "push up");
    drain(); advance(20);
}

static bool label(lv_obj_t *obj, const char *text)
{
    if (lv_obj_check_type(obj, &lv_label_class) && !strcmp(lv_label_get_text(obj), text)) return true;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i)
        if (label(lv_obj_get_child(obj, (int32_t)i), text)) return true;
    return false;
}

static void verify_texture(void)
{
    int width, height;
    require(SDL_GetRendererOutputSize(renderer, &width, &height) == 0, "output size");
    assert(width == scale * 240 && height == scale * 240);
    uint16_t *readback = malloc((size_t)width * (size_t)height * sizeof(*readback));
    require(readback != NULL, "readback allocation");
    require(SDL_RenderCopy(renderer, texture, NULL, NULL) == 0, "readback draw");
    require(SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_RGB565, readback, width * 2) == 0,
            "RGB565 readback");
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
        assert(readback[y * width + x] == ryz_host_display_pixel((unsigned)(x / scale), (unsigned)(y / scale)));
    free(readback);
}

/* Drive the actual SDL -> pointer adapter -> Lua ui.poll path. This catches
 * a missing/blocked factory entry, not just a separately renderable fixture. */
static const struct {
    unsigned at;
    const char *button, *scene, *text, *shot;
} monitor_steps[] = {
    {400, "pause", "monitor", "SYSTEM LOG / LIVE", "sdl-monitor-live"},
    {700, "clear", "monitor", "SYSTEM LOG / PAUSED", "sdl-monitor-paused"},
    {1000, "pause", "monitor", "WAITING FOR DATA", "sdl-monitor-cleared"},
    {1300, "config", "monitor", "SYSTEM LOG / LIVE", NULL},
    {1500, "source", "config", "SYSTEM >", "sdl-monitor-config-system"},
    {1700, "uart", "source", "CAPTURE SOURCE", "sdl-monitor-source"},
    {1900, "use", "source", "RSPORT RX", NULL},
    {2100, "port", "config", "P1 15/16 >", "sdl-monitor-config-uart"},
    {2300, "p3", "port", "RX 15", "sdl-monitor-port"},
    {2500, "swap", "port", "RX 13", NULL},
    {2700, "use", "port", "RX 14", "sdl-monitor-port-swapped"},
    {2900, "baud", "config", "P3 14/13 >", NULL},
    {3100, "b3", "baud", "11 PRESETS", "sdl-monitor-baud"},
    {3300, "use", "baud", "38400", NULL},
    {3500, "apply", "config", "38400 >", "sdl-monitor-ready"},
    {4000, "config", "monitor", "P3 38400 / LIVE", "sdl-monitor-uart"},
    {4200, "cancel", "config", "P3 14/13 >", NULL},
    {4400, "lost-focus", "monitor", "P3 38400 / LIVE", NULL},
    {4700, "cancel-fixture", "monitor", "P3 38400 / LIVE", NULL},
};
static struct {
    const char *directory;
    uint64_t started;
    unsigned step, mode;
    bool down;
} monitor_test;

static void monitor_scroll_test(unsigned elapsed)
{
    static const unsigned times[] = {4200,4250,4500,4540,4580,4750};
    if (monitor_test.step >= sizeof(times)/sizeof(*times) || elapsed < times[monitor_test.step]) return;
    SDL_Event e = {.type = SDL_MOUSEBUTTONDOWN};
    e.button.button = SDL_BUTTON_LEFT;
    e.button.x = 40 * scale; e.button.y = 212 * scale;
    switch (monitor_test.step) {
    case 0: assert(label(lv_screen_active(), "SYSTEM LOG / LIVE")); break;
    case 1: e.type = SDL_MOUSEBUTTONUP; break;
    case 2:
        assert(label(lv_screen_active(), "SYSTEM LOG / PAUSED"));
        assert(!label(lv_screen_active(), "SIMULATED INPUT ONLY"));
        e.button.x = 110 * scale; e.button.y = 80 * scale; break;
    case 3:
        e.type = SDL_MOUSEMOTION;
        e.motion.x = 110 * scale; e.motion.y = 160 * scale; break;
    case 4:
        e.type = SDL_MOUSEBUTTONUP;
        e.button.x = 110 * scale; e.button.y = 160 * scale; break;
    default:
        assert(label(lv_screen_active(), "SIMULATED INPUT ONLY"));
        ryz_host_display_save(monitor_test.directory, "sdl-monitor-scroll-history");
        e.type = SDL_KEYDOWN; e.key.keysym.sym = SDLK_ESCAPE; break;
    }
    require(SDL_PushEvent(&e) == 1, "push Monitor scroll"); drain(); ++monitor_test.step;
}

static void monitor_event_test(void)
{
    unsigned elapsed = (unsigned)((uint64_t)esp_timer_get_time()/1000U - monitor_test.started);
    assert(elapsed < 6500); /* A blocked entry/exit must fail rather than hang. */
    if (monitor_test.mode == 3) { monitor_scroll_test(elapsed); return; }
    if (monitor_test.mode) {
        if (elapsed < 400 || monitor_test.step) return;
        assert(!strcmp(ryz_sim_factory_scene()->id, "monitor"));
        assert(label(lv_screen_active(), "SYSTEM LOG / LIVE"));
        SDL_Event e = {.type = monitor_test.mode == 1 ? SDL_KEYDOWN : SDL_QUIT};
        e.key.keysym.sym = SDLK_ESCAPE;
        require(SDL_PushEvent(&e) == 1, "push Lua cancel/quit"); drain();
        ++monitor_test.step; return;
    }
    if (monitor_test.step >= sizeof(monitor_steps)/sizeof(*monitor_steps)) return;
    const unsigned index = monitor_test.step;
    if (elapsed < monitor_steps[index].at) return;
    SDL_Event e = {.type = SDL_MOUSEBUTTONUP};
    e.button.button = SDL_BUTTON_LEFT;
    if (monitor_test.down) {
        if (elapsed < monitor_steps[index].at + 40) return;
        /* Lua retains the last valid position, just like the touch Adapter. */
        require(SDL_PushEvent(&e) == 1, "push Lua up"); drain();
        monitor_test.down = false; ++monitor_test.step; return;
    }
    const ryz_ui_scene_t *scene = ryz_sim_factory_scene();
    assert(!strcmp(scene->id, monitor_steps[index].scene));
    assert(label(lv_screen_active(), monitor_steps[index].text));
    verify_texture();
    if (monitor_steps[index].shot) ryz_host_display_save(monitor_test.directory, monitor_steps[index].shot);
    if (!strcmp(monitor_steps[index].button, "cancel-fixture")) {
        e.type = SDL_KEYDOWN; e.key.keysym.sym = SDLK_ESCAPE;
        require(SDL_PushEvent(&e) == 1, "push Monitor physical cancel equivalent"); drain();
        ++monitor_test.step; return;
    }
    const bool lost = !strcmp(monitor_steps[index].button, "lost-focus");
    const char *id = lost ? "config" : monitor_steps[index].button;
    const ryz_ui_object_t *button = NULL;
    for (unsigned i = 0; i < scene->object_count; ++i)
        if (!strcmp(scene->objects[i].id, id)) button = &scene->objects[i];
    assert(button && button->kind == RYZ_UI_BUTTON && button->enabled && button->visible);
    e.type = SDL_MOUSEBUTTONDOWN;
    e.button.x = (button->x + button->width/2) * scale;
    e.button.y = (button->y + button->height/2) * scale;
    if (button->parent[0]) for (unsigned i = 0; i < scene->object_count; ++i) {
        const ryz_ui_object_t *parent = &scene->objects[i];
        if (!strcmp(parent->id, button->parent)) {
            e.button.x += parent->x * scale;
            e.button.y += (parent->y - parent->scroll_y) * scale;
        }
    }
    require(SDL_PushEvent(&e) == 1, "push Lua down"); drain();
    if (lost) {
        SDL_Event loss = {.type = SDL_WINDOWEVENT}; loss.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
        require(SDL_PushEvent(&loss) == 1, "push Lua focus loss");
        e.type = SDL_MOUSEBUTTONUP;
        require(SDL_PushEvent(&e) == 1, "push Lua stale up"); drain();
        ++monitor_test.step;
    } else monitor_test.down = true;
}

static void test_monitor(const char *directory)
{
    page(RYZ_SYSTEM_UI_ROUTE_APPS); advance(20);
    assert(label(lv_screen_active(), "tool_monitor.lua"));
    click(70, 128);
    assert(label(lv_screen_active(), "Real factory Lua. SIM input; no device."));
    SDL_Event hold = {.type = SDL_MOUSEBUTTONDOWN};
    hold.button.button = SDL_BUTTON_LEFT; hold.button.x = 150 * scale; hold.button.y = 220 * scale;
    require(SDL_PushEvent(&hold) == 1, "push factory run hold"); drain(); advance(1999);
    assert(!factory_requested);
    advance(1); assert(factory_requested);
    hold.type = SDL_MOUSEBUTTONUP;
    require(SDL_PushEvent(&hold) == 1, "push factory run release"); drain();
    monitor_test.directory = directory;
    monitor_test.started = (uint64_t)esp_timer_get_time()/1000U;
    monitor_test_tick = monitor_event_test;
    run_factory();
    assert(!factory_result.ok && !strcmp(factory_result.phase, "stopped"));
    assert(monitor_test.step == sizeof(monitor_steps)/sizeof(*monitor_steps));
    assert(!ryz_sim_factory_active() && ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_HOME);
    ryz_host_display_save(directory, "sdl-monitor-return-home");
    /* Re-launch in the same process: capture identity and display ownership
     * must survive repeated Esc cancellation and closing the window. */
    const unsigned modes[] = {3,1,2};
    for (unsigned i = 0; i < sizeof(modes)/sizeof(*modes); ++i) {
        unsigned mode = modes[i];
        SDL_Event key = {.type = SDL_KEYDOWN}; key.key.keysym.sym = SDLK_0 + mode;
        require(SDL_PushEvent(&key) == 1, "push Lua test scale"); drain();
        key.key.keysym.sym = SDLK_m;
        require(SDL_PushEvent(&key) == 1, "push Monitor shortcut"); drain();
        assert(factory_requested);
        monitor_test.mode = mode; monitor_test.step = 0;
        monitor_test.started = (uint64_t)esp_timer_get_time()/1000U;
        run_factory();
        assert(!factory_result.ok && !strcmp(factory_result.phase, "stopped"));
        assert(monitor_test.step == (mode == 3 ? 6U : 1U) && !ryz_sim_factory_active());
        if (mode != 2) assert(running && ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_HOME);
        else assert(!running);
    }
    monitor_test_tick = NULL;
    puts("PASS SDL Monitor: factory APPS -> real Lua -> pause/clear/config/source/port/swap/baud/apply -> Esc/Home; paused drag, focus-loss, re-launch, close cleanup");
}


static void home_swipe(int from_x, int to_x, ryz_system_ui_route_t destination)
{
    SDL_Event e = {.type = SDL_MOUSEBUTTONDOWN};
    e.button.button = SDL_BUTTON_LEFT;
    e.button.x = from_x * scale; e.button.y = 135 * scale;
    require(SDL_PushEvent(&e) == 1, "push HOME swipe down"); drain(); advance(50);
    e.type = SDL_MOUSEMOTION;
    e.motion.x = to_x * scale; e.motion.y = 135 * scale;
    require(SDL_PushEvent(&e) == 1, "push HOME swipe move"); drain(); advance(50);
    e.type = SDL_MOUSEBUTTONUP; e.button.button = SDL_BUTTON_LEFT;
    e.button.x = to_x * scale; e.button.y = 135 * scale;
    require(SDL_PushEvent(&e) == 1, "push HOME swipe up"); drain(); advance(20);
    assert(ryz_system_ui_current_route() == destination);
}

static void test_home_metrics(const char *directory)
{
    for(unsigned i=0;i<12;++i) advance(1000);
    assert(state.home_selection.metric==RYZ_HOME_METRIC_NET);
    assert(state.home_value_valid && state.home_history_count>=10);
    assert(state.cpu_temperature_valid && state.cpu_load_valid && state.psram_usage_valid);
    const ryz_home_selection_t fixed=ryz_system_ui_home_selection();
    const int xs[]={40,116,195};
    for(unsigned i=0;i<3;++i) {
        click(xs[i],134);
        assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_HOME);
        assert(ryz_system_ui_home_selection().metric==fixed.metric);
        assert(ryz_system_ui_home_selection().generation==fixed.generation);
        assert(state.home_history_count>=10);
    }
    show(); verify_texture();
    ryz_host_display_save(directory,"sdl-home-fixed-online-240");
    click(145,70);
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_WIFI_CONNECTED);
    click(16,16);
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_HOME);
    SDL_Event key={.type=SDL_KEYDOWN}; key.key.keysym.sym=SDLK_n;
    require(SDL_PushEvent(&key)==1,"push NET offline"); drain(); advance(1000);
    assert(!state.connected && !state.home_value_valid && !state.home_history_count);
    assert(state.cpu_temperature_valid && state.cpu_load_valid && state.psram_usage_valid);
    assert(label(lv_screen_active(),"OFFLINE"));
    char temperature[16];
    snprintf(temperature,sizeof(temperature),"%d \xc2\xb0" "C",(int)state.cpu_temperature_c);
    assert(label(lv_screen_active(),temperature));
    ryz_host_display_save(directory,"sdl-home-fixed-offline-240");
    home_swipe(210,30,RYZ_SYSTEM_UI_ROUTE_APPS);
    home_swipe(30,210,RYZ_SYSTEM_UI_ROUTE_HOME);
    assert(ryz_system_ui_home_selection().metric==RYZ_HOME_METRIC_NET);
    require(SDL_PushEvent(&key)==1,"push NET online"); drain(); advance(20);
    assert(state.connected);
    assert(blocked_actions==0);
    puts("PASS SDL HOME: fixed WLAN trend, simultaneous readouts, inert cards, Wi-Fi/Back and swipe; offline keeps local telemetry");
}

/* Use populated rows, not an empty APPS fixture: a stray release could select
 * a real script while the route itself still reads APPS. SDL supplies MOVE
 * coordinates; the production adapter deliberately sends positionless UP,
 * matching the touch driver. Hardware gesture flags are tested at its seam. */
static void test_apps_swipes(const char *directory)
{
    const struct {
        const char *name;
        int dx[3], dy[3];
        unsigned moves, down_ms;
        ryz_system_ui_route_t destination;
    } paths[] = {
        {"left", {-15,-45,-90}, {0,0,0}, 3, 10, RYZ_SYSTEM_UI_ROUTE_SETTINGS},
        {"right", {15,45,90}, {0,0,0}, 3, 10, RYZ_SYSTEM_UI_ROUTE_HOME},
        {"short-left", {-14,-25}, {0,0}, 2, 10, RYZ_SYSTEM_UI_ROUTE_APPS},
        {"short-right", {14,25}, {0,0}, 2, 10, RYZ_SYSTEM_UI_ROUTE_APPS},
        {"diagonal-left", {-7,-14}, {6,12}, 2, 10, RYZ_SYSTEM_UI_ROUTE_APPS},
        {"diagonal-right", {7,14}, {6,12}, 2, 10, RYZ_SYSTEM_UI_ROUTE_APPS},
        {"fold-left", {-20,-40,0}, {0,0,0}, 3, 10, RYZ_SYSTEM_UI_ROUTE_APPS},
        {"fold-right", {20,40,0}, {0,0,0}, 3, 10, RYZ_SYSTEM_UI_ROUTE_APPS},
        {"highlight-left", {-15,-45,-90}, {0,0,0}, 3, 100, RYZ_SYSTEM_UI_ROUTE_SETTINGS},
        {"highlight-right", {15,45,90}, {0,0,0}, 3, 100, RYZ_SYSTEM_UI_ROUTE_HOME},
    };
    const unsigned before_actions = blocked_actions;
    for (int n = 1; n <= 3; ++n) {
        SDL_Event key = {.type = SDL_KEYDOWN}; key.key.keysym.sym = SDLK_0 + n;
        require(SDL_PushEvent(&key) == 1, "push APPS swipe scale"); drain();
        for (unsigned i = 0; i < sizeof(paths)/sizeof(*paths); ++i) {
            page(RYZ_SYSTEM_UI_ROUTE_APPS); advance(20);
            assert(ryz_v5_apps_has_navigation() &&
                label(lv_screen_active(), "sample_clock.lua") &&
                label(lv_screen_active(), "LUA FILES"));
            if (n == 1 && i == 0) ryz_host_display_save(directory, "sdl-apps-swipe-list-240");
            printf("SDL APPS swipe: %dx %s\n", n, paths[i].name);
            SDL_Event down = {.type = SDL_MOUSEBUTTONDOWN};
            down.button.button = SDL_BUTTON_LEFT;
            down.button.x = 120 * scale; down.button.y = 70 * scale;
            require(SDL_PushEvent(&down) == 1, "push APPS row down"); drain();
            advance(paths[i].down_ms);
            assert(apps.reader.view == RYZ_APPS_VIEW_CATALOG && !label(lv_screen_active(), "APP INFO"));
            for (unsigned p = 0; p < paths[i].moves; ++p) {
                SDL_Event move = {.type = SDL_MOUSEMOTION};
                move.motion.state = SDL_BUTTON_LMASK;
                move.motion.x = (120 + paths[i].dx[p]) * scale;
                move.motion.y = (70 + paths[i].dy[p]) * scale;
                require(SDL_PushEvent(&move) == 1, "push APPS row move"); drain(); advance(10);
                assert(apps.reader.view == RYZ_APPS_VIEW_CATALOG && !label(lv_screen_active(), "APP INFO"));
            }
            SDL_Event up = {.type = SDL_MOUSEBUTTONUP};
            up.button.button = SDL_BUTTON_LEFT;
            up.button.x = (120 + paths[i].dx[paths[i].moves - 1]) * scale;
            up.button.y = (70 + paths[i].dy[paths[i].moves - 1]) * scale;
            require(SDL_PushEvent(&up) == 1, "push APPS row up"); drain(); advance(20);
            assert(ryz_system_ui_current_route() == paths[i].destination);
            assert(apps.reader.view == RYZ_APPS_VIEW_CATALOG && !label(lv_screen_active(), "APP INFO"));
            if (n == 1 && i == 2) ryz_host_display_save(directory, "sdl-apps-short-swipe-240");
            verify_texture();

            /* A consumed swipe must not leave the next contact suppressed. */
            if (paths[i].destination != RYZ_SYSTEM_UI_ROUTE_APPS) {
                click(120, 220);
                assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_APPS);
            }
            click(120, 70);
            assert(apps.reader.view == RYZ_APPS_VIEW_DETAIL && label(lv_screen_active(), "APP INFO"));
            assert(label(lv_screen_active(), "DEMO ONLY. No script is executed."));
            if (n == 1 && i == 0) ryz_host_display_save(directory, "sdl-apps-swipe-next-tap-detail-240");
            click(20, 16);
            assert(apps.reader.view == RYZ_APPS_VIEW_CATALOG &&
                ryz_v5_apps_has_navigation() && label(lv_screen_active(), "LUA FILES"));
            assert(blocked_actions == before_actions);
        }
    }
    page(RYZ_SYSTEM_UI_ROUTE_HOME); advance(20);
    puts("PASS SDL APPS: populated-row left/right, short/diagonal/fold-back and delayed-highlight swipes never select; next tap and detail Back remain usable at 1x/2x/3x");
}

/* Separate SDL unions preserve mouse button identity. The adapter still emits
 * positionless UP, so these checks exercise the same last-MOVE ownership used
 * by the native touch path, not direct calls to view scrolling helpers. */
static void drag_event(uint32_t type, int x, int y)
{
    SDL_Event e = {.type = type};
    if (type == SDL_MOUSEMOTION) {
        e.motion.state = SDL_BUTTON_LMASK;
        e.motion.x = x * scale; e.motion.y = y * scale;
    } else {
        e.button.button = SDL_BUTTON_LEFT;
        e.button.x = x * scale; e.button.y = y * scale;
    }
    require(SDL_PushEvent(&e) == 1, "push detail drag"); drain(); advance(10);
}

static bool same_pixels(const uint16_t *before, int x, int y, int w, int h)
{
    for (int row = y; row < y + h; ++row)
        for (int col = x; col < x + w; ++col)
            if (before[row * 240 + col] != ryz_host_display_pixel((unsigned)col, (unsigned)row))
                return false;
    return true;
}

static void info_demo(ryz_system_ui_route_t route)
{
    ryz_v5_status_t status;
    ryz_v5_status_get(&status);
    strcpy(status.network.sta_ssid, "DEMO LONG NETWORK FOR SCROLL");
    status.network.mac_valid = true; status.network.ipv6_valid = true;
    strcpy(status.network.mac, "02:11:22:33:44:55");
    strcpy(status.network.ipv6, "2001:db8:1234:5678:9abc:def0:1234:5678");
    status.ble = (ryz_v5_ble_model_t){.enabled = true, .bonded = true,
        .linked = true, .bond_known = true, .service_known = true,
        .service_ready = true, .operation_id = 17};
    strcpy(status.ble.local_name, "DEMO RYZOBEE SCROLL FIXTURE");
    strcpy(status.ble.peer_name, "DEMO PHONE WITH LONG NAME");
    strcpy(status.ble.local_address, "02:11:22:33:44:55");
    strcpy(status.ble.role, "PERIPHERAL");
    strcpy(status.version.build, "SDL SCROLL TEST BUILD");
    strcpy(status.version.idf, "5.5.4");
    strcpy(status.version.slot, "DEMO OTA SLOT");
    strcpy(status.version.uptime, "00:42:17");
    strcpy(status.version.chip, "ESP32-S3 / SIMULATED");
    strcpy(status.version.device_id, "0123456789abcdef0123456789abcdef");
    status.version.show_device_id = false;
    (void)ryz_v5_status_set(&status);
    page(route); advance(20);
}

static void test_info_swipes(const char *directory)
{
    const struct { ryz_system_ui_route_t route; const char *name; } pages[] = {
        {RYZ_SYSTEM_UI_ROUTE_WIFI_INFO, "wifi"},
        {RYZ_SYSTEM_UI_ROUTE_BLE_INFO, "ble"},
        {RYZ_SYSTEM_UI_ROUTE_VERSION_INFO, "version"},
    };
    uint16_t initial[240 * 240], displaced[240 * 240];
    const unsigned actions_before = blocked_actions;
    for (int n = 1; n <= 3; ++n) {
        SDL_Event key = {.type = SDL_KEYDOWN}; key.key.keysym.sym = SDLK_0 + n;
        require(SDL_PushEvent(&key) == 1, "push INFO scale"); drain();
        for (unsigned p = 0; p < sizeof(pages) / sizeof(*pages); ++p) {
            const ryz_system_ui_route_t route = pages[p].route;
            info_demo(route);
            memcpy(initial, ryz_host_display_frame(), sizeof(initial));
            char name[64];
            if (n == 1) {
                snprintf(name, sizeof(name), "sdl-%s-info-top-240", pages[p].name);
                ryz_host_display_save(directory, name);
            }
            drag_event(SDL_MOUSEBUTTONDOWN, 150, 170);
            drag_event(SDL_MOUSEMOTION, 150, 150);
            assert(!same_pixels(initial, 8, 58, 224, 132));
            assert(same_pixels(initial, 0, 194, 240, 46));
            memcpy(displaced, ryz_host_display_frame(), sizeof(displaced));
            drag_event(SDL_MOUSEMOTION, 150, 160);
            assert(!same_pixels(displaced, 8, 58, 224, 132));
            drag_event(SDL_MOUSEMOTION, 150, 170);
            assert(same_pixels(initial, 8, 58, 224, 132));
            drag_event(SDL_MOUSEMOTION, 150, 100);
            assert(!same_pixels(initial, 8, 58, 224, 132));
            assert(same_pixels(initial, 0, 194, 240, 46));
            if (n == 1) {
                snprintf(name, sizeof(name), "sdl-%s-info-scrolled-240", pages[p].name);
                ryz_host_display_save(directory, name);
            }
            drag_event(SDL_MOUSEBUTTONUP, 150, 100);
            assert(ryz_system_ui_current_route() == route);

            /* Scroll origin owns the complete contact even over a live fixed
             * footer: VERSION must not toggle its ID; BLE must not enter the
             * destructive confirmation page. A new clean tap remains usable. */
            drag_event(SDL_MOUSEBUTTONDOWN, 150, 170);
            drag_event(SDL_MOUSEMOTION, 150, 215);
            drag_event(SDL_MOUSEBUTTONUP, 150, 215);
            assert(ryz_system_ui_current_route() == route);
            ryz_v5_status_t status;
            ryz_v5_status_get(&status);
            assert(!status.version.show_device_id && blocked_actions == actions_before);
            drag_event(SDL_MOUSEBUTTONDOWN, 120, 215);
            drag_event(SDL_MOUSEMOTION, 144, 215);
            drag_event(SDL_MOUSEMOTION, 120, 215);
            drag_event(SDL_MOUSEBUTTONUP, 120, 215);
            assert(ryz_system_ui_current_route() == route);
            ryz_v5_status_get(&status);
            assert(!status.version.show_device_id && blocked_actions == actions_before);
            if (route == RYZ_SYSTEM_UI_ROUTE_VERSION_INFO) {
                click(120, 215); ryz_v5_status_get(&status);
                assert(status.version.show_device_id);
            } else if (route == RYZ_SYSTEM_UI_ROUTE_BLE_INFO) {
                click(120, 215);
                assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_BLE_FORGET);
            }
            verify_texture();
        }
    }
    puts("PASS SDL INFO: Wi-Fi/BLE/Version vertical drag reverses continuously through origin; fixed footers never move or receive the scrolling contact; clean footer taps work at 1x/2x/3x");
}

static unsigned ble_swipe_test_requests;
static esp_err_t ble_swipe_test_submit(void *context, ryz_v5_ble_action_t action,
                                      uint32_t operation, uint32_t value)
{
    (void)context; (void)action; (void)operation; (void)value;
    ++ble_swipe_test_requests;
    return ESP_ERR_NOT_SUPPORTED; /* Count intent only; no hardware mutation. */
}

static void test_button_swipes(void)
{
    const struct { ryz_system_ui_route_t route; int x, y; bool ble; } buttons[] = {
        {RYZ_SYSTEM_UI_ROUTE_WIFI_CONNECTED, 120, 156, false},
        {RYZ_SYSTEM_UI_ROUTE_BLE_OFF, 202, 44, true},
        {RYZ_SYSTEM_UI_ROUTE_VERSION_AVAILABLE, 150, 200, false},
    };
    const struct { int dx, dy; bool fold; } paths[] = {
        {24, 0, false}, {0, 12, false}, {24, 0, true}, {14, 12, false},
    };
    const ryz_v5_ble_binding_t binding = {.submit = ble_swipe_test_submit};
    ryz_v5_ble_bind_services(&binding);
    for (int n = 1; n <= 3; ++n) {
        SDL_Event key = {.type = SDL_KEYDOWN}; key.key.keysym.sym = SDLK_0 + n;
        require(SDL_PushEvent(&key) == 1, "push button swipe scale"); drain();
        for (unsigned b = 0; b < sizeof(buttons) / sizeof(*buttons); ++b) {
            for (unsigned p = 0; p < sizeof(paths) / sizeof(*paths); ++p) {
                ryz_v5_status_t status;
                ryz_v5_status_get(&status);
                status.ble = (ryz_v5_ble_model_t){.bond_known = true};
                status.network.can_setup = true; status.network.busy = false;
                status.version.update_ready = true;
                (void)ryz_v5_status_set(&status);
                page(buttons[b].route); advance(20);
                const unsigned before = buttons[b].ble ? ble_swipe_test_requests : blocked_actions;
                drag_event(SDL_MOUSEBUTTONDOWN, buttons[b].x, buttons[b].y);
                drag_event(SDL_MOUSEMOTION, buttons[b].x + paths[p].dx, buttons[b].y + paths[p].dy);
                if (paths[p].fold) drag_event(SDL_MOUSEMOTION, buttons[b].x, buttons[b].y);
                drag_event(SDL_MOUSEBUTTONUP, buttons[b].x, buttons[b].y);
                assert(ryz_system_ui_current_route() == buttons[b].route);
                assert((buttons[b].ble ? ble_swipe_test_requests : blocked_actions) == before);
                click(buttons[b].x, buttons[b].y);
                assert((buttons[b].ble ? ble_swipe_test_requests : blocked_actions) == before + 1);
            }
        }
    }
    ryz_v5_ble_bind_services(NULL);
    puts("PASS SDL buttons: Wi-Fi provision/BLE toggle/OTA update suppress horizontal, vertical, fold-back and diagonal drags wholly inside the button; next clean tap submits exactly once at 1x/2x/3x");
}

static void test_version_description_swipes(const char *directory)
{
    uint16_t initial[240 * 240], displaced[240 * 240];
    const unsigned actions_before = blocked_actions;
    SDL_Event key = {.type = SDL_KEYDOWN}; key.key.keysym.sym = SDLK_1;
    require(SDL_PushEvent(&key) == 1, "push description scale"); drain();
    for (unsigned long_text = 0; long_text <= 1; ++long_text) {
        ryz_v5_status_t status;
        ryz_v5_status_get(&status);
        strcpy(status.version.update_description, long_text ?
            "DEMO ONLY. Long update notes exercise scrolling. No release or download is available. Read all lines before continuing. Keep power connected during updates." :
            "DEMO ONLY. No update is performed.");
        status.version.update_ready = true;
        (void)ryz_v5_status_set(&status);
        page(RYZ_SYSTEM_UI_ROUTE_VERSION_AVAILABLE); advance(20);
        assert((ryz_v5_version_available_height(&status.version) > 48) == (bool)long_text);
        memcpy(initial, ryz_host_display_frame(), sizeof(initial));
        ryz_host_display_save(directory, long_text ? "sdl-version-description-top-240" :
                                                  "sdl-version-description-short-240");
        drag_event(SDL_MOUSEBUTTONDOWN, 150, 154);
        drag_event(SDL_MOUSEMOTION, 150, 130);
        assert(same_pixels(initial, 24, 118, 192, 48) == !long_text);
        assert(same_pixels(initial, 0, 166, 240, 74));
        memcpy(displaced, ryz_host_display_frame(), sizeof(displaced));
        drag_event(SDL_MOUSEMOTION, 150, 142);
        assert(same_pixels(displaced, 24, 118, 192, 48) == !long_text);
        drag_event(SDL_MOUSEMOTION, 150, 154);
        assert(same_pixels(initial, 24, 118, 192, 48));
        drag_event(SDL_MOUSEMOTION, 150, 60);
        assert(same_pixels(initial, 0, 166, 240, 74));
        if (long_text) ryz_host_display_save(directory, "sdl-version-description-bottom-240");
        drag_event(SDL_MOUSEMOTION, 150, 204);
        drag_event(SDL_MOUSEBUTTONUP, 150, 204);
        assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_VERSION_AVAILABLE);
        assert(blocked_actions == actions_before);
        verify_texture();
    }
    puts("PASS SDL VERSION AVAILABLE: 192x48 description clips and scrolls only actual overflow; reverse-to-origin is exact, short text remains fixed, crossing UPDATE cannot activate it; DEMO notes only");
}

static void test_v5_detail_swipes(const char *directory)
{
    ryz_v5_status_t before;
    ryz_v5_status_get(&before);
    test_info_swipes(directory);
    test_button_swipes();
    test_version_description_swipes(directory);
    (void)ryz_v5_status_set(&before);
    page(RYZ_SYSTEM_UI_ROUTE_HOME); advance(20);
}

typedef struct {
    unsigned at;
    const char *action,*scene,*text,*shot;
} factory_step_t;
static const factory_step_t i2c_steps[] = {
    {2600,"pins","scan","READ-ONLY SCAN","sdl-i2c-internal"},
    {2800,"#lost:sda","pins","CUSTOM BUS","sdl-i2c-pins"},
    {3000,"sda","pins","CUSTOM BUS",NULL},
    {3200,"p13","sda","SELECT SDA PIN","sdl-i2c-sda"},
    {3400,"use","sda","GPIO13",NULL},
    {3600,"scl","pins","GPIO13",NULL},
    {3800,"p14","scl","SELECT SCL PIN",NULL},
    {4000,"use","scl","GPIO14",NULL},
    {4200,"clock","pins","GPIO14",NULL},
    {4400,"c400","clock","SELECT CLOCK","sdl-i2c-clock"},
    {4600,"use","clock","400 kHz",NULL},
    {4800,"submit","pins","400 kHz",NULL},
    {7400,"#drag","scan","3 FOUND","sdl-i2c-custom"},
    {7800,"#esc","scan","READ-ONLY SCAN","sdl-i2c-drag-release"},
};
static const factory_step_t hardware_steps[] = {
    {200,"#lost:h1","hub","ROOTMAKER SELF TEST","sdl-test-hub"},
    {400,"h1","hub","ROOTMAKER SELF TEST",NULL},
    {600,"primary","lcd","LCD / PIXELS","sdl-test-lcd"},
    {800,"h2","hub","ROOTMAKER SELF TEST",NULL},
    {1000,"g1","touch","0 / 9","sdl-test-touch-empty"},
    {1200,"g2","touch","1 / 9",NULL},
    {1400,"g3","touch","2 / 9",NULL},
    {1600,"g4","touch","3 / 9",NULL},
    {1800,"g5","touch","4 / 9",NULL},
    {2000,"g6","touch","5 / 9",NULL},
    {2200,"g7","touch","6 / 9",NULL},
    {2400,"g8","touch","7 / 9",NULL},
    {2600,"g9","touch","8 / 9",NULL},
    {2800,"primary","touch","9 / 9","sdl-test-touch-complete"},
    {3000,"h3","hub","ROOTMAKER SELF TEST",NULL},
    {4500,"primary","imu","MOTION","sdl-test-imu"},
    {4700,"h4","hub","ROOTMAKER SELF TEST",NULL},
    {4900,"primary","rgb","START","sdl-test-rgb-idle"},
    {5100,"primary","rgb","MATCH","sdl-test-rgb-red"},
    {5300,"primary","rgb","MATCH","sdl-test-rgb-green"},
    {5500,"primary","rgb","MATCH","sdl-test-rgb-blue"},
    {5900,"h5","hub","ROOTMAKER SELF TEST",NULL},
    {6100,"primary","battery","NOT AVAILABLE","sdl-test-battery"},
    {6300,"again","summary","SELF TEST COMPLETE","sdl-test-summary"},
    {6500,"#esc","hub","ROOTMAKER SELF TEST","sdl-test-again"},
};
static const factory_step_t i2c_reopen_steps[] = {
    {400,"#esc","scan","INTERNAL I2C0","sdl-i2c-reopen"},
};
static const factory_step_t boot_exit_steps[] = {
    {200,"#boot","hub","ROOTMAKER SELF TEST",NULL},
    {400,"#focus","hub","ROOTMAKER SELF TEST",NULL},
    /* A lost-focus B down must not cancel at 5s; start a fresh hold. */
    {5600,"#boot","hub","ROOTMAKER SELF TEST","sdl-test-reopen"},
};
static struct {
    const factory_step_t *steps;
    size_t count,index;
    unsigned phase;
    uint64_t started;
    const char *directory;
} factory_test;

static void factory_event_test(void)
{
    unsigned elapsed=(unsigned)((uint64_t)esp_timer_get_time()/1000U-factory_test.started);
    assert(elapsed<18000);
    if(factory_test.index==factory_test.count) return;
    const factory_step_t *step=&factory_test.steps[factory_test.index];
    if(elapsed<step->at+factory_test.phase*40U) return;
    SDL_Event e={.type=SDL_MOUSEBUTTONUP};
    e.button.button=SDL_BUTTON_LEFT;
    if(factory_test.phase) {
        if(!strcmp(step->action,"#drag") && factory_test.phase<3) {
            e.type=SDL_MOUSEMOTION; e.motion.x=110*scale;
            e.motion.y=(factory_test.phase==1 ? 117 : 196)*scale;
            ++factory_test.phase;
        } else {
            factory_test.phase=0; ++factory_test.index;
        }
        require(SDL_PushEvent(&e)==1,"push factory gesture continuation"); drain(); return;
    }
    const ryz_ui_scene_t *scene=ryz_sim_factory_scene();
    if(strcmp(scene->id,step->scene) || !label(lv_screen_active(),step->text))
        fprintf(stderr,"factory SDL step %zu: expected %s / %s, got %s\n",factory_test.index,step->scene,step->text,scene->id);
    assert(!strcmp(scene->id,step->scene) && label(lv_screen_active(),step->text));
    verify_texture();
    if(step->shot) ryz_host_display_save(factory_test.directory,step->shot);
    if(!strcmp(step->action,"#esc") || !strcmp(step->action,"#boot")) {
        e.type=SDL_KEYDOWN; e.key.keysym.sym=!strcmp(step->action,"#esc") ? SDLK_ESCAPE : SDLK_b;
        require(SDL_PushEvent(&e)==1,"push factory exit key"); drain(); ++factory_test.index; return;
    }
    if(!strcmp(step->action,"#focus")) {
        e.type=SDL_WINDOWEVENT; e.window.event=SDL_WINDOWEVENT_FOCUS_LOST;
        require(SDL_PushEvent(&e)==1,"push factory key focus loss"); drain(); ++factory_test.index; return;
    }
    e.type=SDL_MOUSEBUTTONDOWN;
    if(!strcmp(step->action,"#drag")) {
        e.button.x=110*scale; e.button.y=155*scale;
    } else {
        bool lost=!strncmp(step->action,"#lost:",6);
        const char *id=step->action+(lost ? 6 : 0);
        const ryz_ui_object_t *button=NULL;
        for(unsigned i=0;i<scene->object_count;++i)
            if(!strcmp(scene->objects[i].id,id)) button=&scene->objects[i];
        assert(button && button->kind==RYZ_UI_BUTTON && button->enabled && button->visible);
        e.button.x=(button->x+button->width/2)*scale;
        e.button.y=(button->y+button->height/2)*scale;
        if(lost) {
            require(SDL_PushEvent(&e)==1,"push factory interrupted down"); drain();
            SDL_Event loss={.type=SDL_WINDOWEVENT}; loss.window.event=SDL_WINDOWEVENT_FOCUS_LOST;
            require(SDL_PushEvent(&loss)==1,"push factory focus loss");
            e.type=SDL_MOUSEBUTTONUP;
            require(SDL_PushEvent(&e)==1,"push factory stale release"); drain();
            ++factory_test.index; return;
        }
    }
    require(SDL_PushEvent(&e)==1,"push factory button/drag down"); drain(); factory_test.phase=1;
}

static void factory_run_test(const factory_step_t *steps,size_t count,const char *directory)
{
    factory_test.steps=steps; factory_test.count=count; factory_test.index=0; factory_test.phase=0;
    factory_test.directory=directory; factory_test.started=(uint64_t)esp_timer_get_time()/1000U;
    monitor_test_tick=factory_event_test;
    run_factory();
    assert(factory_test.index==count);
    assert(!factory_result.ok && !strcmp(factory_result.phase,"stopped"));
    assert(!ryz_sim_factory_active() && running && ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_HOME);
    monitor_test_tick=NULL;
}

static void factory_apps_hold(ryz_sim_factory_kind_t kind,int row_y)
{
    page(RYZ_SYSTEM_UI_ROUTE_APPS); advance(20);
    click(70,row_y);
    const ryz_sim_factory_source_t *source=ryz_sim_factory_source(kind);
    assert(!strcmp(apps.reader.detail.file.entry.name,source->name));
    assert(!strcmp(apps.reader.detail.file.sha256,source->sha256));
    assert(!strcmp(apps.reader.detail.metadata.version,source->version));
    SDL_Event hold={.type=SDL_MOUSEBUTTONDOWN};
    hold.button.button=SDL_BUTTON_LEFT; hold.button.x=150*scale; hold.button.y=220*scale;
    require(SDL_PushEvent(&hold)==1,"push factory APPS hold"); drain(); advance(1999);
    assert(!factory_requested); advance(1); assert(factory_requested==kind);
    hold.type=SDL_MOUSEBUTTONUP;
    require(SDL_PushEvent(&hold)==1,"push factory APPS release"); drain();
}

static void test_factory_apps(const char *directory)
{
    page(RYZ_SYSTEM_UI_ROUTE_HOME); advance(20);
    SDL_Event key={.type=SDL_KEYDOWN}; key.key.keysym.sym=SDLK_1;
    require(SDL_PushEvent(&key)==1,"push native 1x scale"); drain();
    key.key.keysym.sym=SDLK_i;
    require(SDL_PushEvent(&key)==1,"push I2C factory shortcut"); drain();
    assert(factory_requested==RYZ_SIM_FACTORY_I2C);
    factory_run_test(i2c_steps,sizeof(i2c_steps)/sizeof(*i2c_steps),directory);
    factory_apps_hold(RYZ_SIM_FACTORY_HARDWARE,186);
    factory_run_test(hardware_steps,sizeof(hardware_steps)/sizeof(*hardware_steps),directory);
    factory_apps_hold(RYZ_SIM_FACTORY_I2C,158);
    factory_run_test(i2c_reopen_steps,sizeof(i2c_reopen_steps)/sizeof(*i2c_reopen_steps),directory);
    key.key.keysym.sym=SDLK_t;
    require(SDL_PushEvent(&key)==1,"push DeviceTest factory shortcut"); drain();
    assert(factory_requested==RYZ_SIM_FACTORY_HARDWARE);
    factory_run_test(boot_exit_steps,sizeof(boot_exit_steps)/sizeof(*boot_exit_steps),directory);
    assert((uint64_t)esp_timer_get_time()/1000U-factory_test.started>=10600U);
    assert((uint64_t)esp_timer_get_time()/1000U-factory_test.started<10800U);
    ryz_host_display_save(directory,"sdl-factory-return-home");
    puts("PASS SDL factories: same fs Lua bytes + public APIs, I/T + APPS hold, I2C pin drafts/scan/drag, 5 DeviceTest pages, focus loss, Esc/BOOT 5s, resource cleanup/reopen; SIMULATED ONLY");
}

static void self_test(const char *directory)
{
    static uint16_t boot[240 * 240];
    memcpy(boot, ryz_host_display_frame(), sizeof(boot));
    ryz_host_display_save(directory, "sdl-boot");
    advance(400);
    unsigned changed = 0;
    for (unsigned y = 0; y < 240; ++y) for (unsigned x = 0; x < 240; ++x) {
        if (boot[y * 240 + x] != ryz_host_display_pixel(x, y)) {
            assert(y >= 234 && y < 236 && x >= 80 && x < 160);
            ++changed;
        }
    }
    assert(changed);
    advance(1200);
    assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_HOME);
    ryz_host_display_save(directory, "sdl-home");
    test_home_metrics(directory);
    test_apps_swipes(directory);
    for (int n = 1; n <= 3; ++n) {
        SDL_Event key = {.type = SDL_KEYDOWN}; key.key.keysym.sym = SDLK_0 + n;
        require(SDL_PushEvent(&key) == 1, "push scale"); drain();
        click(120, 220); assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_APPS);
        click(200, 220); assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_SETTINGS);
        click(40, 220); assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_HOME);
        verify_texture();
    }
    click(120, 220); ryz_host_display_save(directory, "sdl-apps");
    click(70, 54); assert(label(lv_screen_active(), "sample_clock.lua"));
    assert(label(lv_screen_active(), "tool_monitor.lua"));
    ryz_host_display_save(directory, "sdl-files");
    SDL_Event list_hold = {.type = SDL_MOUSEBUTTONDOWN};
    list_hold.button.button = SDL_BUTTON_LEFT;
    list_hold.button.x = 70 * scale; list_hold.button.y = 70 * scale;
    require(SDL_PushEvent(&list_hold) == 1,"push list hold"); drain(); advance(3100);
    assert(label(lv_screen_active(),"LUA FILES") && !label(lv_screen_active(),"APP INFO") && blocked_actions==0);
    list_hold.type=SDL_MOUSEBUTTONUP;
    require(SDL_PushEvent(&list_hold) == 1,"release list hold"); drain(); advance(20);
    assert(label(lv_screen_active(),"LUA FILES") && blocked_actions==0);
    click(70, 70); assert(label(lv_screen_active(), "DEMO ONLY. No script is executed."));
    ryz_host_display_save(directory, "sdl-detail");
    SDL_Event hold = {.type = SDL_MOUSEBUTTONDOWN};
    hold.button.button = SDL_BUTTON_LEFT; hold.button.x = 150 * scale; hold.button.y = 220 * scale;
    require(SDL_PushEvent(&hold) == 1, "push run hold"); drain(); advance(600);
    assert(blocked_actions == 0);
    ryz_host_display_save(directory, "sdl-hold");
    advance(2400); assert(blocked_actions == 1);
    hold.type = SDL_MOUSEBUTTONUP; require(SDL_PushEvent(&hold) == 1, "push run release");
    drain(); advance(100); assert(blocked_actions == 1);
    page(RYZ_SYSTEM_UI_ROUTE_HOME);
    SDL_Event down = {.type = SDL_MOUSEBUTTONDOWN};
    down.button.button = SDL_BUTTON_LEFT; down.button.x = 120 * scale; down.button.y = 220 * scale;
    require(SDL_PushEvent(&down) == 1, "push capture"); drain();
    SDL_Event lost = {.type = SDL_WINDOWEVENT}; lost.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    require(SDL_PushEvent(&lost) == 1, "push focus loss");
    down.type = SDL_MOUSEBUTTONUP; require(SDL_PushEvent(&down) == 1, "push stale up");
    drain(); advance(20); assert(ryz_system_ui_current_route() == RYZ_SYSTEM_UI_ROUTE_HOME);
    for (int route = 0; route < RYZ_SYSTEM_UI_ROUTE_COUNT; ++route) {
        page((ryz_system_ui_route_t)route); advance(20);
        /* With no real/demo BLE service, the controller truthfully resolves
         * nonmodal overview/pair routes to OFF. Direct view tests separately
         * cover each positive BLE state using explicit backend fixtures. */
        const bool ble_landing=route>=RYZ_SYSTEM_UI_ROUTE_BLE_PAIRED &&
            route<=RYZ_SYSTEM_UI_ROUTE_BLE_SUCCESS && route!=RYZ_SYSTEM_UI_ROUTE_BLE_REPLACE;
        assert(ryz_system_ui_current_route() == (ble_landing?RYZ_SYSTEM_UI_ROUTE_BLE_OFF:
            (ryz_system_ui_route_t)route));
        char name[32]; snprintf(name, sizeof(name), "sdl-route-%02d", route);
        ryz_host_display_save(directory, name);
    }
    assert(blocked_actions == 1);
    test_v5_detail_swipes(directory);
    page(RYZ_SYSTEM_UI_ROUTE_SETTINGS); click(100,148); advance(20);
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_DISPLAY);
    ryz_host_display_save(directory,"sdl-display-default-240");
    const unsigned previews_before=display_previews;
    const unsigned saves_before=display_requests;
    SDL_Event dim_down={.type=SDL_MOUSEBUTTONDOWN}; dim_down.button.button=SDL_BUTTON_LEFT;
    dim_down.button.x=16*scale; dim_down.button.y=171*scale;
    require(SDL_PushEvent(&dim_down)==1,"brightness preview DOWN"); drain(); advance(20);
    assert(display_preview_brightness==10 && display_previews==previews_before+1);
    SDL_Event dim_move={.type=SDL_MOUSEMOTION}; dim_move.motion.state=SDL_BUTTON_LMASK;
    dim_move.motion.x=224*scale; dim_move.motion.y=171*scale;
    require(SDL_PushEvent(&dim_move)==1,"brightness preview MOVE"); drain(); advance(20);
    assert(display_preview_brightness==100 && display_previews==previews_before+2);
    assert(display_requests==saves_before && display_demo.confirmed.brightness==50);
    ryz_host_display_save(directory,"sdl-display-preview-100-240");
    SDL_Event dim_up={.type=SDL_MOUSEBUTTONUP}; dim_up.button.button=SDL_BUTTON_LEFT;
    dim_up.button.x=224*scale; dim_up.button.y=171*scale;
    require(SDL_PushEvent(&dim_up)==1,"brightness preview UP"); drain(); advance(20);
    click(20,16); click(120,158); assert(display_preview_brightness==100); /* KEEP. */
    click(20,16); click(120,202);
    assert(display_preview_brightness==50 && ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_SETTINGS);
    click(100,148); advance(20);
    click(90,75); click(110,122); click(204,171);
    assert(label(lv_screen_active(),"5 MIN") && label(lv_screen_active(),"90%"));
    ryz_host_display_save(directory,"sdl-display-draft-240");
    click(120,216);
    assert(display_requests==1 && display_requested.rotation==1 &&
           display_requested.sleep_index==3 && display_requested.brightness==90);
    assert(label(lv_screen_active(),"SAVE FAILED / RETRY") && !label(lv_screen_active(),"SAVED"));
    assert(display_preview_brightness==90); /* Demo rejection retains preview, never pretends persistence. */
    ryz_host_display_save(directory,"sdl-display-retry-240");
    click(20,16); assert(label(lv_screen_active(),"DISCARD CHANGES?"));
    ryz_host_display_save(directory,"sdl-display-discard-240");
    click(120,158); assert(!label(lv_screen_active(),"DISCARD CHANGES?"));
    click(20,16); click(120,202);
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_SETTINGS);
    assert(display_preview_brightness==50);
    test_monitor(directory);
    running=true;
    ble_security_demo(0); show(); verify_texture();
    assert(label(lv_screen_active(),"042719"));
    ryz_host_display_save(directory,"sdl-ble-verify-240");
    ble_security_demo(1); assert(label(lv_screen_active(),"SAVING..."));
    ryz_host_display_save(directory,"sdl-ble-committing-240");
    ble_security_demo(2); assert(label(lv_screen_active(),"STOPPING..."));
    ryz_host_display_save(directory,"sdl-ble-stopping-240");
    printf("PASS SDL: boot-only rail, 1x/2x/3x exact RGB565 and mouse navigation, Apps list/detail/hold, blocked run, focus-loss cancellation, %d routes\n",
           RYZ_SYSTEM_UI_ROUTE_COUNT);
}

int main(int argc, char **argv)
{
    const bool factory_test = argc == 3 && !strcmp(argv[1], "--factory-self-test");
    const bool test = argc == 3 && (!strcmp(argv[1], "--self-test") || factory_test);
    const bool monitor = argc == 2 && !strcmp(argv[1], "--monitor");
    const bool i2c = argc == 2 && !strcmp(argv[1], "--i2c");
    const bool hardware = argc == 2 && !strcmp(argv[1], "--device-test");
    const bool ble = argc == 2 && !strcmp(argv[1], "--ble-verify");
    if (argc != 1 && !test && !monitor && !i2c && !hardware && !ble) {
        fprintf(stderr, "Usage: %s [--monitor | --i2c | --device-test | --ble-verify | --self-test DIR | --factory-self-test DIR]\n", argv[0]);
        return EXIT_FAILURE;
    }
    require(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) == 0, "SDL init");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    window = SDL_CreateWindow("RyzoBee DEMO", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              240 * scale, 240 * scale, test ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN);
    require(window != NULL, "window");
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    require(renderer != NULL, "renderer");
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, 240, 240);
    require(texture != NULL, "RGB565 texture");
    ryz_host_display_reset(); fixtures(); start_boot(); show();
    test_mode = test;
    puts("DEMO ONLY: sample values; no hardware, networking or filesystem writes.\n"
         "HOME: fixed WLAN trend; tap WLAN for Wi-Fi. N toggles the SIMULATED network state.\n"
         "M / I / T or APPS: real factory Monitor / I2C / Device Test Lua.\n"
         "SIMULATED bus, logs, IMU and RGB only; no physical tests. Battery unavailable. Esc or hold B 5s: Home.");
    if (factory_test) test_factory_apps(argv[2]);
    else if (test) self_test(argv[2]);
    else {
        factory_requested = monitor ? RYZ_SIM_FACTORY_MONITOR : i2c ? RYZ_SIM_FACTORY_I2C : hardware ? RYZ_SIM_FACTORY_HARDWARE : RYZ_SIM_FACTORY_NONE;
        if(ble) ble_security_demo(0);
        last_ticks = SDL_GetTicks64();
        while (running) {
            drain();
            uint64_t now = SDL_GetTicks64();
            advance((uint32_t)(now - last_ticks)); last_ticks = now;
            run_factory();
            SDL_Delay(10);
        }
    }
    checked(ryz_system_ui_reset_checked());
    if (booting) checked(ryz_v5_boot_end());
    checked(ryz_v5_release());
    SDL_DestroyTexture(texture); SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit();
    return EXIT_SUCCESS;
}
