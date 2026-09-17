#pragma once
#include "ryz_system_ui.h"

esp_err_t ryz_v5_render(ryz_system_ui_route_t route,
                       const ryz_system_ui_snapshot_t *snapshot,
                       bool (*cancelled)(void *), void *context,
                       bool *cancelled_out);
esp_err_t ryz_v5_release(void);
bool ryz_v5_needs_cleanup(void);
