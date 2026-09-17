#include "display.h"
#include "display_test_platform.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t lcd[RYZ_DISPLAY_HEIGHT][RYZ_DISPLAY_WIDTH];
static const uint8_t *pending;
static uint8_t queued_pixels[RYZ_DISPLAY_WIDTH * 16 * 2];
static size_t queued_bytes;
static int px0, py0, px1, py1, clock_hz;
static size_t transfers, transferred_bytes;
static bool fail_draw, fail_drain;
static bool fail_black_final_drain;
static int fail_ledc_set_after = -1, fail_ledc_update_after = -1;
static unsigned fail_ledc_set_count, fail_ledc_update_count;
static size_t ledc_set_calls, ledc_update_calls, ledc_zero_calls;
static size_t display_io_calls, spi_bus_init_calls;
static bool watching_commits;
static uint32_t expected_commits;
static bool privacy_must_be_dark;
static void *allocated_canvas, *allocated_dma;
static size_t allocated_canvas_bytes, allocated_dma_bytes;
static char last_error_tag[32];
static char last_error_log[160];
static gpio_config_t gpio_configs[4];
static size_t gpio_config_count;
typedef struct { int pin, level; } gpio_level_record_t;
static gpio_level_record_t gpio_levels[8];
static size_t gpio_level_count;
static int delays_ms[8];
static size_t delay_count;
static int panel_reset_pin, panel_rgb_order;
static size_t panel_reset_calls, panel_init_calls, panel_invert_calls;
static size_t panel_mirror_calls, panel_swap_calls, panel_display_calls;
static int configured_backlight_duty, staged_backlight_duty, applied_backlight_duty;

typedef struct {
    int command;
    uint8_t data[14];
    size_t length;
} command_record_t;

static command_record_t init_commands[24];
static size_t init_command_count;
static bool runtime_configuration;
static unsigned madctl_calls, fail_madctl_count;
static uint8_t madctl_value;
static int gap_x, gap_y;
static unsigned gap_calls;
static int boot_rotation_to_check = -1;
static const uint8_t expected_madctl[4] = {0x00, 0x60, 0xc0, 0xa0};

static const command_record_t expected_init_commands[] = {
    {0x11, {0}, 0},
    {0x36, {0x00}, 1},
    {0x3a, {0x05}, 1},
    {0x21, {0}, 0},
    {0x2a, {0x00, 0x00, 0x00, 0xef}, 4},
    {0x2b, {0x00, 0x00, 0x00, 0xef}, 4},
    {0xb2, {0x0c, 0x0c, 0x00, 0x33, 0x33}, 5},
    {0xb7, {0x35}, 1},
    {0xbb, {0x1f}, 1},
    {0xc0, {0x2c}, 1},
    {0xc2, {0x01}, 1},
    {0xc3, {0x12}, 1},
    {0xc4, {0x20}, 1},
    {0xc6, {0x0f}, 1},
    {0xd0, {0xa4, 0xa1}, 2},
    {0xe0, {0xd0, 0x08, 0x11, 0x08, 0x0c, 0x15, 0x39,
            0x33, 0x50, 0x36, 0x13, 0x14, 0x29, 0x2d}, 14},
    {0xe1, {0xd0, 0x08, 0x10, 0x08, 0x06, 0x06, 0x39,
            0x44, 0x51, 0x0b, 0x16, 0x14, 0x2f, 0x31}, 14},
    {0x29, {0}, 0},
};

void display_test_log_error(const char *tag, const char *format, ...)
{
    snprintf(last_error_tag, sizeof(last_error_tag), "%s", tag);
    va_list args;
    va_start(args, format);
    vsnprintf(last_error_log, sizeof(last_error_log), format, args);
    va_end(args);
}

esp_err_t gpio_config(const gpio_config_t *p) {
    ++display_io_calls;
    assert(gpio_config_count < sizeof(gpio_configs) / sizeof(gpio_configs[0]));
    gpio_configs[gpio_config_count++] = *p;
    return ESP_OK;
}
esp_err_t gpio_set_level(int p, int v) {
    ++display_io_calls;
    assert(gpio_level_count < sizeof(gpio_levels) / sizeof(gpio_levels[0]));
    gpio_levels[gpio_level_count].pin = p;
    gpio_levels[gpio_level_count].level = v;
    ++gpio_level_count;
    return ESP_OK;
}
void *heap_caps_calloc(size_t n, size_t s, int caps) {
    (void)caps; allocated_canvas_bytes = n * s;
    allocated_canvas = calloc(n, s); return allocated_canvas;
}
void *heap_caps_malloc(size_t n, int caps) {
    (void)caps; allocated_dma_bytes = n;
    allocated_dma = malloc(n); return allocated_dma;
}
void heap_caps_free(void *p) { free(p); }
esp_err_t spi_bus_initialize(int host, const spi_bus_config_t *c, int dma) {
    (void)host; (void)c; (void)dma;
    ++display_io_calls; ++spi_bus_init_calls; return ESP_OK;
}
esp_err_t spi_bus_free(int host) { (void)host; return ESP_OK; }
esp_err_t esp_lcd_new_panel_io_spi(int host, const esp_lcd_panel_io_spi_config_t *c, esp_lcd_panel_io_handle_t *out) {
    ++display_io_calls;
    (void)host; clock_hz = c->pclk_hz; *out = (void *)1; return ESP_OK;
}
esp_err_t esp_lcd_new_panel_st7789(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *c, esp_lcd_panel_handle_t *out) {
    ++display_io_calls;
    (void)io;
    assert(c->data_endian == LCD_RGB_DATA_ENDIAN_BIG);
    panel_reset_pin = c->reset_gpio_num;
    panel_rgb_order = c->rgb_ele_order;
    *out = (void *)2;
    return ESP_OK;
}
esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t p) { (void)p; ++panel_reset_calls; return ESP_OK; }
esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t p) { (void)p; ++panel_init_calls; return ESP_OK; }
esp_err_t esp_lcd_panel_del(esp_lcd_panel_handle_t p) { (void)p; return ESP_OK; }
esp_err_t esp_lcd_panel_invert_color(esp_lcd_panel_handle_t p, bool v) { (void)p; (void)v; ++panel_invert_calls; return ESP_OK; }
esp_err_t esp_lcd_panel_swap_xy(esp_lcd_panel_handle_t p, bool v) { (void)p; (void)v; ++panel_swap_calls; return ESP_OK; }
esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t p, bool v) { (void)p; (void)v; ++panel_display_calls; return ESP_OK; }
esp_err_t esp_lcd_panel_set_gap(esp_lcd_panel_handle_t p, int x, int y) {
    (void)p; assert((x==0 && y==0) || (x==0 && y==80) || (x==80 && y==0));
    ++gap_calls; gap_x=x; gap_y=y; return ESP_OK;
}
esp_err_t esp_lcd_panel_mirror(esp_lcd_panel_handle_t p, bool x, bool y) { (void)p; assert(!x && !y); ++panel_mirror_calls; return ESP_OK; }
esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t p) { (void)p; assert(!pending); return ESP_OK; }
static void check_scrubbed_frame(void) {
    assert(!pending);
    assert(allocated_canvas_bytes == 240U * 240U * 2U);
    assert(allocated_dma_bytes == 240U * 16U * 2U * 2U);
    const unsigned char *bytes = allocated_canvas;
    for (size_t i = 0; i < allocated_canvas_bytes; ++i) assert(bytes[i] == 0);
    bytes = allocated_dma;
    for (size_t i = 0; i < allocated_dma_bytes; ++i) assert(bytes[i] == 0);
    for (int y = 0; y < 240; ++y) for (int x = 0; x < 240; ++x) assert(lcd[y][x] == 0);
}
static bool consume_fault(int *after) {
    if (*after < 0) return false;
    if ((*after)-- == 0) return true;
    return false;
}
esp_err_t ledc_timer_config(const ledc_timer_config_t *p) { (void)p; ++display_io_calls; return ESP_OK; }
esp_err_t ledc_channel_config(const ledc_channel_config_t *p) {
    ++display_io_calls; configured_backlight_duty = p->duty;
    staged_backlight_duty = applied_backlight_duty = p->duty; return ESP_OK;
}
esp_err_t ledc_set_duty(int a, int b, int c) {
    assert(a == LEDC_LOW_SPEED_MODE && b == LEDC_CHANNEL_0);
    ++display_io_calls; ++ledc_set_calls;
    if (!c) ++ledc_zero_calls;
    if (privacy_must_be_dark && c == 127) check_scrubbed_frame();
    if (fail_ledc_set_count) { --fail_ledc_set_count; return ESP_ERR_INVALID_ARG; }
    if (consume_fault(&fail_ledc_set_after)) return ESP_ERR_INVALID_ARG;
    staged_backlight_duty = c; return ESP_OK;
}
esp_err_t ledc_update_duty(int a, int b) {
    assert(a == LEDC_LOW_SPEED_MODE && b == LEDC_CHANNEL_0);
    ++display_io_calls; ++ledc_update_calls;
    if (fail_ledc_update_count) { --fail_ledc_update_count; return ESP_ERR_INVALID_ARG; }
    if (consume_fault(&fail_ledc_update_after)) return ESP_ERR_INVALID_ARG;
    applied_backlight_duty = staged_backlight_duty; return ESP_OK;
}
esp_err_t ledc_stop(int a, int b, int c) { (void)a; (void)b; (void)c; return ESP_OK; }
void vTaskDelay(int ticks) {
    assert(delay_count < sizeof(delays_ms) / sizeof(delays_ms[0]));
    delays_ms[delay_count++] = ticks;
}

esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel, int x0, int y0, int x1, int y1, const void *data) {
    ++display_io_calls;
    (void)panel; assert(!pending);
    if (boot_rotation_to_check >= 0) {
        assert(madctl_value == expected_madctl[boot_rotation_to_check]);
        assert(gap_x == (boot_rotation_to_check == 3 ? 80 : 0));
        assert(gap_y == (boot_rotation_to_check == 2 ? 80 : 0));
        assert(applied_backlight_duty == 0); /* No first frame in default direction. */
    }
    if (privacy_must_be_dark) assert(applied_backlight_duty == 0);
    if (watching_commits) assert(ryz_display_completed_commits() == expected_commits);
    if (fail_draw) { fail_draw = false; return ESP_ERR_TIMEOUT; }
    assert(x0 >= 0 && x0 < x1 && x1 <= RYZ_DISPLAY_WIDTH);
    assert(y0 >= 0 && y0 < y1 && y1 <= RYZ_DISPLAY_HEIGHT);
    queued_bytes = (size_t)(x1 - x0) * (y1 - y0) * 2;
    assert(queued_bytes <= sizeof(queued_pixels));
    memcpy(queued_pixels, data, queued_bytes);
    pending = data; px0 = x0; py0 = y0; px1 = x1; py1 = y1;
    return ESP_OK;
}
esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io, int command, const void *p, size_t n) {
    ++display_io_calls;
    (void)io;
    if (command >= 0) {
        if(runtime_configuration) {
            assert(command==0x36 && n==1 && p);
            ++madctl_calls;
            if(fail_madctl_count) { --fail_madctl_count; return ESP_ERR_TIMEOUT; }
            madctl_value=*(const uint8_t *)p;
            return ESP_OK;
        }
        assert(init_command_count < sizeof(init_commands) / sizeof(init_commands[0]));
        assert(n <= sizeof(init_commands[0].data));
        command_record_t *record = &init_commands[init_command_count++];
        record->command = command;
        record->length = n;
        if (n) { assert(p); memcpy(record->data, p, n); }
        if (command == 0x36) { assert(n == 1); madctl_value = *(const uint8_t *)p; }
        return ESP_OK;
    }
    assert(command == -1 && !p && n == 0);
    if (privacy_must_be_dark) assert(applied_backlight_duty == 0);
    if (watching_commits) assert(ryz_display_completed_commits() == expected_commits);
    if (!pending) return ESP_OK;
    if (fail_drain) { fail_drain = false; return ESP_ERR_TIMEOUT; }
    if (fail_black_final_drain && privacy_must_be_dark && py1 == 240) {
        fail_black_final_drain = false; return ESP_ERR_TIMEOUT;
    }
    /* DMA consumes the source only at completion, not when queued. */
    assert(memcmp(pending, queued_pixels, queued_bytes) == 0);
    size_t i = 0;
    for (int y = py0; y < py1; ++y) for (int x = px0; x < px1; ++x) {
        lcd[y][x] = (uint16_t)((pending[i] << 8) | pending[i + 1]); i += 2;
    }
    transferred_bytes += i; ++transfers; pending = NULL;
    return ESP_OK;
}

static void reset_counts(void) { assert(!pending); transfers = transferred_bytes = 0; }
static void check_pixel(int x, int y, uint16_t expected) {
    uint16_t value; assert(ryz_display_read_pixel(x, y, &value) == ESP_OK);
    assert(value == expected && lcd[y][x] == expected);
}
static bool cancel_after_stripe(void *context) { (void)context; return transfers >= 1; }
static bool cancel_while_pending(void *context) { (void)context; return pending != NULL; }
static bool cancel_immediately(void *context) { (void)context; return true; }

static void check_init_contract(uint8_t rotation, uint8_t brightness) {
    assert(clock_hz == 80000000);
    assert(panel_reset_pin == -1);
    assert(panel_rgb_order == LCD_RGB_ELEMENT_ORDER_RGB);
    assert(panel_reset_calls == 0 && panel_init_calls == 0);
    assert(panel_invert_calls == 0 && panel_mirror_calls == 0);
    assert(panel_swap_calls == 0 && panel_display_calls == 0);
    assert(gpio_config_count >= 1);
    assert(gpio_configs[0].pin_bit_mask == ((1ULL << 7) | (1ULL << 10)));
    assert(gpio_levels[0].pin == 7 && gpio_levels[0].level == 0);
    assert(gpio_levels[1].pin == 10 && gpio_levels[1].level == 0);
    assert(gpio_levels[2].pin == 10 && gpio_levels[2].level == 1);
    assert(delay_count == 3);
    assert(delays_ms[0] == 8 && delays_ms[1] == 64 && delays_ms[2] == 120);
    assert(configured_backlight_duty == 0);
    assert(applied_backlight_duty == brightness * 255U / 100U);
    assert(gap_x == (rotation == 3 ? 80 : 0) && gap_y == (rotation == 2 ? 80 : 0));
    assert(init_command_count == sizeof(expected_init_commands) / sizeof(expected_init_commands[0]));
    for (size_t i = 0; i < init_command_count; ++i) {
        assert(init_commands[i].command == expected_init_commands[i].command);
        assert(init_commands[i].length == expected_init_commands[i].length);
        if (init_commands[i].command == 0x36) {
            assert(init_commands[i].data[0] == expected_madctl[rotation]);
        } else {
            assert(memcmp(init_commands[i].data, expected_init_commands[i].data,
                          init_commands[i].length) == 0);
        }
    }
}

static void check_reference_init_contract(void) { check_init_contract(0, 50); }

