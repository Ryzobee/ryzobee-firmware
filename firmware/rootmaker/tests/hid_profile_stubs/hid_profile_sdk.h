#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Only the external NimBLE transport is replaced. Values/layout of the used
 * members follow ESP-IDF 5.5.4 NimBLE host public headers. */
typedef struct { uint8_t type; } ble_uuid_t;
typedef struct { ble_uuid_t u; uint16_t value; } ble_uuid16_t;
#define BLE_UUID16_INIT(n) { .u = {16}, .value = (n) }
#define BLE_UUID16_DECLARE(n) ((const ble_uuid_t *)&(const ble_uuid16_t)BLE_UUID16_INIT(n))
#define BLE_HS_CONN_HANDLE_NONE 0xffff
#define BLE_HS_EINVAL 3
#define BLE_HS_ENOMEM 6
#define BLE_HS_ENOTCONN 7
#define BLE_HS_ENOTSUP 8
#define BLE_HS_EBUSY 15
#define BLE_HS_EAUTHEN 23
#define BLE_HS_EUNKNOWN 17
#define BLE_ATT_ERR_READ_NOT_PERMITTED 2
#define BLE_ATT_ERR_WRITE_NOT_PERMITTED 3
#define BLE_ATT_ERR_INSUFFICIENT_AUTHOR 8
#define BLE_ATT_ERR_INVALID_OFFSET 7
#define BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN 13
#define BLE_ATT_ERR_UNLIKELY 14
#define BLE_ATT_ERR_INSUFFICIENT_RES 17
#define BLE_ATT_ERR_VALUE_NOT_ALLOWED 19
#define BLE_ATT_F_READ 1
#define BLE_ATT_F_READ_ENC 4
#define BLE_ATT_F_READ_AUTHEN 8
#define BLE_GATT_CHR_F_READ 0x2
#define BLE_GATT_CHR_F_WRITE_NO_RSP 0x4
#define BLE_GATT_CHR_F_WRITE 0x8
#define BLE_GATT_CHR_F_NOTIFY 0x10
#define BLE_GATT_CHR_F_READ_ENC 0x200
#define BLE_GATT_CHR_F_READ_AUTHEN 0x400
#define BLE_GATT_CHR_F_WRITE_ENC 0x1000
#define BLE_GATT_CHR_F_WRITE_AUTHEN 0x2000
#define BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC 0x8000
#define BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN 0x10000
#define BLE_GATT_SVC_TYPE_PRIMARY 1
#define BLE_GATT_ACCESS_OP_READ_CHR 0
#define BLE_GATT_ACCESS_OP_WRITE_CHR 1
#define BLE_GATT_ACCESS_OP_READ_DSC 2
#define BLE_GATT_ACCESS_OP_WRITE_DSC 3

struct os_mbuf { uint16_t len; uint8_t data[256]; bool allocated; };
#define OS_MBUF_PKTLEN(m) ((m)->len)
struct ble_gatt_access_ctxt;
typedef int ble_gatt_access_fn(uint16_t, uint16_t, struct ble_gatt_access_ctxt *, void *);
struct ble_gatt_dsc_def {
    const ble_uuid_t *uuid; uint8_t att_flags, min_key_size;
    ble_gatt_access_fn *access_cb; void *arg;
};
struct ble_gatt_chr_def {
    const ble_uuid_t *uuid; ble_gatt_access_fn *access_cb; void *arg;
    struct ble_gatt_dsc_def *descriptors; uint32_t flags;
    uint8_t min_key_size; uint16_t *val_handle; void *cpfd;
};
struct ble_gatt_svc_def {
    uint8_t type; const ble_uuid_t *uuid;
    const struct ble_gatt_svc_def **includes;
    const struct ble_gatt_chr_def *characteristics;
};
struct ble_gatt_access_ctxt {
    uint8_t op; struct os_mbuf *om;
    union { const struct ble_gatt_chr_def *chr; const struct ble_gatt_dsc_def *dsc; };
    uint16_t offset;
};
int ble_gatts_count_cfg(const struct ble_gatt_svc_def *defs);
int ble_gatts_add_svcs(const struct ble_gatt_svc_def *defs);
int os_mbuf_append(struct os_mbuf *om, const void *data, uint16_t len);
int os_mbuf_copydata(const struct os_mbuf *om, int off, int len, void *dst);
struct os_mbuf *ble_hs_mbuf_from_flat(const void *data, uint16_t len);
int ble_gatts_notify_custom(uint16_t conn, uint16_t attr, struct os_mbuf *om);
