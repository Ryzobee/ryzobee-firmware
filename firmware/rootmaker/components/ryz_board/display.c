/* RootMaker pins and clocks follow ggadc/Ryzobee_arduino_esp32 8ec48fd. The
 * panel commands follow the user-supplied BOE WV015Z1U-N80/ST7789V gamma 2.2
 * table dated 2019-08-27 (SHA-256 5004ea2aa5fcf9287401e62c7643c5400
 * 3a20b719bde9b8079268baf10eb8742). */
#include "display.h"
#include "display_font.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LCD_SCLK 12
#define LCD_MOSI 9
#define LCD_CS 5
#define LCD_DC 3
#define LCD_RST 10
#define LCD_BL 7
#define LCD_HOST SPI2_HOST
/* User-selected high-speed mode. The Ryzobee Arduino board configuration uses
 * 40 MHz, so 80 MHz remains an on-device visual/stability trial. */
#define LCD_SPI_HZ (80 * 1000 * 1000)
#define STRIPE_LINES 16
#define STRIPE_BYTES (RYZ_DISPLAY_WIDTH * STRIPE_LINES * 2)
#define DMA_BUFFER_BYTES (STRIPE_BYTES * 2)

typedef struct {
    uint8_t command;
    uint8_t data[14];
    uint8_t data_size;
    uint16_t delay_ms;
} lcd_init_command_t;

/* Preserve the BOE table's command order and values without mixing in the
 * generic ESP-IDF or LovyanGFX analog/gamma defaults. LCD reset and backlight
 * sequencing remain board-level concerns handled around this table. */
static const lcd_init_command_t lcd_init_commands[] = {
    {0x11, {0}, 0, 120}, /* SLPOUT */
    {0x36, {0x00}, 1, 0},
    {0x3a, {0x05}, 1, 0},
    {0x21, {0}, 0, 0},   /* INVON */
    {0x2a, {0x00, 0x00, 0x00, 0xef}, 4, 0},
    {0x2b, {0x00, 0x00, 0x00, 0xef}, 4, 0},
    {0xb2, {0x0c, 0x0c, 0x00, 0x33, 0x33}, 5, 0},
    {0xb7, {0x35}, 1, 0},
    {0xbb, {0x1f}, 1, 0},
    {0xc0, {0x2c}, 1, 0},
    {0xc2, {0x01}, 1, 0},
    {0xc3, {0x12}, 1, 0},
    {0xc4, {0x20}, 1, 0},
    {0xc6, {0x0f}, 1, 0},
    {0xd0, {0xa4, 0xa1}, 2, 0},
    {0xe0, {0xd0, 0x08, 0x11, 0x08, 0x0c, 0x15, 0x39,
            0x33, 0x50, 0x36, 0x13, 0x14, 0x29, 0x2d}, 14, 0},
    {0xe1, {0xd0, 0x08, 0x10, 0x08, 0x06, 0x06, 0x39,
            0x44, 0x51, 0x0b, 0x16, 0x14, 0x2f, 0x31}, 14, 0},
    {0x29, {0}, 0, 0},   /* DISPON */
};

static const char *TAG = "ryz_display";

static esp_lcd_panel_io_handle_t panel_io;
static esp_lcd_panel_handle_t panel;
/* Store pixels in panel wire order: drawing converts each color once, while
 * flushing can use optimized bulk copies instead of swapping every pixel. */
static uint16_t *canvas;
static uint8_t *dma_stripe;
static bool ready;
static bool dirty;
static int dirty_left, dirty_top, dirty_right, dirty_bottom;
static struct { bool active; int left, right, y, bottom; unsigned stripe; } flush;
static _Atomic uint32_t completed_commits;
static bool privacy_tainted;
static bool privacy_blocked;
static bool configuration_blocked, display_asleep, backlight_uncertain;
static uint8_t confirmed_rotation;
static uint32_t confirmed_duty = 127;

bool ryz_display_configuration_blocked(void) { return configuration_blocked || backlight_uncertain; }

uint32_t ryz_display_completed_commits(void)
{
    return atomic_load_explicit(&completed_commits, memory_order_relaxed);
}

esp_err_t ryz_display_privacy_mark_sensitive(void)
{
    if (!ready || privacy_blocked || configuration_blocked || flush.active) return ESP_ERR_INVALID_STATE;
    privacy_tainted = true;
    return ESP_OK;
}

