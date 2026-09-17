/* Real model projection/admission Adapter; public BLE service is a controlled
 * peer. No NimBLE, NVS, LVGL rendering, navigation or device claim is made. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "workbench_ble.h"

static ryz_ble_snapshot_t peer;
static esp_err_t snapshot_error, request_error, confirm_error;
static unsigned requests, confirmations;
static ryz_ble_action_t requested_action;
static uint32_t requested_id, confirmed_id, confirmed_value;
static bool confirmed_accept;

esp_err_t ryz_ble_get_snapshot(ryz_ble_snapshot_t *out)
{ *out = peer; return snapshot_error; }
esp_err_t ryz_ble_request(ryz_ble_action_t action, uint32_t operation_id)
{
    ++requests; requested_action = action; requested_id = operation_id;
    return request_error;
}
esp_err_t ryz_ble_confirm_numeric(uint32_t id, uint32_t value, bool accept)
{
    ++confirmations; confirmed_id = id; confirmed_value = value; confirmed_accept = accept;
    return confirm_error;
}

static ryz_ble_snapshot_t available(void)
{
    return (ryz_ble_snapshot_t){.available = true, .enabled = true,
        .preference_known = true, .bond_known = true, .operation_id = 9,
        .local_name = "Ryzobee", .peer_name = "Phone/PC", .local_address = "01:02:03:04:05:06"};
}

static void model_case(void)
{
    const ryz_v5_ble_state_t states[] = {
        [RYZ_BLE_OFF] = RYZ_V5_BLE_STATE_LEGACY,
        [RYZ_BLE_EMPTY] = RYZ_V5_BLE_STATE_LEGACY,
        [RYZ_BLE_SAVED_WAITING] = RYZ_V5_BLE_STATE_SAVED_WAITING,
        [RYZ_BLE_PAIRING] = RYZ_V5_BLE_STATE_LEGACY,
        [RYZ_BLE_COMPARISON] = RYZ_V5_BLE_STATE_VERIFY_CODE,
        [RYZ_BLE_COMMITTING] = RYZ_V5_BLE_STATE_COMMITTING,
        [RYZ_BLE_PAIRED] = RYZ_V5_BLE_STATE_LEGACY,
        [RYZ_BLE_LINKED] = RYZ_V5_BLE_STATE_LEGACY,
        [RYZ_BLE_PAIR_TIMEOUT] = RYZ_V5_BLE_STATE_PAIR_TIMEOUT,
        [RYZ_BLE_PAIR_FAILED] = RYZ_V5_BLE_STATE_PAIR_FAILED,
        [RYZ_BLE_STOPPING] = RYZ_V5_BLE_STATE_STOPPING,
        [RYZ_BLE_FORGETTING] = RYZ_V5_BLE_STATE_FORGETTING,
        [RYZ_BLE_FORGET_FAILED] = RYZ_V5_BLE_STATE_FORGET_FAILED,
        [RYZ_BLE_FAILED] = RYZ_V5_BLE_STATE_LEGACY,
    };
    for (unsigned phase = RYZ_BLE_OFF; phase <= RYZ_BLE_FAILED; ++phase) {
        ryz_ble_snapshot_t s = available();
        s.phase = (ryz_ble_phase_t)phase;
        s.enabled = phase != RYZ_BLE_OFF;
        s.linked = s.authenticated = s.bonded = true;
        s.service_known = s.service_ready = true; /* Must not manufacture GATT readiness. */
        s.rssi_valid = true; s.rssi_dbm = -42;
        s.compare_pending = true; s.compare_value = 17;
        s.remaining_valid = true; s.remaining_s = 23;
        ryz_v5_ble_model_t m;
        ryz_workbench_ble_model(&s, ESP_OK, &m);
        if (phase == RYZ_BLE_FAILED) {
            assert(m.unavailable && !m.checking_state && !m.operation_id && !m.linked);
            assert(m.state == RYZ_V5_BLE_STATE_LEGACY);
            continue;
        }
        assert(!m.unavailable && m.state == states[phase]);
        assert(m.operation_id == 9 && m.pairing_window_seconds == 30);
        assert(m.remaining_valid && m.remaining_seconds == 23);
        assert(m.rssi_valid && m.rssi_dbm == -42);
        assert(!m.service_known && !m.service_ready);
        assert(m.compare_pending == (phase == RYZ_BLE_COMPARISON));
        assert(m.compare_value == (phase == RYZ_BLE_COMPARISON ? 17U : 0U));
        assert(!strcmp(m.local_name, "Ryzobee") && !strcmp(m.peer_name, "Phone/PC"));
        assert(!strcmp(m.local_address, "01:02:03:04:05:06"));
        assert(!strcmp(m.role, "Peripheral") && !strcmp(m.mode, "SC / MITM"));
        if (phase == RYZ_BLE_PAIRED) assert(m.page == RYZ_V5_BLE_SUCCESS);
        if (phase == RYZ_BLE_LINKED) assert(m.page == RYZ_V5_BLE_PAIRED);
    }
    ryz_ble_snapshot_t s = available();
    s.phase = RYZ_BLE_PAIRING; s.old_bond_removed = s.replace_after_forget = true;
    ryz_v5_ble_model_t m;
    ryz_workbench_ble_model(&s, ESP_OK, &m);
    assert(m.state == RYZ_V5_BLE_STATE_PAIRING_AFTER_REPLACE && m.old_bond_removed);
    s.phase = RYZ_BLE_PAIRED; s.linked = s.bonded = true;
    ryz_workbench_ble_model(&s, ESP_OK, &m);
    assert(m.page != RYZ_V5_BLE_SUCCESS); /* Authentication not confirmed. */
    s.authenticated = true; s.operation_id = 0;
    ryz_workbench_ble_model(&s, ESP_OK, &m);
    assert(m.operation_id == 0 && m.page != RYZ_V5_BLE_SUCCESS);
    s.phase = RYZ_BLE_FORGETTING; s.operation_id = 27;
    s.checking_state = true; s.retry_allowed = false;
    ryz_workbench_ble_model(&s, ESP_OK, &m);
    assert(m.checking_state && !m.retry_allowed && m.operation_id == 27);
    assert(m.state == RYZ_V5_BLE_STATE_FORGETTING);
    s.failure = RYZ_BLE_FAILURE_SAVE; s.last_error = ESP_FAIL;
    ryz_workbench_ble_model(&s, ESP_OK, &m);
    assert(m.failure == RYZ_V5_BLE_FAILURE_SAVE && !strcmp(m.error_code, "E_SAVE"));
    s.failure = RYZ_BLE_FAILURE_NONE;
    ryz_workbench_ble_model(&s, ESP_OK, &m);
    assert(!strcmp(m.error_code, "E_FFFFFFFF"));
    assert(!requests && !confirmations); /* A projection cannot mutate a radio. */
}

