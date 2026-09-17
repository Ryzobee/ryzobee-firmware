#pragma once

#include "ryz_v5_scripts.h"

/* Boot-once initialization before Owner/RX tasks; no IO, allocation or task. */
esp_err_t ryz_workbench_scripts_init(void);
/* Copy-only, nonblocking admission. Non-OK always clears a non-NULL output.
 * generation is a service view token, not the navigation token. Zero is closed.
 * Boot/APPLY/RESTART remain unavailable until the user startup contract exists. */
esp_err_t ryz_workbench_scripts_get_snapshot(ryz_v5_scripts_snapshot_t *out);
/* One copied intent slot. PAGE/SELECT/DELETE_ALL are the service actions;
 * navigation stays with the system UI. Stale generation/revision is rejected.
 * Acceptance is not proof of completed filesystem work. */
esp_err_t ryz_workbench_scripts_submit(const ryz_v5_scripts_intent_t *intent);
