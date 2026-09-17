#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef bool (*ryz_qr_module_reader_t)(const void *matrix, int x, int y);

typedef esp_err_t (*ryz_qr_matrix_consumer_t)(
    void *context,
    int side_length,
    ryz_qr_module_reader_t read_module,
    const void *matrix);

/* Internal adapter seam. Production uses the managed Espressif qrcode
 * component; host tests supply a deterministic matrix through this contract. */
esp_err_t ryz_qr_encode_text(const char *text,
                             ryz_qr_matrix_consumer_t consume,
                             void *context);
