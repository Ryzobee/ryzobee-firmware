#include "touch_demo.h"
#include "display.h"

#include <stdio.h>
#include <string.h>

_Static_assert(RYZ_TOUCH_WIDTH == RYZ_DISPLAY_WIDTH && RYZ_TOUCH_HEIGHT == RYZ_DISPLAY_HEIGHT,
               "Touch and display need matching coordinates");

static bool enabled;
static bool painted;
static bool previous_pressed;
static bool previous_error;
static bool have_position;
static uint16_t last_x, last_y;
static ryz_touch_event_t previous_event;

#define DRAW(call) do { esp_err_t err = (call); if (err != ESP_OK) return err; } while (0)

static esp_err_t paint(bool pressed, const char *event)
{
    DRAW(ryz_display_clear(0x0841));
    DRAW(ryz_display_rect(0, 0, 240, 3, 0x07ff));
    DRAW(ryz_display_text(22, 12, "TOUCH READY", 11, 0x07ff, 3));
    DRAW(ryz_display_text(22, 43, "TAP / DRAG / LIFT", 17, 0xffff, 2));
    DRAW(ryz_display_rect(8, 72, 224, 1, 0x4208));
    DRAW(ryz_display_rect(8, 167, 224, 1, 0x4208));
    DRAW(ryz_display_rect(8, 72, 1, 96, 0x4208));
    DRAW(ryz_display_rect(231, 72, 1, 96, 0x4208));
    ryz_touch_info_t info;
    ryz_touch_get_info(&info);
    const char *controller = ryz_touch_controller_name(info.chip_id);
    if (controller) DRAW(ryz_display_text(78, 110, controller, strlen(controller), 0x4208, 2));
    char coordinates[24];
    if (have_position) snprintf(coordinates, sizeof(coordinates), "X:%03u Y:%03u", last_x, last_y);
    else snprintf(coordinates, sizeof(coordinates), "X:--- Y:---");
    DRAW(ryz_display_text(10, 181, coordinates, strlen(coordinates), 0xffff, 2));
    DRAW(ryz_display_text(10, 208, event, strlen(event), pressed ? 0x07e0 : 0xffe0, 2));
    if (have_position) {
        int x = last_x < 4 ? 0 : (last_x > 235 ? 231 : last_x - 4);
        int y = last_y < 4 ? 0 : (last_y > 235 ? 231 : last_y - 4);
        DRAW(ryz_display_rect(x, y, 9, 9, pressed ? 0x07e0 : 0xffe0));
    }
    return ryz_display_show();
}

esp_err_t ryz_touch_demo_enable(bool enable)
{
    enabled = false;
    if (!enable) return ESP_OK; /* Leave existing pixels untouched. */
    ryz_touch_info_t info;
    ryz_touch_get_info(&info);
    if (!info.ready) return ESP_ERR_INVALID_STATE;
    painted = previous_pressed = previous_error = have_position = false;
    previous_event = RYZ_TOUCH_NONE;
    esp_err_t err = paint(false, "WAIT");
    enabled = err == ESP_OK;
    return err;
}

bool ryz_touch_demo_enabled(void)
{
    return enabled;
}

esp_err_t ryz_touch_demo_update(const ryz_touch_sample_t *sample, esp_err_t read_status)
{
    if (!enabled) return ESP_OK;
    bool error = read_status != ESP_OK;
    if (!error && !sample) return ESP_ERR_INVALID_ARG;
    bool pressed = !error && sample->pressed;
    ryz_touch_event_t event = error ? RYZ_TOUCH_NONE : sample->event;
    bool changed = !painted || error != previous_error || pressed != previous_pressed;
    if (pressed) {
        changed |= !have_position || sample->x != last_x || sample->y != last_y || event != previous_event;
        last_x = sample->x;
        last_y = sample->y;
        have_position = true;
    }
    if (!changed) return ESP_OK;
    const char *label = error ? "I2C ERROR" :
        pressed ? (event == RYZ_TOUCH_DOWN ? "DOWN" : "MOVE") : have_position ? "UP" : "WAIT";
    esp_err_t err = paint(pressed, label);
    if (err != ESP_OK) { enabled = false; return err; }
    painted = true;
    previous_error = error;
    previous_pressed = pressed;
    previous_event = event;
    return ESP_OK;
}
