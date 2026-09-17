#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef void *rmt_channel_handle_t;
typedef void *rmt_encoder_handle_t;
typedef union {
    struct { uint32_t duration0:15, level0:1, duration1:15, level1:1; };
    uint32_t val;
} rmt_symbol_word_t;
enum { RMT_CLK_SRC_APB = 1 };
typedef struct {
    int gpio_num, clk_src;
    uint32_t resolution_hz;
    size_t mem_block_symbols, trans_queue_depth;
} rmt_tx_channel_config_t;
typedef struct {} rmt_copy_encoder_config_t;
typedef struct {
    int loop_count;
    struct { unsigned eot_level:1, queue_nonblocking:1; } flags;
} rmt_transmit_config_t;
esp_err_t rmt_new_tx_channel(const rmt_tx_channel_config_t *, rmt_channel_handle_t *);
esp_err_t rmt_new_copy_encoder(const rmt_copy_encoder_config_t *, rmt_encoder_handle_t *);
esp_err_t rmt_del_encoder(rmt_encoder_handle_t);
esp_err_t rmt_del_channel(rmt_channel_handle_t);
esp_err_t rmt_enable(rmt_channel_handle_t);
esp_err_t rmt_disable(rmt_channel_handle_t);
esp_err_t rmt_transmit(rmt_channel_handle_t, rmt_encoder_handle_t, const void *, size_t,
                       const rmt_transmit_config_t *);
esp_err_t rmt_tx_wait_all_done(rmt_channel_handle_t, int);