static void invalid_model_case(void)
{
    ryz_ble_snapshot_t s = available();
    s.phase = RYZ_BLE_COMPARISON; s.compare_pending = true; s.compare_value = 1000000;
    s.rssi_valid = true; s.remaining_s = 99;
    memset(s.peer_name, 'x', sizeof(s.peer_name));
    ryz_v5_ble_model_t m;
    ryz_workbench_ble_model(&s, ESP_OK, &m);
    assert(!m.compare_pending && !m.compare_value && !m.rssi_valid && !m.rssi_dbm);
    assert(!m.remaining_valid && !m.remaining_seconds);
    assert(strlen(m.peer_name) == 32);
    s.compare_value = 0; /* Leading zeroes are preserved by the six-digit renderer. */
    ryz_workbench_ble_model(&s, ESP_OK, &m);
    assert(m.compare_pending && !m.compare_value);
    for (unsigned kind = 0; kind < 4; ++kind) {
        memset(&m, 0x55, sizeof(m));
        s.available = kind != 0;
        s.phase = kind == 3 ? (ryz_ble_phase_t)99 : RYZ_BLE_COMPARISON;
        ryz_workbench_ble_model(kind == 2 ? NULL : &s, kind == 1 ? ESP_FAIL : ESP_OK, &m);
        assert(m.unavailable && !m.enabled && !m.bonded && !m.linked);
        assert(!m.operation_id && !m.compare_pending && !m.compare_value);
        assert(!m.local_name[0] && !m.peer_name[0] && !m.local_address[0]);
        assert(!m.service_known && !m.service_ready && !m.reconnect_known);
    }
    ryz_workbench_ble_model(NULL, ESP_FAIL, NULL);
}