static void check_boot_configuration(uint8_t rotation) {
    assert(ryz_display_init_with_configuration(4, 50) == ESP_ERR_INVALID_ARG);
    assert(ryz_display_init_with_configuration(0, 9) == ESP_ERR_INVALID_ARG);
    assert(ryz_display_init_with_configuration(0, 101) == ESP_ERR_INVALID_ARG);
    assert(ryz_display_init_with_configuration(0, 51) == ESP_ERR_INVALID_ARG);
    assert(display_io_calls == 0 && !allocated_canvas && !allocated_dma);
    boot_rotation_to_check = rotation;
    assert(ryz_display_init_with_configuration(rotation, 80) == ESP_OK);
    boot_rotation_to_check = -1;
    check_init_contract(rotation, 80);
    assert(ryz_display_completed_commits() == 1);
    const size_t before = display_io_calls;
    runtime_configuration = true;
    assert(ryz_display_init() == ESP_OK); /* A live panel must not revert to 0. */
    assert(ryz_display_apply_configuration(rotation, 80) == ESP_OK);
    assert(display_io_calls == before && !madctl_calls);
    assert(ryz_display_set_asleep(true) == ESP_OK && applied_backlight_duty == 0);
    assert(ryz_display_set_asleep(false) == ESP_OK && applied_backlight_duty == 204);
    /* Runtime rollback must remember the boot tuple, not hardcoded 0/50. */
    fail_madctl_count = 1;
    assert(ryz_display_apply_configuration((rotation + 1U) % 4U, 10) == ESP_ERR_TIMEOUT);
    assert(madctl_value == expected_madctl[rotation] && applied_backlight_duty == 204);
    assert(gap_x == (rotation == 3 ? 80 : 0) && gap_y == (rotation == 2 ? 80 : 0));
    assert(!ryz_display_configuration_blocked());
    printf("DISPLAY_BOOT_CONFIGURATION_PASS rotation=%u\n", rotation);
}

static void check_completed_commits(void) {
    assert(ryz_display_completed_commits() == 0);
    assert(ryz_display_init() == ESP_OK);
    check_reference_init_contract();
    assert(ryz_display_completed_commits() == 1);
    assert(ryz_display_init() == ESP_OK);
    assert(ryz_display_completed_commits() == 1);
    reset_counts();
    assert(ryz_display_show() == ESP_OK && transfers == 0);
    assert(ryz_display_completed_commits() == 1);
    assert(ryz_display_clear(0x1234) == ESP_OK);
    assert(ryz_display_completed_commits() == 1);
    assert(ryz_display_show() == ESP_OK && transfers == 15 && !pending);
    assert(ryz_display_completed_commits() == 2);
    watching_commits = true;
    expected_commits = 2;
    reset_counts();
    assert(ryz_display_clear(0x3456) == ESP_OK);
    bool done = true;
    for (unsigned stripe = 0; stripe < 15; ++stripe) {
        assert(ryz_display_show_step(stripe == 0, &done) == ESP_OK);
        assert(!done && pending && ryz_display_completed_commits() == 2);
    }
    /* Even the last queued stripe is not a complete transaction until its
     * separate terminal call successfully drains the actual pending DMA. */
    assert(transfers == 14);
    assert(ryz_display_show_step(false, &done) == ESP_OK);
    assert(done && !pending && transfers == 15);
    assert(ryz_display_completed_commits() == 3);
    expected_commits = 3;
    reset_counts();
    assert(ryz_display_show() == ESP_OK);
    assert(ryz_display_show_step(true, &done) == ESP_OK && done);
    assert(ryz_display_show_step(false, &done) == ESP_ERR_INVALID_STATE);
    ryz_display_show_abort();
    assert(!pending && transfers == 0 && ryz_display_completed_commits() == 3);

    /* An abort drains the one and only queued stripe but does not commit the
     * abandoned transaction; the subsequent explicit repair counts once. */
    assert(ryz_display_rect(2, 2, 2, 1, 0x4567) == ESP_OK);
    assert(ryz_display_show_step(true, &done) == ESP_OK && !done && pending);
    ryz_display_show_abort();
    assert(!pending && transfers == 1 && ryz_display_completed_commits() == 3);
    ryz_display_show_abort();
    assert(ryz_display_show() == ESP_OK && !pending);
    assert(ryz_display_completed_commits() == 4);
    expected_commits = 4;

    reset_counts();
    bool cancelled_out = false;
    assert(ryz_display_show_checked_status(cancel_immediately, NULL, &cancelled_out) == ESP_ERR_TIMEOUT);
    assert(cancelled_out && transfers == 0 && ryz_display_completed_commits() == 4);
    assert(ryz_display_clear(0x5678) == ESP_OK);
    assert(ryz_display_show_checked_status(cancel_immediately, NULL, &cancelled_out) == ESP_ERR_TIMEOUT);
    assert(cancelled_out && transfers == 0 && ryz_display_completed_commits() == 4);
    assert(ryz_display_show_checked_status(cancel_after_stripe, NULL, &cancelled_out) == ESP_ERR_TIMEOUT);
    assert(cancelled_out && transfers == 1 && !pending && ryz_display_completed_commits() == 4);
    assert(ryz_display_show() == ESP_OK);
    assert(ryz_display_completed_commits() == 5);
    expected_commits = 5;

    reset_counts();
    assert(ryz_display_clear(0x6789) == ESP_OK);
    assert(ryz_display_show_checked_status(cancel_while_pending, NULL, &cancelled_out) == ESP_ERR_TIMEOUT);
    assert(cancelled_out && transfers == 1 && !pending && ryz_display_completed_commits() == 5);
    assert(ryz_display_show() == ESP_OK);
    assert(ryz_display_completed_commits() == 6);
    expected_commits = 6;

    reset_counts();
    assert(ryz_display_rect(2, 2, 2, 1, 0x789a) == ESP_OK);
    fail_draw = true;
    assert(ryz_display_show() == ESP_ERR_TIMEOUT);
    assert(!pending && transfers == 0 && ryz_display_completed_commits() == 6);
    fail_draw = true;
    assert(ryz_display_show_step(true, &done) == ESP_ERR_TIMEOUT);
    assert(!done && !pending && transfers == 0 && ryz_display_completed_commits() == 6);
    assert(ryz_display_show_step(true, &done) == ESP_OK && !done);
    assert(ryz_display_show_step(false, &done) == ESP_OK && done);
    assert(ryz_display_completed_commits() == 7);
    expected_commits = 7;
    puts("DISPLAY_COMMITS_PASS: initialization, empty calls, blocking/stepped transactions, abort, cancellation and failed-draw repair");
}

static void check_failed_final_drain(bool stepped) {
    assert(ryz_display_init() == ESP_OK);
    check_reference_init_contract();
    assert(ryz_display_completed_commits() == 1);
    expected_commits = 1;
    watching_commits = true;
    reset_counts();
    assert(ryz_display_rect(4, 4, 2, 1, 0x1234) == ESP_OK);
    if (stepped) {
        bool done = true;
        assert(ryz_display_show_step(true, &done) == ESP_OK && !done && pending);
        fail_drain = true;
        assert(ryz_display_show_step(false, &done) == ESP_ERR_TIMEOUT && !done);
    } else {
        /* A one-stripe transaction has no pending DMA on its initial drain;
         * this fault therefore hits the final completion, not an early stripe. */
        fail_drain = true;
        bool cancelled_out = true;
        assert(ryz_display_show_checked_status(NULL, NULL, &cancelled_out) == ESP_ERR_TIMEOUT);
        assert(!cancelled_out);
    }
    assert(pending && transfers == 0 && ryz_display_completed_commits() == 1);
    assert(ryz_display_show() == ESP_ERR_INVALID_STATE);
    assert(ryz_display_completed_commits() == 1);
    puts(stepped ? "DISPLAY_STEP_DRAIN_PASS: final DMA failure never increments" :
                   "DISPLAY_BLOCK_DRAIN_PASS: final DMA failure never increments");
}