/* MADCTL owns pixel orientation; neither LVGL nor the canvas rotates pixels.
 * Keep the same mapping for bootstrap and runtime changes. */
static const uint8_t rotation_madctl[4] = {0x00, 0x60, 0xc0, 0xa0};

static bool configuration_valid(uint8_t rotation, uint8_t brightness)
{
    return rotation <= 3 && brightness >= 10 && brightness <= 100 && brightness % 5 == 0;
}

static esp_err_t set_panel_gap(uint8_t rotation)
{
    return esp_lcd_panel_set_gap(panel, rotation == 3 ? 80 : 0, rotation == 2 ? 80 : 0);
}

static esp_err_t initialize_panel_from_boe_reference(uint8_t rotation)
{
    for (size_t i = 0; i < sizeof(lcd_init_commands) / sizeof(lcd_init_commands[0]); ++i) {
        const lcd_init_command_t *entry = &lcd_init_commands[i];
        const void *data = entry->data_size ? entry->data : NULL;
        /* Only the orientation byte is user-configurable; BOE analog/gamma,
         * reset dwell and command ordering remain unchanged. */
        if (entry->command == 0x36) data = &rotation_madctl[rotation];
        esp_err_t err = esp_lcd_panel_io_tx_param(
            panel_io, entry->command, data, entry->data_size);
        if (err != ESP_OK) return err;
        if (entry->delay_ms) vTaskDelay(pdMS_TO_TICKS(entry->delay_ms));
    }
    return ESP_OK;
}

static void mark_dirty(int left, int top, int right, int bottom)
{
    if (!dirty) {
        dirty_left = left; dirty_top = top;
        dirty_right = right; dirty_bottom = bottom;
        dirty = true;
        return;
    }
    if (left < dirty_left) dirty_left = left;
    if (top < dirty_top) dirty_top = top;
    if (right > dirty_right) dirty_right = right;
    if (bottom > dirty_bottom) dirty_bottom = bottom;
}

static uint16_t wire_color(uint16_t color)
{
    /* ESP32-S3 is little-endian; the ST7789 remains in big-endian RGB565 mode. */
    return __builtin_bswap16(color);
}

esp_err_t ryz_display_init(void)
{
    return ryz_display_init_with_configuration(0, 50);
}

