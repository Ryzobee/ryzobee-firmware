#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Private, single-owner Interface. Every entry runs on the NimBLE host owner;
 * callers must not call these functions from Lua/UI tasks. authorize must
 * verify this connection's current encryption, MITM and durable bond. */
typedef bool (*ryz_ble_hid_authorize_fn)(uint16_t conn, void *context);
int ryz_ble_hid_profile_init(ryz_ble_hid_authorize_fn authorize, void *context);
void ryz_ble_hid_profile_link(uint16_t conn);
/* Only after physical disconnection / host reset, not to discard a held key. */
void ryz_ble_hid_profile_reset(void);
void ryz_ble_hid_profile_subscribe(uint16_t conn, uint16_t attr, bool notify);
bool ryz_ble_hid_profile_ready(void);
bool ryz_ble_hid_profile_busy(void);
/* bit 0 play/pause; 1 stop; 2 next; 3 previous; 4 mute; 5 volume+; 6 volume-.
 * Press is explicit only; busy rejects. The owner must poll at least every
 * 100 ms, and stop admitting taps as soon as script cancellation is admitted. */
int ryz_ble_hid_profile_tap(uint8_t bits, uint64_t now_us);
void ryz_ble_hid_profile_release(void);
/* A nonzero result is fatal for this connection: owner must terminate it and
 * wait for disconnection before reset/link. Never silently clear held state. */
int ryz_ble_hid_profile_poll(uint64_t now_us);
int ryz_ble_hid_profile_last_error(void);
void ryz_ble_hid_profile_update_battery(bool valid, uint8_t percent);
/* Explicitly provision only an assigned Bluetooth (1) / USB (2) vendor ID.
 * No fabricated default: unknown identity reads fail, so an unprovisioned
 * development candidate MUST NOT be described as a conforming HOGP product.
 * Call before GATT startup; this does not alter the database structure. */
int ryz_ble_hid_profile_set_identity(uint8_t source, uint16_t vendor,
                                    uint16_t product, uint16_t version_bcd);
/* Copy actual project version, at most 32 bytes, before host/GATT startup. */
int ryz_ble_hid_profile_set_firmware_version(const char *version);