static void check_privacy_clear(void) {
    assert(ryz_display_privacy_clear() == ESP_OK);
    assert(ryz_display_privacy_mark_sensitive() == ESP_ERR_INVALID_STATE);
    assert(display_io_calls == 0);
    assert(ryz_display_init() == ESP_OK);
    check_reference_init_contract();
    size_t calls = display_io_calls;
    assert(ryz_display_privacy_clear() == ESP_OK);
    assert(ryz_display_completed_commits() == 1 && display_io_calls == calls);
    assert(ryz_display_rect(4, 4, 1, 1, 0x1234) == ESP_OK);
    assert(ryz_display_privacy_clear() == ESP_OK && display_io_calls == calls);
    uint16_t ordinary_pixel = 0;
    assert(ryz_display_read_pixel(4, 4, &ordinary_pixel) == ESP_OK && ordinary_pixel == 0x1234);
    assert(ryz_display_privacy_mark_sensitive() == ESP_OK);
    assert(ryz_display_privacy_mark_sensitive() == ESP_OK);
    assert(display_io_calls == calls);
    assert(ryz_display_clear(0xf81f) == ESP_OK);
    assert(ryz_display_show() == ESP_OK);
    const uint32_t commits = ryz_display_completed_commits();
    privacy_must_be_dark = true;
    assert(ryz_display_privacy_clear() == ESP_OK);
    privacy_must_be_dark = false;
    assert(applied_backlight_duty == 127 && !pending);
    assert(ryz_display_completed_commits() == commits + 1);
    check_scrubbed_frame();
    for (int y = 0; y < 240; ++y) for (int x = 0; x < 240; ++x) check_pixel(x, y, 0);
    calls = display_io_calls;
    assert(ryz_display_privacy_clear() == ESP_OK && display_io_calls == calls);
    assert(ryz_display_completed_commits() == commits + 1);
    puts("DISPLAY_PRIVACY_PASS: dark-before-DMA, full memory scrub, black frame before backlight restore");
}

static void check_privacy_isolated(void) {
    const size_t calls = display_io_calls;
    const uint32_t commits = ryz_display_completed_commits();
    uint16_t pixel = 0xa5a5;
    const uint8_t big_endian[] = {0xff, 0xff};
    bool done = false, cancelled = true;
    assert(ryz_display_init() == ESP_ERR_INVALID_STATE);
    assert(ryz_display_privacy_mark_sensitive() == ESP_ERR_INVALID_STATE);
    assert(ryz_display_read_pixel(0, 0, &pixel) == ESP_ERR_INVALID_STATE);
    assert(pixel == 0xa5a5);
    assert(ryz_display_clear(0xffff) == ESP_ERR_INVALID_STATE);
    assert(ryz_display_rect(0, 0, 1, 1, 0xffff) == ESP_ERR_INVALID_STATE);
    assert(ryz_display_blit_rgb565_native(0, 0, 1, 1, &pixel, 1) == ESP_ERR_INVALID_STATE);
    assert(ryz_display_blit_rgb565_be(0, 0, 1, 1, big_endian, 2) == ESP_ERR_INVALID_STATE);
    assert(ryz_display_text(0, 0, "A", 1, 0xffff, 1) == ESP_ERR_INVALID_STATE);
    assert(ryz_display_show() == ESP_ERR_INVALID_STATE);
    assert(ryz_display_show_checked_status(cancel_immediately, NULL, &cancelled) == ESP_ERR_INVALID_STATE);
    assert(!cancelled);
    assert(ryz_display_show_step(true, &done) == ESP_ERR_INVALID_STATE);
    assert(ryz_display_show_step(false, &done) == ESP_ERR_INVALID_STATE);
    ryz_display_show_abort();
    assert(display_io_calls == calls && ryz_display_completed_commits() == commits);
}

static void check_privacy_fault(const char *fault) {
    assert(ryz_display_init() == ESP_OK);
    check_reference_init_contract();
    assert(ryz_display_privacy_mark_sensitive() == ESP_OK);
    assert(ryz_display_clear(0xa55a) == ESP_OK);
    assert(ryz_display_show() == ESP_OK);
    const uint32_t commits = ryz_display_completed_commits();
    bool done = true;
    esp_err_t expected_error = ESP_ERR_TIMEOUT;
    bool completed_black = false;
    if (strcmp(fault, "privacy-disable-set") == 0) {
        fail_ledc_set_after = 0; expected_error = ESP_ERR_INVALID_ARG;
    } else if (strcmp(fault, "privacy-disable-update") == 0) {
        fail_ledc_update_after = 0; expected_error = ESP_ERR_INVALID_ARG;
    } else if (strcmp(fault, "privacy-drain") == 0) {
        assert(ryz_display_rect(0, 0, 2, 1, 0xdead) == ESP_OK);
        assert(ryz_display_show_step(true, &done) == ESP_OK && !done && pending);
        fail_drain = true;
    } else if (strcmp(fault, "privacy-draw") == 0) {
        fail_draw = true;
    } else if (strcmp(fault, "privacy-final-drain") == 0) {
        fail_black_final_drain = true;
    } else if (strcmp(fault, "privacy-restore-set") == 0) {
        fail_ledc_set_after = 1; expected_error = ESP_ERR_INVALID_ARG; completed_black = true;
    } else if (strcmp(fault, "privacy-restore-update") == 0) {
        fail_ledc_update_after = 1; expected_error = ESP_ERR_INVALID_ARG; completed_black = true;
    } else assert(!"unknown privacy fault");
    privacy_must_be_dark = true;
    assert(ryz_display_privacy_clear() == expected_error);
    assert(fail_ledc_set_after == -1 && fail_ledc_update_after == -1);
    assert(!fail_draw && !fail_drain && !fail_black_final_drain);
    assert(ryz_display_completed_commits() == commits + (completed_black ? 1U : 0U));
    if (strstr(fault, "disable")) {
        /* Failed LEDC requests do not establish physical darkness. No DMA was
         * attempted; the isolation, not a fictitious black screen, is proven. */
        assert(applied_backlight_duty == 127);
    } else assert(applied_backlight_duty == 0);
    check_privacy_isolated();
    assert(ryz_display_privacy_clear() == ESP_OK);
    privacy_must_be_dark = false;
    assert(applied_backlight_duty == 127);
    check_scrubbed_frame();
    assert(ryz_display_completed_commits() == commits + (completed_black ? 2U : 1U));
    assert(spi_bus_init_calls == 1);
    check_reference_init_contract();
    assert(ryz_display_show_step(false, &done) == ESP_ERR_INVALID_STATE);
    assert(ryz_display_rect(0, 0, 1, 1, 0x1234) == ESP_OK);
    assert(ryz_display_show() == ESP_OK);
    check_pixel(0, 0, 0x1234);
    printf("DISPLAY_PRIVACY_FAULT_PASS: %s isolated then explicit black-only recovery\n", fault);
}

static void check_privacy_pending(bool previously_failed) {
    assert(ryz_display_init() == ESP_OK);
    check_reference_init_contract();
    assert(ryz_display_privacy_mark_sensitive() == ESP_OK);
    assert(ryz_display_clear(0xbabe) == ESP_OK);
    assert(ryz_display_show() == ESP_OK);
    assert(ryz_display_rect(4, 4, 2, 1, 0xdead) == ESP_OK);
    const uint32_t commits = ryz_display_completed_commits();
    bool done = true;
    if (previously_failed) {
        fail_drain = true;
        assert(ryz_display_show() == ESP_ERR_TIMEOUT && pending);
        /* Normal initialization must not allocate over an uncertain sensitive
         * DMA, even before the first explicit privacy_clear call. */
        assert(ryz_display_init() == ESP_ERR_INVALID_STATE);
        assert(ryz_display_read_pixel(4, 4, &(uint16_t){0}) == ESP_ERR_INVALID_STATE);
    } else {
        assert(ryz_display_show_step(true, &done) == ESP_OK && !done && pending);
        const size_t calls = display_io_calls;
        assert(ryz_display_privacy_mark_sensitive() == ESP_ERR_INVALID_STATE);
        assert(display_io_calls == calls);
    }
    assert(ryz_display_completed_commits() == commits);
    privacy_must_be_dark = true;
    /* The pending-source byte spy rejects wiping or buffer reuse before drain. */
    assert(ryz_display_privacy_clear() == ESP_OK);
    privacy_must_be_dark = false;
    check_scrubbed_frame();
    assert(ryz_display_completed_commits() == commits + 1);
    assert(ryz_display_show_step(false, &done) == ESP_ERR_INVALID_STATE);
    assert(spi_bus_init_calls == 1);
    check_reference_init_contract();
    assert(ryz_display_privacy_mark_sensitive() == ESP_OK);
    privacy_must_be_dark = true;
    assert(ryz_display_privacy_clear() == ESP_OK);
    privacy_must_be_dark = false;
    puts(previously_failed ? "DISPLAY_PRIVACY_UNREADY_PASS: retained failed DMA safely drained without board reinit" :
                             "DISPLAY_PRIVACY_PENDING_PASS: active stepped DMA drained before scrub and cannot resume");
}

