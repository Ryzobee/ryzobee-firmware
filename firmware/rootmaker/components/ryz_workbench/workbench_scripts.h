#pragma once

#include "ryz_workbench_scripts.h"
#include "ryz_ui_navigation.h"
#include "workbench_write_guard.h"

typedef ryz_workbench_write_guard_t ryz_workbench_scripts_write_guard_t;

/* Sole UI Owner: reader requests/cache copies only, no filesystem work.
 * A valid Scripts modal is active. The caller must close the prior Apps or
 * Scripts controller BEFORE activating the other consumer of the one reader.
 * Subroute changes revoke stale intents while retaining suitable directory and
 * selection data. Inactive closes the reader only if this controller owned it.
 * Boot identity TIMEOUT retries at most twice, separated by 50 Owner ticks;
 * unchanged failed views do not poll forever. A deliberate subroute re-entry
 * or a new catalog revision allows a fresh bounded read sequence.
 * Do not reenter Owner/RX ticks, including from a write-guard callback. */
void ryz_workbench_scripts_owner_tick(bool active, ryz_ui_nav_token_t token,
                                     ryz_v5_scripts_page_t page);
/* Sole existing RX task: bounded boot identity read, source-to-boot copy, or
 * exact confirmed boot.lua deletion. No bulk deletion is reachable.
 * A delete abandoned before RX's short acceptance gate does not perform IO.
 * After acceptance, navigation cannot roll back its side effects; preserve its
 * exact committed/failed/unknown result without late navigation or retry.
 * Settings may offer a new explicit confirmation after a known terminal result;
 * the old modal's boot SHA/revision and result never become a new confirmation.
 * No controller lock spans a reader/Store call or external callback. The guard
 * is mandatory for mutation; failure to acquire it does not call the Store.
 * Its implementation must serialize all Job starts and file writes. */
void ryz_workbench_scripts_rx_tick(const ryz_workbench_scripts_write_guard_t *guard);
