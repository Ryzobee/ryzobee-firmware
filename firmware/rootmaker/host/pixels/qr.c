/* Same managed QR encoder and production adapter; only log level is an OS
 * stub here. Neither fixture passwords nor payloads are printed. */
#include "esp_log.h"
static esp_log_level_t level=ESP_LOG_NONE;
void esp_log_level_set(const char *tag,esp_log_level_t value) { (void)tag; level=value; }
esp_log_level_t esp_log_level_get(const char *tag) { (void)tag; return level; }
