#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "qrcode.h"
#include "ryz_qr_encoder.h"

static esp_log_level_t s_qrcode_log_level = ESP_LOG_INFO;
static int s_generate_count;
static int s_consume_count;
static const uint8_t s_matrix[] = {0};

void esp_log_level_set(const char *tag, esp_log_level_t level)
{
    assert(strcmp(tag, "QRCODE") == 0);
    s_qrcode_log_level = level;
}

esp_log_level_t esp_log_level_get(const char *tag)
{
    assert(strcmp(tag, "QRCODE") == 0);
    return s_qrcode_log_level;
}

esp_err_t esp_qrcode_generate(esp_qrcode_config_t *config, const char *text)
{
    assert(config != NULL);
    assert(strcmp(text, "WIFI:T:WPA;S:RYZOBEE-ABCD;P:LOCAL-SECRET;;") == 0);
    assert(s_qrcode_log_level == ESP_LOG_NONE);
    assert(config->max_qrcode_version == 10);
    assert(config->qrcode_ecc_level == ESP_QRCODE_ECC_MED);
    assert(config->user_data != NULL);
    ++s_generate_count;
    config->display_func_with_cb(s_matrix, config->user_data);
    return ESP_OK;
}

int esp_qrcode_get_size(esp_qrcode_handle_t qrcode)
{
    assert(qrcode == s_matrix);
    return 21;
}

bool esp_qrcode_get_module(esp_qrcode_handle_t qrcode, int x, int y)
{
    assert(qrcode == s_matrix);
    return x == y;
}

static esp_err_t consume_matrix(void *context, int side_length,
                                ryz_qr_module_reader_t read_module,
                                const void *matrix)
{
    assert(context == NULL);
    assert(side_length == 21);
    assert(read_module(matrix, 0, 0));
    assert(!read_module(matrix, 1, 0));
    ++s_consume_count;
    return ESP_OK;
}

int main(void)
{
    assert(ryz_qr_encode_text(
               "WIFI:T:WPA;S:RYZOBEE-ABCD;P:LOCAL-SECRET;;",
               consume_matrix, NULL) == ESP_OK);
    assert(s_generate_count == 1);
    assert(s_consume_count == 1);
    assert(s_qrcode_log_level == ESP_LOG_NONE);
    return 0;
}
