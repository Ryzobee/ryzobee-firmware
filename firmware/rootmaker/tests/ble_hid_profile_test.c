#include "ryz_ble_hid_profile.h"
#include "hid_profile_sdk.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct ble_gatt_svc_def *registered;
static bool authorized, allocation_fail;
static int count_error, add_error, notify_error, count_calls, add_calls;
static unsigned sends;
static struct { uint16_t conn, attr; uint8_t data; } sent[32];
static uint16_t report_handle, battery_handle;

static bool allow(uint16_t conn, void *context) {
    assert(context == &authorized);
    return authorized && conn == 7;
}
static uint16_t uuid16(const ble_uuid_t *uuid) {
    assert(uuid && uuid->type == 16);
    return ((const ble_uuid16_t *)uuid)->value;
}
int ble_gatts_count_cfg(const struct ble_gatt_svc_def *defs) {
    assert(defs); count_calls++; return count_error;
}
int ble_gatts_add_svcs(const struct ble_gatt_svc_def *defs) {
    add_calls++;
    if (add_error) return add_error;
    registered = defs;
    uint16_t handle = 20;
    for (const struct ble_gatt_svc_def *svc = defs; svc->type; ++svc) {
        for (const struct ble_gatt_chr_def *chr = svc->characteristics; chr->uuid; ++chr) {
            if (chr->val_handle) *chr->val_handle = handle;
            if (uuid16(chr->uuid) == 0x2a4d) report_handle = handle;
            if (uuid16(chr->uuid) == 0x2a19) battery_handle = handle;
            handle += 4;
        }
    }
    return 0;
}
int os_mbuf_append(struct os_mbuf *om, const void *data, uint16_t len) {
    if (om->len + len > sizeof(om->data)) return -1;
    memcpy(om->data + om->len, data, len); om->len += len; return 0;
}
int os_mbuf_copydata(const struct os_mbuf *om, int off, int len, void *dst) {
    if (off < 0 || len < 0 || off + len > om->len) return -1;
    memcpy(dst, om->data + off, (size_t)len); return 0;
}
struct os_mbuf *ble_hs_mbuf_from_flat(const void *data, uint16_t len) {
    if (allocation_fail) return NULL;
    struct os_mbuf *om = calloc(1, sizeof(*om)); assert(om);
    om->allocated = true; assert(os_mbuf_append(om, data, len) == 0); return om;
}
int ble_gatts_notify_custom(uint16_t conn, uint16_t attr, struct os_mbuf *om) {
    assert(om && om->allocated && om->len == 1 && sends < 32);
    sent[sends].conn = conn; sent[sends].attr = attr; sent[sends].data = om->data[0]; sends++;
    free(om); return notify_error; /* Real SDK consumes mbuf even on failure. */
}
static const struct ble_gatt_chr_def *find_chr(uint16_t uuid) {
    for (const struct ble_gatt_svc_def *svc = registered; svc->type; ++svc)
        for (const struct ble_gatt_chr_def *chr = svc->characteristics; chr->uuid; ++chr)
            if (uuid16(chr->uuid) == uuid) return chr;
    return NULL;
}
static int read_chr(uint16_t uuid, struct os_mbuf *out) {
    const struct ble_gatt_chr_def *chr = find_chr(uuid); assert(chr);
    struct ble_gatt_access_ctxt ctx = {.op=BLE_GATT_ACCESS_OP_READ_CHR,.chr=chr,.om=out};
    return chr->access_cb(7, chr->val_handle ? *chr->val_handle : 0, &ctx, chr->arg);
}
static int control(const uint8_t *data, uint16_t len) {
    const struct ble_gatt_chr_def *chr = find_chr(0x2a4c); assert(chr);
    struct os_mbuf om = {0}; assert(os_mbuf_append(&om, data, len) == 0);
    struct ble_gatt_access_ctxt ctx = {.op=BLE_GATT_ACCESS_OP_WRITE_CHR,.chr=chr,.om=&om};
    return chr->access_cb(7, 0, &ctx, chr->arg);
}
static void setup(void) {
    assert(ryz_ble_hid_profile_set_firmware_version("0.9.0-test") == 0);
    assert(ryz_ble_hid_profile_init(allow, &authorized) == 0);
    assert(count_calls == 1 && add_calls == 1 && registered);
    ryz_ble_hid_profile_link(7); authorized = true;
}
static void ready(void) {
    setup(); ryz_ble_hid_profile_subscribe(7, report_handle, true);
    assert(ryz_ble_hid_profile_ready());
}
static void database(void) {
    setup(); unsigned services = 0;
    for (const struct ble_gatt_svc_def *svc = registered; svc->type; ++svc) {
        assert(svc->type == BLE_GATT_SVC_TYPE_PRIMARY); ++services;
        uint16_t id = uuid16(svc->uuid); assert(id == 0x1812 || id == 0x180f || id == 0x180a);
        for (const struct ble_gatt_chr_def *chr = svc->characteristics; chr->uuid; ++chr) {
            assert(chr->min_key_size == 16);
            if (chr->flags & BLE_GATT_CHR_F_READ)
                assert((chr->flags & (BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN)) ==
                       (BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN));
            if (chr->flags & BLE_GATT_CHR_F_NOTIFY)
                assert((chr->flags & (BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC | BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN)) ==
                       (BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC | BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN));
        }
    }
    assert(services == 3 && find_chr(0x2a50) && find_chr(0x2a19));
    assert(!find_chr(0x2a4e) && !find_chr(0x2a22) && !find_chr(0x2a32) && !find_chr(0x2a33));
    const struct ble_gatt_chr_def *report = find_chr(0x2a4d);
    assert(report->flags & BLE_GATT_CHR_F_NOTIFY);
    assert(!(report->flags & (BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP)));
    assert(report->descriptors && uuid16(report->descriptors[0].uuid) == 0x2908);
    assert(report->descriptors[0].min_key_size == 16);
    const struct ble_gatt_dsc_def *dsc = &report->descriptors[0];
    struct os_mbuf om = {0};
    struct ble_gatt_access_ctxt ctx = {.op=BLE_GATT_ACCESS_OP_READ_DSC,.dsc=dsc,.om=&om};
    assert(dsc->access_cb(7, 0, &ctx, dsc->arg) == 0);
    assert(om.len == 2 && om.data[0] == 1 && om.data[1] == 1);
    authorized = false; om.len = 0;
    assert(dsc->access_cb(7, 0, &ctx, dsc->arg) == BLE_ATT_ERR_INSUFFICIENT_AUTHOR);
    assert(read_chr(0x2a4b, &om) == BLE_ATT_ERR_INSUFFICIENT_AUTHOR && om.len == 0);
    authorized = true;
    assert(read_chr(0x2a29, &om) == 0 && om.len == 7 && !memcmp(om.data, "RyzoBee", 7));
    om.len = 0; assert(read_chr(0x2a24, &om) == 0 && !memcmp(om.data, "RootMaker", 9));
    om.len = 0; assert(read_chr(0x2a26, &om) == 0 && !memcmp(om.data, "0.9.0-test", 10));
}
static void map(void) {
    setup(); struct os_mbuf om = {0}; assert(read_chr(0x2a4b, &om) == 0);
    /* Independent wire contract: Consumer Control Application, Report ID 1,
     * seven 1-bit usages plus one constant pad bit, no keyboard/mouse. */
    static const uint8_t expected[] = {
        0x05,0x0c,0x09,0x01,0xa1,0x01,0x85,0x01,0x15,0x00,0x25,0x01,
        0x75,0x01,0x95,0x07,0x09,0xcd,0x09,0xb7,0x09,0xb5,0x09,0xb6,
        0x09,0xe2,0x09,0xe9,0x09,0xea,0x81,0x02,0x95,0x01,0x81,0x03,0xc0
    };
    assert(om.len == sizeof(expected) && !memcmp(om.data, expected, sizeof(expected)));
    om.len = 0; assert(read_chr(0x2a4a, &om) == 0);
    /* Owner advertises only in bounded windows, not permanent connectability. */
    assert(om.len == 4 && om.data[0] == 0x11 && om.data[1] == 1 && om.data[2] == 0 && om.data[3] == 0);
}
static void tap(void) {
    ready(); assert(sends == 0);
    for (unsigned bit = 0; bit < 7; bit++) {
        uint64_t now = 1000000 + bit * 200000;
        assert(ryz_ble_hid_profile_tap((uint8_t)(1u << bit), now) == 0);
        assert(sent[bit*2].data == (1u << bit) && sent[bit*2].conn == 7);
        assert(sent[bit*2].attr == report_handle && ryz_ble_hid_profile_busy());
        assert(ryz_ble_hid_profile_poll(now + 99999) == 0 && sends == bit*2+1);
        assert(ryz_ble_hid_profile_poll(now + 100000) == 0 && sends == bit*2+2);
        assert(sent[bit*2+1].data == 0 && !ryz_ble_hid_profile_busy());
    }
}
static void gates(void) {
    setup(); assert(!ryz_ble_hid_profile_ready());
    assert(ryz_ble_hid_profile_tap(1, 0) != 0 && sends == 0);
    ryz_ble_hid_profile_subscribe(8, report_handle, true);
    assert(!ryz_ble_hid_profile_ready());
    ryz_ble_hid_profile_subscribe(7, report_handle, true); authorized = false;
    assert(!ryz_ble_hid_profile_ready() && ryz_ble_hid_profile_tap(1, 0) != 0 && sends == 0);
    authorized = true;
    assert(ryz_ble_hid_profile_tap(0, 0) == BLE_HS_EINVAL);
    assert(ryz_ble_hid_profile_tap(128, 0) == BLE_HS_EINVAL);
    assert(ryz_ble_hid_profile_tap(1, UINT64_MAX - 10) == BLE_HS_EINVAL);
    assert(ryz_ble_hid_profile_tap(1, 1) == 0);
    assert(ryz_ble_hid_profile_tap(2, 2) == BLE_HS_EBUSY && sends == 1);
}
static void release(void) {
    ready(); ryz_ble_hid_profile_release(); assert(ryz_ble_hid_profile_poll(0) == 0 && sends == 0);
    assert(ryz_ble_hid_profile_tap(32, 1) == 0); ryz_ble_hid_profile_release();
    assert(ryz_ble_hid_profile_poll(2) == 0 && sends == 2 && sent[1].data == 0);
    ryz_ble_hid_profile_release(); assert(ryz_ble_hid_profile_poll(3) == 0 && sends == 2);
}
static void suspend(void) {
    ready(); const uint8_t suspend = 0, resume = 1, bad = 2;
    assert(control(&bad, 1) == BLE_ATT_ERR_VALUE_NOT_ALLOWED);
    assert(control(&bad, 0) == BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
    uint8_t long_value[2] = {0}; assert(control(long_value, 2) == BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
    assert(control(&suspend, 1) == 0 && !ryz_ble_hid_profile_ready());
    assert(ryz_ble_hid_profile_tap(1, 0) != 0 && sends == 0);
    assert(control(&resume, 1) == 0 && ryz_ble_hid_profile_ready());
    assert(ryz_ble_hid_profile_tap(1, 0) == 0);
    assert(control(&suspend, 1) == 0);
    assert(ryz_ble_hid_profile_poll(100000) != 0 && sends == 1);
}
static void release_fail(void) {
    ready(); assert(ryz_ble_hid_profile_tap(1, 0) == 0); notify_error = 61;
    assert(ryz_ble_hid_profile_poll(100000) == 61);
    notify_error = 0; assert(ryz_ble_hid_profile_poll(200000) == 61);
    assert(sends == 2 && !ryz_ble_hid_profile_ready() && ryz_ble_hid_profile_busy());
    assert(ryz_ble_hid_profile_last_error() == 61);
    ryz_ble_hid_profile_reset(); assert(!ryz_ble_hid_profile_busy() && ryz_ble_hid_profile_poll(0) == 0);
}
static void press_fail(void) {
    ready(); allocation_fail = true;
    assert(ryz_ble_hid_profile_tap(1, 0) == BLE_HS_ENOMEM && sends == 0);
    assert(ryz_ble_hid_profile_poll(1) == BLE_HS_ENOMEM);
    ryz_ble_hid_profile_reset(); ryz_ble_hid_profile_link(7); allocation_fail = false;
    ryz_ble_hid_profile_subscribe(7, report_handle, true); notify_error = 71;
    assert(ryz_ble_hid_profile_tap(1, 0) == 71 && ryz_ble_hid_profile_poll(0) == 71);
}
static void revoke(void) {
    ready(); assert(ryz_ble_hid_profile_tap(1, 0) == 0); authorized = false;
    assert(ryz_ble_hid_profile_poll(100000) != 0 && sends == 1);
    authorized = true; assert(!ryz_ble_hid_profile_ready());
    ryz_ble_hid_profile_reset(); ryz_ble_hid_profile_link(7);
    ryz_ble_hid_profile_subscribe(7, report_handle, true);
    assert(ryz_ble_hid_profile_tap(1, 0) == 0);
    ryz_ble_hid_profile_subscribe(7, report_handle, false);
    assert(ryz_ble_hid_profile_poll(100000) != 0 && sends == 2);
}
static void reset(void) {
    ready(); assert(ryz_ble_hid_profile_tap(1, 0) == 0);
    ryz_ble_hid_profile_reset(); ryz_ble_hid_profile_link(7);
    assert(!ryz_ble_hid_profile_ready() && !ryz_ble_hid_profile_busy());
    assert(ryz_ble_hid_profile_poll(200000) == 0 && sends == 1);
    struct os_mbuf om = {0}; assert(read_chr(0x2a4d, &om) == 0 && om.data[0] == 0);
}
static void unknown(void) {
    setup(); struct os_mbuf om = {0};
    assert(read_chr(0x2a19, &om) == BLE_ATT_ERR_UNLIKELY && om.len == 0);
    assert(read_chr(0x2a50, &om) == BLE_ATT_ERR_UNLIKELY && om.len == 0);
    ryz_ble_hid_profile_update_battery(true, 101);
    assert(read_chr(0x2a19, &om) == BLE_ATT_ERR_UNLIKELY && om.len == 0);
}
static void battery(void) {
    ready(); struct os_mbuf om = {0}; ryz_ble_hid_profile_update_battery(true, 42);
    assert(read_chr(0x2a19, &om) == 0 && om.len == 1 && om.data[0] == 42);
    assert(ryz_ble_hid_profile_poll(0) == 0 && sends == 0);
    ryz_ble_hid_profile_subscribe(7, battery_handle, true);
    assert(ryz_ble_hid_profile_poll(0) == 0 && sends == 1 && sent[0].attr == battery_handle && sent[0].data == 42);
    assert(ryz_ble_hid_profile_poll(1) == 0 && sends == 1);
    ryz_ble_hid_profile_update_battery(true, 42); assert(ryz_ble_hid_profile_poll(2) == 0 && sends == 1);
    ryz_ble_hid_profile_update_battery(false, 0); assert(ryz_ble_hid_profile_poll(3) == 0 && sends == 1);
    ryz_ble_hid_profile_update_battery(true, 43); authorized = false;
    assert(ryz_ble_hid_profile_poll(4) == 0 && sends == 1);
    authorized = true; assert(ryz_ble_hid_profile_poll(5) == 0 && sends == 2 && sent[1].data == 43);
}
static void identity(void) {
    assert(ryz_ble_hid_profile_set_identity(0, 0x1234, 2, 0x100) == BLE_HS_EINVAL);
    assert(ryz_ble_hid_profile_set_identity(3, 0x1234, 2, 0x100) == BLE_HS_EINVAL);
    assert(ryz_ble_hid_profile_set_identity(1, 0xffff, 2, 0x100) == BLE_HS_EINVAL);
    assert(ryz_ble_hid_profile_set_identity(2, 0, 2, 0x100) == BLE_HS_EINVAL);
    assert(ryz_ble_hid_profile_set_identity(1, 0x1234, 2, 0x1a0) == BLE_HS_EINVAL);
    /* Synthetic identity exists only in the host fixture, not product config. */
    assert(ryz_ble_hid_profile_set_identity(1, 0x1234, 0x5678, 0x0109) == 0);
    setup(); struct os_mbuf om = {0}; assert(read_chr(0x2a50, &om) == 0);
    const uint8_t expected[] = {1,0x34,0x12,0x78,0x56,0x09,0x01};
    assert(om.len == sizeof(expected) && !memcmp(om.data, expected, sizeof(expected)));
    assert(ryz_ble_hid_profile_set_firmware_version("changed") == BLE_HS_EBUSY);
}
static void registration(void) {
    assert(ryz_ble_hid_profile_init(NULL, NULL) == BLE_HS_EINVAL && count_calls == 0);
    count_error = 81;
    assert(ryz_ble_hid_profile_init(allow, &authorized) == 81 && add_calls == 0);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    struct { const char *name; void (*run)(void); } cases[] = {
        {"database",database},{"map",map},{"tap",tap},{"gates",gates},{"release",release},
        {"suspend",suspend},{"release_fail",release_fail},{"press_fail",press_fail},
        {"revoke",revoke},{"reset",reset},{"unknown",unknown},{"battery",battery},
        {"identity",identity},{"registration",registration}
    };
    for (unsigned i=0; i<sizeof(cases)/sizeof(cases[0]); ++i) if (!strcmp(argv[1],cases[i].name)) {
        cases[i].run(); puts("RYZ_HID_PROFILE_PASS"); return 0;
    }
    abort();
}
