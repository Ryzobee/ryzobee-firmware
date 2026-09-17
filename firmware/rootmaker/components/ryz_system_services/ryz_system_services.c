#include "ryz_system_services.h"

#include <stddef.h>
#include <stdatomic.h>
#include <string.h>

#include "ryz_ota.h"
#include "ryz_system_services_platform.h"
#include "ryz_system_metrics.h"

#define SERVICE_INTERVAL_MS 200
#define RETRY_INTERVAL_US INT64_C(5000000)
#define BOOT_CONNECT_WINDOW_US INT64_C(10000000)

enum { STOPPED, STARTING, RUNNING };
static atomic_int s_lifecycle = ATOMIC_VAR_INIT(STOPPED);
static ryz_system_services_snapshot_t s_snapshot;
static ryz_system_network_operation_t s_network_operation;
/* Nonblocking admission gate, distinct from the snapshot mutex. Automatic
 * boot control retains it during I/O: competing explicit requests fail fast,
 * whereas already accepted requests always supersede the boot decision. */
static atomic_flag s_network_gate = ATOMIC_FLAG_INIT;
static struct {
    uint32_t requested;
    uint32_t completed;
    esp_err_t result;
} s_reprovision;

typedef struct {
    ryz_system_services_snapshot_t snapshot;
    int64_t retry_at[RYZ_SYSTEM_STAGE_COUNT];
    int64_t log_at[RYZ_SYSTEM_STAGE_COUNT];
    uint32_t suppressed[RYZ_SYSTEM_STAGE_COUNT];
    ryz_system_metrics_t metrics;
    bool boot_start_attempted;
    bool boot_network_hold;
} owner_state_t;

static void record_result(owner_state_t *owner,
                          ryz_system_services_stage_t stage,
                          esp_err_t error, int64_t now)
{
    owner->snapshot.errors[stage] = error;
    if (error == ESP_OK) return;
    if (now < owner->log_at[stage]) {
        if (owner->suppressed[stage] != UINT32_MAX) ++owner->suppressed[stage];
        return;
    }
    /* Fixed stage/error only: never pass the network/password to a logger. */
    ryz_system_services_platform_report(stage, error, owner->suppressed[stage]);
    owner->suppressed[stage] = 0;
    owner->log_at[stage] = now + RETRY_INTERVAL_US;
}

static void initialize_dependency(owner_state_t *owner, bool *ready,
                                   ryz_system_services_stage_t stage,
                                   esp_err_t (*initialize)(void), int64_t now)
{
    if (*ready || now < owner->retry_at[stage]) return;
    esp_err_t error = initialize();
    *ready = error == ESP_OK;
    owner->retry_at[stage] = now + RETRY_INTERVAL_US;
    record_result(owner, stage, error, now);
}

static void update_prerequisites(owner_state_t *owner, bool network_ready,
                                  int64_t now)
{
    bool clock_valid = false;
    owner->snapshot.time_valid = false;
    memset(&owner->snapshot.time, 0, sizeof(owner->snapshot.time));
    if (owner->snapshot.time_ready) {
        /* Offline must revoke network admission immediately, even during a
         * failed SNTP start/sync backoff. Going offline also resets it. */
        if (!network_ready || now >= owner->retry_at[RYZ_SYSTEM_TIME_NETWORK]) {
            esp_err_t error = ryz_time_set_network_ready(network_ready);
            owner->retry_at[RYZ_SYSTEM_TIME_NETWORK] =
                error == ESP_OK ? 0 : now + RETRY_INTERVAL_US;
            record_result(owner, RYZ_SYSTEM_TIME_NETWORK, error, now);
        }
        esp_err_t error = ryz_time_get_snapshot(&owner->snapshot.time);
        owner->snapshot.time_valid = error == ESP_OK;
        if (error == ESP_OK) {
            clock_valid = owner->snapshot.time.clock_valid;
        } else {
            memset(&owner->snapshot.time, 0, sizeof(owner->snapshot.time));
        }
        record_result(owner, RYZ_SYSTEM_TIME_SNAPSHOT, error, now);
    }
    if (owner->snapshot.ota_ready) {
        esp_err_t error = ryz_ota_set_prerequisites(network_ready, clock_valid);
        record_result(owner, RYZ_SYSTEM_OTA_PREREQUISITES, error, now);
    }
}

