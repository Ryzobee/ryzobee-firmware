#pragma once

#include "ryz_v5_status.h"

/* Pure, allocation-free comparisons for the current original V5 route. They
 * report visible or conservatively safety-relevant changes, not pixel equality.
 * Unknown routes or missing snapshots require a repaint. Neither call changes
 * navigation, input, privacy state, source data, nor the existing dirty latch.
 *
 * Keep full background snapshots current even when these return false. They
 * are NOT presentation acknowledgements: force/navigation/failed renders and
 * the existing Apps/Scripts/password controllers retain their own dirty and
 * checked-cleanup gates. Future renderer fields must update this policy. */
bool ryz_v5_system_visible_changed(ryz_system_ui_route_t route,
    const ryz_system_ui_snapshot_t *before,
    const ryz_system_ui_snapshot_t *after);
bool ryz_v5_status_visible_changed(ryz_system_ui_route_t route,
    const ryz_v5_status_t *before, const ryz_v5_status_t *after);

/* Sole UI Owner. Compare with the existing status cache, then publish the
 * ENTIRE next snapshot regardless of visibility. No second large status copy
 * is allocated on the UI stack. NULL does not publish and requires repaint.
 * The older ryz_v5_status_set keeps its full-state-change return contract. */
bool ryz_v5_status_publish_visible(ryz_system_ui_route_t route,
    const ryz_v5_status_t *next);
