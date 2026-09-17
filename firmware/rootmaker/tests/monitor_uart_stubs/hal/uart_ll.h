#pragma once
/* IDF 5.5.4 exposes interrupt masks here, NOT in driver/uart.h. */
typedef enum { UART_INTR_FRAM_ERR = (0x1 << 3) } uart_intr_t;
