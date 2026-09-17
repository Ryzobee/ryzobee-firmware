#include "ryz_rgb_driver.h"

#include "driver/rmt_tx.h"
#include "esp_attr.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
/* Pinned ESP-IDF 5.5.4 diagnostic: refuse a rounded/shared clock rather than
 * silently changing the WS2812 timing. Keep this dependency inside the driver. */
#include "esp_private/rmt.h"
#include "soc/soc_caps.h"

#if !CONFIG_IDF_TARGET_ESP32S3 || ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(5, 5, 4)
#error "RGB cleanup requires the audited ESP32-S3 / ESP-IDF 5.5.4"
#endif
#if CONFIG_PM_ENABLE || CONFIG_FREERTOS_SMP || CONFIG_FREERTOS_UNICORE
#error "RGB cleanup requires the audited dual-core, non-SMP, non-PM configuration"
#endif
_Static_assert(SOC_RMT_SUPPORT_ASYNC_STOP, "Bounded recovery requires async stop");
_Static_assert(SOC_RMT_MEM_WORDS_PER_CHANNEL >= 27, "Entire frame must fit in RMT RAM");

enum { RGB_PIN = 45, CLOCK_HZ = 80000000, WAIT_MS = 20, FRAME_SYMBOLS = 26 };
static rmt_channel_handle_t s_channel;
static rmt_encoder_handle_t s_encoder;
static bool s_enabled, s_pending;
static bool s_releasing;
static esp_err_t s_cleanup_error;
static TaskHandle_t s_owner;
/* The driver retains this exact internal-RAM payload even on timeout. The
 * copy encoder sees the entire frame + EOF in the first 48-symbol RAM fill;
 * there is no mid-frame ISR refill or user callback. */
static DRAM_ATTR rmt_symbol_word_t s_symbols[FRAME_SYMBOLS];

static bool claim_owner(void)
{
    if (xPortGetCoreID() != 1 || xTaskGetCoreID(NULL) != 1) return false;
    TaskHandle_t current = xTaskGetCurrentTaskHandle();
    if (!current || (s_owner && s_owner != current)) return false;
    s_owner = current;
    return true;
}

static esp_err_t quiesce_impl(void)
{
    if (s_enabled) {
        esp_err_t error = rmt_disable(s_channel);
        /* A failed disable (including an ISR FSM transition) is not a stop.
         * Do not modify the payload, reset the encoder or release handles. */
        if (error != ESP_OK) return error;
        s_enabled = false;
    }
    if (s_pending) {
        /* With a single, enabled-only transaction, successful S3 disable moves
         * an interrupted transaction to COMPLETE. Reclaim it before reuse.
         * This is cleanup, never evidence that the whole frame was sent. */
        esp_err_t error = rmt_tx_wait_all_done(s_channel, WAIT_MS);
        if (error != ESP_OK) return error;
        s_pending = false;
    }
    return ESP_OK;
}

static esp_err_t quiesce(void)
{
    s_cleanup_error = quiesce_impl();
    return s_cleanup_error;
}

static esp_err_t prepare(void)
{
    esp_err_t error = quiesce();
    if (error != ESP_OK) return error;
    if (!s_channel) {
        rmt_tx_channel_config_t config = {
            .gpio_num = RGB_PIN,
            .clk_src = RMT_CLK_SRC_APB,
            .resolution_hz = CLOCK_HZ,
            .mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL,
            .trans_queue_depth = 1,
        };
        error = rmt_new_tx_channel(&config, &s_channel);
        if (error != ESP_OK) return error;
    }
    uint32_t resolution = 0;
    error = rmt_get_channel_resolution(s_channel, &resolution);
    if (error != ESP_OK) return error;
    if (resolution != CLOCK_HZ) return ESP_ERR_NOT_SUPPORTED;
    if (!s_encoder) {
        const rmt_copy_encoder_config_t config = {};
        error = rmt_new_copy_encoder(&config, &s_encoder);
    }
    return error;
}

static void encode(ryz_rgb_color_t color)
{
    /* 80MHz = 12.5ns/tick. 0:350/900ns, 1:700/587.5ns. Numerically inside
     * the cited original/V5 tables, not a claim about an unverified LED lot.
     * 350us LOW before data resynchronizes an interrupted/unknown old frame;
     * the trailing 350us LOW latches this frame. Both halves are nonzero. */
    s_symbols[0] = (rmt_symbol_word_t){.duration0 = 14000, .duration1 = 14000};
    const uint32_t grb = ((uint32_t)color.green << 16) |
                         ((uint32_t)color.red << 8) | color.blue;
    for (unsigned bit = 0; bit < 24; ++bit) {
        const bool one = (grb & (1U << (23 - bit))) != 0;
        s_symbols[bit + 1] = (rmt_symbol_word_t){
            .level0 = 1, .duration0 = one ? 56 : 28,
            .level1 = 0, .duration1 = one ? 47 : 72,
        };
    }
    s_symbols[25] = s_symbols[0];
}

esp_err_t ryz_rgb_driver_write(ryz_rgb_color_t color)
{
    if (!claim_owner()) return ESP_ERR_INVALID_STATE;
    /* A failed destroy can already revoke GPIO while retaining the handle.
     * Finish that teardown before any new allocation or transmission. */
    if (s_releasing) {
        esp_err_t cleanup = ryz_rgb_driver_cleanup();
        if (cleanup != ESP_OK) return cleanup;
    }
    esp_err_t error = prepare();
    if (error != ESP_OK) return error;
    encode(color);
    error = rmt_enable(s_channel);
    if (error != ESP_OK) return error;
    s_enabled = true;
    const rmt_transmit_config_t config = {
        .loop_count = 0,
        .flags = {.eot_level = 0, .queue_nonblocking = 1},
    };
    /* Conservatively require cleanup even if admission returns an error. */
    s_pending = true;
    error = rmt_transmit(s_channel, s_encoder, s_symbols, sizeof(s_symbols), &config);
    if (error == ESP_OK) {
        error = rmt_tx_wait_all_done(s_channel, WAIT_MS);
        if (error == ESP_OK) s_pending = false;
    }
    const esp_err_t cleanup_error = quiesce();
    return error != ESP_OK ? error : cleanup_error;
}

esp_err_t ryz_rgb_driver_cleanup(void)
{
    if (!claim_owner()) return ESP_ERR_INVALID_STATE;
    s_releasing = true;
    esp_err_t error = quiesce();
    if (error != ESP_OK) return error;
    if (s_encoder) {
        error = rmt_del_encoder(s_encoder);
        if (error != ESP_OK) return s_cleanup_error = error;
        s_encoder = NULL;
    }
    if (s_channel) {
        /* Exact IDF/PM-off/non-DMA and same-core interrupt ownership above
         * exclude a failure after freeing an interrupt but before freeing
         * this channel. Do not reuse a partially torn-down channel. */
        error = rmt_del_channel(s_channel);
        if (error != ESP_OK) return s_cleanup_error = error;
        s_channel = NULL;
    }
    s_releasing = false;
    return s_cleanup_error = ESP_OK;
}

ryz_rgb_driver_status_t ryz_rgb_driver_get_status(void)
{
    return (ryz_rgb_driver_status_t){
        .resources_held = s_channel || s_encoder || s_enabled || s_pending,
        .cleanup_error = s_cleanup_error,
    };
}
