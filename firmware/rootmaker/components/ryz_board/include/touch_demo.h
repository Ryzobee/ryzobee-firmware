#pragma once

#include "touch.h"

/* Opt-in native bring-up UI, not a persistent Lua callback/task. Main task only. */
esp_err_t ryz_touch_demo_enable(bool enable);
bool ryz_touch_demo_enabled(void);
esp_err_t ryz_touch_demo_update(const ryz_touch_sample_t *sample, esp_err_t read_status);