static void commands_case(void)
{
    const struct { ryz_v5_ble_action_t ui; ryz_ble_action_t service; } cases[] = {
        {RYZ_V5_BLE_ACTION_ENABLE, RYZ_BLE_ENABLE}, {RYZ_V5_BLE_ACTION_DISABLE, RYZ_BLE_DISABLE},
        {RYZ_V5_BLE_ACTION_PAIR, RYZ_BLE_PAIR_NEW}, {RYZ_V5_BLE_ACTION_CANCEL_PAIR, RYZ_BLE_CANCEL_PAIR},
        {RYZ_V5_BLE_ACTION_CONFIRM_REPLACE, RYZ_BLE_REPLACE}, {RYZ_V5_BLE_ACTION_CONFIRM_FORGET, RYZ_BLE_FORGET},
        {RYZ_V5_BLE_ACTION_RETRY_PAIR, RYZ_BLE_RETRY_PAIR}, {RYZ_V5_BLE_ACTION_RETRY_FORGET, RYZ_BLE_RETRY_FORGET},
    };
    peer = available(); peer.operation_id = 81;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        request_error = i & 1 ? ESP_ERR_INVALID_STATE : ESP_OK;
        assert(ryz_workbench_ble_submit(NULL, cases[i].ui, 81, 0) == request_error);
        assert(requests == i + 1 && requested_action == cases[i].service && requested_id == 81);
    }
    const unsigned accepted = requests;
    assert(ryz_workbench_ble_submit(NULL, RYZ_V5_BLE_ACTION_PAIR, 80, 0) == ESP_ERR_INVALID_STATE);
    assert(ryz_workbench_ble_submit(NULL, RYZ_V5_BLE_ACTION_CONFIRM_FORGET, 82, 0) == ESP_ERR_INVALID_STATE);
    snapshot_error = ESP_FAIL;
    assert(ryz_workbench_ble_submit(NULL, RYZ_V5_BLE_ACTION_DISABLE, 81, 0) == ESP_ERR_INVALID_STATE);
    snapshot_error = ESP_OK; peer.available = false;
    assert(ryz_workbench_ble_submit(NULL, RYZ_V5_BLE_ACTION_ENABLE, 81, 0) == ESP_ERR_INVALID_STATE);
    peer.available = true;
    peer.phase = RYZ_BLE_FAILED;
    assert(ryz_workbench_ble_submit(NULL, RYZ_V5_BLE_ACTION_ENABLE, 81, 0) == ESP_ERR_INVALID_STATE);
    peer.phase = RYZ_BLE_OFF;
    const ryz_v5_ble_action_t navigation[] = {RYZ_V5_BLE_ACTION_NONE, RYZ_V5_BLE_ACTION_BACK,
        RYZ_V5_BLE_ACTION_DONE, RYZ_V5_BLE_ACTION_INFO, RYZ_V5_BLE_ACTION_REPLACE, RYZ_V5_BLE_ACTION_FORGET};
    for (unsigned i = 0; i < sizeof(navigation) / sizeof(navigation[0]); ++i)
        assert(ryz_workbench_ble_submit(NULL, navigation[i], 81, 0) == ESP_ERR_NOT_SUPPORTED);
    assert(requests == accepted && !confirmations);
}

static void unavailable_error_case(void)
{
    ryz_ble_snapshot_t s = available();
    s.phase = RYZ_BLE_FAILED; s.failure = RYZ_BLE_FAILURE_LOAD;
    s.last_error = ESP_FAIL; s.available = false;
    s.linked = s.bonded = s.authenticated = s.compare_pending = true;
    s.compare_value = 123456;
    ryz_v5_ble_model_t m;
    ryz_workbench_ble_model(&s, ESP_OK, &m);
    assert(m.unavailable && m.failure == RYZ_V5_BLE_FAILURE_START);
    assert(!strcmp(m.error_code, "E_FFFFFFFF"));
    assert(!m.enabled && !m.bonded && !m.linked && !m.operation_id && !m.compare_pending);
    assert(!m.peer_name[0] && !m.local_address[0] && !m.retry_allowed);
    s.available = true; s.failure = RYZ_BLE_FAILURE_START; s.last_error = ESP_ERR_INVALID_STATE;
    ryz_workbench_ble_model(&s, ESP_OK, &m);
    assert(m.unavailable && !strcmp(m.error_code, "E_00000103"));
    s.failure = RYZ_BLE_FAILURE_STOP; s.last_error = ESP_OK; s.sdk_error = 42;
    ryz_workbench_ble_model(&s, ESP_OK, &m);
    assert(m.unavailable && !strcmp(m.error_code, "SDK_0000002A"));
    ryz_workbench_ble_model(&s, ESP_ERR_NO_MEM, &m); /* Failed reads must not trust stale snapshot details. */
    assert(m.unavailable && m.failure == RYZ_V5_BLE_FAILURE_START && !strcmp(m.error_code, "E_00000101"));
    memset(&s, 0, sizeof(s));
    s.boot_error = ESP_ERR_TIMEOUT;
    ryz_workbench_ble_model(&s, ESP_OK, &m);
    assert(m.unavailable && !strcmp(m.error_code, "E_00000107"));
    s.boot_error = ESP_OK;
    ryz_workbench_ble_model(&s, ESP_OK, &m);
    assert(m.unavailable && !m.error_code[0] && m.failure == RYZ_V5_BLE_FAILURE_UNKNOWN);
    s.phase = (ryz_ble_phase_t)99; s.last_error = ESP_FAIL;
    ryz_workbench_ble_model(&s, ESP_OK, &m);
    assert(m.unavailable && !m.error_code[0]); /* Invalid producer state is not a trusted error report. */
}

