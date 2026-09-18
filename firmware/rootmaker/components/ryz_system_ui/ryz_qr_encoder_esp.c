#include "ryz_qr_encoder.h"

#include <stddef.h>
#include <stdint.h>

#include "esp_log.h"
#include "qrcode.h"

enum {
    RYZ_QR_MAX_VERSION = 10,
};

typedef struct {
    ryz_qr_matrix_consumer_t consume;
    void *context;
    esp_err_t result;
    bool called;
} encoder_context_t;

static bool read_esp_module(const void *matrix, int x, int y)
{
    return esp_qrcode_get_module((esp_qrcode_handle_t)matrix, x, y);
}

static void consume_esp_qrcode(esp_qrcode_handle_t qrcode, void *user_data)
{
    encoder_context_t *context = user_data;
    context->called = true;
    context->result = context->consume(
        context->context, esp_qrcode_get_size(qrcode), read_esp_module,
        qrcode);
}

esp_err_t ryz_qr_encode_text(const char *text,
                             ryz_qr_matrix_consumer_t consume,
                             void *context)
{
    if (text == NULL || consume == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    encoder_context_t encoder = {
        .consume = consume,
        .context = context,
        .result = ESP_FAIL,
        .called = false,
    };
    esp_qrcode_config_t config = ESP_QRCODE_CONFIG_DEFAULT();
    config.display_func_with_cb = consume_esp_qrcode;
    config.max_qrcode_version = RYZ_QR_MAX_VERSION;
    config.qrcode_ecc_level = ESP_QRCODE_ECC_MED;
    config.user_data = &encoder;

    /* Espressif qrcode 0.2.0 logs the complete input at INFO. Suppress its tag
     * immediately before every encoding and never restore it: text contains
     * the AP password. This project requires dynamic log-level control. */
#if !CONFIG_LOG_DYNAMIC_LEVEL_CONTROL
#error "ryz_system_ui requires CONFIG_LOG_DYNAMIC_LEVEL_CONTROL to protect QR credentials"
#endif
    esp_log_level_set("QRCODE", ESP_LOG_NONE);
    if (esp_log_level_get("QRCODE") != ESP_LOG_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t error = esp_qrcode_generate(&config, text);
    if (error != ESP_OK) {
        return error;
    }
    return encoder.called ? encoder.result : ESP_FAIL;
}
