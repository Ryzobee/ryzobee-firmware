#pragma once

#include <stddef.h>
#include <stdint.h>

/* Private test controls, never linked in device firmware. All filesystem
 * adapters refuse paths outside the explicitly supplied temporary root. */
typedef enum {
    SCRIPT_HOST_WRITE_PARTIAL,
    SCRIPT_HOST_WRITE_CLOSE,
    SCRIPT_HOST_RENAME_BEFORE,
    SCRIPT_HOST_RENAME_AFTER,
    /* Simulated process interruption after a named real rename, exit code 86.
     * This does not model storage-controller caches or SPIFFS power loss. */
    SCRIPT_HOST_RENAME_INTERRUPT_AFTER,
    SCRIPT_HOST_REMOVE_BEFORE,
    SCRIPT_HOST_REMOVE_AFTER,
    SCRIPT_HOST_READ_INVALID_STATE,
    SCRIPT_HOST_VISIT_INVALID_STATE,
    SCRIPT_HOST_CAPACITY_INVALID_STATE,
} script_store_host_fault_t;

void script_store_host_configure(const char *temporary_root);
void script_store_host_fault(script_store_host_fault_t fault,
                             const char *source_leaf,
                             const char *destination_leaf,
                             unsigned skip_matches, unsigned failures);
void script_store_host_add_fault(script_store_host_fault_t fault,
                                 const char *source_leaf,
                                 const char *destination_leaf,
                                 unsigned skip_matches, unsigned failures);
void script_store_host_clear_fault(void);
unsigned script_store_host_fault_hits(void);
void script_store_host_capacity(size_t total, size_t used);
void script_store_host_real_capacity(void);
size_t script_store_host_platform_calls(void);
void script_store_host_reset_allocation_peak(void);
size_t script_store_host_allocation_peak(void);
void script_store_host_sha_gate_arm(void);
void script_store_host_sha_gate_wait(void);
void script_store_host_sha_gate_release(void);
void script_store_host_capacity_gate_arm(void);
void script_store_host_capacity_gate_wait(void);
void script_store_host_capacity_gate_release(void);
void script_store_host_fail_allocation_once(void);
/* Build historic on-disk journal fixtures with the same real SHA primitive. */
void script_store_host_fixture_sha256(const void *bytes, size_t length, char out[65]);
/* Explicit trusted test epoch; defaults to unknown, never the developer clock. */
void script_store_host_utc(int64_t unix_seconds);
