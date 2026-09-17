#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Copy-only, bounded Adapter contract. No credentials, radio configuration,
 * register access, pin choices, or native handles can cross this boundary. */
typedef enum {
    RYZ_LUA_WIFI_CONNECTED,
    RYZ_LUA_BLE_CONNECTED,
    RYZ_LUA_IMU_INIT,
    RYZ_LUA_IMU_READ,
    RYZ_LUA_IMU_DEINIT,
    RYZ_LUA_HID_READY,
    RYZ_LUA_HID_PLAY_PAUSE,
    RYZ_LUA_HID_STOP,
    RYZ_LUA_HID_NEXT_TRACK,
    RYZ_LUA_HID_PREVIOUS_TRACK,
    RYZ_LUA_HID_MUTE,
    RYZ_LUA_HID_VOLUME_UP,
    RYZ_LUA_HID_VOLUME_DOWN,
} ryz_lua_hardware_op_t;

typedef enum {
    RYZ_LUA_HW_OK,
    RYZ_LUA_HW_UNAVAILABLE,
    RYZ_LUA_HW_NOT_INITIALIZED,
    RYZ_LUA_HW_NOT_READY,
    RYZ_LUA_HW_FAILED,
    RYZ_LUA_HW_BUSY,
} ryz_lua_hardware_result_t;

typedef struct {
    bool connected;
    float x_mg, y_mg, z_mg;
    uint64_t timestamp_us;
    uint32_t sequence;
} ryz_lua_hardware_value_t;

typedef ryz_lua_hardware_result_t (*ryz_lua_hardware_fn)(
    void *context, ryz_lua_hardware_op_t op, ryz_lua_hardware_value_t *out);
