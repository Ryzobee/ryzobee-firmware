#pragma once

#include "ryz_system_services.h"
#include "ryz_v5_network.h"

/* UI Owner adapter. Maps control state only; telemetry and trusted system
 * fields are supplied separately. No hardware, allocation or navigation. */
void ryz_workbench_network_controls(const ryz_system_services_snapshot_t *services,
                                    ryz_v5_network_model_t *view);

/* Submit one native intent through the same service slot used by RPC. The
 * legacy UI name REPROVISION means OPEN_AP, never credential deletion.
 * Retry uses the current completed operation, not a retained UI page label.
 * Caller captures its navigation token before admission and navigates only
 * on success; this function never schedules a future navigation callback. */
esp_err_t ryz_workbench_network_request(ryz_system_ui_action_t intent,
                                       uint32_t *operation_id);
