#pragma once

#include "ryz_ble.h"
#include "ryz_v5_ble.h"

/* Copy-only projection. No navigation, service mutation or timer-derived radio
 * state; route ownership and pairing-success navigation remain in C UI. */
void ryz_workbench_ble_model(const ryz_ble_snapshot_t *source,
                             esp_err_t snapshot_error,
                             ryz_v5_ble_model_t *out);

/* UI binding: pass the exact displayed operation/value to the service. */
esp_err_t ryz_workbench_ble_submit(void *context, ryz_v5_ble_action_t action,
                                  uint32_t operation_id, uint32_t compare_value);

/* Failed task creation is a settled failure, never a reason to hide recovery
 * UI. Successful admission waits for the backend's actual boot milestone. */
bool ryz_workbench_ble_boot_settled(esp_err_t start_error,
                                   esp_err_t snapshot_error,
                                   const ryz_ble_snapshot_t *snapshot);
