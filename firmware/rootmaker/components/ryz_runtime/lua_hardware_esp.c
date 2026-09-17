#include "lua_hardware_esp.h"

#include <string.h>
#include "imu.h"
#include "ryz_provisioning.h"
#include "ryz_ble.h"

static ryz_lua_hardware_result_t hid_result(esp_err_t error)
{
    if (error == ESP_OK) return RYZ_LUA_HW_OK;
    if (error == ESP_ERR_NOT_SUPPORTED) return RYZ_LUA_HW_UNAVAILABLE;
    if (error == ESP_ERR_INVALID_STATE) return RYZ_LUA_HW_NOT_READY;
    if (error == ESP_ERR_NOT_FINISHED) return RYZ_LUA_HW_BUSY;
    return RYZ_LUA_HW_FAILED;
}

ryz_lua_hardware_result_t ryz_lua_hardware_esp_call(
    void *context, ryz_lua_hardware_op_t op, ryz_lua_hardware_value_t *out)
{
    ryz_lua_hardware_session_t *session = context;
    if (!session || !out) return RYZ_LUA_HW_FAILED;
    memset(out, 0, sizeof(*out));
    if (op == RYZ_LUA_WIFI_CONNECTED) {
        ryz_provisioning_snapshot_t state;
        if (ryz_provisioning_get_snapshot(&state) != ESP_OK)
            return RYZ_LUA_HW_UNAVAILABLE;
        /* ONLINE is STA with an assigned IP, not merely a SoftAP client or
         * saved credentials. This is not proof of Internet reachability. */
        out->connected = state.enabled && !state.stop_confirmed &&
            state.state == RYZ_PROVISIONING_ONLINE && state.ipv4[0] != 0;
        return RYZ_LUA_HW_OK;
    }
    if (op == RYZ_LUA_BLE_CONNECTED) {
        ryz_ble_snapshot_t state;
        if (ryz_ble_get_snapshot(&state) != ESP_OK || !state.available)
            return RYZ_LUA_HW_UNAVAILABLE;
        /* Link alone or an uncommitted pairing is not a trusted connection.
         * No GATT readiness or Lua radio-control permission is implied. */
        out->connected = state.linked && state.authenticated && state.bonded;
        return RYZ_LUA_HW_OK;
    }
    if (op == RYZ_LUA_HID_READY) {
        ryz_ble_snapshot_t state;
        if (ryz_ble_get_snapshot(&state) != ESP_OK || !state.available)
            return RYZ_LUA_HW_UNAVAILABLE;
        /* Security alone does not mean the host has subscribed to reports. */
        out->connected = state.linked && state.authenticated && state.bonded &&
            state.hid_ready;
        return RYZ_LUA_HW_OK;
    }
    if (op >= RYZ_LUA_HID_PLAY_PAUSE && op <= RYZ_LUA_HID_VOLUME_DOWN) {
        static const ryz_ble_hid_key_t keys[] = {
            RYZ_BLE_HID_PLAY_PAUSE, RYZ_BLE_HID_STOP, RYZ_BLE_HID_NEXT_TRACK,
            RYZ_BLE_HID_PREVIOUS_TRACK, RYZ_BLE_HID_MUTE,
            RYZ_BLE_HID_VOLUME_UP, RYZ_BLE_HID_VOLUME_DOWN,
        };
        ryz_ble_snapshot_t state;
        if (ryz_ble_get_snapshot(&state) != ESP_OK || !state.available)
            return RYZ_LUA_HW_UNAVAILABLE;
        if (!state.linked || !state.authenticated || !state.bonded || !state.hid_ready)
            return RYZ_LUA_HW_NOT_READY;
        if (!session->hid_session) {
            esp_err_t error = ryz_ble_hid_open(&session->hid_session);
            if (error != ESP_OK) {
                session->hid_session = 0;
                return hid_result(error);
            }
        }
        /* Success is bounded admission, not proof of a phone-side action.
         * The C owner validates security/readiness again under its own lock. */
        return hid_result(ryz_ble_hid_tap(session->hid_session,
            keys[op - RYZ_LUA_HID_PLAY_PAUSE]));
    }
    if (op == RYZ_LUA_IMU_INIT) {
        if (session->imu_owned) {
            ryz_imu_info_t info;
            if (ryz_imu_get_info(&info) == ESP_OK && info.ready) return RYZ_LUA_HW_OK;
        }
        if (ryz_imu_init() != ESP_OK) return RYZ_LUA_HW_FAILED;
        session->imu_owned = true;
        return RYZ_LUA_HW_OK;
    }
    if (op == RYZ_LUA_IMU_DEINIT) {
        if (!session->imu_owned) return RYZ_LUA_HW_OK;
        if (ryz_imu_deinit() != ESP_OK) return RYZ_LUA_HW_FAILED;
        session->imu_owned = false;
        return RYZ_LUA_HW_OK;
    }
    if (op != RYZ_LUA_IMU_READ) return RYZ_LUA_HW_FAILED;
    if (!session->imu_owned) return RYZ_LUA_HW_NOT_INITIALIZED;
    ryz_imu_sample_t sample;
    esp_err_t error = ryz_imu_read_sample(&sample);
    if (error == ESP_ERR_NOT_FINISHED) return RYZ_LUA_HW_NOT_READY;
    if (error == ESP_ERR_INVALID_STATE) return RYZ_LUA_HW_NOT_INITIALIZED;
    if (error != ESP_OK) return RYZ_LUA_HW_FAILED;
    out->x_mg = sample.x_mg;
    out->y_mg = sample.y_mg;
    out->z_mg = sample.z_mg;
    out->timestamp_us = sample.timestamp_us;
    out->sequence = sample.sequence;
    return RYZ_LUA_HW_OK;
}

void ryz_lua_hardware_esp_cleanup(ryz_lua_hardware_session_t *session)
{
    if (session && session->hid_session) {
        ryz_ble_hid_close(session->hid_session);
        session->hid_session = 0;
    }
    /* Bounded best effort power-down. Even if the sensor is absent/failing,
     * discard only the job lease; never tear down touch's shared I2C bus. */
    if (session && session->imu_owned) {
        (void)ryz_imu_deinit();
        session->imu_owned = false;
    }
}