static uint16_t native_frame[RYZ_DISPLAY_WIDTH * RYZ_DISPLAY_HEIGHT];

/* Reproduce the bridge's real partial-mode handoff: fifteen 240x16 native
 * blits, then one checked show after LVGL reports its final area. */
static void native_frame_blit(void) {
    for (int y = 0; y < RYZ_DISPLAY_HEIGHT; y += 16) {
        assert(ryz_display_blit_rgb565_native(0, y, RYZ_DISPLAY_WIDTH, 16,
            native_frame + y * RYZ_DISPLAY_WIDTH, RYZ_DISPLAY_WIDTH * 16) == ESP_OK);
    }
}

static void prepare_native_frame(void) {
    assert(ryz_display_init() == ESP_OK);
    check_reference_init_contract();
    for (size_t i = 0; i < sizeof(native_frame) / sizeof(native_frame[0]); ++i)
        native_frame[i] = 0x1234;
    native_frame_blit();
    assert(ryz_display_show() == ESP_OK);
    reset_counts();
}

static void check_native_unchanged(void) {
    prepare_native_frame();
    const uint32_t commits = ryz_display_completed_commits();
    const size_t io_calls = display_io_calls;
    native_frame_blit();
    bool cancelled_out = true;
    assert(ryz_display_show_checked_status(NULL, NULL, &cancelled_out) == ESP_OK);
    fprintf(stderr, "DISPLAY_NATIVE_IDENTICAL: transfers=%zu bytes=%zu io_calls=%zu\n",
            transfers, transferred_bytes, display_io_calls - io_calls);
    assert(!cancelled_out && !pending && transfers == 0 && transferred_bytes == 0);
    assert(display_io_calls == io_calls && ryz_display_completed_commits() == commits);
    /* Empty work must not bypass the checked cancellation entry point. */
    cancelled_out = false;
    assert(ryz_display_show_checked_status(cancel_immediately, NULL, &cancelled_out) == ESP_ERR_TIMEOUT);
    assert(cancelled_out && display_io_calls == io_calls);
    bool done = false;
    assert(ryz_display_show_step(true, &done) == ESP_OK && done);
    assert(display_io_calls == io_calls && ryz_display_completed_commits() == commits);
    for (int y = 0; y < RYZ_DISPLAY_HEIGHT; ++y)
        for (int x = 0; x < RYZ_DISPLAY_WIDTH; ++x) check_pixel(x, y, 0x1234);
    puts("DISPLAY_NATIVE_IDENTICAL_PASS: identical complete frame has no SPI or new completed commit");
}

static void check_native_one_pixel(void) {
    prepare_native_frame();
    const uint32_t commits = ryz_display_completed_commits();
    native_frame[101 * RYZ_DISPLAY_WIDTH + 81] = 0xf800;
    native_frame_blit();
    assert(ryz_display_show_checked_status(NULL, NULL, NULL) == ESP_OK);
    fprintf(stderr, "DISPLAY_NATIVE_ONE_PIXEL: transfers=%zu bytes=%zu last_window=[%d,%d,%d,%d]\n",
            transfers, transferred_bytes, px0, py0, px1, py1);
    assert(transfers == 1 && transferred_bytes == 4 && !pending);
    assert(px0 == 80 && px1 == 82 && py0 == 101 && py1 == 102);
    assert(ryz_display_completed_commits() == commits + 1);
    for (int y = 0; y < RYZ_DISPLAY_HEIGHT; ++y)
        for (int x = 0; x < RYZ_DISPLAY_WIDTH; ++x)
            check_pixel(x, y, x == 81 && y == 101 ? 0xf800 : 0x1234);
    puts("DISPLAY_NATIVE_ONE_PIXEL_PASS: one changed pixel in a complete frame sends one aligned 2x1 window");
}

static void check_native_repair(void) {
    prepare_native_frame();
    uint32_t commits = ryz_display_completed_commits();
    for (size_t i = 0; i < sizeof(native_frame) / sizeof(native_frame[0]); ++i)
        native_frame[i] = 0x3456;
    native_frame_blit();
    bool cancelled_out = false;
    assert(ryz_display_show_checked_status(cancel_after_stripe, NULL, &cancelled_out) == ESP_ERR_TIMEOUT);
    assert(cancelled_out && transfers == 1 && !pending);
    assert(lcd[0][0] == 0x3456 && lcd[239][239] == 0x1234);
    assert(ryz_display_completed_commits() == commits);
    reset_counts();
    native_frame_blit(); /* Already matches canvas, not the partially sent panel. */
    assert(ryz_display_show_checked_status(NULL, NULL, &cancelled_out) == ESP_OK);
    assert(!cancelled_out && transfers == 15 && transferred_bytes == sizeof(native_frame));
    assert(ryz_display_completed_commits() == ++commits);
    for (int y = 0; y < RYZ_DISPLAY_HEIGHT; ++y)
        for (int x = 0; x < RYZ_DISPLAY_WIDTH; ++x) check_pixel(x, y, 0x3456);
    fprintf(stderr, "DISPLAY_NATIVE_CANCEL_REPAIR: transfers=%zu bytes=%zu\n", transfers, transferred_bytes);

    reset_counts();
    native_frame[101 * RYZ_DISPLAY_WIDTH + 81] = 0xabcd;
    native_frame_blit();
    fail_draw = true;
    cancelled_out = true;
    assert(ryz_display_show_checked_status(NULL, NULL, &cancelled_out) == ESP_ERR_TIMEOUT);
    assert(!cancelled_out && !pending && !transfers && ryz_display_completed_commits() == commits);
    native_frame_blit(); /* Failed draw's unchanged source must still repair. */
    assert(ryz_display_show() == ESP_OK);
    assert(transfers == 1 && transferred_bytes == 4);
    assert(ryz_display_completed_commits() == commits + 1);
    check_pixel(81, 101, 0xabcd); check_pixel(80, 101, 0x3456);
    fprintf(stderr, "DISPLAY_NATIVE_FAILED_DRAW_REPAIR: transfers=%zu bytes=%zu\n", transfers, transferred_bytes);
    puts("DISPLAY_NATIVE_REPAIR_PASS: identical input preserves prior cancelled and failed dirty windows");
}

