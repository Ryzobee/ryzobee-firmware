#include "ryz_v5_boot.h"
#include "ryz_lvgl_system.h"
#include "ryz_v5_widgets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Figma 155:3413. The whole static artwork, including outlined typography,
 * is one indexed image. Only 155:3426 animates; live network text replaces
 * the static footer on demand, using audited glyphs, no second framebuffer. */
extern const lv_image_dsc_t ryz_v5_boot_asset;
static lv_obj_t *root, *segment, *status, *status_panel;
static const char *status_text;
static uint32_t status_color;
static TaskHandle_t owner;
static bool timed_out;
#define BOOT_LEG_MS 800U

static esp_err_t present_changes(void)
{
    /* Boot ticks are 20 ms, while the shared display defaults to 33 ms.
     * Make its existing timer due, without changing the global period or
     * invalidating the whole screen. Only the dirty rail/footer is flushed. */
    lv_timer_ready(lv_display_get_refr_timer(lv_obj_get_display(root)));
    return ryz_lvgl_system_pump(NULL, NULL, NULL);
}

esp_err_t ryz_v5_boot_begin(void)
{
    if (root) return ESP_ERR_INVALID_STATE;
    lv_obj_t *candidate = NULL;
    esp_err_t error = ryz_lvgl_system_create(&candidate);
    if (error != ESP_OK) return error;
    ryz_v5_widgets_begin();
    lv_obj_t *art = lv_image_create(candidate);
    if (!art) {
        (void)ryz_lvgl_system_release();
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(art);
    lv_obj_remove_flag(art, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(art, 24, 12);
    lv_image_set_src(art, &ryz_v5_boot_asset);
    lv_obj_t *rail = ryz_v5_box(candidate, 80, 234, 80, 2, V5_BORDER, 0, 0);
    lv_obj_t *travel = ryz_v5_box(rail, 30, 0, 20, 2, V5_ORANGE, 0, 0);
    /* Construct at first-frame time. Late lv_obj_create briefly invalidates
     * its default origin/size before positioning, needlessly flushing artwork. */
    lv_obj_t *panel = ryz_v5_box(candidate, 12, 212, 216, 18, V5_BLACK, 0, 0);
    lv_obj_t *caption = ryz_v5_label(panel, 0, 0, 216, 18, "STARTING",
        RYZ_FONT_BODY_MEDIUM, 12, V5_MUTED, LV_TEXT_ALIGN_CENTER);
    if (panel) lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    error = ryz_v5_widgets_status();
    if (error == ESP_OK)
        error = ryz_lvgl_system_commit(candidate, NULL, NULL, NULL);
    if (error != ESP_OK) {
        (void)ryz_lvgl_system_release();
        return error;
    }
    root = candidate;
    segment = travel;
    status = caption;
    status_panel = panel;
    owner = xTaskGetCurrentTaskHandle();
    timed_out = false;
    return ESP_OK;
}

esp_err_t ryz_v5_boot_tick(uint32_t elapsed_ms)
{
    if (!root || owner != xTaskGetCurrentTaskHandle()) return ESP_ERR_INVALID_STATE;
    if (!timed_out) {
        /* Begin at the Figma center, then right -> left -> center. Modulo
         * before offset avoids overflow on long waits or uint32 rollover. */
        unsigned phase = (elapsed_ms % (2U * BOOT_LEG_MS) + BOOT_LEG_MS / 2U) % (2U * BOOT_LEG_MS);
        unsigned distance = phase <= BOOT_LEG_MS ? phase : 2U * BOOT_LEG_MS - phase;
        lv_obj_set_x(segment, (int32_t)((distance * 60U) / BOOT_LEG_MS));
    }
    return present_changes();
}

static esp_err_t set_status(const char *text, uint32_t color)
{
    if (status_text == text && status_color == color) return ESP_OK;
    /* All messages are static literals: no allocation on status updates. */
    lv_label_set_text_static(status, text);
    lv_obj_set_style_text_color(status, lv_color_hex(color), 0);
    lv_obj_remove_flag(status_panel, LV_OBJ_FLAG_HIDDEN);
    status_text = text;
    status_color = color;
    return present_changes();
}

esp_err_t ryz_v5_boot_network(const ryz_provisioning_snapshot_t *network)
{
    if (!root || owner != xTaskGetCurrentTaskHandle()) return ESP_ERR_INVALID_STATE;
    if (timed_out) return ESP_OK;
    const char *text = "WIFI UNAVAILABLE";
    uint32_t color = V5_WARNING;
    if (network) {
        switch (network->boot_phase) {
        case RYZ_PROVISIONING_BOOT_OFF:
            text = "WIFI OFF"; color = V5_MUTED; break;
        case RYZ_PROVISIONING_BOOT_SCANNING:
            text = "WIFI SCANNING"; color = V5_MUTED; break;
        case RYZ_PROVISIONING_BOOT_NOT_FOUND:
            text = network->state == RYZ_PROVISIONING_AP_READY && network->portal_active && network->portal_ip[0] ?
                "SSID NOT FOUND / AP" : "SSID NOT FOUND / SKIP";
            break;
        case RYZ_PROVISIONING_BOOT_SCAN_FAILED:
            text = "WIFI SCAN FAILED"; color = V5_RED; break;
        case RYZ_PROVISIONING_BOOT_CONNECTING:
            text = "WIFI CONNECTING"; color = V5_MUTED; break;
        case RYZ_PROVISIONING_BOOT_CONNECTED:
            if (network->state == RYZ_PROVISIONING_ONLINE && network->ipv4[0]) {
                text = "WIFI CONNECTED"; color = V5_GREEN;
            } else if (network->state == RYZ_PROVISIONING_CONNECTING) {
                text = "WIFI CONNECTING"; color = V5_MUTED;
            } else { text = "WIFI CONNECT FAILED"; color = V5_RED; }
            break;
        case RYZ_PROVISIONING_BOOT_CONNECT_FAILED:
            text = "WIFI CONNECT FAILED"; color = V5_RED; break;
        case RYZ_PROVISIONING_BOOT_NO_CREDENTIALS:
            if (network->state == RYZ_PROVISIONING_AP_READY && network->portal_active && network->portal_ip[0]) {
                text = "WIFI AP READY"; color = V5_MUTED;
            } else if (network->state == RYZ_PROVISIONING_FAILED) {
                text = "WIFI AP FAILED"; color = V5_RED;
            } else { text = "WIFI STARTING AP"; color = V5_MUTED; }
            break;
        default:
            if (network->state != RYZ_PROVISIONING_FAILED) {
                text = "WIFI STARTING"; color = V5_MUTED;
            }
            break;
        }
    }
    return set_status(text, color);
}

esp_err_t ryz_v5_boot_timeout(void)
{
    if (!root || owner != xTaskGetCurrentTaskHandle()) return ESP_ERR_INVALID_STATE;
    if (!timed_out) {
        timed_out = true;
        return set_status("START TIMEOUT", V5_WARNING);
    }
    return present_changes();
}

esp_err_t ryz_v5_boot_end(void)
{
    if (!root) return ESP_OK;
    if (owner != xTaskGetCurrentTaskHandle()) return ESP_ERR_INVALID_STATE;
    esp_err_t error = ryz_lvgl_system_release();
    if (error == ESP_OK) {
        root = segment = status = status_panel = NULL;
        status_text = NULL;
        status_color = 0;
        owner = NULL;
        timed_out = false;
        ryz_v5_widgets_release();
    }
    return error;
}
