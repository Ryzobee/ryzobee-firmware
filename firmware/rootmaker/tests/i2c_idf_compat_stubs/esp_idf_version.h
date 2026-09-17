#pragma once
#ifndef TEST_IDF_PATCH
#define TEST_IDF_PATCH 4
#endif
#define ESP_IDF_VERSION_VAL(major, minor, patch) (((major) << 16) | ((minor) << 8) | (patch))
#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(5, 5, TEST_IDF_PATCH)
