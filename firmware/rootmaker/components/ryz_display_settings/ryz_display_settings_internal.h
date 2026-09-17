#pragma once
#include "ryz_display_settings.h"

/* Persistence worker boundary, also used by host fault tests. Read returns
 * NOT_FOUND for absent data, INVALID_ARG for malformed tuples. Write must
 * report whether a fresh read confirms the complete tuple after commit. */
typedef struct {
    esp_err_t (*read)(ryz_display_settings_value_t *value);
    esp_err_t (*write)(ryz_display_settings_value_t value, bool *confirmed);
} ryz_display_settings_storage_t;
esp_err_t ryz_display_settings_core_init(const ryz_display_settings_storage_t *storage);
void ryz_display_settings_worker_tick(void);
