#pragma once
#include "ryz_v5_network.h"
#include "ryz_v5_ble.h"
#include "ryz_v5_scripts.h"
#include "ryz_v5_version.h"

/* Trusted single UI Owner cache. Service workers publish their own snapshots;
 * only the Owner maps/copies them here. Never serialize: network has AP secret.
 * Page fields are selected by the navigation token, not service callbacks. */
typedef struct {
    ryz_v5_network_model_t network;
    ryz_v5_ble_model_t ble;
    ryz_v5_scripts_snapshot_t scripts;
    ryz_v5_version_model_t version;
} ryz_v5_status_t;

void ryz_v5_status_get(ryz_v5_status_t *out);
bool ryz_v5_status_set(const ryz_v5_status_t *status);
/* Local view-only toggle; does not read a MAC or call a service. */
void ryz_v5_status_toggle_device_id(void);
/* View-only continuous scroll, with geometry measured from current values. */
bool ryz_v5_detail_scroll(ryz_system_ui_route_t route, int delta);
void ryz_v5_detail_scroll_reset(ryz_system_ui_route_t route);
esp_err_t ryz_v5_detail_draw(lv_obj_t *root, ryz_system_ui_route_t route,
                             const ryz_system_ui_snapshot_t *system);
const char *ryz_v5_detail_title(ryz_system_ui_route_t route);
ryz_v5_network_action_t ryz_v5_network_route_hit(ryz_system_ui_route_t route,
                                                 uint16_t x,uint16_t y);
ryz_v5_ble_action_t ryz_v5_ble_route_hit(ryz_system_ui_route_t route,
                                         uint16_t x,uint16_t y);
ryz_v5_version_action_t ryz_v5_version_route_hit(ryz_system_ui_route_t route,
                                                 uint16_t x,uint16_t y);
bool ryz_v5_scripts_route_hit(ryz_system_ui_route_t route,uint16_t x,uint16_t y,
                               ryz_v5_scripts_intent_t *out);
/* Route adapter never rewrites the service-owned Scripts page/generation.
 * A newly navigated page stays inert until its own snapshot is published. */
void ryz_v5_scripts_route_snapshot(ryz_system_ui_route_t route, ryz_v5_scripts_snapshot_t *out);
void ryz_v5_scripts_admission_error(uint64_t generation, esp_err_t error);