esp_err_t ryz_display_init_with_configuration(uint8_t rotation, uint8_t brightness)
{
    if (!configuration_valid(rotation, brightness)) return ESP_ERR_INVALID_ARG;
    /* A failed sensitive transfer retains live DMA/panel resources. Only the
     * privacy barrier may recover these; initialization must not replace them. */
    if (privacy_blocked || configuration_blocked || (privacy_tainted && !ready)) return ESP_ERR_INVALID_STATE;
    if (ready) return ESP_OK;
    esp_err_t err = ESP_OK;
    bool bus_created = false;
    bool light_created = false;
#define INIT_TRY(call) do { err = (call); if (err != ESP_OK) goto fail; } while (0)

    const gpio_config_t panel_pins = {
        .pin_bit_mask = (1ULL << LCD_BL) | (1ULL << LCD_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    INIT_TRY(gpio_config(&panel_pins));
    INIT_TRY(gpio_set_level(LCD_BL, 0));
    INIT_TRY(gpio_set_level(LCD_RST, 0));
    vTaskDelay(pdMS_TO_TICKS(8));
    canvas = heap_caps_calloc(RYZ_DISPLAY_WIDTH * RYZ_DISPLAY_HEIGHT,
                              sizeof(*canvas), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    dma_stripe = heap_caps_malloc(DMA_BUFFER_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!canvas || !dma_stripe) { err = ESP_ERR_NO_MEM; goto fail; }

    const spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCLK, .mosi_io_num = LCD_MOSI, .miso_io_num = -1,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = STRIPE_BYTES,
    };
    INIT_TRY(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));
    bus_created = true;
    const esp_lcd_panel_io_spi_config_t io = {
        .cs_gpio_num = LCD_CS, .dc_gpio_num = LCD_DC, .spi_mode = 0,
        .pclk_hz = LCD_SPI_HZ, .trans_queue_depth = 1,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
    };
    INIT_TRY(esp_lcd_new_panel_io_spi(LCD_HOST, &io, &panel_io));
    const esp_lcd_panel_dev_config_t device = {
        /* Reset is driven explicitly above to match the board library's dwell
         * times instead of ESP-IDF's generic 10 ms / 10 ms sequence. */
        .reset_gpio_num = -1, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG, .bits_per_pixel = 16,
    };
    INIT_TRY(esp_lcd_new_panel_st7789(panel_io, &device, &panel));
    INIT_TRY(gpio_set_level(LCD_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(64));
    INIT_TRY(initialize_panel_from_boe_reference(rotation));
    INIT_TRY(set_panel_gap(rotation));

    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0, .freq_hz = 44100, .clk_cfg = LEDC_AUTO_CLK,
    };
    INIT_TRY(ledc_timer_config(&timer));
    const ledc_channel_config_t light = {
        .gpio_num = LCD_BL, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0,
        .duty = 0, .hpoint = 0,
    };
    INIT_TRY(ledc_channel_config(&light));
    light_created = true;
    ready = true;
    mark_dirty(0, 0, RYZ_DISPLAY_WIDTH, RYZ_DISPLAY_HEIGHT);
    INIT_TRY(ryz_display_show());
    const uint32_t duty = (uint32_t)brightness * 255U / 100U;
    INIT_TRY(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty));
    INIT_TRY(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
    confirmed_rotation = rotation;
    confirmed_duty = duty;
    display_asleep = false;
    backlight_uncertain = false;
    return ESP_OK;

fail:
    ready = false;
    if (light_created) ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    else gpio_set_level(LCD_BL, 0);
    gpio_set_level(LCD_RST, 0);
    if (panel) { esp_lcd_panel_del(panel); panel = NULL; }
    /* Deleting IO drains any outstanding DMA before freeing its source buffer. */
    if (panel_io) { esp_lcd_panel_io_del(panel_io); panel_io = NULL; }
    if (bus_created) spi_bus_free(LCD_HOST);
    heap_caps_free(dma_stripe);
    heap_caps_free(canvas);
    dma_stripe = NULL;
    canvas = NULL;
    return err;
#undef INIT_TRY
}

esp_err_t ryz_display_clear(uint16_t color)
{
    if (!ready || privacy_blocked || configuration_blocked) return ESP_ERR_INVALID_STATE;
    /* show() always drains DMA before returning, so the same internal buffer is
     * safe to reuse as a fill pattern. Bulk copies avoid slow 16-bit PSRAM stores. */
    uint16_t encoded = wire_color(color);
    uint16_t *pattern = (uint16_t *)dma_stripe;
    for (size_t i = 0; i < STRIPE_BYTES / 2; ++i) pattern[i] = encoded;
    size_t pixels = RYZ_DISPLAY_WIDTH * RYZ_DISPLAY_HEIGHT;
    for (size_t offset = 0; offset < pixels;) {
        size_t count = pixels - offset;
        if (count > STRIPE_BYTES / 2) count = STRIPE_BYTES / 2;
        memcpy(canvas + offset, pattern, count * sizeof(*canvas));
        offset += count;
    }
    mark_dirty(0, 0, RYZ_DISPLAY_WIDTH, RYZ_DISPLAY_HEIGHT);
    return ESP_OK;
}

esp_err_t ryz_display_read_pixel(int x, int y, uint16_t *color)
{
    if (!ready || privacy_blocked || configuration_blocked) return ESP_ERR_INVALID_STATE;
    if (!color || x < 0 || x >= RYZ_DISPLAY_WIDTH || y < 0 || y >= RYZ_DISPLAY_HEIGHT) {
        return ESP_ERR_INVALID_ARG;
    }
    *color = wire_color(canvas[y * RYZ_DISPLAY_WIDTH + x]);
    return ESP_OK;
}

esp_err_t ryz_display_rect(int x, int y, int width, int height, uint16_t color)
{
    if (!ready || privacy_blocked || configuration_blocked) return ESP_ERR_INVALID_STATE;
    if (width < 1 || width > RYZ_DISPLAY_WIDTH || height < 1 || height > RYZ_DISPLAY_HEIGHT
        || x < 0 || x > RYZ_DISPLAY_WIDTH - width || y < 0 || y > RYZ_DISPLAY_HEIGHT - height) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t encoded = wire_color(color);
    for (int row = y; row < y + height; ++row) {
        for (int col = x; col < x + width; ++col) canvas[row * RYZ_DISPLAY_WIDTH + col] = encoded;
    }
    mark_dirty(x, y, x + width, y + height);
    return ESP_OK;
}

esp_err_t ryz_display_blit_rgb565_native(int x, int y, int width, int height,
                                        const uint16_t *pixels,
                                        size_t pixel_count)
{
    if (!ready || privacy_blocked || configuration_blocked) return ESP_ERR_INVALID_STATE;
    if (!pixels || width < 1 || width > RYZ_DISPLAY_WIDTH ||
        height < 1 || height > RYZ_DISPLAY_HEIGHT ||
        x < 0 || x > RYZ_DISPLAY_WIDTH - width ||
        y < 0 || y > RYZ_DISPLAY_HEIGHT - height ||
        pixel_count != (size_t)width * (size_t)height) {
        return ESP_ERR_INVALID_ARG;
    }
    int changed_left = width, changed_top = height;
    int changed_right = 0, changed_bottom = 0;
    for (int row = 0; row < height; ++row) {
        const uint16_t *source = pixels + (size_t)row * (size_t)width;
        uint16_t *destination =
            canvas + (y + row) * RYZ_DISPLAY_WIDTH + x;
        int row_left = width, row_right = 0;
        for (int column = 0; column < width; ++column) {
            const uint16_t encoded = wire_color(source[column]);
            if (destination[column] == encoded) continue;
            destination[column] = encoded;
            if (row_left == width) row_left = column;
            row_right = column + 1;
        }
        if (!row_right) continue;
        if (row_left < changed_left) changed_left = row_left;
        if (row_right > changed_right) changed_right = row_right;
        if (changed_top == height) changed_top = row;
        changed_bottom = row + 1;
    }
    /* LVGL may redraw a complete tree even when only a label (or nothing)
     * changed. Compare in the canvas's wire order, then extend only the actual
     * changed bounds. Never clear existing dirt: a prior cancelled/failed show
     * still needs repair even when this input already matches its canvas. */
    if (changed_right)
        mark_dirty(x + changed_left, y + changed_top,
                   x + changed_right, y + changed_bottom);
    return ESP_OK;
}

esp_err_t ryz_display_blit_rgb565_be(int x, int y, int width, int height,
                                    const uint8_t *pixels,
                                    size_t byte_count)
{
    if (!ready || privacy_blocked || configuration_blocked) return ESP_ERR_INVALID_STATE;
    if (!pixels || width < 1 || width > RYZ_DISPLAY_WIDTH ||
        height < 1 || height > RYZ_DISPLAY_HEIGHT ||
        x < 0 || x > RYZ_DISPLAY_WIDTH - width ||
        y < 0 || y > RYZ_DISPLAY_HEIGHT - height ||
        byte_count != (size_t)width * (size_t)height * 2U) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int row = 0; row < height; ++row) {
        memcpy(canvas + (y + row) * RYZ_DISPLAY_WIDTH + x,
               pixels + (size_t)row * (size_t)width * 2U,
               (size_t)width * 2U);
    }
    mark_dirty(x, y, x + width, y + height);
    return ESP_OK;
}

