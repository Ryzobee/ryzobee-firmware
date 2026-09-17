#pragma once
#include "ryz_ble_sdk.h"
#include <stdbool.h>
#include <stddef.h>

#define RYZ_BLE_CCCD_MAX 4
typedef struct {
    bool enabled, local_valid, bonded, our_valid, peer_valid;
    struct ble_store_value_local_irk local;
    struct ble_store_value_sec our, peer;
    uint8_t cccd_count;
    struct ble_store_value_cccd cccd[RYZ_BLE_CCCD_MAX];
    bool csfc_valid;
    struct ble_store_value_csfc csfc;
} ryz_ble_record_t;

void ryz_ble_wipe(void *memory, size_t size);
bool ryz_ble_addr_equal(const ble_addr_t *a, const ble_addr_t *b);
esp_err_t ryz_ble_record_load(ryz_ble_record_t *out);
/** Whole-record atomic NVS commit followed by a fresh read-only handle.
 * Never changes the caller's authoritative cache on error. */
esp_err_t ryz_ble_record_save(const ryz_ble_record_t *record);
void ryz_ble_record_clear_peer(ryz_ble_record_t *record);
