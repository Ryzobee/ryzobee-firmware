#include "tool_gate_test.h"
#ifndef RYZ_TEST_TOOL
#error "Select the RPC tool whose gate is being checked"
#endif
unsigned test_gate_calls;
bool test_gate_active;
esp_err_t test_gate_error;

esp_err_t ryz_tools_external(ryz_tool_t tool, esp_err_t (*admit)(void *), void *context)
{
    assert(tool == RYZ_TEST_TOOL && admit && context && !test_gate_active);
    ++test_gate_calls;
    if (test_gate_error != ESP_OK) return test_gate_error;
    test_gate_active = true;
    esp_err_t result = admit(context);
    test_gate_active = false;
    return result;
}
