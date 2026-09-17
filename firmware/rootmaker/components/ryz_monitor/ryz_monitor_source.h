#pragma once
#include "ryz_monitor.h"

typedef struct {
    size_t length;
    uint8_t bytes[RYZ_MONITOR_RECORD_BYTES];
    uint32_t fifo_overflow, buffer_full, frame_error, parity_error, break_events;
} ryz_monitor_sample_t;

/* Worker-only native UART seam; RX-only, UART1, no UART0 calls. end retries
 * only still-owned resources; queue handles consumed by SDK are discarded.
 * A failed begin may still own resources. poll is bounded and zero-wait. */
esp_err_t ryz_monitor_uart_begin(const ryz_monitor_config_t *config);
esp_err_t ryz_monitor_uart_poll(ryz_monitor_sample_t *out);
esp_err_t ryz_monitor_uart_end(void);
bool ryz_monitor_uart_resources_held(void);

/* Filtered ESP logger hook stays installed after stop but only captures with
 * an accepting SYSTEM stream. Never changes original stdout/log-level policy. */
esp_err_t ryz_monitor_log_install(void);
void ryz_monitor_log_capture(bool enabled);