static void numeric_case(void)
{
    peer = available(); peer.phase = RYZ_BLE_COMPARISON;
    peer.operation_id = 41; peer.compare_pending = true; peer.compare_value = 0;
    assert(ryz_workbench_ble_submit(NULL, RYZ_V5_BLE_ACTION_CONFIRM_CODE, 41, 0) == ESP_OK);
    assert(confirmations == 1 && confirmed_id == 41 && !confirmed_value && confirmed_accept);
    confirm_error = ESP_ERR_INVALID_STATE;
    assert(ryz_workbench_ble_submit(NULL, RYZ_V5_BLE_ACTION_REJECT_CODE, 41, 0) == ESP_ERR_INVALID_STATE);
    assert(confirmations == 2 && !confirmed_accept);
    assert(ryz_workbench_ble_submit(NULL, RYZ_V5_BLE_ACTION_CONFIRM_CODE, 40, 0) == ESP_ERR_INVALID_STATE);
    assert(ryz_workbench_ble_submit(NULL, RYZ_V5_BLE_ACTION_CONFIRM_CODE, 41, 1) == ESP_ERR_INVALID_STATE);
    peer.compare_pending = false;
    assert(ryz_workbench_ble_submit(NULL, RYZ_V5_BLE_ACTION_REJECT_CODE, 41, 0) == ESP_ERR_INVALID_STATE);
    peer.compare_pending = true; peer.phase = RYZ_BLE_COMMITTING;
    assert(ryz_workbench_ble_submit(NULL, RYZ_V5_BLE_ACTION_REJECT_CODE, 41, 0) == ESP_ERR_INVALID_STATE);
    peer.phase = RYZ_BLE_COMPARISON; peer.compare_value = 1000000;
    assert(ryz_workbench_ble_submit(NULL, RYZ_V5_BLE_ACTION_CONFIRM_CODE, 41, 1000000) == ESP_ERR_INVALID_STATE);
    assert(confirmations == 2 && !requests);
}

static void boot_case(void)
{
    ryz_ble_snapshot_t s = {0};
    assert(!ryz_workbench_ble_boot_settled(ESP_OK, ESP_OK, &s));
    s.available = s.linked = s.bonded = s.authenticated = true;
    assert(!ryz_workbench_ble_boot_settled(ESP_OK, ESP_OK, &s));
    s.boot_settled = true; s.boot_error = ESP_ERR_TIMEOUT; s.available = false;
    assert(ryz_workbench_ble_boot_settled(ESP_OK, ESP_OK, &s));
    assert(!ryz_workbench_ble_boot_settled(ESP_OK, ESP_FAIL, &s));
    assert(!ryz_workbench_ble_boot_settled(ESP_OK, ESP_OK, NULL));
    assert(ryz_workbench_ble_boot_settled(ESP_ERR_NO_MEM, ESP_FAIL, NULL));
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "model")) model_case();
    else if (!strcmp(argv[1], "invalid")) invalid_model_case();
    else if (!strcmp(argv[1], "commands")) commands_case();
    else if (!strcmp(argv[1], "numeric")) numeric_case();
    else if (!strcmp(argv[1], "boot")) boot_case();
    else if (!strcmp(argv[1], "unavailable-error")) unavailable_error_case();
    else assert(!"unknown case");
    printf("WORKBENCH_BLE_PASS %s\n", argv[1]);
    return 0;
}
