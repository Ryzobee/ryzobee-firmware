#include "ryz_ble_hid_profile.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include <stddef.h>
#include <string.h>

#define RELEASE_US UINT64_C(100000)
#define SECURE_READ (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN)
#define SECURE_NOTIFY (BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC | BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN)
#define SECURE_COMMAND (BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN)

/* Report-only Consumer Control. No keyboard, mouse, Boot Protocol, output
 * report, or remote wake. The report's ID lives in Report Reference, not in
 * the single-byte value sent over GATT. HID spec version 1.11. */
static const uint8_t s_report_map[] = {
    0x05,0x0c,0x09,0x01,0xa1,0x01,0x85,0x01,0x15,0x00,0x25,0x01,
    0x75,0x01,0x95,0x07,0x09,0xcd,0x09,0xb7,0x09,0xb5,0x09,0xb6,
    0x09,0xe2,0x09,0xe9,0x09,0xea,0x81,0x02,0x95,0x01,0x81,0x03,0xc0
};
/* Bounded pairing/reconnect windows are not NormallyConnectable. Claiming
 * bit 1 would promise perpetual connectable advertising while disconnected
 * (HOGP 1.0 section 5.1.4), contrary to the owner's saved OFF/timeout policy. */
static const uint8_t s_hid_info[] = {0x11,0x01,0x00,0x00};
static const uint8_t s_report_reference[] = {1,1};
static const char s_manufacturer[] = "RyzoBee";
static const char s_model[] = "RootMaker";
static char s_firmware[33];
static uint8_t s_pnp_id[7];
static bool s_identity_valid, s_initialized;
static bool s_battery_valid;
static uint8_t s_battery;
static uint16_t s_report_handle, s_battery_handle;
static ryz_ble_hid_authorize_fn s_authorize;
static void *s_context;
static struct {
    uint64_t deadline;
    int fatal, last_error;
    uint16_t conn;
    uint8_t report;
    bool report_subscribed, battery_subscribed, battery_dirty;
    bool suspended, release_requested;
} s = {.conn = BLE_HS_CONN_HANDLE_NONE};

enum attribute {
    ATTR_HID_INFO, ATTR_MAP, ATTR_REPORT, ATTR_REFERENCE, ATTR_CONTROL,
    ATTR_BATTERY, ATTR_MANUFACTURER, ATTR_MODEL, ATTR_FIRMWARE, ATTR_PNP,
};

static bool authorized(uint16_t conn)
{
    return s_initialized && conn != BLE_HS_CONN_HANDLE_NONE && conn == s.conn &&
           s_authorize && s_authorize(conn, s_context);
}

