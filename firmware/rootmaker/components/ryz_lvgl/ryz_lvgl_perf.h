#pragma once

#include "esp_err.h"
#include "lvgl.h"

/* Internal, single-display adapter. Bridge holds the UI-owner call guard. */
esp_err_t ryz_lvgl_perf_init(lv_display_t *display);
bool ryz_lvgl_perf_read(lv_display_t *display, uint16_t *fps);