static bool enter_network(void)
{ return !atomic_flag_test_and_set_explicit(&s_network_gate, memory_order_acquire); }
static void leave_network(void)
{ atomic_flag_clear_explicit(&s_network_gate, memory_order_release); }

static ryz_system_network_operation_t network_operation(void)
{
    ryz_system_services_platform_lock();
    ryz_system_network_operation_t operation = s_network_operation;
    ryz_system_services_platform_unlock();
    return operation;
}

static void command_phase(ryz_system_network_phase_t phase)
{
    ryz_system_services_platform_lock();
    s_network_operation.phase = phase;
    ryz_system_services_platform_unlock();
}

static void run_network_command(owner_state_t *owner, int64_t now)
{
    ryz_system_network_operation_t operation = network_operation();
    if (operation.requested == operation.completed ||
        operation.requested == owner->snapshot.network_operation.completed) return;

    /* An accepted command already holds new OTA admission. Keep other network
     * prerequisites revoked while waiting; a business FAILED phase alone is
     * never evidence that the old transfer released its handles. */
    update_prerequisites(owner, false, now);
    esp_err_t error = ESP_OK;
    if (owner->snapshot.ota_ready) {
        ryz_ota_snapshot_t ota;
        error = ryz_ota_get_snapshot(&ota);
        if (error == ESP_OK && ota.worker_active) {
            command_phase(RYZ_SYSTEM_NETWORK_WAITING_OTA);
            return;
        }
        if (error == ESP_OK && !ota.cleanup_confirmed)
            error = ota.cleanup_error != ESP_OK ? ota.cleanup_error : ESP_FAIL;
    }
    /* An uninitialized OTA cannot have admitted a transfer, and its future
     * initialization preserves the already-held intent. */
    if (error == ESP_OK) {
        command_phase(RYZ_SYSTEM_NETWORK_APPLYING);
        switch (operation.action) {
        case RYZ_SYSTEM_NETWORK_ON:
            error = ryz_provisioning_set_enabled(true);
            break;
        case RYZ_SYSTEM_NETWORK_OFF:
            error = ryz_provisioning_set_enabled(false);
            break;
        case RYZ_SYSTEM_NETWORK_REPROVISION:
            error = ryz_provisioning_request_reprovision();
            break;
        case RYZ_SYSTEM_NETWORK_OPEN_AP:
            error = ryz_provisioning_open_portal();
            break;
        default:
            error = ESP_ERR_INVALID_ARG;
            break;
        }
    }
    record_result(owner, operation.action == RYZ_SYSTEM_NETWORK_REPROVISION
        ? RYZ_SYSTEM_REPROVISION : RYZ_SYSTEM_NETWORK_CONTROL, error, now);
    operation.completed = operation.requested;
    operation.completed_action = operation.action;
    operation.result = error;
    operation.phase = error == ESP_OK ? RYZ_SYSTEM_NETWORK_DONE : RYZ_SYSTEM_NETWORK_FAILED;
    owner->snapshot.network_operation = operation;
    /* This legacy name means an initial intent has settled, including OFF;
     * healthy-image confirmation must not require a secretly started radio. */
    if (error == ESP_OK) owner->snapshot.provisioning_started = true;
    if (operation.action == RYZ_SYSTEM_NETWORK_REPROVISION) {
        owner->snapshot.reprovision_completed = operation.completed;
        owner->snapshot.reprovision_result = error;
    }
}

static void sample_network(owner_state_t *owner, int64_t now)
{
    owner->snapshot.network_valid = false;
    memset(&owner->snapshot.network, 0, sizeof(owner->snapshot.network));
    owner->snapshot.network.state = RYZ_PROVISIONING_FAILED;
    if (!owner->snapshot.provisioning_ready) return;
    esp_err_t error = ryz_provisioning_get_snapshot(&owner->snapshot.network);
    owner->snapshot.network_valid = error == ESP_OK;
    if (error != ESP_OK) {
        memset(&owner->snapshot.network, 0, sizeof(owner->snapshot.network));
        owner->snapshot.network.state = RYZ_PROVISIONING_FAILED;
        owner->snapshot.network.last_error = error;
    }
    record_result(owner, RYZ_SYSTEM_NETWORK_SNAPSHOT, error, now);
}

