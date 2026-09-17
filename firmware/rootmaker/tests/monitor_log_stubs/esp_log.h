#pragma once
#include <inttypes.h>
#include <stdarg.h>
/* Production config disables colors; the format itself is the actual SDK's. */
#define LOG_COLOR_W ""
#define LOG_COLOR_I ""
#define LOG_COLOR_E ""
#define LOG_RESET_COLOR ""
#include "esp_log_format.h"
typedef int (*vprintf_like_t)(const char *, va_list);
vprintf_like_t esp_log_set_vprintf(vprintf_like_t callback);
extern vprintf_like_t esp_log_vprint_func;
typedef enum {
    ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_VERBOSE,
} esp_log_level_t;
/* Existing cases drive the shared vprintf sink directly using audited V1
 * formats. Application write output is not under test in this harness. */
#define ESP_LOG_LEVEL_LOCAL(level, tag, format, ...) ((void)(level), (void)(tag))
