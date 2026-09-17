#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "ryz_provisioning.h"
#include "ryz_time.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RYZ_SYSTEM_PROVISIONING_INIT = 0,
    RYZ_SYSTEM_PROVISIONING_START,
    RYZ_SYSTEM_NETWORK_SNAPSHOT,
    RYZ_SYSTEM_TIME_INIT,
    RYZ_SYSTEM_TIME_NETWORK,
    RYZ_SYSTEM_TIME_SNAPSHOT,
    RYZ_SYSTEM_OTA_INIT,
    RYZ_SYSTEM_OTA_PREREQUISITES,
    RYZ_SYSTEM_REPROVISION,
    RYZ_SYSTEM_NETWORK_CONTROL,
    RYZ_SYSTEM_BOOT_NETWORK,
    RYZ_SYSTEM_STAGE_COUNT,
} ryz_system_services_stage_t;

typedef enum {
    RYZ_SYSTEM_NETWORK_NONE, RYZ_SYSTEM_NETWORK_ON,
    RYZ_SYSTEM_NETWORK_OFF, RYZ_SYSTEM_NETWORK_REPROVISION,
    RYZ_SYSTEM_NETWORK_OPEN_AP,
} ryz_system_network_action_t;
typedef enum {
    RYZ_SYSTEM_NETWORK_IDLE, RYZ_SYSTEM_NETWORK_QUEUED,
    RYZ_SYSTEM_NETWORK_WAITING_OTA, RYZ_SYSTEM_NETWORK_APPLYING,
    RYZ_SYSTEM_NETWORK_DONE, RYZ_SYSTEM_NETWORK_FAILED,
} ryz_system_network_phase_t;
typedef struct {
    uint32_t requested, completed;
    ryz_system_network_action_t action, completed_action;
    ryz_system_network_phase_t phase;
    /** Result belongs to completed; never a result for a still-pending ID. */
    esp_err_t result;
    /** Native snapshot sampled after that completed action, in its completion
     * cycle. Retained unchanged while a newer request waits/runs and during
     * later telemetry. This is a state classification, not a connection epoch.
     * A failed sample sets valid=false and clears state to UNCONFIGURED; never
     * interpret that cleared value as a real network state. Trusted C only. */
    bool completed_network_valid;
    ryz_provisioning_state_t completed_network_state;
} ryz_system_network_operation_t;

/** Optional observations, never part of the boot health gate. Temperature is
 * the chip-internal sensor, not ambient; LOAD is the last measured dual-core
 * scheduler non-idle fraction (0..1000), not a lifetime average. An invalid
 * item has a zero value and its own error; first LOAD needs two samples.
 * sampled_at_us is the actual sampling-start monotonic time, not UTC. Network
 * work can delay this owner; consumers must impose freshness independently. */
typedef struct {
    bool temperature_valid;
    int16_t temperature_deci_c;
    bool load_valid;
    uint16_t load_permille;
    uint64_t sampled_at_us;
    esp_err_t temperature_error;
    esp_err_t load_error;
} ryz_system_metrics_snapshot_t;

/** Trusted C-only state: network contains the local AP QR password. Never
 * serialize this structure wholesale, log it, or expose it to user Lua. */
typedef struct {
    /** Completed-cycle revision. Command acceptance is visible immediately
     * through requested/completed and may precede the next cycle revision. */
    uint32_t revision;
    uint64_t cycles;
    int64_t updated_at_us;
    bool provisioning_ready;
    /** Initial startup or an explicit intent has settled successfully. OFF
     * counts as settled; this is not evidence of an active Wi-Fi radio. */
    bool provisioning_started;
    /** One-shot boot decision, distinct from service startup and OTA health.
     * OFF/AP/ONLINE settle immediately; saved STA gets a 10-second window.
     * Failure also settles so the recovery UI can open. Never resets after
     * disconnection. Explicit controls supersede this automatic decision. */
    bool boot_network_settled;
    esp_err_t boot_network_error;
    /** Monotonic connection deadline, measured after the initial start call.
     * Zero until start returns. Does not bound SDK shutdown/OTA cleanup. */
    int64_t boot_network_deadline_us;
    bool time_ready;
    bool ota_ready;
    bool network_valid;
    bool time_valid;
    esp_err_t errors[RYZ_SYSTEM_STAGE_COUNT];
    ryz_provisioning_snapshot_t network;
    ryz_time_snapshot_t time;
    /** One shared slot for ON/OFF/OPEN_AP/legacy reprovision. Request immediately holds
     * new OTA admission; worker waits for actual transfer cleanup before any
     * Wi-Fi/NVS mutation. ON completion means startup accepted, not ONLINE. */
    ryz_system_network_operation_t network_operation;
    /** One bounded command slot, including execution. Nonzero IDs are local
     * to this boot. Acceptance is not successful credential deletion. */
    uint32_t reprovision_requested;
    uint32_t reprovision_completed;
    /** Result belongs to completed, and is unchanged by a new request. It is
     * meaningful only when completed != 0. */
    esp_err_t reprovision_result;
    ryz_system_metrics_snapshot_t metrics;
} ryz_system_services_snapshot_t;

/** Start the boot-lifetime C owner. Success means its task was created, not
 * that each dependency initialized or that the network is connected. Safe to
 * call again after success; concurrent startup returns INVALID_STATE.
 * Failed task creation can be retried. No display/TP/LVGL calls occur here. */
esp_err_t ryz_system_services_start(void);

/** Copy the latest completed cycle plus current command status. Dependency
 * errors remain explicit; invalid samples must not be treated as ONLINE or a
 * trusted clock. Initialization and retries continue without UI polling. */
esp_err_t ryz_system_services_get_snapshot(
    ryz_system_services_snapshot_t *out_snapshot);

/** Queue the existing explicit forget-and-reprovision action. Only accepted
 * after provisioning initialization, with at most one pending/running command.
 * Returns INVALID_STATE while unavailable/busy; never waits for Wi-Fi/NVS.
 * Observe requested/completed/result to distinguish accepted from finished. */
esp_err_t ryz_system_services_request_reprovision(uint32_t *out_operation_id);

/** Nonblocking command admission; no Wi-Fi/NVS I/O on the caller. Explicit
 * ON/OFF persist the next-boot preference before radio control; a save failure
 * leaves the current radio unchanged and completes with an error. Runtime
 * control failure does not undo a committed preference. All three of
 * ON/OFF/OPEN_AP preserve saved credentials. OPEN_AP explicitly opens local
 * provisioning, even with a saved network; it is not ON or credential erasure.
 * OPEN_AP does not change the persistent ON/OFF preference.
 * IDs never wrap. Before an
 * explicit command completes, no other network command can replace it.
 * Failed OTA cleanup preserves the actual network and reports command failure.
 * Legacy reprovision uses this same slot, retaining its existing diagnostics. */
esp_err_t ryz_system_services_request_network(ryz_system_network_action_t action,
                                             uint32_t *out_operation_id);

#ifdef __cplusplus
}
#endif
