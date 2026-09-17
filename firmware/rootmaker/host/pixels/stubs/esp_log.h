#pragma once
#include <stdio.h>
typedef enum { ESP_LOG_NONE=0, ESP_LOG_ERROR=1, ESP_LOG_INFO=3 } esp_log_level_t;
void esp_log_level_set(const char *tag,esp_log_level_t value);
esp_log_level_t esp_log_level_get(const char *tag);
#define ESP_LOGI(tag, format, ...) do { (void)(tag); } while(0)
#define ESP_LOGE(tag, format, ...) \
    fprintf(stderr, "%s: " format "\n", tag, ##__VA_ARGS__)
