#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "ryz_monitor_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SYSTEM accepts only -1/-1/0, is a filtered ESP diagnostic tap, not raw
 * stdout, Lua output, RPC frames or UART0 RX. UART is receive-only on UART1,
 * 8N1; RX/TX must be distinct eligible pins. TX is leased but not driven.
 * Baud: 1200,2400,4800,9600,19200,38400,57600,115200,230400,460800,921600.
 * Validation is pure; resource availability is checked again by the worker. */
esp_err_t ryz_monitor_validate_config(const ryz_monitor_config_t *config);

/* Async requests use one boot-lifetime worker pinned CPU1. No GPIO/UART or
 * logger installation in callers. Only one operation may be pending, except
 * token-matched stop may supersede start/configure. IDs never wrap; start's
 * operation ID is also its new session ID. Query status after accepted ACK. */
esp_err_t ryz_monitor_start(uint32_t expected_revision, uint32_t *out_operation);
/* CONFIGURE is software-only when inactive. While running it replaces the
 * input: stop capture, release old source, open candidate, then commit config
 * and clear the view. On failure retain old config and try to restore it;
 * restoration failure is explicit, never fake continued reception. Capture
 * session stays the same on running reconfiguration; old view tokens expire.
 * expected_session is 0 only before the first session, otherwise the current
 * session ID, preventing late forms from affecting a later run. */
esp_err_t ryz_monitor_configure(const ryz_monitor_config_t *config,
                              uint32_t expected_revision, uint32_t expected_session,
                              uint32_t *out_operation);
/* Stops capture before teardown; STOPPED only after confirmed release. The
 * same FAILED/held session can retry cleanup, with no new receive operation. */
esp_err_t ryz_monitor_stop(uint32_t session_id, uint32_t *out_operation);

/* Cache-only CAS actions; only RUNNING or inactive historical views, never
 * during start/reconfigure/stop. Pause freezes a separate fixed-size view;
 * live reception continues with bounded overwrite. Resume jumps to live.
 * Clear removes both views and expires in-flight capture tokens, not bytes
 * still buffered in UART hardware/SDK. Session counters survive clear. */
esp_err_t ryz_monitor_set_paused(uint32_t session_id, uint32_t expected_generation,
                               bool paused, uint32_t *out_generation);
esp_err_t ryz_monitor_clear(uint32_t session_id, uint32_t expected_generation,
                            uint32_t *out_generation);
/* Cached bounded copies. read is non-consuming; after_sequence=0 starts at
 * the oldest retained record. An evicted cursor returns gap=true. Stale view
 * generation/session or future cursor is rejected. No borrowed pointers.
 * Before worker creation get_snapshot returns INVALID_STATE and clears out. */
esp_err_t ryz_monitor_get_snapshot(ryz_monitor_snapshot_t *out);
esp_err_t ryz_monitor_read(uint32_t session_id, uint32_t view_generation,
                          uint32_t after_sequence, ryz_monitor_page_t *out);

#ifdef __cplusplus
}
#endif
