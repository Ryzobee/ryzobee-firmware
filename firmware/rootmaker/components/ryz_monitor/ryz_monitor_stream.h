#pragma once
#include "ryz_monitor.h"

/* Private fixed-storage stream. Every call tries its short atomic lock once;
 * NOT_FINISHED changes no cache transaction. No OS, logger or device calls
 * under this lock. Producers never wait for a UI/JSON consumer. */
typedef struct {
    uint32_t session_id, epoch;
    ryz_monitor_source_t source;
} ryz_monitor_capture_token_t;
esp_err_t ryz_monitor_stream_reset(uint32_t session, uint32_t config_revision,
                                  ryz_monitor_source_t source);
/* Candidate source must already be ready and old capture disabled. Atomically
 * replace view/config/source AND enable capture so the caller can commit its
 * matching configuration under its own short mutex without a second step. */
esp_err_t ryz_monitor_stream_reconfigure(uint32_t session, uint32_t config_revision,
                                        ryz_monitor_source_t source);
esp_err_t ryz_monitor_stream_accept(uint32_t session, bool enabled);
esp_err_t ryz_monitor_stream_token(ryz_monitor_capture_token_t *out);
esp_err_t ryz_monitor_stream_append(ryz_monitor_capture_token_t token,
                                  const void *bytes, size_t kept, size_t original,
                                  int64_t captured_ms);
esp_err_t ryz_monitor_stream_filtered(ryz_monitor_capture_token_t token);
/* Mark admission contention/uncertainty, not a physical loss count. Boot lifetime. */
void ryz_monitor_stream_gap(void);
esp_err_t ryz_monitor_stream_info(ryz_monitor_stream_info_t *out);
esp_err_t ryz_monitor_stream_page(uint32_t session, uint32_t generation,
                                uint32_t after_sequence, ryz_monitor_page_t *out);
esp_err_t ryz_monitor_stream_pause(uint32_t session, uint32_t generation,
                                 bool paused, uint32_t *out_generation);
esp_err_t ryz_monitor_stream_clear(uint32_t session, uint32_t generation,
                                 uint32_t *out_generation);