static void check_native_bounds(void) {
    prepare_native_frame();
    const uint16_t partial[] = {0x1234, 0x1234, 0x1234, 0x1234, 0x1234, 0xabcd};
    assert(ryz_display_blit_rgb565_native(31, 45, 3, 2, partial, 6) == ESP_OK);
    assert(ryz_display_show() == ESP_OK);
    assert(transfers == 1 && transferred_bytes == 4);
    assert(px0 == 32 && py0 == 46 && px1 == 34 && py1 == 47);
    check_pixel(31, 45, 0x1234); check_pixel(32, 46, 0x1234); check_pixel(33, 46, 0xabcd);
    reset_counts();
    const uint16_t first = 0x07e0, last = 0x001f;
    assert(ryz_display_blit_rgb565_native(0, 0, 1, 1, &first, 1) == ESP_OK);
    assert(ryz_display_show() == ESP_OK);
    assert(transfers == 1 && transferred_bytes == 4);
    assert(px0 == 0 && py0 == 0 && px1 == 2 && py1 == 1);
    check_pixel(0, 0, first); check_pixel(1, 0, 0x1234);
    reset_counts();
    assert(ryz_display_blit_rgb565_native(239, 239, 1, 1, &last, 1) == ESP_OK);
    assert(ryz_display_show() == ESP_OK);
    assert(transfers == 1 && transferred_bytes == 4);
    assert(px0 == 238 && py0 == 239 && px1 == 240 && py1 == 240);
    check_pixel(239, 239, last); check_pixel(238, 239, 0x1234);

    /* Existing dirt must union with later blits, even when distant changes
     * necessarily make the single bounding window as large as the screen. */
    reset_counts();
    assert(ryz_display_blit_rgb565_native(0, 0, 1, 1, &last, 1) == ESP_OK);
    assert(ryz_display_blit_rgb565_native(239, 239, 1, 1, &first, 1) == ESP_OK);
    assert(ryz_display_show() == ESP_OK);
    assert(transfers == 15 && transferred_bytes == sizeof(native_frame));
    for (int y = 0; y < RYZ_DISPLAY_HEIGHT; ++y) {
        for (int x = 0; x < RYZ_DISPLAY_WIDTH; ++x) {
            uint16_t expected = x == 0 && y == 0 ? last :
                x == 239 && y == 239 ? first : x == 33 && y == 46 ? 0xabcd : 0x1234;
            check_pixel(x, y, expected);
        }
    }
    puts("DISPLAY_NATIVE_BOUNDS_PASS: offset source rows, first/last pixel alignment and existing dirty union");
}

static void check_native_privacy_black(void) {
    assert(ryz_display_init() == ESP_OK);
    check_reference_init_contract();
    memset(native_frame, 0, sizeof(native_frame));
    reset_counts();
    assert(ryz_display_privacy_mark_sensitive() == ESP_OK);
    native_frame_blit();
    assert(ryz_display_show() == ESP_OK && !transfers);
    const uint32_t commits = ryz_display_completed_commits();
    privacy_must_be_dark = true;
    assert(ryz_display_privacy_clear() == ESP_OK);
    privacy_must_be_dark = false;
    assert(!pending && transfers == 15 && transferred_bytes == sizeof(native_frame));
    assert(ryz_display_completed_commits() == commits + 1 && applied_backlight_duty == 127);
    check_scrubbed_frame();
    fprintf(stderr, "DISPLAY_NATIVE_PRIVACY_BLACK: transfers=%zu bytes=%zu\n", transfers, transferred_bytes);
    puts("DISPLAY_NATIVE_PRIVACY_BLACK_PASS: already-black canvas cannot elide required full privacy repaint");
}

static void check_configuration(void) {
    assert(ryz_display_apply_configuration(0,50)==ESP_ERR_INVALID_STATE);
    assert(ryz_display_init()==ESP_OK);
    check_reference_init_contract();
    runtime_configuration=true;
    assert(ryz_display_apply_configuration(4,50)==ESP_ERR_INVALID_ARG);
    assert(ryz_display_apply_configuration(0,9)==ESP_ERR_INVALID_ARG);
    assert(ryz_display_apply_configuration(0,51)==ESP_ERR_INVALID_ARG);
    assert(ryz_display_apply_configuration(0,50)==ESP_OK && madctl_calls==0);
    const uint8_t values[]={0x00,0x60,0xc0,0xa0};
    const int gaps_x[]={0,0,0,80},gaps_y[]={0,0,80,0};
    for(unsigned i=0;i<4;++i) {
        assert(ryz_display_apply_configuration(i,100)==ESP_OK);
        assert(madctl_value==values[i] && gap_x==gaps_x[i] && gap_y==gaps_y[i]);
        assert(applied_backlight_duty==255 && !ryz_display_configuration_blocked());
    }
    assert(ryz_display_apply_configuration(3,10)==ESP_OK && applied_backlight_duty==25);
    assert(ryz_display_set_asleep(true)==ESP_OK && applied_backlight_duty==0);
    assert(ryz_display_apply_configuration(1,80)==ESP_OK && applied_backlight_duty==0);
    assert(ryz_display_set_asleep(false)==ESP_OK && applied_backlight_duty==204);
    /* A failed off update leaves physical PWM uncertain. Confirming awake
     * must perform actual LEDC writes even though logical asleep stayed false. */
    fail_ledc_update_after=0;
    assert(ryz_display_set_asleep(true)==ESP_ERR_INVALID_ARG);
    size_t wake_io=display_io_calls;
    assert(ryz_display_set_asleep(false)==ESP_OK && applied_backlight_duty==204);
    assert(display_io_calls==wake_io+2);
    assert(ryz_display_privacy_mark_sensitive()==ESP_OK);
    assert(ryz_display_apply_configuration(2,50)==ESP_ERR_INVALID_STATE);
    assert(ryz_display_privacy_clear()==ESP_OK && applied_backlight_duty==204);
    /* A failed new orientation is not Saved. Safe rollback restores both
     * address mapping and current brightness, never changes touch mapping. */
    fail_madctl_count=1;
    assert(ryz_display_apply_configuration(2,50)==ESP_ERR_TIMEOUT);
    assert(!ryz_display_configuration_blocked());
    assert(madctl_value==0x60 && gap_x==0 && gap_y==0 && applied_backlight_duty==204);
    /* If rollback is uncertain, every public draw/init/wake path stays shut. */
    fail_madctl_count=2;
    assert(ryz_display_apply_configuration(2,50)==ESP_ERR_TIMEOUT);
    assert(ryz_display_configuration_blocked() && applied_backlight_duty==0);
    assert(ryz_display_init()==ESP_ERR_INVALID_STATE);
    assert(ryz_display_clear(0)==ESP_ERR_INVALID_STATE);
    assert(ryz_display_set_asleep(false)==ESP_ERR_INVALID_STATE);
    assert(ryz_display_apply_configuration(2,50)==ESP_OK);
    assert(!ryz_display_configuration_blocked() && applied_backlight_duty==127);
    assert(madctl_value==0xc0 && gap_y==80);
    assert(ryz_display_privacy_mark_sensitive()==ESP_OK);
    fail_ledc_set_after=0;
    assert(ryz_display_privacy_clear()!=ESP_OK);
    assert(ryz_display_set_asleep(false)==ESP_ERR_INVALID_STATE);
    assert(ryz_display_apply_configuration(0,100)==ESP_ERR_INVALID_STATE);
    puts("DISPLAY_CONFIGURATION_PASS: four MADCTL/gap pairs, PWM, sleep, privacy and rollback barrier");
}

static void check_brightness_configuration(void) {
    assert(ryz_display_init() == ESP_OK);
    runtime_configuration = true;
    reset_counts();
    const size_t before = display_io_calls;
    const unsigned gaps = gap_calls;
    const uint32_t commits = ryz_display_completed_commits();
    assert(ryz_display_apply_configuration(0, 75) == ESP_OK);
    assert(applied_backlight_duty == 191);
    assert(display_io_calls == before + 2); /* LEDC set/update only; no dark repaint. */
    assert(!madctl_calls && gap_calls == gaps && !transfers && !pending);
    assert(ryz_display_completed_commits() == commits);
    puts("DISPLAY_BRIGHTNESS_CONFIGURATION_PASS: same-direction SAVE changes PWM only");
}