/* Only called with the admission gate; release cannot clear a newer command's
 * OTA hold. No public snapshot lock is held while calling another Module. */
static void settle_boot_network(owner_state_t *owner, esp_err_t error, int64_t now)
{
    owner->snapshot.boot_network_settled = true;
    owner->snapshot.boot_network_error = error;
    record_result(owner, RYZ_SYSTEM_BOOT_NETWORK, error, now);
    if (owner->boot_network_hold) {
        (void)ryz_ota_network_hold(false);
        owner->boot_network_hold = false;
    }
}

static void advance_boot_network(owner_state_t *owner, int64_t now)
{
    if (owner->snapshot.boot_network_settled || !enter_network()) return;
    const ryz_system_network_operation_t command = network_operation();
    if (command.requested) {
        /* Its own hold now owns admission. Never release it here, even if
         * this cycle already executed the command but has not published it. */
        owner->boot_network_hold = false;
        settle_boot_network(owner, ESP_OK, now);
    } else if (!owner->snapshot.provisioning_ready) {
        settle_boot_network(owner, owner->snapshot.errors[RYZ_SYSTEM_PROVISIONING_INIT], now);
    } else if (!owner->snapshot.network_valid) {
        settle_boot_network(owner, owner->snapshot.errors[RYZ_SYSTEM_NETWORK_SNAPSHOT], now);
    } else if (owner->boot_start_attempted) {
        const ryz_provisioning_snapshot_t *network = &owner->snapshot.network;
        if (!network->boot_restore_pending && owner->snapshot.provisioning_started) {
            /* A first-boot AP may already have accepted a browser submission;
             * that is a different operation with its own 60-second window.
             * Likewise, first ONLINE may precede this sampling cycle. */
            settle_boot_network(owner, network->state == RYZ_PROVISIONING_FAILED ?
                (network->last_error != ESP_OK ? network->last_error : ESP_FAIL) : ESP_OK, now);
        } else if ((!network->enabled && network->stop_confirmed && network->state == RYZ_PROVISIONING_OFF) ||
            (network->enabled && !network->stop_confirmed &&
             ((network->state == RYZ_PROVISIONING_ONLINE && network->ipv4[0]) ||
              (network->state == RYZ_PROVISIONING_AP_READY && network->portal_active && network->portal_ip[0])))) {
            settle_boot_network(owner, ESP_OK, now);
        } else if (!network->enabled || !network->credentials_stored ||
                   (network->state != RYZ_PROVISIONING_CONNECTING &&
                    network->state != RYZ_PROVISIONING_CREDENTIALS_RECEIVED &&
                    network->state != RYZ_PROVISIONING_FAILED)) {
            settle_boot_network(owner, network->last_error != ESP_OK ? network->last_error : ESP_FAIL, now);
        } else if (network->state == RYZ_PROVISIONING_FAILED ||
                   now < 0 || now >= owner->snapshot.boot_network_deadline_us) {
            /* This is not manual OPEN_AP: the platform rechecks live STA
             * under its network lock before deciding to stop it. */
            if (!owner->boot_network_hold) {
                (void)ryz_ota_network_hold(true);
                owner->boot_network_hold = true;
            }
            update_prerequisites(owner, false, now);
            esp_err_t error = ESP_OK;
            bool waiting = false;
            if (owner->snapshot.ota_ready) {
                ryz_ota_snapshot_t ota;
                error = ryz_ota_get_snapshot(&ota);
                if (error == ESP_OK) {
                    waiting = ota.worker_active;
                    if (!waiting && !ota.cleanup_confirmed)
                        error = ota.cleanup_error != ESP_OK ? ota.cleanup_error : ESP_FAIL;
                }
            }
            if (!waiting) {
                if (error == ESP_OK) {
                    error = ryz_provisioning_boot_fallback();
                    if (error == ESP_OK) owner->snapshot.provisioning_started = true;
                    sample_network(owner, now);
                    if (error == ESP_OK && !owner->snapshot.network_valid)
                        error = owner->snapshot.errors[RYZ_SYSTEM_NETWORK_SNAPSHOT];
                }
                settle_boot_network(owner, error, now);
            }
        }
    }
    leave_network();
}

