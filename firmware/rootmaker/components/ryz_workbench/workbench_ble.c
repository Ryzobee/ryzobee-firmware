#include "workbench_ble.h"

#include <stdio.h>
#include <string.h>

static void copy_field(char *out, size_t capacity, const char *in, size_t size)
{
    const size_t length = strnlen(in, size);
    const size_t count = length < capacity - 1U ? length : capacity - 1U;
    memcpy(out, in, count);
    out[count] = '\0';
}

void ryz_workbench_ble_model(const ryz_ble_snapshot_t *s, esp_err_t error,
                             ryz_v5_ble_model_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->page = RYZ_V5_BLE_OFF;
    out->unavailable = true;
    if (!s || error != ESP_OK || !s->available || s->phase == RYZ_BLE_FAILED ||
        s->phase < RYZ_BLE_OFF || s->phase > RYZ_BLE_FAILED) {
        /* Preserve only bounded diagnostic facts. A failed service must not
         * leak stale radio, peer, comparison or command-admission state. */
        if (error != ESP_OK) {
            out->failure = RYZ_V5_BLE_FAILURE_START;
            snprintf(out->error_code, sizeof(out->error_code), "E_%08X", (unsigned)error);
        } else if (s && s->phase >= RYZ_BLE_OFF && s->phase <= RYZ_BLE_FAILED) {
            switch (s->failure) {
            case RYZ_BLE_FAILURE_START:
            case RYZ_BLE_FAILURE_LOAD: out->failure = RYZ_V5_BLE_FAILURE_START; break;
            case RYZ_BLE_FAILURE_AUTH: out->failure = RYZ_V5_BLE_FAILURE_AUTH; break;
            case RYZ_BLE_FAILURE_SAVE: out->failure = RYZ_V5_BLE_FAILURE_SAVE; break;
            default: break;
            }
            const esp_err_t actual = s->last_error != ESP_OK ? s->last_error : s->boot_error;
            if (actual != ESP_OK)
                snprintf(out->error_code, sizeof(out->error_code), "E_%08X", (unsigned)actual);
            else if (s->sdk_error)
                snprintf(out->error_code, sizeof(out->error_code), "SDK_%08X", (unsigned)s->sdk_error);
        }
        return;
    }
    out->unavailable = false;
    out->enabled = s->enabled;
    out->bond_known = s->bond_known;
    out->bonded = s->bonded;
    out->linked = s->linked;
    out->pairing_active = s->phase == RYZ_BLE_PAIRING ||
        s->phase == RYZ_BLE_COMPARISON || s->phase == RYZ_BLE_COMMITTING;
    out->pairing_window_seconds = 30;
    out->remaining_valid = s->remaining_valid;
    out->remaining_seconds = s->remaining_valid ? s->remaining_s : 0;
    out->rssi_valid = s->rssi_valid && s->linked;
    out->rssi_dbm = out->rssi_valid ? s->rssi_dbm : 0;
    out->compare_pending = s->compare_pending && s->phase == RYZ_BLE_COMPARISON &&
        s->operation_id != 0 && s->compare_value <= UINT32_C(999999);
    out->compare_value = out->compare_pending ? s->compare_value : 0;
    out->operation_id = s->operation_id;
    out->checking_state = s->checking_state;
    out->retry_allowed = s->retry_allowed;
    out->replace_after_forget = s->replace_after_forget;
    out->old_bond_removed = s->old_bond_removed;
    /* The current Peripheral owner allows saved-peer reconnect while enabled.
     * This does not promise that the phone OS initiates an automatic connect. */
    out->reconnect_known = s->preference_known && s->bond_known;
    out->reconnect_enabled = s->enabled && s->bonded;
    copy_field(out->local_name, sizeof(out->local_name), s->local_name, sizeof(s->local_name));
    copy_field(out->local_address, sizeof(out->local_address), s->local_address, sizeof(s->local_address));
    copy_field(out->peer_name, sizeof(out->peer_name), s->peer_name, sizeof(s->peer_name));
    memcpy(out->role, "Peripheral", sizeof("Peripheral"));
    memcpy(out->mode, "SC / MITM", sizeof("SC / MITM"));
    /* No application GATT protocol is implemented. Link/auth/bond cannot be
     * promoted into service readiness, even if a producer returns dirty data. */
    out->service_known = false;
    out->service_ready = false;
    const char *failure = NULL;
    switch (s->failure) {
    case RYZ_BLE_FAILURE_START: out->failure = RYZ_V5_BLE_FAILURE_START; failure = "E_START"; break;
    case RYZ_BLE_FAILURE_AUTH: out->failure = RYZ_V5_BLE_FAILURE_AUTH; failure = "E_AUTH"; break;
    case RYZ_BLE_FAILURE_SAVE: out->failure = RYZ_V5_BLE_FAILURE_SAVE; failure = "E_SAVE"; break;
    case RYZ_BLE_FAILURE_STOP: failure = "E_STOP"; break;
    case RYZ_BLE_FAILURE_LOAD: failure = "E_LOAD"; break;
    default: break;
    }
    if (failure) copy_field(out->error_code, sizeof(out->error_code), failure, strlen(failure));
    else if (s->last_error != ESP_OK)
        snprintf(out->error_code, sizeof(out->error_code), "E_%08X", (unsigned)s->last_error);

    out->page = s->enabled ? s->bonded ? RYZ_V5_BLE_PAIRED : RYZ_V5_BLE_EMPTY : RYZ_V5_BLE_OFF;
    switch (s->phase) {
    case RYZ_BLE_SAVED_WAITING: out->state = RYZ_V5_BLE_STATE_SAVED_WAITING; break;
    case RYZ_BLE_LINKED: break; /* No application service exists to wait for. */
    case RYZ_BLE_PAIRED:
        if (s->operation_id && s->bonded && s->authenticated && s->linked)
            out->page = RYZ_V5_BLE_SUCCESS;
        break;
    case RYZ_BLE_PAIRING:
        out->page = RYZ_V5_BLE_PAIRING;
        if (s->old_bond_removed && s->replace_after_forget)
            out->state = RYZ_V5_BLE_STATE_PAIRING_AFTER_REPLACE;
        break;
    case RYZ_BLE_COMPARISON:
        out->page = RYZ_V5_BLE_PAIRING; out->state = RYZ_V5_BLE_STATE_VERIFY_CODE; break;
    case RYZ_BLE_COMMITTING:
        out->page = RYZ_V5_BLE_PAIRING; out->state = RYZ_V5_BLE_STATE_COMMITTING; break;
    case RYZ_BLE_PAIR_TIMEOUT:
        out->page = RYZ_V5_BLE_PAIRING; out->state = RYZ_V5_BLE_STATE_PAIR_TIMEOUT; break;
    case RYZ_BLE_PAIR_FAILED:
        out->page = RYZ_V5_BLE_PAIRING; out->state = RYZ_V5_BLE_STATE_PAIR_FAILED; break;
    case RYZ_BLE_STOPPING: out->state = RYZ_V5_BLE_STATE_STOPPING; break;
    case RYZ_BLE_FORGETTING:
        out->page = RYZ_V5_BLE_FORGET; out->state = RYZ_V5_BLE_STATE_FORGETTING; break;
    case RYZ_BLE_FORGET_FAILED:
        out->page = RYZ_V5_BLE_FORGET; out->state = RYZ_V5_BLE_STATE_FORGET_FAILED; break;
    default: break;
    }
}