static void check_brightness_preview(void) {
    assert(ryz_display_set_brightness(50) == ESP_ERR_INVALID_STATE);
    assert(display_io_calls == 0);
    assert(ryz_display_init() == ESP_OK);
    runtime_configuration = true;
    assert(ryz_display_rect(5, 7, 1, 1, 0x1234) == ESP_OK);
    assert(ryz_display_show() == ESP_OK);
    reset_counts();
    const unsigned gaps = gap_calls;
    const size_t zeros = ledc_zero_calls;
    const uint32_t commits = ryz_display_completed_commits();
    for (unsigned value = 0; value <= UINT8_MAX; ++value) {
        size_t before = display_io_calls;
        if (value >= 10 && value <= 100 && value % 5 == 0) {
            assert(ryz_display_set_brightness(value) == ESP_OK);
            assert(applied_backlight_duty == (int)(value * 255U / 100U));
            assert(display_io_calls == before + 2);
            before = display_io_calls;
            assert(ryz_display_set_brightness(value) == ESP_OK);
            assert(display_io_calls == before); /* Same preview is zero IO. */
        } else {
            assert(ryz_display_set_brightness(value) == ESP_ERR_INVALID_ARG);
            assert(display_io_calls == before);
        }
    }
    assert(!madctl_calls && gap_calls == gaps && !transfers && !pending);
    assert(ledc_zero_calls == zeros && ryz_display_completed_commits() == commits);
    check_pixel(5, 7, 0x1234);
    assert(ryz_display_set_asleep(true) == ESP_OK && applied_backlight_duty == 0);
    size_t before = display_io_calls;
    assert(ryz_display_set_brightness(55) == ESP_OK);
    assert(ryz_display_set_brightness(65) == ESP_OK);
    assert(ryz_display_set_brightness(65) == ESP_OK);
    assert(display_io_calls == before && applied_backlight_duty == 0);
    assert(ryz_display_set_asleep(false) == ESP_OK && applied_backlight_duty == 165);
    assert(ryz_display_set_asleep(true) == ESP_OK);
    fail_ledc_update_after = 0;
    assert(ryz_display_set_asleep(false) == ESP_ERR_INVALID_ARG);
    assert(ryz_display_configuration_blocked());
    before = display_io_calls;
    assert(ryz_display_set_brightness(80) == ESP_OK);
    assert(display_io_calls == before + 2 && applied_backlight_duty == 0);
    assert(!ryz_display_configuration_blocked());
    assert(ryz_display_set_asleep(false) == ESP_OK && applied_backlight_duty == 204);
    assert(!madctl_calls && gap_calls == gaps && !transfers && !pending);
    assert(ryz_display_completed_commits() == commits);
    puts("DISPLAY_BRIGHTNESS_PREVIEW_PASS: all 256 inputs, same-value zero IO, PWM-only, preserved canvas and sleep duty");
}

static void check_brightness_fault(bool update, bool rollback) {
    assert(ryz_display_init() == ESP_OK);
    runtime_configuration = true;
    reset_counts();
    const unsigned gaps = gap_calls;
    const size_t before = display_io_calls, sets = ledc_set_calls;
    const size_t updates = ledc_update_calls, zeros = ledc_zero_calls;
    const uint32_t commits = ryz_display_completed_commits();
    if (update) fail_ledc_update_count = rollback ? 2 : 1;
    else fail_ledc_set_count = rollback ? 2 : 1;
    assert(ryz_display_set_brightness(80) == ESP_ERR_INVALID_ARG);
    assert(ryz_display_configuration_blocked() == rollback);
    assert(ledc_set_calls == sets + 2);
    assert(ledc_update_calls == updates + (update ? 2 : rollback ? 0 : 1));
    assert(display_io_calls == before + (update ? 4 : rollback ? 2 : 3));
    assert(!madctl_calls && gap_calls == gaps && !transfers && !pending);
    assert(ledc_zero_calls == zeros && ryz_display_completed_commits() == commits);
    const size_t retry_before = display_io_calls;
    assert(ryz_display_set_brightness(50) == ESP_OK);
    assert(display_io_calls == retry_before + (rollback ? 2 : 0));
    assert(!ryz_display_configuration_blocked() && applied_backlight_duty == 127);
    assert(ryz_display_set_asleep(true) == ESP_OK && applied_backlight_duty == 0);
    assert(ryz_display_set_asleep(false) == ESP_OK && applied_backlight_duty == 127);
    assert(!madctl_calls && gap_calls == gaps && !transfers && !pending);
    printf("DISPLAY_BRIGHTNESS_FAULT_PASS update=%d rollback_failed=%d\n", update, rollback);
}

