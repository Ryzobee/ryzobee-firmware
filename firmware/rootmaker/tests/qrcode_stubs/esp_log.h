#pragma once

typedef enum {
    ESP_LOG_NONE = 0,
    ESP_LOG_ERROR,
    ESP_LOG_WARN,
    ESP_LOG_INFO,
} esp_log_level_t;

void esp_log_level_set(const char *tag, esp_log_level_t level);
esp_log_level_t esp_log_level_get(const char *tag);