esp_err_t ryz_display_show(void)
{
    return ryz_display_show_checked(NULL, NULL);
}

esp_err_t ryz_display_show_checked(bool (*cancelled)(void *), void *context)
{
    return ryz_display_show_checked_status(cancelled, context, NULL);
}

/* Caller has established ordinary access, or has drained and scrubbed every
 * retained buffer for a privacy-only black repaint. Keep cancellation behavior
 * and transaction completion identical for ordinary callers. */
static esp_err_t show_canvas_checked_status(
    bool (*cancelled)(void *), void *context, bool *cancelled_out)
{
    if (cancelled_out) *cancelled_out = false;
    if (cancelled && cancelled(context)) {
        if (cancelled_out) *cancelled_out = true;
        return ESP_ERR_TIMEOUT;
    }
    if (!dirty) return ESP_OK;
    /* Expand horizontally to whole 32-bit words: packed DMA rows then have no
     * stride or unaligned tail. Neighbour pixels come from the unchanged canvas. */
    int left = dirty_left & ~1;
    int right = (dirty_right + 1) & ~1;
    int width = right - left;
    unsigned stripe = 0;
    esp_err_t err = ESP_OK;
    bool cancellation_requested = false;
    for (int y = dirty_top; y < dirty_bottom; y += STRIPE_LINES) {
        if (cancelled && cancelled(context)) {
            cancellation_requested = true;
            break;
        }
        int lines = dirty_bottom - y;
        if (lines > STRIPE_LINES) lines = STRIPE_LINES;
        /* Prepare the next stripe while DMA reads the other buffer. Neither
         * buffer is reused until the preceding transfer has been drained. */
        uint8_t *pixels = dma_stripe + stripe * STRIPE_BYTES;
        if (width == RYZ_DISPLAY_WIDTH) {
            memcpy(pixels, canvas + y * RYZ_DISPLAY_WIDTH, lines * width * 2);
        } else {
            for (int row = 0; row < lines; ++row) {
                memcpy(pixels + row * width * 2,
                       canvas + (y + row) * RYZ_DISPLAY_WIDTH + left, width * 2);
            }
        }
        /* tx_param(-1, NULL, 0) is a documented no-command transaction that drains
         * the previous color transfer. It already yields while DMA is busy. */
        esp_err_t drained = esp_lcd_panel_io_tx_param(panel_io, -1, NULL, 0);
        if (drained != ESP_OK) { ready = false; return drained; }
        if (cancelled && cancelled(context)) {
            cancellation_requested = true;
            break;
        }
        err = esp_lcd_panel_draw_bitmap(panel, left, y, right, y + lines, pixels);
        if (err != ESP_OK) break;
        stripe ^= 1;
    }
    /* Also drain on cancellation/error: the caller may immediately draw again
     * or destroy its Lua VM. No DMA source may remain in use on return. */
    esp_err_t drained = esp_lcd_panel_io_tx_param(panel_io, -1, NULL, 0);
    if (drained != ESP_OK) { ready = false; return drained; }
    if (cancellation_requested) {
        if (cancelled_out) *cancelled_out = true;
        return ESP_ERR_TIMEOUT;
    }
    if (err != ESP_OK) return err;
    /* Keep the entire dirty window on cancellation/error, so a later show can
     * repair a partially transferred frame. Clear only after complete success. */
    dirty = false;
    atomic_fetch_add_explicit(&completed_commits, UINT32_C(1), memory_order_relaxed);
    return ESP_OK;
}

