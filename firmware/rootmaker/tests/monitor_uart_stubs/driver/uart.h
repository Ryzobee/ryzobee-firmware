#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
typedef int uart_port_t;
enum { UART_NUM_1 = 1, UART_DATA_8_BITS = 3, UART_PARITY_DISABLE = 0,
       UART_STOP_BITS_1 = 1, UART_HW_FLOWCTRL_DISABLE = 0, UART_SCLK_APB = 1 };
#define UART_PIN_NO_CHANGE (-1)
typedef struct {
    int baud_rate, data_bits, parity, stop_bits, flow_ctrl;
    uint8_t rx_flow_ctrl_thresh;
    int source_clk;
    struct { unsigned allow_pd : 1; unsigned backup_before_sleep : 1; } flags;
} uart_config_t;
typedef enum { UART_DATA, UART_BREAK, UART_BUFFER_FULL, UART_FIFO_OVF,
               UART_FRAME_ERR, UART_PARITY_ERR, UART_DATA_BREAK,
               UART_PATTERN_DET, UART_WAKEUP, UART_EVENT_MAX } uart_event_type_t;
typedef struct { uart_event_type_t type; size_t size; bool timeout_flag; } uart_event_t;
bool uart_is_driver_installed(uart_port_t port);
esp_err_t uart_driver_install(uart_port_t port, int rx, int tx, int depth,
                             QueueHandle_t *queue, int flags);
esp_err_t uart_param_config(uart_port_t port, const uart_config_t *config);
esp_err_t uart_get_baudrate(uart_port_t port, uint32_t *out);
esp_err_t uart_enable_intr_mask(uart_port_t port, uint32_t mask);
esp_err_t uart_set_pin(uart_port_t port, int tx, int rx, int rts, int cts);
int uart_read_bytes(uart_port_t port, void *out, uint32_t length, TickType_t wait);
esp_err_t uart_driver_delete(uart_port_t port);
