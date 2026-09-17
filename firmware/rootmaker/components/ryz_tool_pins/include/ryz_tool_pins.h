#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RYZ_TOOL_PIN_AVAILABLE = 0,
    RYZ_TOOL_PIN_INVALID,
    RYZ_TOOL_PIN_NOT_EXPOSED,
    RYZ_TOOL_PIN_MEMORY,
    RYZ_TOOL_PIN_USB,
    RYZ_TOOL_PIN_BOARD,
    RYZ_TOOL_PIN_DEBUG,
} ryz_tool_pin_reason_t;

#define RYZ_TOOL_PINS_MAX 4

/* Exclusive SDK controllers used by native services and public Lua drivers.
 * LEDC leases cover fixed timer N / channel N pairs (N = 1..3). */
typedef enum {
    RYZ_TOOL_CONTROLLER_UART1 = 0,
    RYZ_TOOL_CONTROLLER_UART2,
    RYZ_TOOL_CONTROLLER_I2C1,
    RYZ_TOOL_CONTROLLER_SPI3,
    RYZ_TOOL_CONTROLLER_ADC1,
    RYZ_TOOL_CONTROLLER_ADC2,
    RYZ_TOOL_CONTROLLER_LEDC_TIMER1,
    RYZ_TOOL_CONTROLLER_LEDC_TIMER2,
    RYZ_TOOL_CONTROLLER_LEDC_TIMER3,
    RYZ_TOOL_CONTROLLER_COUNT,
} ryz_tool_controller_t;

/* Fixed-board eligibility, not proof of PCB/electrical availability. No GPIO
 * calls. Excludes reserved board functions; 33..37 additionally excluded for
 * Octal flash/PSRAM builds. The default shared I2C pair is not a custom pair. */
ryz_tool_pin_reason_t ryz_tool_pin_reason(int pin);
const char *ryz_tool_pin_reason_name(ryz_tool_pin_reason_t reason);

/* Trusted C leases of 1..RYZ_TOOL_PINS_MAX eligible, distinct pins. Check is only
 * a momentary availability observation; claim atomically reserves every pin
 * before GPIO changes. NULL pins, invalid count or duplicate pins -> INVALID_ARG.
 * Conflict -> INVALID_STATE; internal lock contention -> NOT_FINISHED without
 * spinning. Tokens are boot-local, never reused/wrapped. All out values clear
 * on failure. No OS allocation, GPIO or IDF handles escape this Module. */
esp_err_t ryz_tool_pins_check(const int *pins, size_t count);
esp_err_t ryz_tool_pins_claim(const int *pins, size_t count, uint32_t *out_token);
/* Compatibility wrappers with identical ownership and error semantics. */
esp_err_t ryz_tool_pins_check_pair(int first, int second);
esp_err_t ryz_tool_pins_claim_pair(int first, int second, uint32_t *out_token);
/* Release only after hardware teardown. Unknown/stale token cannot release a
 * later lease. NOT_FINISHED retains ownership and is safe to retry. */
esp_err_t ryz_tool_pins_release(uint32_t token);

/* Claim before creating/configuring the SDK controller, even when the pin set
 * differs from an existing user. Uses the same nonblocking lock and monotonic
 * token sequence as pin leases. Controller tokens must be released through
 * release_controller only; pin and controller tokens are not interchangeable.
 * Retain both leases on failed hardware teardown. */
esp_err_t ryz_tool_pins_claim_controller(ryz_tool_controller_t controller,
                                         uint32_t *out_token);
esp_err_t ryz_tool_pins_release_controller(uint32_t token);

#ifdef __cplusplus
}
#endif
