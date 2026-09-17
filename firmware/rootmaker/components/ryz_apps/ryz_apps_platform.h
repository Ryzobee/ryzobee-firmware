#pragma once

#include "esp_err.h"

esp_err_t ryz_apps_platform_init(void);
void ryz_apps_platform_deinit(void);
esp_err_t ryz_apps_platform_start(void (*entry)(void *));
void ryz_apps_platform_lock(void);
void ryz_apps_platform_unlock(void);
void ryz_apps_platform_wake(void);
void ryz_apps_platform_wait(void);
