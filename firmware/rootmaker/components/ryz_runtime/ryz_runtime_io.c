#include "ryz_runtime_io.h"

#include <string.h>

#include "display.h"
#include "ryz_lvgl.h"
#include "touch_demo.h"

bool ryz_runtime_io_is_cleanup(const ryz_runtime_io_request_t *request)
{
    return request &&
        (request->kind == RYZ_IO_ABORT || request->kind == RYZ_IO_UI_CLOSE);
}

void ryz_runtime_io_execute(
    ryz_runtime_io_request_t *request,
    esp_err_t (*read_touch)(void *context, ryz_touch_sample_t *sample),
    void *input_context)
{
    if (!request) return;
    request->status = ESP_ERR_INVALID_ARG;
    request->done = false;
    if (!ryz_runtime_io_is_cleanup(request) && request->cancelled &&
        request->cancelled(request->cancel_context)) {
        request->status = ESP_ERR_TIMEOUT;
        return;
    }

    bool drawing = false;
    switch (request->kind) {
    case RYZ_IO_CLEAR:
        request->status = ryz_display_clear(request->color);
        drawing = true;
        break;
    case RYZ_IO_READ_PIXEL:
        request->status = ryz_display_read_pixel(
            request->x, request->y, &request->color);
        break;
    case RYZ_IO_RECT:
        request->status = ryz_display_rect(
            request->x, request->y, request->width, request->height,
            request->color);
        drawing = true;
        break;
    case RYZ_IO_TEXT:
        request->status = ryz_display_text(
            request->x, request->y, request->text, request->text_length,
            request->color, request->scale);
        drawing = true;
        break;
    case RYZ_IO_SHOW:
        request->status = ryz_display_show_checked(
            request->cancelled, request->cancel_context);
        drawing = true;
        break;
    case RYZ_IO_NEURO_PAINT:
        if (request->begin) {
            request->status = ryz_touch_demo_enable(false);
            if (request->status != ESP_OK) break;
            request->status = ryz_display_clear(request->color);
            if (request->status != ESP_OK) break;
        }
        request->status = ryz_display_show_step(request->begin, &request->done);
        break;
    case RYZ_IO_ABORT:
        ryz_display_show_abort();
        request->status = ESP_OK;
        break;
    case RYZ_IO_TOUCH_INFO:
        memset(&request->touch_info, 0, sizeof(request->touch_info));
        ryz_touch_get_info(&request->touch_info);
        request->demo_enabled = ryz_touch_demo_enabled();
        request->status = ESP_OK;
        break;
    case RYZ_IO_TOUCH_READ:
        memset(&request->touch, 0, sizeof(request->touch));
        request->status = read_touch ?
            read_touch(input_context, &request->touch) :
            ryz_touch_read(&request->touch);
        if (request->status != ESP_OK) {
            memset(&request->touch, 0, sizeof(request->touch));
        }
        break;
    case RYZ_IO_TOUCH_DEMO:
        request->status = ryz_touch_demo_enable(request->enabled);
        break;
    case RYZ_IO_UI_MOUNT:
        request->status = ryz_touch_demo_enable(false);
        if (request->status == ESP_OK) {
            request->status = ryz_lvgl_mount_checked(
                request->scene, request->cancelled, request->cancel_context);
        }
        break;
    case RYZ_IO_UI_PUMP:
        request->status = ryz_lvgl_pump_checked(
            request->cancelled, request->cancel_context);
        break;
    case RYZ_IO_UI_UPDATE:
        request->status = ryz_lvgl_update_checked(
            request->scene, request->cancelled, request->cancel_context);
        break;
    case RYZ_IO_UI_CLOSE:
        ryz_lvgl_release();
        request->status = ESP_OK;
        break;
    default:
        break;
    }
    if (drawing && request->status == ESP_OK) {
        request->status = ryz_touch_demo_enable(false);
    }
}