static void owner_task(void *unused)
{
    (void)unused;
    owner_state_t owner = {.snapshot.boot_network_error = ESP_ERR_NOT_FINISHED};
    for (;;) {
        int64_t now = ryz_system_services_platform_now_us();
        initialize_dependency(&owner, &owner.snapshot.provisioning_ready,
                              RYZ_SYSTEM_PROVISIONING_INIT,
                              ryz_provisioning_init, now);
        initialize_dependency(&owner, &owner.snapshot.time_ready,
                              RYZ_SYSTEM_TIME_INIT, ryz_time_init, now);
        initialize_dependency(&owner, &owner.snapshot.ota_ready,
                              RYZ_SYSTEM_OTA_INIT, ryz_ota_init, now);

        /* Any explicit command supersedes automatic boot startup, including
         * when that command ultimately fails. No hidden radio ON after a
         * settled boot error; initialization itself may still recover. */
        ryz_system_network_operation_t command = network_operation();
        if (!command.requested && owner.snapshot.provisioning_ready &&
            !owner.snapshot.boot_network_settled && !owner.boot_start_attempted && enter_network()) {
            if (!network_operation().requested) {
                owner.boot_start_attempted = true;
                esp_err_t error = ryz_provisioning_start();
                owner.snapshot.provisioning_started = error == ESP_OK;
                const int64_t started = ryz_system_services_platform_now_us();
                owner.snapshot.boot_network_deadline_us = started < 0 ? 0 :
                    (started > INT64_MAX - BOOT_CONNECT_WINDOW_US ? INT64_MAX : started + BOOT_CONNECT_WINDOW_US);
                record_result(&owner, RYZ_SYSTEM_PROVISIONING_START, error, now);
            }
            leave_network();
        }
        const uint32_t previously_completed = owner.snapshot.network_operation.completed;
        run_network_command(&owner, now);
        const bool completed_this_cycle =
            owner.snapshot.network_operation.completed != previously_completed;

        sample_network(&owner, now);
        advance_boot_network(&owner, ryz_system_services_platform_now_us());
        if (completed_this_cycle) {
            /* Capture the actual post-action sample once. A contended final
             * publication gate or later link telemetry cannot relabel it. */
            owner.snapshot.network_operation.completed_network_valid = owner.snapshot.network_valid;
            owner.snapshot.network_operation.completed_network_state = owner.snapshot.network_valid
                ? owner.snapshot.network.state : RYZ_PROVISIONING_UNCONFIGURED;
        }
        const bool network_ready = owner.snapshot.network_valid &&
            owner.snapshot.network.state == RYZ_PROVISIONING_ONLINE &&
            !owner.boot_network_hold &&
            command.requested == owner.snapshot.network_operation.completed;
        /* Always observe time, even when the network revision did not change:
         * the SNTP callback can validate TLS while Lua occupies the UI owner. */
        update_prerequisites(&owner, network_ready, now);
        /* Optional chip telemetry shares this owner but neither controls the
         * dependency state machines nor gates successful boot confirmation. */
        ryz_system_metrics_poll(&owner.metrics, &owner.snapshot.metrics);
        ++owner.snapshot.revision;
        ++owner.snapshot.cycles;
        owner.snapshot.updated_at_us = ryz_system_services_platform_now_us();
        /* Keep the slot busy through prerequisite publication. Admission and
         * release share this short gate so a late release cannot undo the next
         * command's OTA hold. A contended gate defers only final publication. */
        command = network_operation();
        bool finishing = command.requested != command.completed &&
            owner.snapshot.network_operation.completed == command.requested;
        bool publish_command = finishing && enter_network();
        if (publish_command) (void)ryz_ota_network_hold(false);
        ryz_system_services_platform_lock();
        s_snapshot = owner.snapshot;
        /* Completion and its resulting network/prerequisite view become
         * observable together. Until here the command slot remains busy. */
        if (publish_command) {
            s_network_operation = owner.snapshot.network_operation;
            s_reprovision.completed = owner.snapshot.reprovision_completed;
            s_reprovision.result = owner.snapshot.reprovision_result;
        }
        ryz_system_services_platform_unlock();
        if (publish_command) leave_network();
        ryz_system_services_platform_wait_ms(SERVICE_INTERVAL_MS);
    }
}

