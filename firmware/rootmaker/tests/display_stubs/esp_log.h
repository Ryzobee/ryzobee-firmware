#pragma once

void display_test_log_error(const char *tag, const char *format, ...);

#define ESP_LOGE(tag, format, ...) \
    display_test_log_error((tag), (format), ##__VA_ARGS__)