static void check_brightness_guards(void) {
    assert(ryz_display_init() == ESP_OK);
    runtime_configuration = true;
    assert(ryz_display_rect(1, 1, 1, 1, 0x1234) == ESP_OK);
    bool done = true;
    assert(ryz_display_show_step(true, &done) == ESP_OK && !done && pending);
    size_t before = display_io_calls;
    assert(ryz_display_set_brightness(80) == ESP_ERR_INVALID_STATE);
    assert(display_io_calls == before && pending);
    ryz_display_show_abort();
    assert(ryz_display_privacy_mark_sensitive() == ESP_OK);
    before = display_io_calls;
    assert(ryz_display_set_brightness(50) == ESP_ERR_INVALID_STATE);
    assert(ryz_display_set_brightness(80) == ESP_ERR_INVALID_STATE);
    assert(display_io_calls == before);
    fail_ledc_set_after = 0;
    assert(ryz_display_privacy_clear() == ESP_ERR_INVALID_ARG);
    before = display_io_calls;
    assert(ryz_display_set_brightness(80) == ESP_ERR_INVALID_STATE);
    assert(display_io_calls == before);
    assert(ryz_display_privacy_clear() == ESP_OK);
    fail_madctl_count = 2;
    assert(ryz_display_apply_configuration(1, 80) == ESP_ERR_TIMEOUT);
    assert(ryz_display_configuration_blocked());
    before = display_io_calls;
    assert(ryz_display_set_brightness(80) == ESP_ERR_INVALID_STATE);
    assert(display_io_calls == before);
    assert(ryz_display_apply_configuration(0, 50) == ESP_OK);
    assert(!ryz_display_configuration_blocked());
    assert(ryz_display_rect(1, 1, 1, 1, 0x3456) == ESP_OK);
    fail_drain = true;
    assert(ryz_display_show() == ESP_ERR_TIMEOUT);
    before = display_io_calls;
    assert(ryz_display_set_brightness(80) == ESP_ERR_INVALID_STATE);
    assert(display_io_calls == before);
    puts("DISPLAY_BRIGHTNESS_GUARDS_PASS: active DMA, sensitive page, failed privacy/rotation and unready transfer reject without IO");
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "boot-configuration")) {
        unsigned rotation = (unsigned)strtoul(argv[2], NULL, 10);
        assert(rotation < 4); check_boot_configuration((uint8_t)rotation); return 0;
    }
    assert(argc == 1 || argc == 2);
    if (argc == 2) {
        if (strcmp(argv[1], "configuration") == 0) check_configuration();
        else if (strcmp(argv[1], "brightness-configuration") == 0) check_brightness_configuration();
        else if (strcmp(argv[1], "brightness-preview") == 0) check_brightness_preview();
        else if (strcmp(argv[1], "brightness-set") == 0) check_brightness_fault(false, false);
        else if (strcmp(argv[1], "brightness-update") == 0) check_brightness_fault(true, false);
        else if (strcmp(argv[1], "brightness-rollback-set") == 0) check_brightness_fault(false, true);
        else if (strcmp(argv[1], "brightness-rollback-update") == 0) check_brightness_fault(true, true);
        else if (strcmp(argv[1], "brightness-guards") == 0) check_brightness_guards();
        else if (strcmp(argv[1], "commits") == 0) check_completed_commits();
        else if (strcmp(argv[1], "block-drain") == 0) check_failed_final_drain(false);
        else if (strcmp(argv[1], "step-drain") == 0) check_failed_final_drain(true);
        else if (strcmp(argv[1], "privacy") == 0) check_privacy_clear();
        else if (strcmp(argv[1], "privacy-pending") == 0) check_privacy_pending(false);
        else if (strcmp(argv[1], "privacy-unready") == 0) check_privacy_pending(true);
        else if (strcmp(argv[1], "native-identical") == 0) check_native_unchanged();
        else if (strcmp(argv[1], "native-one-pixel") == 0) check_native_one_pixel();
        else if (strcmp(argv[1], "native-repair") == 0) check_native_repair();
        else if (strcmp(argv[1], "native-bounds") == 0) check_native_bounds();
        else if (strcmp(argv[1], "native-privacy-black") == 0) check_native_privacy_black();
        else if (strncmp(argv[1], "privacy-", 8) == 0) check_privacy_fault(argv[1]);
        else assert(!"unknown display test scenario");
        return 0;
    }
    assert(ryz_display_init() == ESP_OK);
    check_reference_init_contract();
    reset_counts();
    assert(ryz_display_show() == ESP_OK);
    assert(transfers == 0); /* This is red on the old always-full-screen driver. */
    assert(ryz_display_clear(0x1234) == ESP_OK);
    assert(ryz_display_show() == ESP_OK);
    assert(transferred_bytes == 240 * 240 * 2);
    check_pixel(0, 0, 0x1234); check_pixel(239, 239, 0x1234);
    reset_counts();
    assert(ryz_display_rect(81, 101, 3, 2, 0xf800) == ESP_OK);
    assert(ryz_display_show() == ESP_OK);
    assert(transferred_bytes <= 4 * 2 * 2);
    check_pixel(80, 101, 0x1234); check_pixel(81, 101, 0xf800);
    check_pixel(83, 102, 0xf800); check_pixel(84, 102, 0x1234);
    reset_counts();
    assert(ryz_display_rect(0, 0, 1, 1, 0x07e0) == ESP_OK);
    assert(ryz_display_rect(239, 239, 1, 1, 0x001f) == ESP_OK);
    assert(ryz_display_show() == ESP_OK);
    check_pixel(0, 0, 0x07e0); check_pixel(239, 239, 0x001f);
    reset_counts();
    assert(ryz_display_rect(239, 239, 2, 1, 0) == ESP_ERR_INVALID_ARG);
    const uint16_t native_pixels[] = {
        0xf800, 0x07e0, 0x001f,
        0xffff, 0x1234, 0xabcd,
    };
    reset_counts();
    assert(ryz_display_blit_rgb565_native(
               31, 45, 3, 2, native_pixels,
               sizeof(native_pixels) / sizeof(native_pixels[0])) == ESP_OK);
    assert(ryz_display_show() == ESP_OK);
    assert(transferred_bytes == 4 * 2 * 2); /* even-word DMA expansion */
    check_pixel(30, 45, 0x1234);
    check_pixel(31, 45, 0xf800);
    check_pixel(32, 45, 0x07e0);
    check_pixel(33, 45, 0x001f);
    check_pixel(31, 46, 0xffff);
    check_pixel(32, 46, 0x1234);
    check_pixel(33, 46, 0xabcd);
    check_pixel(34, 46, 0x1234);

    reset_counts();
    assert(ryz_display_blit_rgb565_native(
               0, 0, 3, 2, NULL, 6) == ESP_ERR_INVALID_ARG);
    assert(ryz_display_blit_rgb565_native(
               0, 0, 3, 2, native_pixels, 5) == ESP_ERR_INVALID_ARG);
    assert(ryz_display_blit_rgb565_native(
               0, 0, 3, 2, native_pixels, 7) == ESP_ERR_INVALID_ARG);
    assert(ryz_display_blit_rgb565_native(
               -1, 0, 3, 2, native_pixels, 6) == ESP_ERR_INVALID_ARG);
    assert(ryz_display_blit_rgb565_native(
               238, 0, 3, 2, native_pixels, 6) == ESP_ERR_INVALID_ARG);
    assert(ryz_display_blit_rgb565_native(
               0, 239, 3, 2, native_pixels, 6) == ESP_ERR_INVALID_ARG);
    assert(ryz_display_blit_rgb565_native(
               0, 0, 0, 2, native_pixels, 0) == ESP_ERR_INVALID_ARG);
    assert(ryz_display_blit_rgb565_native(
               0, 0, 3, 0, native_pixels, 0) == ESP_ERR_INVALID_ARG);
    assert(ryz_display_show() == ESP_OK && transfers == 0);
    last_error_tag[0] = '\0';
    last_error_log[0] = '\0';
    assert(ryz_display_text(1, 1, "bad", 3, 0xffff, 1) == ESP_ERR_NOT_SUPPORTED);
    assert(strcmp(last_error_tag, "ryz_display") == 0);
    assert(strcmp(last_error_log,
                  "display.text unsupported glyph at index=0") == 0);
    assert(ryz_display_show() == ESP_OK && transfers == 0);
    /* The system UI uses these chevrons on the first HOME frame and AP back
     * control. Exercise the production font lookup, not a permissive UI spy. */
    assert(ryz_display_text(10, 40, "<>", 2, 0xffff, 1) == ESP_OK);
    assert(ryz_display_show() == ESP_OK);
    check_pixel(13, 40, 0xffff); /* '<' top diagonal */
    check_pixel(10, 43, 0xffff); /* '<' point */
    check_pixel(17, 40, 0xffff); /* '>' top diagonal */
    check_pixel(20, 43, 0xffff); /* '>' point */
    reset_counts();
    assert(ryz_display_text(10, 20, "HI", 2, 0xffff, 2) == ESP_OK);
    assert(ryz_display_show() == ESP_OK);
    assert(transferred_bytes <= 24 * 14 * 2);
    check_pixel(10, 20, 0xffff); check_pixel(12, 20, 0x1234);
    reset_counts();
    assert(ryz_display_clear(0x55aa) == ESP_OK);
    bool cancelled_out = false;
    assert(ryz_display_show_checked_status(
               cancel_after_stripe, NULL, &cancelled_out) == ESP_ERR_TIMEOUT);
    assert(cancelled_out);
    assert(transfers == 1 && !pending);
    reset_counts();
    assert(ryz_display_show() == ESP_OK);
    assert(transferred_bytes == 240 * 240 * 2);
    check_pixel(239, 239, 0x55aa);
    reset_counts();
    assert(ryz_display_rect(100, 100, 2, 2, 0) == ESP_OK);
    fail_draw = true;
    assert(ryz_display_show() == ESP_ERR_TIMEOUT && !pending);
    assert(ryz_display_show() == ESP_OK);
    check_pixel(100, 100, 0); check_pixel(102, 100, 0x55aa);
    reset_counts();
    assert(ryz_display_clear(0x2345) == ESP_OK);
    assert(ryz_display_show_checked(cancel_while_pending, NULL) == ESP_ERR_TIMEOUT);
    assert(transfers == 1 && !pending);
    assert(ryz_display_clear(0x3456) == ESP_OK);
    assert(ryz_display_show() == ESP_OK);
    check_pixel(0, 0, 0x3456); check_pixel(239, 239, 0x3456);
    reset_counts();
    assert(ryz_display_clear(0x4567) == ESP_OK);
    const uint32_t before_failed_drain = ryz_display_completed_commits();
    cancelled_out = true;
    fail_drain = true;
    assert(ryz_display_show_checked_status(
               cancel_while_pending, NULL, &cancelled_out) == ESP_ERR_TIMEOUT);
    assert(!cancelled_out);
    assert(ryz_display_completed_commits() == before_failed_drain);
    puts("DISPLAY_DRIVER_PASS: dirty windows, RGB565, stride, bounds, cancellation, failed-transfer retry");
    return 0;
}