esp_err_t ryz_workbench_ble_submit(void *context, ryz_v5_ble_action_t action,
                                  uint32_t operation_id, uint32_t compare_value)
{
    (void)context;
    ryz_ble_snapshot_t current;
    if (ryz_ble_get_snapshot(&current) != ESP_OK || !current.available || current.phase == RYZ_BLE_FAILED ||
        current.operation_id != operation_id) return ESP_ERR_INVALID_STATE;
    if (action == RYZ_V5_BLE_ACTION_CONFIRM_CODE || action == RYZ_V5_BLE_ACTION_REJECT_CODE) {
        if (!current.compare_pending || current.phase != RYZ_BLE_COMPARISON ||
            !operation_id || compare_value > UINT32_C(999999) ||
            current.compare_value != compare_value) return ESP_ERR_INVALID_STATE;
        return ryz_ble_confirm_numeric(operation_id, compare_value,
                                      action == RYZ_V5_BLE_ACTION_CONFIRM_CODE);
    }
    ryz_ble_action_t command;
    switch (action) {
    case RYZ_V5_BLE_ACTION_ENABLE: command = RYZ_BLE_ENABLE; break;
    case RYZ_V5_BLE_ACTION_DISABLE: command = RYZ_BLE_DISABLE; break;
    case RYZ_V5_BLE_ACTION_PAIR: command = RYZ_BLE_PAIR_NEW; break;
    case RYZ_V5_BLE_ACTION_CANCEL_PAIR: command = RYZ_BLE_CANCEL_PAIR; break;
    case RYZ_V5_BLE_ACTION_CONFIRM_REPLACE: command = RYZ_BLE_REPLACE; break;
    case RYZ_V5_BLE_ACTION_CONFIRM_FORGET: command = RYZ_BLE_FORGET; break;
    case RYZ_V5_BLE_ACTION_RETRY_PAIR: command = RYZ_BLE_RETRY_PAIR; break;
    case RYZ_V5_BLE_ACTION_RETRY_FORGET: command = RYZ_BLE_RETRY_FORGET; break;
    default: return ESP_ERR_NOT_SUPPORTED;
    }
    return ryz_ble_request(command, operation_id);
}

bool ryz_workbench_ble_boot_settled(esp_err_t start_error, esp_err_t snapshot_error,
                                   const ryz_ble_snapshot_t *snapshot)
{
    return start_error != ESP_OK ||
        (snapshot_error == ESP_OK && snapshot && snapshot->boot_settled);
}
