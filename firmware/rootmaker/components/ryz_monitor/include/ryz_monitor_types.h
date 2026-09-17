#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Portable value contract shared by native C and the application facade.
 * Error fields retain native diagnostic integers; no driver handles or SDK
 * dependency. The native Adapter interprets their numeric error namespace. */
#define RYZ_MONITOR_RECORD_BYTES 96U
#define RYZ_MONITOR_CAPACITY 32U
#define RYZ_MONITOR_PAGE_RECORDS 8U

typedef enum { RYZ_MONITOR_SYSTEM = 0, RYZ_MONITOR_UART } ryz_monitor_source_t;
typedef struct {
    ryz_monitor_source_t source;
    int rx, tx;
    uint32_t baud;
} ryz_monitor_config_t;
#define RYZ_MONITOR_DEFAULT_CONFIG ((ryz_monitor_config_t){RYZ_MONITOR_SYSTEM, -1, -1, 0})

typedef enum {
    RYZ_MONITOR_IDLE = 0, RYZ_MONITOR_STARTING, RYZ_MONITOR_RUNNING,
    RYZ_MONITOR_RECONFIGURING, RYZ_MONITOR_STOPPING, RYZ_MONITOR_STOPPED,
    RYZ_MONITOR_FAILED,
} ryz_monitor_phase_t;
typedef enum {
    RYZ_MONITOR_OP_NONE = 0, RYZ_MONITOR_OP_START, RYZ_MONITOR_OP_CONFIGURE,
    RYZ_MONITOR_OP_STOP,
} ryz_monitor_operation_t;

typedef struct {
    uint32_t sequence;
    int64_t captured_ms; /* Boot-monotonic, not UTC or a UART wire timestamp. */
    uint16_t length;
    bool truncated;
    uint8_t bytes[RYZ_MONITOR_RECORD_BYTES]; /* Binary-safe; not a C string. */
} ryz_monitor_record_t;

typedef struct {
    uint32_t session_id, config_revision, view_generation;
    ryz_monitor_source_t source;
    bool accepting, paused, sequence_exhausted;
    /* Boot-lifetime admission-contention latch: input completeness cannot be
     * guaranteed. It is not proof/count of physical input loss, and is not
     * reset by clear or attributed to a new session. */
    bool capture_gap_since_boot;
    uint32_t first_sequence, last_sequence;
    uint16_t retained_records;
    uint64_t chunks, bytes, overwritten_chunks, overwritten_bytes;
    uint64_t truncated_bytes, filtered_logs;
} ryz_monitor_stream_info_t;

typedef struct {
    uint64_t fifo_overflow, buffer_full, frame_error, parity_error, break_events;
    /* SDK events themselves may be dropped. These are observations, not exact
     * physical loss counts, even when the counters are all zero. */
    bool events_may_be_lost;
} ryz_monitor_uart_events_t;

typedef struct {
    ryz_monitor_config_t config, capture_config, requested_config;
    uint32_t config_revision, session_id, operation_id;
    ryz_monitor_operation_t operation;
    ryz_monitor_phase_t phase;
    bool operation_pending, source_active, resources_held;
    int operation_error, cleanup_error, restore_error;
    int64_t started_ms, operation_finished_ms;
    ryz_monitor_stream_info_t stream;
    ryz_monitor_uart_events_t uart_events;
} ryz_monitor_snapshot_t;

typedef struct {
    ryz_monitor_stream_info_t stream;
    uint32_t next_sequence;
    bool gap, more;
    uint16_t count;
    ryz_monitor_record_t records[RYZ_MONITOR_PAGE_RECORDS];
} ryz_monitor_page_t;