esp_err_t ryz_display_show_checked_status(
    bool (*cancelled)(void *), void *context, bool *cancelled_out)
{
    if (cancelled_out) *cancelled_out = false;
    if (!ready || privacy_blocked || configuration_blocked) return ESP_ERR_INVALID_STATE;
    return show_canvas_checked_status(cancelled, context, cancelled_out);
}

static void wipe_bytes(void *memory, size_t length)
{
    volatile uint8_t *bytes = memory;
    while (length--) *bytes++ = 0;
}

static esp_err_t set_backlight_duty(uint32_t duty)
{
    esp_err_t error = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    if (error != ESP_OK) return error;
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

esp_err_t ryz_display_set_brightness(uint8_t brightness)
{
    if (!configuration_valid(0, brightness)) return ESP_ERR_INVALID_ARG;
    if (!ready || privacy_tainted || privacy_blocked || configuration_blocked || flush.active)
        return ESP_ERR_INVALID_STATE;
    const uint32_t duty = (uint32_t)brightness * 255U / 100U;
    if (!backlight_uncertain && (display_asleep || duty == confirmed_duty)) {
        /* A sleeping panel remains dark; only its next confirmed wake changes. */
        confirmed_duty = duty;
        return ESP_OK;
    }
    const uint32_t old_effective_duty = display_asleep ? 0 : confirmed_duty;
    esp_err_t error = set_backlight_duty(display_asleep ? 0 : duty);
    if (error == ESP_OK) {
        confirmed_duty = duty;
        backlight_uncertain = false;
    } else {
        /* A failed update may have taken effect; only a successful rollback
         * confirms the old duty. Preserve the first error for the UI owner. */
        backlight_uncertain = set_backlight_duty(old_effective_duty) != ESP_OK;
    }
    return error;
}

static esp_err_t apply_panel_configuration(uint8_t rotation, uint32_t duty)
{
    /* RootMaker config uses 240x240 at offset0 in ST7789 240x320 RAM.
     * Rotating the 80 hidden rows moves the address gap to Y (180) or X (270).
     * Keep RGB/BOE analog setup unchanged; only MADCTL and the address gap vary. */
    esp_err_t error = set_backlight_duty(0);
    if (error == ESP_OK) error = esp_lcd_panel_io_tx_param(panel_io, -1, NULL, 0);
    if (error == ESP_OK) error = esp_lcd_panel_io_tx_param(panel_io, 0x36, &rotation_madctl[rotation], 1);
    if (error == ESP_OK) error = set_panel_gap(rotation);
    if (error == ESP_OK) {
        mark_dirty(0, 0, RYZ_DISPLAY_WIDTH, RYZ_DISPLAY_HEIGHT);
        error = show_canvas_checked_status(NULL, NULL, NULL);
    }
    if (error == ESP_OK) error = set_backlight_duty(display_asleep ? 0 : duty);
    if (error != ESP_OK) (void)set_backlight_duty(0);
    return error;
}

esp_err_t ryz_display_apply_configuration(uint8_t rotation, uint8_t brightness)
{
    if (!configuration_valid(rotation, brightness)) return ESP_ERR_INVALID_ARG;
    if (privacy_blocked || privacy_tainted || flush.active || !panel_io || !panel || !canvas || !dma_stripe ||
        (!ready && !configuration_blocked)) return ESP_ERR_INVALID_STATE;
    if (!configuration_blocked && rotation == confirmed_rotation)
        return ryz_display_set_brightness(brightness);
    uint32_t duty = (uint32_t)brightness * 255U / 100U; /* 50% preserves127. */
    configuration_blocked = true;
    esp_err_t error = apply_panel_configuration(rotation, duty);
    if (error == ESP_OK) {
        confirmed_rotation = rotation;
        confirmed_duty = duty;
        ready = true;
        configuration_blocked = false;
        backlight_uncertain = false;
    } else if (apply_panel_configuration(confirmed_rotation, confirmed_duty) == ESP_OK) {
        /* Definite rollback allows old-coordinate UI to show APPLY FAILED.
         * Persistence is a different layer and is NOT claimed rolled back. */
        ready = true;
        configuration_blocked = false;
        backlight_uncertain = false;
    }
    return error;
}

esp_err_t ryz_display_set_asleep(bool asleep)
{
    if (!ready || privacy_blocked || configuration_blocked) return ESP_ERR_INVALID_STATE;
    if (asleep == display_asleep && !backlight_uncertain) return ESP_OK;
    esp_err_t error = set_backlight_duty(asleep ? 0 : confirmed_duty);
    backlight_uncertain = error != ESP_OK;
    if (error == ESP_OK) display_asleep = asleep;
    return error;
}

esp_err_t ryz_display_privacy_clear(void)
{
    if (!privacy_tainted && !privacy_blocked) return ESP_OK;
    privacy_blocked = true;
    /* mark_sensitive requires a completed initialization. A later transfer
     * failure can revoke ready but does not free these resources. Do not rebuild
     * the board, or assume a missing handle can safely be reconstructed here. */
    if (!panel_io || !panel || !canvas || !dma_stripe) return ESP_ERR_INVALID_STATE;
    esp_err_t error = set_backlight_duty(0);
    if (error != ESP_OK) return error;
    error = esp_lcd_panel_io_tx_param(panel_io, -1, NULL, 0);
    if (error != ESP_OK) { ready = false; return error; }
    flush.active = false;
    /* Never wipe a buffer before its DMA completion is confirmed. Both stripes
     * can contain a former sensitive frame even when no transfer is active. */
    wipe_bytes(canvas, RYZ_DISPLAY_WIDTH * RYZ_DISPLAY_HEIGHT * sizeof(*canvas));
    wipe_bytes(dma_stripe, DMA_BUFFER_BYTES);
    mark_dirty(0, 0, RYZ_DISPLAY_WIDTH, RYZ_DISPLAY_HEIGHT);
    error = show_canvas_checked_status(NULL, NULL, NULL);
    if (error != ESP_OK) return error;
    error = set_backlight_duty(display_asleep ? 0 : confirmed_duty);
    if (error != ESP_OK) {
        /* Restoration may have partially taken effect. Request darkness again,
         * preserving the original failure; this is not proof of physical dark. */
        (void)set_backlight_duty(0);
        return error;
    }
    ready = true;
    privacy_tainted = false;
    privacy_blocked = false;
    backlight_uncertain = false;
    return ESP_OK;
}

void ryz_display_show_abort(void)
{
    if (privacy_blocked || configuration_blocked) return;
    if (!flush.active) return;
    if (esp_lcd_panel_io_tx_param(panel_io, -1, NULL, 0) != ESP_OK) ready = false;
    flush.active = false; /* dirty window remains for a later repair */
}

esp_err_t ryz_display_show_step(bool begin, bool *done)
{
    if (!ready || privacy_blocked || configuration_blocked || !done) return ESP_ERR_INVALID_STATE;
    *done = false;
    if (begin) {
        if (flush.active) return ESP_ERR_INVALID_STATE;
        if (!dirty) { *done = true; return ESP_OK; }
        flush.active = true; flush.left = dirty_left & ~1; flush.right = (dirty_right + 1) & ~1;
        flush.y = dirty_top; flush.bottom = dirty_bottom; flush.stripe = 0;
    }
    if (!flush.active) return ESP_ERR_INVALID_STATE;
    if (flush.y >= flush.bottom) {
        esp_err_t err = esp_lcd_panel_io_tx_param(panel_io, -1, NULL, 0);
        flush.active = false;
        if (err != ESP_OK) { ready = false; return err; }
        dirty = false;
        atomic_fetch_add_explicit(&completed_commits, UINT32_C(1), memory_order_relaxed);
        *done = true;
        return ESP_OK;
    }
    int width = flush.right - flush.left, lines = flush.bottom - flush.y;
    if (lines > STRIPE_LINES) lines = STRIPE_LINES;
    uint8_t *pixels = dma_stripe + flush.stripe * STRIPE_BYTES;
    if (width == RYZ_DISPLAY_WIDTH) memcpy(pixels, canvas + flush.y * RYZ_DISPLAY_WIDTH, lines * width * 2);
    else for (int row = 0; row < lines; ++row) memcpy(pixels + row * width * 2, canvas + (flush.y + row) * RYZ_DISPLAY_WIDTH + flush.left, width * 2);
    esp_err_t err = esp_lcd_panel_io_tx_param(panel_io, -1, NULL, 0);
    if (err == ESP_OK) err = esp_lcd_panel_draw_bitmap(panel, flush.left, flush.y, flush.right, flush.y + lines, pixels);
    if (err != ESP_OK) { ryz_display_show_abort(); return err; }
    flush.y += lines; flush.stripe ^= 1;
    return ESP_OK;
}

esp_err_t ryz_display_text(int x, int y, const char *text, size_t length,
                           uint16_t color, int scale)
{
    if (!ready || privacy_blocked || configuration_blocked) return ESP_ERR_INVALID_STATE;
    if (!text || length < 1 || length > 40 || scale < 1 || scale > 6) return ESP_ERR_INVALID_ARG;
    int width = ((int)length * 6 - 1) * scale;
    int height = 7 * scale;
    if (x < 0 || y < 0 || x > RYZ_DISPLAY_WIDTH - width || y > RYZ_DISPLAY_HEIGHT - height) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < length; ++i) {
        if (!display_glyph(text[i])) {
            /* Do not log the character or source text: this API can render
             * local SSIDs. The fixed operation and offset are enough to locate
             * the failing boundary without exposing display content. */
            ESP_LOGE(TAG, "display.text unsupported glyph at index=%u",
                     (unsigned)i);
            return ESP_ERR_NOT_SUPPORTED;
        }
    }
    uint16_t encoded = wire_color(color);
    for (size_t i = 0; i < length; ++i) {
        const uint8_t *rows = display_glyph(text[i]);
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if (!(rows[row] & (1 << (4 - col)))) continue;
                int left = x + ((int)i * 6 + col) * scale;
                int top = y + row * scale;
                for (int dy = 0; dy < scale; ++dy) {
                    for (int dx = 0; dx < scale; ++dx) {
                        canvas[(top + dy) * RYZ_DISPLAY_WIDTH + left + dx] = encoded;
                    }
                }
            }
        }
    }
    mark_dirty(x, y, x + width, y + height);
    return ESP_OK;
}
