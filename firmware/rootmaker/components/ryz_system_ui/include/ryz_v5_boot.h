#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "ryz_provisioning.h"

/* Boot-only screen on the existing system LVGL lease. All calls belong to
 * the same UI Owner as the subsequent normal pages. No input or Lua lease.
 * begin presents the initial frame synchronously, before other startup work.
 * tick accepts elapsed boot milliseconds; only the 80x2 rail can change.
 * network replaces the footer with static-font status, without network I/O.
 * timeout freezes the animation and replaces STARTING with START TIMEOUT.
 * end releases objects without clearing the physical panel. */
esp_err_t ryz_v5_boot_begin(void);
esp_err_t ryz_v5_boot_tick(uint32_t elapsed_ms);
/** NULL means the owner has settled a network initialization/read failure.
 * Non-NULL is a coherent copy from provisioning, including during its scan. */
esp_err_t ryz_v5_boot_network(const ryz_provisioning_snapshot_t *network);
esp_err_t ryz_v5_boot_timeout(void);
esp_err_t ryz_v5_boot_end(void);