esp_err_t ryz_system_services_start(void)
{
    int expected = STOPPED;
    if (!atomic_compare_exchange_strong(&s_lifecycle, &expected, STARTING)) {
        return expected == RUNNING ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = ryz_system_services_platform_init();
    if (error == ESP_OK) {
        memset(&s_snapshot, 0, sizeof(s_snapshot));
        s_snapshot.metrics.temperature_error=ESP_ERR_NOT_FINISHED;
        s_snapshot.metrics.load_error=ESP_ERR_NOT_FINISHED;
        s_snapshot.network.state = RYZ_PROVISIONING_FAILED;
        s_snapshot.boot_network_error = ESP_ERR_NOT_FINISHED;
        memset(&s_reprovision, 0, sizeof(s_reprovision));
        memset(&s_network_operation, 0, sizeof(s_network_operation));
        error = ryz_system_services_platform_start(owner_task);
        if (error != ESP_OK) ryz_system_services_platform_deinit();
    }
    atomic_store_explicit(&s_lifecycle,
                          error == ESP_OK ? RUNNING : STOPPED,
                          memory_order_release);
    return error;
}

esp_err_t ryz_system_services_get_snapshot(
    ryz_system_services_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL) return ESP_ERR_INVALID_ARG;
    if (atomic_load_explicit(&s_lifecycle, memory_order_acquire) != RUNNING) {
        return ESP_ERR_INVALID_STATE;
    }
    ryz_system_services_platform_lock();
    *out_snapshot = s_snapshot;
    out_snapshot->network_operation = s_network_operation;
    out_snapshot->reprovision_requested = s_reprovision.requested;
    out_snapshot->reprovision_completed = s_reprovision.completed;
    out_snapshot->reprovision_result = s_reprovision.result;
    ryz_system_services_platform_unlock();
    return ESP_OK;
}

esp_err_t ryz_system_services_request_network(ryz_system_network_action_t action,
                                             uint32_t *out_operation_id)
{
    if (out_operation_id == NULL) return ESP_ERR_INVALID_ARG;
    *out_operation_id = 0;
    if (action != RYZ_SYSTEM_NETWORK_ON && action != RYZ_SYSTEM_NETWORK_OFF &&
        action != RYZ_SYSTEM_NETWORK_REPROVISION && action != RYZ_SYSTEM_NETWORK_OPEN_AP)
        return ESP_ERR_INVALID_ARG;
    if (atomic_load_explicit(&s_lifecycle, memory_order_acquire) != RUNNING || !enter_network()) {
        return ESP_ERR_INVALID_STATE;
    }
    ryz_system_services_platform_lock();
    if (!s_snapshot.provisioning_ready ||
        s_network_operation.requested != s_network_operation.completed ||
        s_network_operation.requested == UINT32_MAX) {
        ryz_system_services_platform_unlock();
        leave_network();
        return ESP_ERR_INVALID_STATE;
    }
    ryz_system_services_platform_unlock();
    /* No service snapshot lock across another Module. This hold is accepted
     * even before OTA init; its result does not claim transfer cancellation. */
    (void)ryz_ota_network_hold(true);
    ryz_system_services_platform_lock();
    ++s_network_operation.requested;
    s_network_operation.action = action;
    s_network_operation.phase = RYZ_SYSTEM_NETWORK_QUEUED;
    *out_operation_id = s_network_operation.requested;
    if (action == RYZ_SYSTEM_NETWORK_REPROVISION)
        s_reprovision.requested = s_network_operation.requested;
    ryz_system_services_platform_unlock();
    leave_network();
    return ESP_OK;
}

esp_err_t ryz_system_services_request_reprovision(uint32_t *out_operation_id)
{
    return ryz_system_services_request_network(RYZ_SYSTEM_NETWORK_REPROVISION, out_operation_id);
}
