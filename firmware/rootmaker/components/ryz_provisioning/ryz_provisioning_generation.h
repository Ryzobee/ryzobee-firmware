#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RYZ_PROVISIONING_RECONNECT_MAX_ATTEMPTS 3U
#define RYZ_PROVISIONING_RECONNECT_BACKOFF_BASE_MS 500U

typedef enum {
    RYZ_PROVISIONING_RECONNECT_IGNORE = 0,
    RYZ_PROVISIONING_RECONNECT_RETRY,
    RYZ_PROVISIONING_RECONNECT_FAILED_CREDENTIALS,
    RYZ_PROVISIONING_RECONNECT_FAILED_EXHAUSTED,
} ryz_provisioning_reconnect_action_t;

typedef struct {
    uint32_t generation;
    uint32_t delay_ms;
    uint8_t attempt;
} ryz_provisioning_reconnect_ticket_t;

typedef struct {
    uint32_t generation;
    bool connect_pending;
    uint8_t reconnect_attempts;
    bool reconnect_pending;
} ryz_provisioning_generation_state_t;

static inline void ryz_provisioning_begin_generation(
    ryz_provisioning_generation_state_t *state)
{
    if (state == NULL) {
        return;
    }
    ++state->generation;
    state->connect_pending = false;
    state->reconnect_attempts = 0;
    state->reconnect_pending = false;
}

/**
 * Atomically claim one pending connect command while the caller holds the
 * command-state lock. A stale command must leave a newer pending command
 * untouched.
 */
static inline bool ryz_provisioning_claim_connect_command(
    ryz_provisioning_generation_state_t *state,
    uint32_t command_generation)
{
    if (state == NULL || !state->connect_pending ||
        command_generation != state->generation) {
        return false;
    }
    state->connect_pending = false;
    state->reconnect_attempts = 0;
    state->reconnect_pending = false;
    ++state->generation;
    return true;
}

/**
 * Release a portal command reservation without advancing the generation.
 * Only the command that still owns the current AP reservation may release it;
 * a stale request must not clear a newer request's pending flag.
 */
static inline bool ryz_provisioning_release_connect_command(
    ryz_provisioning_generation_state_t *state,
    bool access_point_mode,
    uint32_t command_generation)
{
    if (state == NULL || !access_point_mode || !state->connect_pending ||
        command_generation != state->generation) {
        return false;
    }
    state->connect_pending = false;
    return true;
}

static inline ryz_provisioning_reconnect_action_t
ryz_provisioning_schedule_reconnect(
    ryz_provisioning_generation_state_t *state,
    bool station_mode,
    bool credentials_failure,
    ryz_provisioning_reconnect_ticket_t *out_ticket)
{
    if (state == NULL || out_ticket == NULL || !station_mode) {
        return RYZ_PROVISIONING_RECONNECT_IGNORE;
    }
    if (credentials_failure) {
        ryz_provisioning_begin_generation(state);
        return RYZ_PROVISIONING_RECONNECT_FAILED_CREDENTIALS;
    }
    if (state->reconnect_pending) {
        return RYZ_PROVISIONING_RECONNECT_IGNORE;
    }
    if (state->reconnect_attempts >=
        RYZ_PROVISIONING_RECONNECT_MAX_ATTEMPTS) {
        ryz_provisioning_begin_generation(state);
        return RYZ_PROVISIONING_RECONNECT_FAILED_EXHAUSTED;
    }

    ++state->reconnect_attempts;
    state->reconnect_pending = true;
    out_ticket->generation = state->generation;
    out_ticket->attempt = state->reconnect_attempts;
    out_ticket->delay_ms =
        (uint32_t)state->reconnect_attempts *
        RYZ_PROVISIONING_RECONNECT_BACKOFF_BASE_MS;
    return RYZ_PROVISIONING_RECONNECT_RETRY;
}

static inline bool ryz_provisioning_claim_reconnect(
    ryz_provisioning_generation_state_t *state,
    bool station_mode,
    const ryz_provisioning_reconnect_ticket_t *ticket)
{
    if (state == NULL || ticket == NULL || !station_mode ||
        !state->reconnect_pending ||
        ticket->generation != state->generation ||
        ticket->attempt != state->reconnect_attempts) {
        return false;
    }
    state->reconnect_pending = false;
    ++state->generation;
    return true;
}

/**
 * Give a got-IP observation already waiting in the mailbox priority over a
 * retry whose deadline just expired. The caller evaluates both facts while
 * holding the same command-state lock.
 */
static inline bool ryz_provisioning_claim_reconnect_unless_online(
    ryz_provisioning_generation_state_t *state,
    bool station_mode,
    const ryz_provisioning_reconnect_ticket_t *ticket,
    bool online_observed,
    uint32_t online_generation)
{
    if (ticket != NULL && online_observed &&
        online_generation == ticket->generation) {
        return false;
    }
    return ryz_provisioning_claim_reconnect(state, station_mode, ticket);
}

/**
 * Mark the current STA epoch online without advancing it. A physical
 * disconnect may already be queued immediately behind got-IP and must remain
 * current; only retry bookkeeping is reset here.
 */
static inline void ryz_provisioning_mark_online(
    ryz_provisioning_generation_state_t *state)
{
    if (state == NULL) {
        return;
    }
    state->reconnect_attempts = 0;
    state->reconnect_pending = false;
}

/**
 * Reconcile the final disconnected level of one coalesced event batch. Seeing
 * got-IP earlier in the same STA epoch closes the previous outage before this
 * new outage is counted.
 */
static inline ryz_provisioning_reconnect_action_t
ryz_provisioning_handle_disconnect(
    ryz_provisioning_generation_state_t *state,
    bool station_mode,
    bool observed_online,
    bool credentials_failure,
    ryz_provisioning_reconnect_ticket_t *out_ticket)
{
    if (state != NULL && station_mode && observed_online) {
        ryz_provisioning_mark_online(state);
    }
    return ryz_provisioning_schedule_reconnect(
        state,
        station_mode,
        credentials_failure,
        out_ticket);
}

static inline bool ryz_provisioning_network_event_is_current(
    const ryz_provisioning_generation_state_t *state,
    bool station_mode,
    uint32_t event_generation)
{
    return state != NULL && station_mode &&
           event_generation == state->generation;
}
