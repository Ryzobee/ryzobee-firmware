#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Generic, job-owned peripheral Interface, shared by both Lua entrypoints.
 * Buffers are borrowed only during a synchronous call. No SDK handle, radio
 * credential, tool state or Lua callback crosses this seam. */
#define RYZ_PERIPHERAL_BUFFER_MAX 256
#define RYZ_PERIPHERAL_HANDLE_MAX 16
#define RYZ_PERIPHERAL_WAIT_MAX_MS 20

typedef enum {
    RYZ_PERIPHERAL_GPIO, RYZ_PERIPHERAL_TIMER, RYZ_PERIPHERAL_PWM,
    RYZ_PERIPHERAL_I2C, RYZ_PERIPHERAL_SPI, RYZ_PERIPHERAL_UART,
    RYZ_PERIPHERAL_ADC, RYZ_PERIPHERAL_LOG, RYZ_PERIPHERAL_LED,
    RYZ_PERIPHERAL_KIND_COUNT
} ryz_peripheral_kind_t;

typedef enum {
    RYZ_PERIPHERAL_OPEN, RYZ_PERIPHERAL_CLOSE, RYZ_PERIPHERAL_READ,
    RYZ_PERIPHERAL_WRITE, RYZ_PERIPHERAL_TRANSFER, RYZ_PERIPHERAL_PROBE,
    RYZ_PERIPHERAL_SET_DUTY, RYZ_PERIPHERAL_POLL,
    RYZ_PERIPHERAL_READ_MV, RYZ_PERIPHERAL_STATUS
} ryz_peripheral_op_t;

typedef enum {
    RYZ_PERIPHERAL_LED_IDLE, RYZ_PERIPHERAL_LED_PENDING,
    RYZ_PERIPHERAL_LED_READY, RYZ_PERIPHERAL_LED_FAILED
} ryz_peripheral_led_state_t;

typedef enum {
    RYZ_PERIPHERAL_OK, RYZ_PERIPHERAL_UNAVAILABLE, RYZ_PERIPHERAL_INVALID,
    RYZ_PERIPHERAL_BUSY, RYZ_PERIPHERAL_CLOSED, RYZ_PERIPHERAL_TIMEOUT,
    RYZ_PERIPHERAL_NOT_FOUND, RYZ_PERIPHERAL_NO_MEMORY,
    RYZ_PERIPHERAL_UNSUPPORTED, RYZ_PERIPHERAL_FAILED
} ryz_peripheral_result_t;

typedef union {
    struct { int pin; unsigned mode, pull, initial; } gpio;
    struct { uint32_t period_ms; bool periodic; } timer;
    struct { int pin; uint32_t frequency_hz; unsigned duty; } pwm;
    struct { int sda, scl; uint32_t frequency_hz; bool board; } i2c;
    struct { int sclk, mosi, miso, cs; uint32_t frequency_hz; unsigned mode; } spi;
    struct { int port, tx, rx; uint32_t baud; unsigned bits, parity, stop; } uart;
    struct { int pin; unsigned attenuation_db; } adc;
    struct { unsigned level; } log;
    struct { bool board; } led;
} ryz_peripheral_config_t;

typedef struct {
    ryz_peripheral_kind_t kind;
    ryz_peripheral_op_t op;
    uint32_t handle;
    ryz_peripheral_config_t config;
    const uint8_t *data;
    size_t length, read_length;
    uint32_t value;
    unsigned address, timeout_ms;
} ryz_peripheral_request_t;

typedef struct {
    uint32_t handle, value, dropped;
    uint32_t error_events;
    bool loss_possible;
    struct {
        ryz_peripheral_led_state_t state;
        uint8_t red, green, blue;
        bool output_known;
    } led;
    size_t length;
    uint8_t data[RYZ_PERIPHERAL_BUFFER_MAX];
} ryz_peripheral_reply_t;

typedef ryz_peripheral_result_t (*ryz_peripheral_call_fn)(void *context,
    const ryz_peripheral_request_t *request, ryz_peripheral_reply_t *reply);

/* Target Adapter storage is allocated lazily in PSRAM. The session is owned
 * by the job, not a coroutine. cleanup runs after all Lua calls are revoked. */
typedef struct { void *state; } ryz_peripheral_session_t;
ryz_peripheral_result_t ryz_peripheral_esp_call(void *session,
    const ryz_peripheral_request_t *request, ryz_peripheral_reply_t *reply);
void ryz_peripheral_esp_cleanup(ryz_peripheral_session_t *session);
