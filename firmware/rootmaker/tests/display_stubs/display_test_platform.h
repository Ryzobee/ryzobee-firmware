#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Host-only ESP-IDF boundary. The production display.c is compiled unchanged. */
typedef int esp_err_t;
enum { ESP_OK, ESP_ERR_INVALID_STATE, ESP_ERR_INVALID_ARG, ESP_ERR_NO_MEM,
       ESP_ERR_NOT_SUPPORTED, ESP_ERR_TIMEOUT };
enum { GPIO_MODE_OUTPUT, SPI2_HOST = 2, SPI_DMA_CH_AUTO,
       MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2, MALLOC_CAP_INTERNAL = 4,
       MALLOC_CAP_DMA = 8, LCD_RGB_ELEMENT_ORDER_RGB, LCD_RGB_ELEMENT_ORDER_BGR,
       LCD_RGB_DATA_ENDIAN_BIG,
       LEDC_LOW_SPEED_MODE, LEDC_TIMER_8_BIT, LEDC_TIMER_0, LEDC_AUTO_CLK,
       LEDC_CHANNEL_0 };
#define pdMS_TO_TICKS(ms) (ms)
typedef struct { uint64_t pin_bit_mask; int mode; } gpio_config_t;
typedef struct { int sclk_io_num, mosi_io_num, miso_io_num, quadwp_io_num,
    quadhd_io_num, max_transfer_sz; } spi_bus_config_t;
typedef struct { int cs_gpio_num, dc_gpio_num, spi_mode, pclk_hz,
    trans_queue_depth, lcd_cmd_bits, lcd_param_bits; } esp_lcd_panel_io_spi_config_t;
typedef struct { int reset_gpio_num, rgb_ele_order, data_endian,
    bits_per_pixel; } esp_lcd_panel_dev_config_t;
typedef struct { int speed_mode, duty_resolution, timer_num, freq_hz,
    clk_cfg; } ledc_timer_config_t;
typedef struct { int gpio_num, speed_mode, channel, timer_sel, duty,
    hpoint; } ledc_channel_config_t;
typedef void *esp_lcd_panel_io_handle_t;
typedef void *esp_lcd_panel_handle_t;
esp_err_t gpio_config(const gpio_config_t *);
esp_err_t gpio_set_level(int, int);
void *heap_caps_calloc(size_t, size_t, int);
void *heap_caps_malloc(size_t, int);
void heap_caps_free(void *);
esp_err_t spi_bus_initialize(int, const spi_bus_config_t *, int);
esp_err_t spi_bus_free(int);
esp_err_t esp_lcd_new_panel_io_spi(int, const esp_lcd_panel_io_spi_config_t *, esp_lcd_panel_io_handle_t *);
esp_err_t esp_lcd_new_panel_st7789(esp_lcd_panel_io_handle_t, const esp_lcd_panel_dev_config_t *, esp_lcd_panel_handle_t *);
esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t);
esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t);
esp_err_t esp_lcd_panel_invert_color(esp_lcd_panel_handle_t, bool);
esp_err_t esp_lcd_panel_set_gap(esp_lcd_panel_handle_t, int, int);
esp_err_t esp_lcd_panel_mirror(esp_lcd_panel_handle_t, bool, bool);
esp_err_t esp_lcd_panel_swap_xy(esp_lcd_panel_handle_t, bool);
esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t, bool);
esp_err_t esp_lcd_panel_del(esp_lcd_panel_handle_t);
esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t);
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t, int, int, int, int, const void *);
esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t, int, const void *, size_t);
esp_err_t ledc_timer_config(const ledc_timer_config_t *);
esp_err_t ledc_channel_config(const ledc_channel_config_t *);
esp_err_t ledc_set_duty(int, int, int);
esp_err_t ledc_update_duty(int, int);
esp_err_t ledc_stop(int, int, int);
void vTaskDelay(int);