static int append(struct os_mbuf *om, const void *bytes, size_t length)
{
    return os_mbuf_append(om, bytes, (uint16_t)length) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int access_value(uint16_t conn, uint16_t attr,
                        struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr;
    if (!authorized(conn)) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    enum attribute which = (enum attribute)(uintptr_t)arg;
    if (which == ATTR_CONTROL) {
        if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_READ_NOT_PERMITTED;
        if (ctxt->offset != 0) return BLE_ATT_ERR_INVALID_OFFSET;
        if (OS_MBUF_PKTLEN(ctxt->om) != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        uint8_t value;
        if (os_mbuf_copydata(ctxt->om, 0, 1, &value) != 0) return BLE_ATT_ERR_UNLIKELY;
        if (value > 1) return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
        s.suspended = value == 0;
        if (s.suspended && s.report) s.release_requested = true;
        return 0;
    }
    if (ctxt->op != (which == ATTR_REFERENCE ? BLE_GATT_ACCESS_OP_READ_DSC : BLE_GATT_ACCESS_OP_READ_CHR))
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    /* NimBLE ble_gatts_val_access trims long reads by ctxt->offset itself. */
    switch (which) {
    case ATTR_HID_INFO: return append(ctxt->om, s_hid_info, sizeof(s_hid_info));
    case ATTR_MAP: return append(ctxt->om, s_report_map, sizeof(s_report_map));
    case ATTR_REPORT: return append(ctxt->om, &s.report, sizeof(s.report));
    case ATTR_REFERENCE: return append(ctxt->om, s_report_reference, sizeof(s_report_reference));
    case ATTR_BATTERY:
        return s_battery_valid ? append(ctxt->om, &s_battery, 1) : BLE_ATT_ERR_UNLIKELY;
    case ATTR_MANUFACTURER: return append(ctxt->om, s_manufacturer, sizeof(s_manufacturer) - 1);
    case ATTR_MODEL: return append(ctxt->om, s_model, sizeof(s_model) - 1);
    case ATTR_FIRMWARE:
        return s_firmware[0] ? append(ctxt->om, s_firmware, strlen(s_firmware)) : BLE_ATT_ERR_UNLIKELY;
    case ATTR_PNP:
        /* No reserved/third-party VID is substituted for an unknown identity. */
        return s_identity_valid ? append(ctxt->om, s_pnp_id, sizeof(s_pnp_id)) : BLE_ATT_ERR_UNLIKELY;
    default: return BLE_ATT_ERR_UNLIKELY;
    }
}

static struct ble_gatt_dsc_def s_report_descriptors[] = {
    {.uuid=BLE_UUID16_DECLARE(0x2908),
     .att_flags=BLE_ATT_F_READ | BLE_ATT_F_READ_ENC | BLE_ATT_F_READ_AUTHEN,
     .min_key_size=16, .access_cb=access_value, .arg=(void *)ATTR_REFERENCE},
    {0},
};
#define READ_CHR(uuid16, kind) { .uuid=BLE_UUID16_DECLARE(uuid16), .access_cb=access_value, \
    .arg=(void *)(kind), .flags=SECURE_READ, .min_key_size=16 }
static const struct ble_gatt_chr_def s_hid_characteristics[] = {
    READ_CHR(0x2a4a, ATTR_HID_INFO),
    READ_CHR(0x2a4b, ATTR_MAP),
    {.uuid=BLE_UUID16_DECLARE(0x2a4c), .access_cb=access_value, .arg=(void *)ATTR_CONTROL,
     .flags=SECURE_COMMAND, .min_key_size=16},
    {.uuid=BLE_UUID16_DECLARE(0x2a4d), .access_cb=access_value, .arg=(void *)ATTR_REPORT,
     .descriptors=s_report_descriptors, .flags=SECURE_READ | SECURE_NOTIFY,
     .min_key_size=16, .val_handle=&s_report_handle},
    {0},
};
static const struct ble_gatt_chr_def s_battery_characteristics[] = {
    {.uuid=BLE_UUID16_DECLARE(0x2a19), .access_cb=access_value, .arg=(void *)ATTR_BATTERY,
     .flags=SECURE_READ | SECURE_NOTIFY, .min_key_size=16, .val_handle=&s_battery_handle},
    {0},
};
static const struct ble_gatt_chr_def s_information_characteristics[] = {
    READ_CHR(0x2a29, ATTR_MANUFACTURER),
    READ_CHR(0x2a24, ATTR_MODEL),
    READ_CHR(0x2a26, ATTR_FIRMWARE),
    READ_CHR(0x2a50, ATTR_PNP),
    {0},
};
static const struct ble_gatt_svc_def s_services[] = {
    {.type=BLE_GATT_SVC_TYPE_PRIMARY, .uuid=BLE_UUID16_DECLARE(0x1812), .characteristics=s_hid_characteristics},
    {.type=BLE_GATT_SVC_TYPE_PRIMARY, .uuid=BLE_UUID16_DECLARE(0x180f), .characteristics=s_battery_characteristics},
    {.type=BLE_GATT_SVC_TYPE_PRIMARY, .uuid=BLE_UUID16_DECLARE(0x180a), .characteristics=s_information_characteristics},
    {0},
};

void ryz_ble_hid_profile_reset(void)
{
    memset(&s, 0, sizeof(s));
    s.conn = BLE_HS_CONN_HANDLE_NONE;
}

int ryz_ble_hid_profile_init(ryz_ble_hid_authorize_fn authorize, void *context)
{
    if (!authorize) return BLE_HS_EINVAL;
    if (s_initialized) return BLE_HS_EBUSY;
    int rc = ble_gatts_count_cfg(s_services);
    if (!rc) rc = ble_gatts_add_svcs(s_services);
    if (rc) return rc;
    s_authorize = authorize;
    s_context = context;
    s_initialized = true;
    ryz_ble_hid_profile_reset();
    return 0;
}

void ryz_ble_hid_profile_link(uint16_t conn)
{
    ryz_ble_hid_profile_reset();
    s.conn = conn;
}

void ryz_ble_hid_profile_subscribe(uint16_t conn, uint16_t attr, bool notify)
{
    if (conn == BLE_HS_CONN_HANDLE_NONE || conn != s.conn) return;
    if (s_report_handle && attr == s_report_handle) s.report_subscribed = notify;
    if (s_battery_handle && attr == s_battery_handle) {
        s.battery_subscribed = notify;
        s.battery_dirty = notify && s_battery_valid;
    }
}

bool ryz_ble_hid_profile_ready(void)
{
    return !s.fatal && !s.suspended && s_report_handle && s.report_subscribed && authorized(s.conn);
}

bool ryz_ble_hid_profile_busy(void) { return s.report != 0; }
int ryz_ble_hid_profile_last_error(void) { return s.last_error; }

static int fatal(int rc)
{
    s.last_error = s.fatal = rc ? rc : BLE_HS_EUNKNOWN;
    return s.fatal;
}

static int notify_byte(uint16_t attr, uint8_t value)
{
    /* Recheck immediately before allocating/sending, including a release. */
    if (!authorized(s.conn)) return BLE_HS_EAUTHEN;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(&value, 1);
    if (!om) return BLE_HS_ENOMEM;
    /* ble_gatts_notify_custom consumes om on success AND failure. */
    return ble_gatts_notify_custom(s.conn, attr, om);
}

int ryz_ble_hid_profile_tap(uint8_t bits, uint64_t now_us)
{
    if (!bits || (bits & 0x80) || now_us > UINT64_MAX - RELEASE_US) return BLE_HS_EINVAL;
    if (s.fatal) return s.fatal;
    if (s.report) return BLE_HS_EBUSY;
    if (!ryz_ble_hid_profile_ready()) return BLE_HS_EAUTHEN;
    /* Retain pending state even on ambiguous transport failure. The owner
     * must disconnect; clearing here could permit a second nonzero report. */
    s.report = bits;
    s.deadline = now_us + RELEASE_US;
    int rc = notify_byte(s_report_handle, bits);
    if (rc) return fatal(rc);
    s.last_error = 0;
    return 0;
}

void ryz_ble_hid_profile_release(void)
{
    if (s.report) s.release_requested = true;
}

int ryz_ble_hid_profile_poll(uint64_t now_us)
{
    if (s.fatal) return s.fatal;
    if (s.report) {
        /* No more notifications after suspend/unsubscribe/revocation. A
         * disconnect is the only safe release when a zero cannot be sent. */
        if (!ryz_ble_hid_profile_ready()) return fatal(BLE_HS_EAUTHEN);
        if (s.release_requested || now_us >= s.deadline) {
            int rc = notify_byte(s_report_handle, 0);
            if (rc) return fatal(rc);
            s.report = 0;
            s.release_requested = false;
            s.deadline = 0;
        }
    }
    if (s.battery_dirty && s.battery_subscribed && s_battery_valid && !s.suspended && authorized(s.conn)) {
        int rc = notify_byte(s_battery_handle, s_battery);
        if (rc) return fatal(rc);
        s.battery_dirty = false;
    }
    return 0;
}

void ryz_ble_hid_profile_update_battery(bool valid, uint8_t percent)
{
    valid = valid && percent <= 100;
    if (valid && (!s_battery_valid || percent != s_battery)) s.battery_dirty = true;
    if (!valid) s.battery_dirty = false;
    s_battery_valid = valid;
    if (valid) s_battery = percent;
}

int ryz_ble_hid_profile_set_identity(uint8_t source, uint16_t vendor,
                                    uint16_t product, uint16_t version_bcd)
{
    if (s_initialized) return BLE_HS_EBUSY;
    if ((source != 1 && source != 2) || vendor == 0 || vendor == UINT16_MAX) return BLE_HS_EINVAL;
    for (unsigned shift = 0; shift < 16; shift += 4)
        if (((version_bcd >> shift) & 0xf) > 9) return BLE_HS_EINVAL;
    s_pnp_id[0] = source;
    s_pnp_id[1] = (uint8_t)vendor; s_pnp_id[2] = (uint8_t)(vendor >> 8);
    s_pnp_id[3] = (uint8_t)product; s_pnp_id[4] = (uint8_t)(product >> 8);
    s_pnp_id[5] = (uint8_t)version_bcd; s_pnp_id[6] = (uint8_t)(version_bcd >> 8);
    s_identity_valid = true;
    return 0;
}

int ryz_ble_hid_profile_set_firmware_version(const char *version)
{
    if (s_initialized) return BLE_HS_EBUSY;
    if (!version || !version[0]) return BLE_HS_EINVAL;
    size_t length = 0;
    while (length < sizeof(s_firmware) && version[length]) ++length;
    if (length == sizeof(s_firmware)) return BLE_HS_EINVAL;
    memcpy(s_firmware, version, length + 1);
    return 0;
}
