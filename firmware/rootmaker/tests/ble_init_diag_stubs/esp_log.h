#pragma once

void diag_test_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
#define ESP_LOGE(tag, ...) diag_test_log(__VA_ARGS__)
