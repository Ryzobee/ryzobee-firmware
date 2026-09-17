#include "ryz_tool_pins.h"
#include "sdkconfig.h"
#include <stdatomic.h>
#include <stdbool.h>

static atomic_flag s_lock = ATOMIC_FLAG_INIT;
static uint32_t s_pin_tokens[49];
static uint32_t s_controller_tokens[RYZ_TOOL_CONTROLLER_COUNT];
static uint32_t s_last_token;

ryz_tool_pin_reason_t ryz_tool_pin_reason(int pin)
{
    if (pin < 0 || pin > 48 || (pin >= 22 && pin <= 25)) return RYZ_TOOL_PIN_INVALID;
    if (pin >= 26 && pin <= 32) return RYZ_TOOL_PIN_MEMORY;
#if CONFIG_ESPTOOLPY_OCT_FLASH || CONFIG_SPIRAM_MODE_OCT
    if (pin >= 33 && pin <= 37) return RYZ_TOOL_PIN_MEMORY;
#endif
    if (pin == 19 || pin == 20) return RYZ_TOOL_PIN_USB;
    if (pin == 39) return RYZ_TOOL_PIN_DEBUG;
    if ((pin >= 13 && pin <= 18) || pin == 21 ||
        (pin >= 33 && pin <= 38) || pin == 47 || pin == 48) return RYZ_TOOL_PIN_AVAILABLE;
    switch (pin) {
    case 0: case 1: case 2: case 3: case 5: case 7: case 9: case 10: case 12:
    case 40: case 41: case 42: case 43: case 44: case 45: case 46:
        return RYZ_TOOL_PIN_BOARD;
    default:
        return RYZ_TOOL_PIN_NOT_EXPOSED;
    }
}
const char *ryz_tool_pin_reason_name(ryz_tool_pin_reason_t reason)
{
    switch (reason) {
    case RYZ_TOOL_PIN_AVAILABLE: return "available";
    case RYZ_TOOL_PIN_INVALID: return "invalid";
    case RYZ_TOOL_PIN_NOT_EXPOSED: return "not_exposed";
    case RYZ_TOOL_PIN_MEMORY: return "memory";
    case RYZ_TOOL_PIN_USB: return "usb";
    case RYZ_TOOL_PIN_BOARD: return "board";
    case RYZ_TOOL_PIN_DEBUG: return "debug";
    default: return "invalid";
    }
}
static bool valid_pins(const int *pins, size_t count)
{
    if (!pins || !count || count > RYZ_TOOL_PINS_MAX) return false;
    for (size_t i = 0; i < count; ++i) {
        if (ryz_tool_pin_reason(pins[i]) != RYZ_TOOL_PIN_AVAILABLE) return false;
        for (size_t j = 0; j < i; ++j) {
            if (pins[i] == pins[j]) return false;
        }
    }
    return true;
}

static esp_err_t pins_operation(const int *pins, size_t count, uint32_t *out_token)
{
    if (!valid_pins(pins, count)) return ESP_ERR_INVALID_ARG;
    if (atomic_flag_test_and_set_explicit(&s_lock, memory_order_acquire)) {
        return ESP_ERR_NOT_FINISHED;
    }
    bool available = true;
    for (size_t i = 0; i < count; ++i) {
        if (s_pin_tokens[pins[i]]) {
            available = false;
            break;
        }
    }
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (available) {
        if (!out_token) error = ESP_OK;
        else if (s_last_token != UINT32_MAX) {
            const uint32_t token = ++s_last_token;
            for (size_t i = 0; i < count; ++i) s_pin_tokens[pins[i]] = token;
            *out_token = token;
            error = ESP_OK;
        }
    }
    atomic_flag_clear_explicit(&s_lock, memory_order_release);
    return error;
}
esp_err_t ryz_tool_pins_check(const int *pins, size_t count)
{
    return pins_operation(pins, count, NULL);
}
esp_err_t ryz_tool_pins_claim(const int *pins, size_t count, uint32_t *out_token)
{
    if (!out_token) return ESP_ERR_INVALID_ARG;
    *out_token = 0;
    return pins_operation(pins, count, out_token);
}
esp_err_t ryz_tool_pins_check_pair(int first, int second)
{
    const int pins[] = { first, second };
    return ryz_tool_pins_check(pins, 2);
}
esp_err_t ryz_tool_pins_claim_pair(int first, int second, uint32_t *out_token)
{
    const int pins[] = { first, second };
    return ryz_tool_pins_claim(pins, 2, out_token);
}
esp_err_t ryz_tool_pins_release(uint32_t token)
{
    if (!token) return ESP_ERR_INVALID_ARG;
    if (atomic_flag_test_and_set_explicit(&s_lock, memory_order_acquire)) {
        return ESP_ERR_NOT_FINISHED;
    }
    unsigned count = 0;
    for (int pin = 0; pin < 49; ++pin) {
        if (s_pin_tokens[pin] == token) ++count;
    }
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (count >= 1 && count <= RYZ_TOOL_PINS_MAX) {
        for (int pin = 0; pin < 49; ++pin) {
            if (s_pin_tokens[pin] == token) s_pin_tokens[pin] = 0;
        }
        error = ESP_OK;
    }
    atomic_flag_clear_explicit(&s_lock, memory_order_release);
    return error;
}

esp_err_t ryz_tool_pins_claim_controller(ryz_tool_controller_t controller,
                                         uint32_t *out_token)
{
    if (!out_token) return ESP_ERR_INVALID_ARG;
    *out_token = 0;
    if ((unsigned)controller >= RYZ_TOOL_CONTROLLER_COUNT) return ESP_ERR_INVALID_ARG;
    if (atomic_flag_test_and_set_explicit(&s_lock, memory_order_acquire)) {
        return ESP_ERR_NOT_FINISHED;
    }
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (!s_controller_tokens[controller] && s_last_token != UINT32_MAX) {
        const uint32_t token = ++s_last_token;
        s_controller_tokens[controller] = token;
        *out_token = token;
        error = ESP_OK;
    }
    atomic_flag_clear_explicit(&s_lock, memory_order_release);
    return error;
}

esp_err_t ryz_tool_pins_release_controller(uint32_t token)
{
    if (!token) return ESP_ERR_INVALID_ARG;
    if (atomic_flag_test_and_set_explicit(&s_lock, memory_order_acquire)) {
        return ESP_ERR_NOT_FINISHED;
    }
    esp_err_t error = ESP_ERR_INVALID_STATE;
    for (unsigned controller = 0; controller < RYZ_TOOL_CONTROLLER_COUNT; ++controller) {
        if (s_controller_tokens[controller] != token) continue;
        s_controller_tokens[controller] = 0;
        error = ESP_OK;
        break;
    }
    atomic_flag_clear_explicit(&s_lock, memory_order_release);
    return error;
}
