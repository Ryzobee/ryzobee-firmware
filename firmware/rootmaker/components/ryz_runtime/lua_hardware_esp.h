#pragma once

#include "ryz_lua_hardware.h"

typedef struct {
    bool imu_owned;
    /* Native lease belongs to the whole job, never a coroutine or Lua value. */
    uint32_t hid_session;
} ryz_lua_hardware_session_t;
ryz_lua_hardware_result_t ryz_lua_hardware_esp_call(
    void *session, ryz_lua_hardware_op_t op, ryz_lua_hardware_value_t *out);
void ryz_lua_hardware_esp_cleanup(ryz_lua_hardware_session_t *session);
