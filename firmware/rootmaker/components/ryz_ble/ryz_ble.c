#include "ryz_ble.h"
#include "ryz_ble_store.h"
#include "ryz_ble_hid_profile.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
#if !defined(RYZ_BLE_HOST_TEST)
#include "esp_app_desc.h"
#endif
#if !defined(RYZ_BLE_HOST_TEST) || defined(RYZ_BLE_INIT_DIAGNOSTICS_HOST_TEST)
#include <inttypes.h>
#include "esp_bt.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#define RYZ_BLE_INIT_DIAGNOSTICS 1
#endif

#define BOOT_WAIT_US UINT64_C(10000000)
#define PAIR_WAIT_US UINT64_C(30000000)
#define STOP_WAIT_US UINT64_C(5000000)
#define POLL_MS 100U

/* ANCS belongs to the iPhone (GATT server). AD type 0x15 requests it;
 * advertising this as our own service (0x07) would describe the wrong role. */
static const ble_uuid128_t s_ancs_uuid=BLE_UUID128_INIT(
    0xd0,0x00,0x2d,0x12,0x1e,0x4b,0x0f,0xa4,0x99,0x4e,0xce,0xb5,0x31,0xf4,0x05,0x79);
static const ble_uuid128_t s_ancs_source_uuid=BLE_UUID128_INIT(
    0xbd,0x1d,0xa2,0x99,0xe6,0x25,0x58,0x8c,0xd9,0x42,0x01,0x63,0x0d,0x12,0xbf,0x9f);
static const ble_uuid16_t s_gatt_uuid=BLE_UUID16_INIT(0x1801);
static const ble_uuid16_t s_changed_uuid=BLE_UUID16_INIT(0x2a05);
static const ble_uuid16_t s_cccd_uuid=BLE_UUID16_INIT(0x2902);
static const ble_uuid16_t s_hid_uuid=BLE_UUID16_INIT(0x1812);
#define ANCS_REQUEST_WAIT_US UINT64_C(10000000)

typedef enum { ANCS_NONE, ANCS_GATT_SERVICE, ANCS_GATT_CHARS, ANCS_GATT_DESCS,
               ANCS_WATCH_CHANGED, ANCS_SERVICE, ANCS_CHARS } ancs_step_t;
/* Bounded per-link metadata only. No notification buffer, heap allocation,
 * task, keys, persistent handles, or public Lua access is added. */
static struct {
    ryz_ble_ancs_state_t state;
    ancs_step_t active, next;
    int error;
    uint32_t epoch;
    uint64_t deadline;
    uint16_t first, last, changed, changed_last, cccd, source;
    bool watching, dirty;
} s_ancs;
/* Never reused, including across disconnect or Service Changed rediscovery. */
static uint32_t s_ancs_request;

typedef enum { MODE_NONE, MODE_BOOT, MODE_SAVED, MODE_PAIR } radio_mode_t;
typedef enum { CLOSE_NONE, CLOSE_OFF, CLOSE_EMPTY, CLOSE_TIMEOUT, CLOSE_PAIR_FAILED,
               CLOSE_FORGET, CLOSE_BOOT_FAILED, CLOSE_FATAL } close_target_t;
typedef struct {
    bool present, numeric, accept;
    ryz_ble_action_t action;
    uint32_t operation, number;
} command_t;

/* Only the NimBLE host event task touches owner state. Public callers touch
 * only the bounded mailbox/snapshot/authorization gate under this spinlock.
 * No critical section spans NVS, HCI, a task wait, or an SDK callback. */
static portMUX_TYPE s_guard=portMUX_INITIALIZER_UNLOCKED;
static ryz_ble_snapshot_t s_view;
static ryz_ble_init_diagnostics_t s_init_diagnostics;
static command_t s_command;
static bool s_started, s_queue_ready, s_write_allowed, s_committing;
static struct ble_npl_event s_command_event;
static struct ble_npl_callout s_poll;
static ryz_ble_record_t s_record, s_candidate;
static bool s_record_known, s_synced, s_pair_approved, s_stage_armed;
static bool s_candidate_our, s_candidate_peer, s_boot_settled, s_compare;
static bool s_old_removed, s_replace, s_enabled;
static bool s_adv_stop_uncertain;
static bool s_terminate_pending;
static uint64_t s_terminate_retry_due;
static bool s_store_error_pending;
static uint32_t s_operation=1, s_conn_epoch, s_connection_operation, s_number;
static uint16_t s_conn=BLE_HS_CONN_HANDLE_NONE;
static ble_addr_t s_peer;
static struct ble_store_value_rpa_rec s_rpa;
static bool s_rpa_valid;
static uint8_t s_own_addr_type;
static uint64_t s_deadline, s_close_deadline, s_rssi_due;
static radio_mode_t s_mode;
static close_target_t s_closing;
static ryz_ble_phase_t s_phase=RYZ_BLE_OFF;
static ryz_ble_failure_t s_failure;
static esp_err_t s_error, s_boot_error;
static int s_sdk_error;
/* One job input lease, separate from pairing/NVS ownership. These mailbox
 * fields are protected by s_guard; only the host calls the HID profile. */
static uint32_t s_hid_token_counter, s_hid_session, s_hid_public_epoch, s_hid_tap_epoch;
static bool s_hid_tap_pending, s_hid_close_pending;
static ryz_ble_hid_key_t s_hid_key;
static int s_hid_error;

static uint64_t now_us(void) { return (uint64_t)esp_timer_get_time(); }
static int gap_event(struct ble_gap_event *event, void *context);
static void begin_close(close_target_t target, ryz_ble_failure_t failure, esp_err_t error, int sdk);
static void finish_close(void);
static void publish(void);

#if RYZ_BLE_INIT_DIAGNOSTICS
/* Bounded IDF 5.5.4 startup diagnostics. GNU ld wraps external calls only; each
 * wrapper forwards the original arguments exactly once and preserves rc.
 * Only the first init is measured, never regular BLE traffic. Failure logging
 * is deferred until the entire port init has returned so it cannot consume
 * heap between the HCI allocations being measured. */
esp_err_t __real_ble_buf_alloc(void);
esp_err_t __real_esp_vhci_host_register_callback(const esp_vhci_host_callback_t *callback);

static void init_probe_begin(ryz_ble_init_probe_t *probe)
{
    const uint32_t caps=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
    uint32_t free_bytes=(uint32_t)heap_caps_get_free_size(caps);
    uint32_t largest=(uint32_t)heap_caps_get_largest_free_block(caps);
    portENTER_CRITICAL(&s_guard);
    *probe=(ryz_ble_init_probe_t){.entered=true,.internal_free_before=free_bytes,
                                .internal_largest_before=largest};
    portEXIT_CRITICAL(&s_guard);
}
static void init_probe_end(ryz_ble_init_probe_t *probe, esp_err_t result)
{
    const uint32_t caps=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
    uint32_t free_bytes=(uint32_t)heap_caps_get_free_size(caps);
    uint32_t largest=(uint32_t)heap_caps_get_largest_free_block(caps);
    portENTER_CRITICAL(&s_guard);
    probe->result=result; probe->internal_free_after=free_bytes;
    probe->internal_largest_after=largest; probe->returned=true;
    portEXIT_CRITICAL(&s_guard);
}
static bool init_probe_needed(const ryz_ble_init_probe_t *probe)
{
    portENTER_CRITICAL(&s_guard);
    bool needed=s_init_diagnostics.port.entered && !s_init_diagnostics.finished && !probe->entered;
    portEXIT_CRITICAL(&s_guard); return needed;
}
esp_err_t __wrap_ble_buf_alloc(void)
{
    bool capture=init_probe_needed(&s_init_diagnostics.buffers);
    if (capture) init_probe_begin(&s_init_diagnostics.buffers);
    esp_err_t result=__real_ble_buf_alloc();
    if (capture) init_probe_end(&s_init_diagnostics.buffers,result);
    return result;
}
esp_err_t __wrap_esp_vhci_host_register_callback(const esp_vhci_host_callback_t *callback)
{
    bool capture=init_probe_needed(&s_init_diagnostics.vhci);
    if (capture) init_probe_begin(&s_init_diagnostics.vhci);
    esp_err_t result=__real_esp_vhci_host_register_callback(callback);
    if (capture) init_probe_end(&s_init_diagnostics.vhci,result);
    return result;
}
static void log_init_failure(const ryz_ble_init_diagnostics_t *diagnostics)
{
    const char *stage="port";
    const ryz_ble_init_probe_t *probe=&diagnostics->port;
    if (diagnostics->buffers.returned && diagnostics->buffers.result!=ESP_OK) {
        stage="buffers"; probe=&diagnostics->buffers;
    } else if (diagnostics->vhci.returned && diagnostics->vhci.result!=ESP_OK) {
        stage="vhci"; probe=&diagnostics->vhci;
    }
    ESP_LOGE("ryz_ble", "startup failed stage=%s rc=%" PRId32 " port_rc=%" PRId32
             " internal_free_before=%" PRIu32 " internal_largest_before=%" PRIu32
             " internal_free_after=%" PRIu32 " internal_largest_after=%" PRIu32,
             stage,(int32_t)probe->result,(int32_t)diagnostics->port.result,
             probe->internal_free_before,probe->internal_largest_before,
             probe->internal_free_after,probe->internal_largest_after);
}
static esp_err_t init_port_with_diagnostics(void)
{
    init_probe_begin(&s_init_diagnostics.port);
    esp_err_t result=nimble_port_init();
    init_probe_end(&s_init_diagnostics.port,result);
    portENTER_CRITICAL(&s_guard);
    s_init_diagnostics.finished=true;
    ryz_ble_init_diagnostics_t copy=s_init_diagnostics;
    portEXIT_CRITICAL(&s_guard);
    if (result!=ESP_OK) log_init_failure(&copy);
    return result;
}
#else
static esp_err_t init_port_with_diagnostics(void) { return nimble_port_init(); }
#endif

static void revoke_pairing(void)
{
    portENTER_CRITICAL(&s_guard); s_write_allowed=false; portEXIT_CRITICAL(&s_guard);
    s_stage_armed=s_pair_approved=s_compare=false;
}
static bool write_allowed(void)
{
    portENTER_CRITICAL(&s_guard); bool allowed=s_write_allowed && !s_command.present;
    portEXIT_CRITICAL(&s_guard); return allowed;
}
static void address_text(char out[18], const ble_addr_t *a)
{
    snprintf(out,18,"%02X:%02X:%02X:%02X:%02X:%02X",a->val[5],a->val[4],a->val[3],a->val[2],a->val[1],a->val[0]);
}
static void publish(void)
{
    uint64_t now=now_us();
    /* ready() invokes authorization, which takes s_guard. Never nest it. */
    bool hid_ready=ryz_ble_hid_profile_ready();
    bool hid_busy=ryz_ble_hid_profile_busy();
    portENTER_CRITICAL(&s_guard);
    s_view.operation_id=s_operation; s_view.phase=s_phase;
    s_view.available=s_synced; s_view.enabled=s_enabled;
    s_view.preference_known=s_record_known; s_view.saved_enabled=s_record_known && s_record.enabled;
    s_view.bond_known=s_record_known; s_view.bonded=s_record_known && s_record.bonded;
    s_view.linked=s_conn!=BLE_HS_CONN_HANDLE_NONE;
    if (!s_view.linked || !s_record_known || !s_record.bonded || s_closing!=CLOSE_NONE)
        s_view.authenticated=false;
    s_view.service_known=s_view.service_ready=false;
    s_view.ancs_state=s_ancs.state;
    s_view.ancs_sdk_error=s_ancs.error;
    s_view.ancs_service_changed_subscribed=s_ancs.watching && s_view.authenticated;
    s_view.hid_ready=hid_ready && s_view.authenticated && !s_command.present;
    s_view.hid_busy=hid_busy || s_hid_tap_pending || s_hid_close_pending;
    s_view.hid_sdk_error=s_hid_error;
    s_hid_public_epoch=s_conn_epoch;
    s_view.compare_pending=s_compare && s_closing==CLOSE_NONE;
    s_view.compare_value=s_view.compare_pending?s_number:0;
    s_view.remaining_valid=s_deadline!=0 && s_closing==CLOSE_NONE;
    uint64_t remain=s_deadline>now?s_deadline-now:0;
    s_view.remaining_s=s_view.remaining_valid?(uint16_t)((remain+999999U)/1000000U):0;
    s_view.boot_settled=s_boot_settled; s_view.boot_error=s_boot_error;
    s_view.failure=s_failure; s_view.last_error=s_error; s_view.sdk_error=s_sdk_error;
    s_view.checking_state=!s_record_known || s_phase==RYZ_BLE_COMMITTING || s_closing!=CLOSE_NONE;
    s_view.retry_allowed=s_record_known && s_closing==CLOSE_NONE &&
        (s_phase==RYZ_BLE_PAIR_FAILED || s_phase==RYZ_BLE_PAIR_TIMEOUT || s_phase==RYZ_BLE_FORGET_FAILED);
    s_view.replace_after_forget=s_replace; s_view.old_bond_removed=s_old_removed;
    s_view.command_pending=s_command.present || s_committing;
    if (s_record_known && s_record.bonded) address_text(s_view.peer_address,&s_record.peer.peer_addr);
    else if (s_conn!=BLE_HS_CONN_HANDLE_NONE) address_text(s_view.peer_address,&s_peer);
    else s_view.peer_address[0]=0;
    snprintf(s_view.peer_name,sizeof(s_view.peer_name),"%s",s_view.bonded || s_view.linked ? "Phone/PC" : "");
    if (!s_view.linked || s_closing!=CLOSE_NONE) { s_view.rssi_valid=false; s_view.rssi_dbm=0; }
    ++s_view.revision;
    portEXIT_CRITICAL(&s_guard);
}
static bool advance_operation(void)
{
    if (s_operation==UINT32_MAX) { begin_close(CLOSE_FATAL,RYZ_BLE_FAILURE_START,ESP_ERR_INVALID_STATE,0); return false; }
    ++s_operation; return true;
}
static void commit_gate(bool busy)
{ portENTER_CRITICAL(&s_guard); s_committing=busy; portEXIT_CRITICAL(&s_guard); }

/* On failure refresh actual durable state. Unknown never means no bond. The
 * caller decides cleanup/closure; no failed write silently mutates the cache. */
static esp_err_t save_record(const ryz_ble_record_t *next)
{
    esp_err_t e=ryz_ble_record_save(next);
    if (e==ESP_OK) { s_record=*next; s_record_known=true; }
    else {
        ryz_ble_record_t actual={0};
        s_record_known=ryz_ble_record_load(&actual)==ESP_OK;
        if (s_record_known) s_record=actual;
        ryz_ble_wipe(&actual,sizeof(actual));
    }
    return e;
}

/* IDF 5.5.4 private adapter: the public unpair/automatic IRK restore APIs can
 * swallow controller errors. Stop admissions first, then check every HCI
 * acknowledgement; no public view is derived from a best-effort helper. */
static int rebuild_resolver(void)
{
    /* Reinstall the SAME local IRK: this checked private function clears the
     * controller peer resolving list, then installs the local-only entry. */
    int rc=ble_hs_pvcy_set_our_irk(s_record.local.irk);
    if (!rc && s_record.bonded && s_record.peer.irk_present)
        rc=ble_hs_pvcy_add_entry(s_record.peer.peer_addr.val,s_record.peer.peer_addr.type,s_record.peer.irk);
    return rc;
}
static int clear_resolver(void)
{
    return s_record_known && s_record.local_valid?ble_hs_pvcy_set_our_irk(s_record.local.irk):BLE_HS_ESTORE_FAIL;
}

static bool security_key_valid(const struct ble_store_value_sec *sec)
{ return sec->key_size==16 && sec->authenticated && sec->sc && sec->peer_addr.type<=BLE_ADDR_RANDOM_ID; }
static bool store_window(const ble_addr_t *peer)
{
    return s_record_known && !s_record.bonded && s_mode==MODE_PAIR &&
        s_closing==CLOSE_NONE && s_conn!=BLE_HS_CONN_HANDLE_NONE &&
        s_connection_operation==s_operation && s_pair_approved && s_stage_armed &&
        now_us()<s_deadline && ryz_ble_addr_equal(peer,&s_peer) && write_allowed();
}
static bool key_match(const ble_addr_t *key, uint8_t idx, const ble_addr_t *peer)
{ return idx==0 && (ble_addr_cmp(key,BLE_ADDR_ANY)==0 || ryz_ble_addr_equal(key,peer)); }

static int store_read(int type, const union ble_store_key *key, union ble_store_value *out)
{
    if (!key || !out || !s_record_known) return BLE_HS_ESTORE_FAIL;
    /* SDK ble_store_read_* helpers cast a member-sized caller object to this
     * union ABI. Only write the selected member after a successful lookup;
     * clearing sizeof(*out) would overwrite the caller's stack (e.g. IRK). */
    if (type==BLE_STORE_OBJ_TYPE_LOCAL_IRK) {
        if (!s_record.local_valid || key->local_irk.idx) return BLE_HS_ENOENT;
        out->local_irk=s_record.local; return 0;
    }
    if (type==BLE_STORE_OBJ_TYPE_PEER_ADDR) {
        if (!s_rpa_valid || key->rpa_rec.idx ||
            !ryz_ble_addr_equal(&key->rpa_rec.peer_rpa_addr,&s_rpa.peer_rpa_addr)) return BLE_HS_ENOENT;
        out->rpa_rec=s_rpa; return 0;
    }
    if (!s_record.bonded) return BLE_HS_ENOENT;
    if (type==BLE_STORE_OBJ_TYPE_OUR_SEC || type==BLE_STORE_OBJ_TYPE_PEER_SEC) {
        if (!key_match(&key->sec.peer_addr,key->sec.idx,&s_record.peer.peer_addr)) return BLE_HS_ENOENT;
        out->sec=type==BLE_STORE_OBJ_TYPE_OUR_SEC?s_record.our:s_record.peer; return 0;
    }
    if (type==BLE_STORE_OBJ_TYPE_CCCD) {
        unsigned skip=key->cccd.idx;
        for (unsigned i=0;i<s_record.cccd_count;++i)
            if (key_match(&key->cccd.peer_addr,0,&s_record.peer.peer_addr) &&
                (!key->cccd.chr_val_handle || key->cccd.chr_val_handle==s_record.cccd[i].chr_val_handle)) {
                if (skip) --skip; else { out->cccd=s_record.cccd[i]; return 0; }
            }
        return BLE_HS_ENOENT;
    }
    if (type==BLE_STORE_OBJ_TYPE_CSFC && s_record.csfc_valid &&
        key_match(&key->csfc.peer_addr,key->csfc.idx,&s_record.peer.peer_addr)) { out->csfc=s_record.csfc; return 0; }
    return BLE_HS_ENOENT;
}

static int store_write(int type, const union ble_store_value *value)
{
    if (!value || !s_record_known) return BLE_HS_ESTORE_FAIL;
    if (type==BLE_STORE_OBJ_TYPE_LOCAL_IRK) {
        /* Device-level key generation, separate from any pairing operation.
         * Never rotate a known IRK underneath a saved peer. */
        if (s_record.local_valid)
            return !memcmp(s_record.local.irk,value->local_irk.irk,16)?0:BLE_HS_ESTORE_FAIL;
        ryz_ble_record_t next=s_record; next.local=value->local_irk;
        next.local.addr.type&=1U; next.local_valid=true;
        commit_gate(true); esp_err_t e=save_record(&next); commit_gate(false);
        ryz_ble_wipe(&next,sizeof(next));
        if (e!=ESP_OK) { s_error=e; s_failure=RYZ_BLE_FAILURE_SAVE; }
        return e==ESP_OK?0:BLE_HS_ESTORE_FAIL;
    }
    if (type==BLE_STORE_OBJ_TYPE_OUR_SEC || type==BLE_STORE_OBJ_TYPE_PEER_SEC) {
        /* PARING_COMPLETE opens this RAM-only staging window. The SDK ignores
         * callback errors, so ENC_CHANGE independently checks all gates again.
         * No SDK call here: NimBLE invokes store callbacks with its lock held. */
        if (!security_key_valid(&value->sec) || !store_window(&value->sec.peer_addr)) return BLE_HS_ESTORE_FAIL;
        struct ble_store_value_sec *slot=type==BLE_STORE_OBJ_TYPE_OUR_SEC?&s_candidate.our:&s_candidate.peer;
        *slot=value->sec; slot->peer_addr.type&=1U;
        if (type==BLE_STORE_OBJ_TYPE_OUR_SEC) s_candidate_our=true; else s_candidate_peer=true;
        return 0;
    }
    if (type==BLE_STORE_OBJ_TYPE_PEER_ADDR) {
        if (!store_window(&value->rpa_rec.peer_addr)) return BLE_HS_ESTORE_FAIL;
        s_rpa=value->rpa_rec; s_rpa.peer_addr.type&=1U; s_rpa_valid=true; return 0;
    }
    /* Standard GATT cache metadata only, and only for the already-verified
     * encrypted peer. No application GATT service or arbitrary payload store. */
    portENTER_CRITICAL(&s_guard);
    bool secure=s_view.authenticated && s_record_known && s_record.bonded && !s_command.present && !s_committing;
    if (secure) s_committing=true;
    portEXIT_CRITICAL(&s_guard);
    if (!secure) return BLE_HS_ESTORE_FAIL;
    ryz_ble_record_t next=s_record; bool permitted=false;
    if (type==BLE_STORE_OBJ_TYPE_CCCD && ryz_ble_addr_equal(&value->cccd.peer_addr,&s_record.peer.peer_addr)) {
        unsigned i=0; while (i<next.cccd_count && next.cccd[i].chr_val_handle!=value->cccd.chr_val_handle) ++i;
        if (i<RYZ_BLE_CCCD_MAX && value->cccd.chr_val_handle) {
            if (i==next.cccd_count) ++next.cccd_count;
            next.cccd[i]=value->cccd; next.cccd[i].peer_addr.type&=1U; permitted=true;
        }
    } else if (type==BLE_STORE_OBJ_TYPE_CSFC && ryz_ble_addr_equal(&value->csfc.peer_addr,&s_record.peer.peer_addr)) {
        next.csfc=value->csfc; next.csfc.peer_addr.type&=1U; next.csfc_valid=true; permitted=true;
    }
    esp_err_t e=permitted?save_record(&next):ESP_ERR_INVALID_ARG;
    commit_gate(false); ryz_ble_wipe(&next,sizeof(next));
    if (e!=ESP_OK && permitted) {
        s_error=e; s_failure=RYZ_BLE_FAILURE_SAVE; s_store_error_pending=true;
        /* Do not call GAP while the SDK holds its store lock. The next owner
         * event closes the link; public authentication is revoked now. */
        portENTER_CRITICAL(&s_guard); s_view.authenticated=false; portEXIT_CRITICAL(&s_guard);
        publish();
    }
    return e==ESP_OK?0:BLE_HS_ESTORE_FAIL;
}
static int store_delete(int type, const union ble_store_key *key)
{
    /* SDK CCCD disable uses delete, NOT write(flags=0). Permit only that
     * exact authenticated-peer metadata deletion, never key eviction. No
     * GAP/host-lock calls here: some SDK store callers hold their own lock. */
    if (type!=BLE_STORE_OBJ_TYPE_CCCD || !key || key->cccd.idx || !key->cccd.chr_val_handle)
        return BLE_HS_ESTORE_FAIL;
    portENTER_CRITICAL(&s_guard);
    bool permitted=s_view.authenticated && s_record_known && s_record.bonded &&
        !s_command.present && !s_committing &&
        ryz_ble_addr_equal(&key->cccd.peer_addr,&s_record.peer.peer_addr);
    if (permitted) s_committing=true;
    portEXIT_CRITICAL(&s_guard);
    if (!permitted) return BLE_HS_ESTORE_FAIL;
    ryz_ble_record_t next=s_record;
    unsigned i=0;
    while (i<next.cccd_count && next.cccd[i].chr_val_handle!=key->cccd.chr_val_handle) ++i;
    esp_err_t e=ESP_OK;
    if (i<next.cccd_count) {
        for (unsigned j=i+1;j<next.cccd_count;++j) next.cccd[j-1]=next.cccd[j];
        --next.cccd_count;
        ryz_ble_wipe(&next.cccd[next.cccd_count],sizeof(next.cccd[0]));
        e=save_record(&next);
    }
    commit_gate(false); ryz_ble_wipe(&next,sizeof(next));
    if (e!=ESP_OK) {
        s_error=e; s_failure=RYZ_BLE_FAILURE_SAVE; s_store_error_pending=true;
        portENTER_CRITICAL(&s_guard); s_view.authenticated=false; portEXIT_CRITICAL(&s_guard);
        publish();
    }
    return e==ESP_OK?0:BLE_HS_ESTORE_FAIL;
}
static int store_status(struct ble_store_status_event *event, void *arg)
{ (void)event; (void)arg; return BLE_HS_ESTORE_CAP; }

static int advertise(void)
{
    struct ble_hs_adv_fields fields={0};
    fields.flags=BLE_HS_ADV_F_DISC_GEN|BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids16=&s_hid_uuid; fields.num_uuids16=1;
    /* Incomplete list: BAS/DIS/GAP/GATT also exist in our database. */
    fields.uuids16_is_complete=0;
    fields.appearance=0x03c0; fields.appearance_is_present=1; /* Generic HID, not keyboard. */
    fields.sol_uuids128=&s_ancs_uuid; fields.sol_num_uuids128=1;
    /* Flags 3 + HID 4 + appearance 4 + ANCS solicitation 18 = 29 bytes. */
    int rc=ble_gap_adv_set_fields(&fields);
    struct ble_hs_adv_fields response={0};
    response.name=(const uint8_t *)s_view.local_name;
    response.name_len=(uint8_t)strlen(s_view.local_name); response.name_is_complete=1;
    response.tx_pwr_lvl=BLE_HS_ADV_TX_PWR_LVL_AUTO; response.tx_pwr_lvl_is_present=1;
    if (!rc) rc=ble_gap_adv_rsp_set_fields(&response); /* 12 bytes; controller reads actual TX power. */
    struct ble_gap_adv_params params={0};
    params.conn_mode=BLE_GAP_CONN_MODE_UND; params.disc_mode=BLE_GAP_DISC_MODE_GEN;
    /* Apple's initial discovery interval: 20 ms for our bounded 30 s pair
     * window. Saved-peer background reconnect uses 152.5 ms instead. */
    params.itvl_min=params.itvl_max=s_mode==MODE_PAIR?32:244;
    if (!rc) rc=ble_gap_adv_start(s_own_addr_type,NULL,BLE_HS_FOREVER,&params,gap_event,(void *)(uintptr_t)s_operation);
    return rc;
}
static void start_pair(void)
{
    if (!s_record_known || s_record.bonded || s_conn!=BLE_HS_CONN_HANDLE_NONE || ble_gap_adv_active()) {
        begin_close(CLOSE_FATAL,RYZ_BLE_FAILURE_START,ESP_ERR_INVALID_STATE,0); return;
    }
    s_candidate=s_record; s_candidate_our=s_candidate_peer=false;
    s_rpa_valid=s_pair_approved=s_stage_armed=s_compare=false;
    s_mode=MODE_PAIR; s_enabled=true; s_deadline=now_us()+PAIR_WAIT_US;
    s_phase=RYZ_BLE_PAIRING; s_failure=RYZ_BLE_FAILURE_NONE; s_error=ESP_OK; s_sdk_error=0;
    portENTER_CRITICAL(&s_guard); s_write_allowed=true; portEXIT_CRITICAL(&s_guard);
    int rc=advertise();
    if (rc) begin_close(CLOSE_PAIR_FAILED,RYZ_BLE_FAILURE_START,ESP_FAIL,rc);
    else publish();
}
static void start_saved(bool boot)
{
    s_mode=boot?MODE_BOOT:MODE_SAVED; s_enabled=true;
    s_deadline=boot?now_us()+BOOT_WAIT_US:0;
    s_phase=RYZ_BLE_SAVED_WAITING; s_failure=RYZ_BLE_FAILURE_NONE; s_error=ESP_OK; s_sdk_error=0;
    int rc=rebuild_resolver(); if (!rc) rc=advertise();
    if (rc) begin_close(boot?CLOSE_BOOT_FAILED:CLOSE_FATAL,RYZ_BLE_FAILURE_START,ESP_FAIL,rc);
    else publish();
}

static void finish_forget(void)
{
    int rc=clear_resolver();
    if (rc) {
        s_closing=CLOSE_NONE; s_phase=RYZ_BLE_FORGET_FAILED;
        s_failure=RYZ_BLE_FAILURE_STOP; s_error=ESP_FAIL; s_sdk_error=rc; publish(); return;
    }
    ryz_ble_record_t next=s_record; ryz_ble_record_clear_peer(&next);
    commit_gate(true); esp_err_t e=save_record(&next); commit_gate(false);
    ryz_ble_wipe(&next,sizeof(next)); s_closing=CLOSE_NONE;
    if (e!=ESP_OK) {
        /* A commit-then-error may already have deleted the peer. Do not call
         * that FORGET_FAILED (which promises the old bond remains). */
        s_old_removed=s_record_known && !s_record.bonded;
        s_phase=s_record_known && s_record.bonded?RYZ_BLE_FORGET_FAILED:RYZ_BLE_FAILED;
        s_failure=RYZ_BLE_FAILURE_SAVE; s_error=e; publish(); return;
    }
    s_old_removed=true; s_rpa_valid=false; s_error=ESP_OK; s_failure=RYZ_BLE_FAILURE_NONE;
    if (s_replace) start_pair();
    else { s_mode=MODE_NONE; s_enabled=true; s_phase=RYZ_BLE_EMPTY; publish(); }
}

static void finish_close(void)
{
    if (s_closing==CLOSE_NONE || s_conn!=BLE_HS_CONN_HANDLE_NONE || ble_gap_adv_active() || s_adv_stop_uncertain) return;
    close_target_t target=s_closing;
    s_deadline=0; s_mode=MODE_NONE; s_compare=false;
    ryz_ble_wipe(&s_candidate,sizeof(s_candidate)); s_candidate_our=s_candidate_peer=false;
    if (target==CLOSE_FORGET) { finish_forget(); return; }
    if (s_record_known && !s_record.bonded && s_record.local_valid) {
        int rc=clear_resolver();
        if (rc) { s_error=ESP_FAIL; s_failure=RYZ_BLE_FAILURE_STOP; s_sdk_error=rc; s_phase=RYZ_BLE_STOPPING; return; }
        s_rpa_valid=false;
    }
    s_closing=CLOSE_NONE;
    if (target==CLOSE_OFF || target==CLOSE_BOOT_FAILED || target==CLOSE_FATAL) {
        s_enabled=false; s_phase=target==CLOSE_FATAL?RYZ_BLE_FAILED:RYZ_BLE_OFF;
    } else if (target==CLOSE_TIMEOUT) s_phase=RYZ_BLE_PAIR_TIMEOUT;
    else if (target==CLOSE_PAIR_FAILED) s_phase=RYZ_BLE_PAIR_FAILED;
    else s_phase=RYZ_BLE_EMPTY;
    if (!s_boot_settled && (target==CLOSE_BOOT_FAILED || target==CLOSE_FATAL || target==CLOSE_OFF)) {
        s_boot_settled=true; s_boot_error=s_error;
    }
    publish();
}
static void begin_close(close_target_t target, ryz_ble_failure_t failure, esp_err_t error, int sdk)
{
    bool compare=s_compare;
    /* Release while the current authenticated link can still carry a zero.
     * Failure is not treated as release: physical termination is required. */
    ryz_ble_hid_profile_release();
    if (s_closing==CLOSE_NONE) {
        int hid_rc=ryz_ble_hid_profile_poll(now_us());
        if (hid_rc) s_hid_error=hid_rc;
    }
    revoke_pairing(); s_closing=target; s_close_deadline=now_us()+STOP_WAIT_US; s_adv_stop_uncertain=false;
    s_terminate_pending=false; s_terminate_retry_due=now_us()+(uint64_t)POLL_MS*1000U;
    memset(&s_ancs,0,sizeof(s_ancs));
    portENTER_CRITICAL(&s_guard); s_hid_tap_pending=false; portEXIT_CRITICAL(&s_guard);
    ryz_ble_hid_profile_release(); /* Physical disconnect, not local reset, clears a held key. */
    s_failure=failure; s_error=error; s_sdk_error=sdk;
    s_phase=target==CLOSE_FORGET?RYZ_BLE_FORGETTING:RYZ_BLE_STOPPING;
    portENTER_CRITICAL(&s_guard); s_view.authenticated=false; portEXIT_CRITICAL(&s_guard);
    publish();
    if (compare && s_conn!=BLE_HS_CONN_HANDLE_NONE) {
        struct ble_sm_io reject={.action=BLE_SM_IOACT_NUMCMP,.numcmp_accept=0};
        (void)ble_sm_inject_io(s_conn,&reject); /* Termination acknowledgement still required. */
        if (s_closing!=target) return; /* A synchronous disconnect may finish cleanup. */
    }
    int rc=ble_gap_adv_active()?ble_gap_adv_stop():0;
    if (rc && rc!=BLE_HS_EALREADY) { s_adv_stop_uncertain=true; s_error=ESP_FAIL; s_failure=RYZ_BLE_FAILURE_STOP; s_sdk_error=rc; }
    if (s_conn!=BLE_HS_CONN_HANDLE_NONE) {
        rc=ble_gap_terminate(s_conn,BLE_ERR_REM_USER_CONN_TERM);
        s_terminate_pending=rc==0;
        if (rc) {
            struct ble_gap_conn_desc desc={0};
            if (rc==BLE_HS_ENOTCONN && ble_gap_conn_find(s_conn,&desc)==BLE_HS_ENOTCONN) {
                s_conn=BLE_HS_CONN_HANDLE_NONE; ryz_ble_hid_profile_reset();
            }
            else { s_error=ESP_FAIL; s_failure=RYZ_BLE_FAILURE_STOP; s_sdk_error=rc; }
        }
    }
    finish_close(); publish();
}

static bool secure_desc(struct ble_gap_conn_desc *desc)
{
    return s_conn!=BLE_HS_CONN_HANDLE_NONE && ble_gap_conn_find(s_conn,desc)==0 &&
        ryz_ble_addr_equal(&desc->peer_id_addr,&s_peer) && desc->sec_state.encrypted &&
        desc->sec_state.authenticated && desc->sec_state.key_size==16;
}

static bool hid_authorized(uint16_t conn, void *unused)
{
    (void)unused;
    portENTER_CRITICAL(&s_guard);
    bool allowed=s_view.authenticated && !s_command.present && !s_committing;
    portEXIT_CRITICAL(&s_guard);
    struct ble_gap_conn_desc desc={0};
    return allowed && conn==s_conn && s_enabled && s_closing==CLOSE_NONE &&
        s_record_known && s_record.bonded && secure_desc(&desc);
}
static void hid_poll(void)
{
    bool take=false, release=false;
    ryz_ble_hid_key_t key=RYZ_BLE_HID_PLAY_PAUSE;
    portENTER_CRITICAL(&s_guard);
    release=s_hid_close_pending;
    if (s_hid_tap_pending) {
        take=s_hid_session && !release && !s_command.present &&
             s_hid_tap_epoch==s_conn_epoch && s_closing==CLOSE_NONE;
        key=s_hid_key; s_hid_tap_pending=false;
        /* Dequeue is dispatch commitment: a concurrent close can cancel a
         * queued tap, but cannot unsend a report already handed to the host.
         * It always requests release and blocks new-job admission. */
    }
    portEXIT_CRITICAL(&s_guard);
    if (s_closing==CLOSE_NONE) {
        if (release) ryz_ble_hid_profile_release();
        if (take) {
            int rc=ryz_ble_hid_profile_tap((uint8_t)(1U<<(unsigned)key),now_us());
            if (rc) s_hid_error=rc;
        }
        int rc=ryz_ble_hid_profile_poll(now_us());
        if (rc) {
            s_hid_error=rc;
            begin_close(CLOSE_FATAL,RYZ_BLE_FAILURE_STOP,ESP_FAIL,rc);
        }
    }
    if (!ryz_ble_hid_profile_busy()) {
        portENTER_CRITICAL(&s_guard); s_hid_close_pending=false; portEXIT_CRITICAL(&s_guard);
    }
}

static bool ancs_allowed(void)
{
    portENTER_CRITICAL(&s_guard);
    bool allowed=s_view.authenticated && !s_command.present;
    portEXIT_CRITICAL(&s_guard);
    struct ble_gap_conn_desc desc={0};
    return allowed && s_closing==CLOSE_NONE && s_enabled &&
        s_record_known && s_record.bonded && secure_desc(&desc);
}
static void ancs_begin(void)
{
    memset(&s_ancs,0,sizeof(s_ancs));
    s_ancs.epoch=s_conn_epoch; s_ancs.state=RYZ_BLE_ANCS_DISCOVERING;
    s_ancs.next=ANCS_GATT_SERVICE;
}
static void ancs_fail(int error)
{
    s_ancs.error=error; s_ancs.state=RYZ_BLE_ANCS_ERROR;
    s_ancs.active=s_ancs.next=ANCS_NONE; s_ancs.deadline=0;
    /* Do not retry a timed-out ATT transaction on the same connection.
     * NimBLE owns its outstanding procedure until completion/disconnect. */
    publish();
}
static bool ancs_current(uint16_t handle, void *arg)
{
    if (s_ancs.active==ANCS_NONE || (uint32_t)(uintptr_t)arg!=s_ancs_request ||
        handle!=s_conn || s_ancs.epoch!=s_conn_epoch || !ancs_allowed()) return false;
    if (now_us()>=s_ancs.deadline) { ancs_fail(BLE_HS_ETIMEOUT); return false; }
    return true;
}
static void ancs_next(ancs_step_t next)
{
    s_ancs.active=ANCS_NONE; s_ancs.next=next; s_ancs.deadline=0;
    /* Service Changed can arrive during any procedure. Drain that procedure
     * first; never publish its stale result or run overlapping rediscovery. */
    if (s_ancs.dirty) ancs_begin();
    publish();
}
static bool ancs_callback_error(const struct ble_gatt_error *error)
{
    if (!error) { ancs_fail(BLE_HS_EBADDATA); return true; }
    if (error->status && s_ancs.dirty) { ancs_next(ANCS_GATT_SERVICE); return true; }
    if (error->status && error->status!=BLE_HS_EDONE) {
        ancs_fail(error->status);
        return true;
    }
    return false;
}
static int ancs_service_cb(uint16_t handle,const struct ble_gatt_error *error,
                           const struct ble_gatt_svc *svc,void *arg)
{
    if (!ancs_current(handle,arg)) return BLE_HS_EDONE;
    if (ancs_callback_error(error)) return BLE_HS_EDONE;
    bool gatt=s_ancs.active==ANCS_GATT_SERVICE;
    if (error->status==0) {
        if (!svc || !svc->start_handle || svc->end_handle<svc->start_handle || s_ancs.first ||
            ble_uuid_cmp(&svc->uuid.u,gatt?&s_gatt_uuid.u:&s_ancs_uuid.u)) {
            ancs_fail(BLE_HS_EBADDATA); return BLE_HS_EDONE;
        }
        s_ancs.first=svc->start_handle; s_ancs.last=svc->end_handle; return 0;
    }
    if (gatt) ancs_next(s_ancs.first?ANCS_GATT_CHARS:ANCS_SERVICE);
    else if (s_ancs.first) ancs_next(ANCS_CHARS);
    else { s_ancs.state=RYZ_BLE_ANCS_UNAVAILABLE; ancs_next(ANCS_NONE); }
    return 0;
}
static int ancs_chars_cb(uint16_t handle,const struct ble_gatt_error *error,
                         const struct ble_gatt_chr *chr,void *arg)
{
    if (!ancs_current(handle,arg)) return BLE_HS_EDONE;
    if (ancs_callback_error(error)) return BLE_HS_EDONE;
    bool gatt=s_ancs.active==ANCS_GATT_CHARS;
    if (error->status==0) {
        if (!chr || chr->def_handle<s_ancs.first || chr->val_handle<=chr->def_handle || chr->val_handle>s_ancs.last) {
            ancs_fail(BLE_HS_EBADDATA); return BLE_HS_EDONE;
        }
        if (gatt) {
            if (s_ancs.changed && chr->def_handle>s_ancs.changed && chr->def_handle<=s_ancs.changed_last)
                s_ancs.changed_last=chr->def_handle-1;
            if (!ble_uuid_cmp(&chr->uuid.u,&s_changed_uuid.u)) {
                if (s_ancs.changed || !(chr->properties & BLE_GATT_CHR_PROP_INDICATE)) {
                    ancs_fail(BLE_HS_EBADDATA); return BLE_HS_EDONE;
                }
                s_ancs.changed=chr->val_handle; s_ancs.changed_last=s_ancs.last;
            }
        } else if (!ble_uuid_cmp(&chr->uuid.u,&s_ancs_source_uuid.u)) {
            if (s_ancs.source || !(chr->properties & BLE_GATT_CHR_PROP_NOTIFY)) {
                ancs_fail(BLE_HS_EBADDATA); return BLE_HS_EDONE;
            }
            s_ancs.source=chr->val_handle;
        }
        return 0;
    }
    if (gatt) ancs_next(s_ancs.changed && s_ancs.changed<s_ancs.changed_last?ANCS_GATT_DESCS:ANCS_SERVICE);
    else if (s_ancs.source) { s_ancs.state=RYZ_BLE_ANCS_DISCOVERED; ancs_next(ANCS_NONE); }
    else { ancs_fail(BLE_HS_EBADDATA); }
    return 0;
}
static int ancs_descs_cb(uint16_t handle,const struct ble_gatt_error *error,
                         uint16_t chr_handle,const struct ble_gatt_dsc *dsc,void *arg)
{
    if (!ancs_current(handle,arg)) return BLE_HS_EDONE;
    if (ancs_callback_error(error)) return BLE_HS_EDONE;
    if (!error->status) {
        if (!dsc || chr_handle!=s_ancs.changed || dsc->handle<=s_ancs.changed || dsc->handle>s_ancs.changed_last) {
            ancs_fail(BLE_HS_EBADDATA); return BLE_HS_EDONE;
        }
        if (!ble_uuid_cmp(&dsc->uuid.u,&s_cccd_uuid.u)) {
            if (s_ancs.cccd) { ancs_fail(BLE_HS_EBADDATA); return BLE_HS_EDONE; }
            s_ancs.cccd=dsc->handle;
        }
    } else if (s_ancs.cccd) ancs_next(ANCS_WATCH_CHANGED);
    else ancs_fail(BLE_HS_ENOENT);
    return 0;
}
static int ancs_watch_cb(uint16_t handle,const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr,void *arg)
{
    if (!ancs_current(handle,arg)) return BLE_HS_EDONE;
    if (!error || error->status || !attr || attr->handle!=s_ancs.cccd) {
        ancs_fail(error && error->status?error->status:BLE_HS_EBADDATA); return 0;
    }
    s_ancs.watching=true; ancs_next(ANCS_SERVICE); return 0;
}
static void ancs_poll(void)
{
    if (!ancs_allowed()) return;
    if (s_ancs.active!=ANCS_NONE) {
        if (now_us()>=s_ancs.deadline) ancs_fail(BLE_HS_ETIMEOUT);
        return;
    }
    if (s_ancs.next==ANCS_NONE) return;
    if (s_ancs_request==UINT32_MAX) { ancs_fail(BLE_HS_EBADDATA); return; }
    void *arg=(void *)(uintptr_t)++s_ancs_request;
    s_ancs.active=s_ancs.next; s_ancs.next=ANCS_NONE;
    s_ancs.deadline=now_us()+ANCS_REQUEST_WAIT_US;
    int rc;
    switch (s_ancs.active) {
    case ANCS_GATT_SERVICE:
    case ANCS_SERVICE:
        s_ancs.first=s_ancs.last=s_ancs.source=0;
        rc=ble_gattc_disc_svc_by_uuid(s_conn,s_ancs.active==ANCS_GATT_SERVICE?&s_gatt_uuid.u:&s_ancs_uuid.u,ancs_service_cb,arg);
        break;
    case ANCS_GATT_CHARS:
    case ANCS_CHARS:
        rc=ble_gattc_disc_all_chrs(s_conn,s_ancs.first,s_ancs.last,ancs_chars_cb,arg); break;
    case ANCS_GATT_DESCS:
        rc=ble_gattc_disc_all_dscs(s_conn,s_ancs.changed,s_ancs.changed_last,ancs_descs_cb,arg); break;
    case ANCS_WATCH_CHANGED: {
        /* The ONLY write: standard GATT Service Changed indications. Never
         * subscribe to ANCS Notification/Data Source or write Control Point. */
        const uint8_t indications[2]={2,0};
        rc=ble_gattc_write_flat(s_conn,s_ancs.cccd,indications,sizeof(indications),ancs_watch_cb,arg); break;
    }
    default: rc=BLE_HS_EBADDATA; break;
    }
    if (rc) ancs_fail(rc);
}
static void ancs_service_changed(const struct ble_gap_event *event)
{
    if (!ancs_allowed() || s_ancs.state==RYZ_BLE_ANCS_ERROR ||
        (!s_ancs.watching && s_ancs.active!=ANCS_WATCH_CHANGED) || !s_ancs.changed ||
        !event->notify_rx.indication || event->notify_rx.attr_handle!=s_ancs.changed ||
        !event->notify_rx.om || OS_MBUF_PKTLEN(event->notify_rx.om)!=4) return;
    uint8_t range[4];
    if (os_mbuf_copydata(event->notify_rx.om,0,sizeof(range),range)) return;
    uint16_t first=(uint16_t)(range[0]|((uint16_t)range[1]<<8));
    uint16_t last=(uint16_t)(range[2]|((uint16_t)range[3]<<8));
    if (!first || first>last) return;
    if (s_ancs.active!=ANCS_NONE) { s_ancs.dirty=true; s_ancs.state=RYZ_BLE_ANCS_DISCOVERING; }
    else ancs_begin();
    publish();
}
static void authenticated(void)
{
    struct ble_gap_conn_desc desc={0};
    if (s_closing!=CLOSE_NONE || !secure_desc(&desc)) {
        begin_close(s_mode==MODE_BOOT?CLOSE_BOOT_FAILED:CLOSE_PAIR_FAILED,RYZ_BLE_FAILURE_AUTH,ESP_FAIL,BLE_HS_EAUTHEN); return;
    }
    if (s_mode==MODE_PAIR) {
        if (!store_window(&desc.peer_id_addr) || !s_candidate_our || !s_candidate_peer || !s_candidate.our.ltk_present) {
            begin_close(CLOSE_PAIR_FAILED,RYZ_BLE_FAILURE_AUTH,ESP_FAIL,BLE_HS_EAUTHEN); return;
        }
        /* Cancellation admission and commit have one linearization point.
         * COMMITTING rejects new commands rather than promising cancellation
         * while an irreversible flash commit is already in progress. */
        portENTER_CRITICAL(&s_guard);
        bool allowed=s_write_allowed && !s_command.present && !s_committing;
        if (allowed) { s_committing=true; s_write_allowed=false; }
        portEXIT_CRITICAL(&s_guard);
        if (!allowed) { begin_close(CLOSE_EMPTY,RYZ_BLE_FAILURE_NONE,ESP_OK,0); return; }
        s_stage_armed=false; s_phase=RYZ_BLE_COMMITTING; s_compare=false; publish();
        s_candidate.bonded=s_candidate.our_valid=s_candidate.peer_valid=true;
        esp_err_t e=save_record(&s_candidate); commit_gate(false);
        if (e!=ESP_OK) {
            /* A failed pair commit must not leave an accidentally durable
             * partial bond eligible for reboot reconnect. Explicitly clean
             * the actual record; unknown or failed cleanup stays fail-closed. */
            /* The pre-pair snapshot was known empty. Use that exact local
             * identity/preference even if fresh reads have failed; a write
             * may still be able to remove an ambiguously committed candidate. */
            ryz_ble_record_t cleared=s_candidate; ryz_ble_record_clear_peer(&cleared);
            commit_gate(true); esp_err_t cleanup=save_record(&cleared); commit_gate(false);
            ryz_ble_wipe(&cleared,sizeof(cleared));
            if (cleanup!=ESP_OK) s_record_known=false;
            begin_close(s_record_known?CLOSE_PAIR_FAILED:CLOSE_FATAL,RYZ_BLE_FAILURE_SAVE,e,0); return;
        }
        s_phase=RYZ_BLE_PAIRED;
        ryz_ble_wipe(&s_candidate,sizeof(s_candidate));
    } else if (!s_record_known || !s_record.bonded ||
               !ryz_ble_addr_equal(&desc.peer_id_addr,&s_record.peer.peer_addr) || !s_record.our.sc || !s_record.our.authenticated) {
        begin_close(s_mode==MODE_BOOT?CLOSE_BOOT_FAILED:CLOSE_FATAL,RYZ_BLE_FAILURE_AUTH,ESP_FAIL,BLE_HS_EAUTHEN); return;
    } else s_phase=RYZ_BLE_LINKED;
    s_mode=MODE_SAVED; s_deadline=0; s_failure=RYZ_BLE_FAILURE_NONE; s_error=ESP_OK; s_sdk_error=0;
    s_boot_settled=true; s_boot_error=ESP_OK;
    portENTER_CRITICAL(&s_guard); s_view.authenticated=true; portEXIT_CRITICAL(&s_guard);
    if (s_ancs.state==RYZ_BLE_ANCS_IDLE) ancs_begin();
    publish();
}

static bool event_is_current(uint16_t handle, void *context)
{ return handle==s_conn && (uint32_t)(uintptr_t)context==s_conn_epoch && s_conn!=BLE_HS_CONN_HANDLE_NONE; }
static int gap_event(struct ble_gap_event *event, void *context)
{
    if (!event) return 0;
    portENTER_CRITICAL(&s_guard); bool pending=s_command.present; portEXIT_CRITICAL(&s_guard);
    if ((pending || s_closing!=CLOSE_NONE) &&
        (event->type==BLE_GAP_EVENT_PASSKEY_ACTION || event->type==BLE_GAP_EVENT_IDENTITY_RESOLVED ||
         event->type==BLE_GAP_EVENT_PARING_COMPLETE || event->type==BLE_GAP_EVENT_ENC_CHANGE ||
         event->type==BLE_GAP_EVENT_REPEAT_PAIRING)) return 0;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
        if ((uint32_t)(uintptr_t)context!=s_operation || s_closing!=CLOSE_NONE || !s_enabled) {
            if (!event->connect.status && s_conn==BLE_HS_CONN_HANDLE_NONE) {
                /* CONNECT can already be queued when stop/deadline wins.
                 * Track this real link until shutdown acknowledgement too;
                 * merely sending terminate must not leave a fake OFF view. */
                s_conn=event->connect.conn_handle;
                if (s_conn_epoch!=UINT32_MAX) ++s_conn_epoch;
                s_connection_operation=0;
                (void)ble_gap_set_event_cb(s_conn,gap_event,(void *)(uintptr_t)s_conn_epoch);
                close_target_t target=s_closing!=CLOSE_NONE?s_closing:
                    (!s_boot_settled?CLOSE_BOOT_FAILED:CLOSE_OFF);
                begin_close(target,s_failure,s_error,s_sdk_error);
            } else if (!event->connect.status && event->connect.conn_handle!=s_conn) {
                /* MAX_CONNECTIONS=1 is a required target configuration. */
                int rc=ble_gap_terminate(event->connect.conn_handle,BLE_ERR_REM_USER_CONN_TERM);
                if (rc) begin_close(CLOSE_FATAL,RYZ_BLE_FAILURE_STOP,ESP_FAIL,rc);
            }
            return 0;
        }
        if (event->connect.status) { if (s_mode==MODE_BOOT || s_mode==MODE_SAVED || s_mode==MODE_PAIR) {
            int rc=advertise(); if (rc) begin_close(s_mode==MODE_BOOT?CLOSE_BOOT_FAILED:CLOSE_FATAL,RYZ_BLE_FAILURE_START,ESP_FAIL,rc);
        } return 0; }
        if (s_conn!=BLE_HS_CONN_HANDLE_NONE) {
            (void)ble_gap_terminate(event->connect.conn_handle,BLE_ERR_REM_USER_CONN_TERM); return 0;
        }
        s_conn=event->connect.conn_handle;
        if (s_conn_epoch!=UINT32_MAX) ++s_conn_epoch;
        else { begin_close(CLOSE_FATAL,RYZ_BLE_FAILURE_START,ESP_ERR_INVALID_STATE,0); return 0; }
        s_connection_operation=s_operation;
        ryz_ble_hid_profile_link(s_conn); s_hid_error=0;
        int rc=ble_gap_set_event_cb(s_conn,gap_event,(void *)(uintptr_t)s_conn_epoch);
        struct ble_gap_conn_desc desc={0};
        if (!rc) rc=ble_gap_conn_find(s_conn,&desc);
        if (rc) { begin_close(CLOSE_FATAL,RYZ_BLE_FAILURE_START,ESP_FAIL,rc); return 0; }
        s_peer=desc.peer_id_addr; s_peer.type&=1U;
        bool admitted=s_mode==MODE_PAIR ? (!s_record.bonded && now_us()<s_deadline && write_allowed()) :
            (s_record_known && s_record.bonded && ryz_ble_addr_equal(&s_peer,&s_record.peer.peer_addr));
        if (rc || !admitted) { begin_close(s_mode==MODE_BOOT?CLOSE_BOOT_FAILED:CLOSE_FATAL,RYZ_BLE_FAILURE_AUTH,ESP_FAIL,rc?rc:BLE_HS_EAUTHOR); return 0; }
        publish(); rc=ble_gap_security_initiate(s_conn);
        if (rc && rc!=BLE_HS_EALREADY) begin_close(s_mode==MODE_BOOT?CLOSE_BOOT_FAILED:CLOSE_PAIR_FAILED,RYZ_BLE_FAILURE_AUTH,ESP_FAIL,rc);
        return 0;
    }
    case BLE_GAP_EVENT_DISCONNECT:
        if (!event_is_current(event->disconnect.conn.conn_handle,context)) return 0;
        memset(&s_ancs,0,sizeof(s_ancs));
        ryz_ble_hid_profile_reset();
        s_terminate_pending=false;
        portENTER_CRITICAL(&s_guard); s_hid_tap_pending=false; s_hid_close_pending=false; portEXIT_CRITICAL(&s_guard);
        s_conn=BLE_HS_CONN_HANDLE_NONE; s_compare=false; s_stage_armed=false;
        portENTER_CRITICAL(&s_guard); s_view.authenticated=false; portEXIT_CRITICAL(&s_guard);
        if (s_closing!=CLOSE_NONE) finish_close();
        else if (s_mode==MODE_BOOT) { int rc=advertise(); if (rc) begin_close(CLOSE_BOOT_FAILED,RYZ_BLE_FAILURE_START,ESP_FAIL,rc); }
        else if (s_mode==MODE_PAIR) begin_close(CLOSE_PAIR_FAILED,RYZ_BLE_FAILURE_AUTH,ESP_FAIL,event->disconnect.reason);
        else if (s_enabled && s_record_known && s_record.bonded) start_saved(false);
        publish(); return 0;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (!event_is_current(event->passkey.conn_handle,context)) return 0;
        if (s_mode!=MODE_PAIR || s_closing!=CLOSE_NONE || !write_allowed() || now_us()>=s_deadline ||
            event->passkey.params.action!=BLE_SM_IOACT_NUMCMP || event->passkey.params.numcmp>999999U || s_compare || s_pair_approved) {
            begin_close(s_mode==MODE_BOOT?CLOSE_BOOT_FAILED:CLOSE_PAIR_FAILED,RYZ_BLE_FAILURE_AUTH,ESP_FAIL,BLE_HS_EAUTHEN); return 0;
        }
        s_number=event->passkey.params.numcmp; s_compare=true; s_phase=RYZ_BLE_COMPARISON; publish(); return 0;
    case BLE_GAP_EVENT_IDENTITY_RESOLVED:
        if (event_is_current(event->identity_resolved.conn_handle,context) && s_mode==MODE_PAIR && s_closing==CLOSE_NONE &&
            s_pair_approved && write_allowed() && event->identity_resolved.peer_id_addr.type<=BLE_ADDR_RANDOM_ID) {
            struct ble_gap_conn_desc desc={0};
            if (ble_gap_conn_find(s_conn,&desc)==0 && ryz_ble_addr_equal(&desc.peer_id_addr,&event->identity_resolved.peer_id_addr)) {
                s_peer=event->identity_resolved.peer_id_addr; s_peer.type&=1U;
            } else begin_close(CLOSE_PAIR_FAILED,RYZ_BLE_FAILURE_AUTH,ESP_FAIL,BLE_HS_EAUTHEN);
        }
        return 0;
    case BLE_GAP_EVENT_PARING_COMPLETE:
        if (!event_is_current(event->pairing_complete.conn_handle,context)) return 0;
        /* IDF emits this before ENC_CHANGE for saved-key restoration too.
         * It neither authorizes a new key write nor proves authentication. */
        if (s_mode==MODE_BOOT || s_mode==MODE_SAVED) {
            if (event->pairing_complete.status)
                begin_close(s_mode==MODE_BOOT?CLOSE_BOOT_FAILED:CLOSE_FATAL,RYZ_BLE_FAILURE_AUTH,ESP_FAIL,event->pairing_complete.status);
            return 0;
        }
        if (event->pairing_complete.status || s_mode!=MODE_PAIR || !s_pair_approved || !write_allowed() || now_us()>=s_deadline) {
            begin_close(s_mode==MODE_BOOT?CLOSE_BOOT_FAILED:CLOSE_PAIR_FAILED,RYZ_BLE_FAILURE_AUTH,ESP_FAIL,event->pairing_complete.status); return 0;
        }
        s_stage_armed=true; return 0; /* Not persisted yet. */
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (!event_is_current(event->enc_change.conn_handle,context)) return 0;
        if (event->enc_change.status) begin_close(s_mode==MODE_BOOT?CLOSE_BOOT_FAILED:CLOSE_PAIR_FAILED,RYZ_BLE_FAILURE_AUTH,ESP_FAIL,event->enc_change.status);
        else authenticated();
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* Never auto-delete an old bond to make room. Phone must forget its
         * old entry and the user must explicitly authorize REPLACE locally. */
        if (event_is_current(event->repeat_pairing.conn_handle,context))
            begin_close(s_mode==MODE_BOOT?CLOSE_BOOT_FAILED:CLOSE_PAIR_FAILED,RYZ_BLE_FAILURE_AUTH,ESP_FAIL,BLE_HS_EAUTHEN);
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if ((uint32_t)(uintptr_t)context==s_operation && s_closing!=CLOSE_NONE) finish_close();
        return 0;
    case BLE_GAP_EVENT_TERM_FAILURE:
        if (event_is_current(event->term_failure.conn_handle,context) && s_closing!=CLOSE_NONE) {
            s_terminate_pending=false;
            s_failure=RYZ_BLE_FAILURE_STOP; s_error=ESP_FAIL; s_sdk_error=event->term_failure.status; publish();
        }
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX:
        if (event_is_current(event->notify_rx.conn_handle,context)) ancs_service_changed(event);
        /* All ANCS/user notifications are intentionally ignored. The SDK
         * retains and frees its mbuf; no notification content is copied. */
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event_is_current(event->subscribe.conn_handle,context)) {
            ryz_ble_hid_profile_subscribe(event->subscribe.conn_handle,event->subscribe.attr_handle,
                                         event->subscribe.cur_notify!=0);
            publish();
        }
        return 0;
    default: return 0;
    }
}

static bool action_allowed(ryz_ble_action_t action, const ryz_ble_snapshot_t *v)
{
    if (!v->available || v->checking_state) return false;
    if (action==RYZ_BLE_ENABLE || action==RYZ_BLE_DISABLE) return true;
    if (!v->enabled) return false;
    if (action==RYZ_BLE_CANCEL_PAIR) return v->phase==RYZ_BLE_PAIRING || v->phase==RYZ_BLE_COMPARISON;
    if (action==RYZ_BLE_PAIR_NEW) return !v->bonded && v->phase==RYZ_BLE_EMPTY;
    if (action==RYZ_BLE_RETRY_PAIR) return !v->bonded && (v->phase==RYZ_BLE_PAIR_FAILED || v->phase==RYZ_BLE_PAIR_TIMEOUT);
    if (action==RYZ_BLE_FORGET || action==RYZ_BLE_REPLACE) return v->bond_known && v->bonded;
    if (action==RYZ_BLE_RETRY_FORGET) return v->phase==RYZ_BLE_FORGET_FAILED && v->bond_known && v->bonded;
    return false;
}
static void execute_command(struct ble_npl_event *event)
{
    (void)event;
    portENTER_CRITICAL(&s_guard); command_t command=s_command; s_command.present=false;
    portEXIT_CRITICAL(&s_guard);
    if (!command.present || command.operation!=s_operation) { publish(); return; }
    if (command.numeric) {
        if (!command.accept) { begin_close(CLOSE_EMPTY,RYZ_BLE_FAILURE_NONE,ESP_OK,0); return; }
        if (!s_compare || s_number!=command.number || s_connection_operation!=s_operation || s_mode!=MODE_PAIR || s_closing!=CLOSE_NONE || now_us()>=s_deadline) {
            begin_close(CLOSE_PAIR_FAILED,RYZ_BLE_FAILURE_AUTH,ESP_ERR_INVALID_STATE,0); return;
        }
        struct ble_gap_conn_desc desc={0};
        if (ble_gap_conn_find(s_conn,&desc)!=0 || !ryz_ble_addr_equal(&desc.peer_id_addr,&s_peer)) {
            begin_close(CLOSE_PAIR_FAILED,RYZ_BLE_FAILURE_AUTH,ESP_FAIL,BLE_HS_EAUTHEN); return;
        }
        s_pair_approved=true; s_compare=false; s_phase=RYZ_BLE_PAIRING;
        struct ble_sm_io accept={.action=BLE_SM_IOACT_NUMCMP,.numcmp_accept=1};
        int rc=ble_sm_inject_io(s_conn,&accept);
        if (rc) begin_close(CLOSE_PAIR_FAILED,RYZ_BLE_FAILURE_AUTH,ESP_FAIL,rc);
        else publish();
        return;
    }
    if (command.action==RYZ_BLE_ENABLE && s_enabled && s_record_known && s_record.enabled) { publish(); return; }
    if (!advance_operation()) return;
    s_boot_settled=true; s_boot_error=ESP_OK;
    if (command.action==RYZ_BLE_ENABLE || command.action==RYZ_BLE_DISABLE) {
        bool enable=command.action==RYZ_BLE_ENABLE;
        if (!s_record_known) { s_error=ESP_ERR_INVALID_STATE; s_failure=RYZ_BLE_FAILURE_LOAD; publish(); return; }
        if (s_record.enabled!=enable) {
            ryz_ble_record_t next=s_record; next.enabled=enable;
            commit_gate(true); esp_err_t e=save_record(&next); commit_gate(false); ryz_ble_wipe(&next,sizeof(next));
            if (e!=ESP_OK) { begin_close(CLOSE_FATAL,RYZ_BLE_FAILURE_SAVE,e,0); return; }
        }
        if (!enable) begin_close(CLOSE_OFF,RYZ_BLE_FAILURE_NONE,ESP_OK,0);
        else if (s_conn!=BLE_HS_CONN_HANDLE_NONE || ble_gap_adv_active()) {
            /* Already ON: preserve the owned link/advertising operation. */
            s_connection_operation=s_operation; s_enabled=true; publish();
        } else if (s_record.bonded) start_saved(false);
        else { s_enabled=true; s_mode=MODE_NONE; s_phase=RYZ_BLE_EMPTY; s_error=ESP_OK; s_failure=RYZ_BLE_FAILURE_NONE; publish(); }
        return;
    }
    if (command.action==RYZ_BLE_CANCEL_PAIR) { begin_close(CLOSE_EMPTY,RYZ_BLE_FAILURE_NONE,ESP_OK,0); return; }
    if (command.action==RYZ_BLE_FORGET || command.action==RYZ_BLE_REPLACE || command.action==RYZ_BLE_RETRY_FORGET) {
        if (command.action!=RYZ_BLE_RETRY_FORGET) s_replace=command.action==RYZ_BLE_REPLACE;
        begin_close(CLOSE_FORGET,RYZ_BLE_FAILURE_NONE,ESP_OK,0); return;
    }
    start_pair();
}

static void poll_owner(struct ble_npl_event *event)
{
    (void)event; uint64_t now=now_us();
    if (s_store_error_pending) {
        s_store_error_pending=false;
        begin_close(CLOSE_FATAL,RYZ_BLE_FAILURE_SAVE,s_error,0);
    }
    if (s_closing!=CLOSE_NONE) {
        if (s_conn!=BLE_HS_CONN_HANDLE_NONE) {
            struct ble_gap_conn_desc desc={0};
            if (ble_gap_conn_find(s_conn,&desc)==BLE_HS_ENOTCONN) {
                s_conn=BLE_HS_CONN_HANDLE_NONE; ryz_ble_hid_profile_reset();
            } else if (!s_terminate_pending && now>=s_terminate_retry_due && now<s_close_deadline) {
                s_terminate_retry_due=now+(uint64_t)POLL_MS*1000U;
                int rc=ble_gap_terminate(s_conn,BLE_ERR_REM_USER_CONN_TERM);
                s_terminate_pending=rc==0;
                if (rc) { s_error=ESP_FAIL; s_failure=RYZ_BLE_FAILURE_STOP; s_sdk_error=rc; }
            }
        }
        if (s_adv_stop_uncertain && now<s_close_deadline) {
            int rc=ble_gap_adv_stop();
            if ((rc==0 || rc==BLE_HS_EALREADY) && !ble_gap_adv_active()) s_adv_stop_uncertain=false;
        }
        finish_close();
        if (s_closing!=CLOSE_NONE && now>=s_close_deadline) {
            s_error=ESP_ERR_TIMEOUT; s_failure=RYZ_BLE_FAILURE_STOP;
            s_phase=s_closing==CLOSE_FORGET?RYZ_BLE_FORGETTING:RYZ_BLE_STOPPING;
            if (!s_boot_settled) { s_boot_settled=true; s_boot_error=s_error; }
        }
    } else if (s_deadline && now>=s_deadline) {
        begin_close(s_mode==MODE_BOOT?CLOSE_BOOT_FAILED:CLOSE_TIMEOUT,
                    s_mode==MODE_BOOT?RYZ_BLE_FAILURE_AUTH:RYZ_BLE_FAILURE_NONE,
                    s_mode==MODE_BOOT?ESP_ERR_TIMEOUT:ESP_OK,0);
    } else if (s_synced && s_conn!=BLE_HS_CONN_HANDLE_NONE && now>=s_rssi_due) {
        s_rssi_due=now+1000000U; int8_t rssi=0; int rc=ble_gap_conn_rssi(s_conn,&rssi);
        portENTER_CRITICAL(&s_guard); s_view.rssi_valid=rc==0; s_view.rssi_dbm=rc?0:rssi; portEXIT_CRITICAL(&s_guard);
    }
    hid_poll();
    ancs_poll();
    publish();
    int rc=ble_npl_callout_reset(&s_poll,ble_npl_time_ms_to_ticks32(POLL_MS));
    if (rc) begin_close(CLOSE_FATAL,RYZ_BLE_FAILURE_START,ESP_FAIL,rc);
}
static void on_reset(int reason)
{
    memset(&s_ancs,0,sizeof(s_ancs));
    ryz_ble_hid_profile_reset();
    portENTER_CRITICAL(&s_guard); s_hid_tap_pending=false; s_hid_close_pending=false; portEXIT_CRITICAL(&s_guard);
    revoke_pairing(); s_synced=false; s_conn=BLE_HS_CONN_HANDLE_NONE; s_enabled=false;
    /* A controller reset aborts, rather than resumes, an in-flight replace or
     * pair. Invalidate already-admitted commands and wipe candidate keys. */
    if (s_operation!=UINT32_MAX) ++s_operation;
    s_closing=CLOSE_NONE; s_replace=false; s_adv_stop_uncertain=false;
    s_terminate_pending=false;
    s_candidate_our=s_candidate_peer=s_rpa_valid=false;
    ryz_ble_wipe(&s_candidate,sizeof(s_candidate));
    s_phase=RYZ_BLE_FAILED; s_failure=RYZ_BLE_FAILURE_START; s_error=ESP_FAIL; s_sdk_error=reason;
    s_boot_settled=true; s_boot_error=ESP_FAIL; s_mode=MODE_NONE; s_deadline=0; publish();
}
static void on_sync(void)
{
    /* Also arm after reset-before-first-sync; otherwise later manual pairing
     * could start without an owner timer enforcing its 30-second deadline. */
    int timer_rc=ble_npl_callout_reset(&s_poll,ble_npl_time_ms_to_ticks32(POLL_MS));
    if (timer_rc) { s_synced=true; begin_close(CLOSE_FATAL,RYZ_BLE_FAILURE_START,ESP_FAIL,timer_rc); return; }
    if (s_boot_settled) { /* Host resets never silently reopen a radio session. */
        s_synced=true; s_enabled=false; s_phase=RYZ_BLE_OFF; publish(); return;
    }
    int rc=ble_hs_id_infer_auto(0,&s_own_addr_type);
    ble_addr_t local={.type=BLE_ADDR_PUBLIC};
    if (!rc) rc=ble_hs_id_copy_addr(BLE_ADDR_PUBLIC,local.val,NULL);
    if (rc || !s_record_known || !s_record.local_valid || s_error!=ESP_OK) {
        s_synced=true; begin_close(CLOSE_FATAL,RYZ_BLE_FAILURE_LOAD,s_error?s_error:ESP_FAIL,rc); return;
    }
    portENTER_CRITICAL(&s_guard); address_text(s_view.local_address,&local); portEXIT_CRITICAL(&s_guard);
    s_synced=true;
    if (!s_record.enabled || !s_record.bonded) {
        s_enabled=false; s_phase=RYZ_BLE_OFF; s_boot_settled=true; s_boot_error=ESP_OK; publish();
    } else start_saved(true);
}
static void bootstrap(void *unused)
{
    (void)unused;
    esp_err_t e=nvs_flash_init();
    if (e==ESP_OK) e=ryz_ble_record_load(&s_record);
    s_record_known=e==ESP_OK;
    if (e==ESP_OK) e=init_port_with_diagnostics();
    if (e!=ESP_OK) {
        s_error=e; s_failure=RYZ_BLE_FAILURE_LOAD; s_phase=RYZ_BLE_FAILED;
        s_boot_settled=true; s_boot_error=e; publish(); vTaskDelete(NULL); return;
    }
    ble_hs_cfg.reset_cb=on_reset; ble_hs_cfg.sync_cb=on_sync;
    ble_hs_cfg.store_read_cb=store_read; ble_hs_cfg.store_write_cb=store_write;
    ble_hs_cfg.store_delete_cb=store_delete; ble_hs_cfg.store_status_cb=store_status;
    ble_hs_cfg.sm_io_cap=BLE_SM_IO_CAP_DISP_YES_NO;
    ble_hs_cfg.sm_bonding=1; ble_hs_cfg.sm_mitm=1; ble_hs_cfg.sm_sc=1; ble_hs_cfg.sm_sc_only=1; ble_hs_cfg.sm_sec_lvl=4;
    ble_hs_cfg.sm_our_key_dist=BLE_SM_PAIR_KEY_DIST_ENC|BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist=BLE_SM_PAIR_KEY_DIST_ENC|BLE_SM_PAIR_KEY_DIST_ID;
    ble_svc_gap_init(); ble_svc_gatt_init();
    portENTER_CRITICAL(&s_guard); snprintf(s_view.local_name,sizeof(s_view.local_name),"RyzoBee"); portEXIT_CRITICAL(&s_guard);
    int rc=ble_svc_gap_device_name_set(s_view.local_name);
    if (!rc) rc=ble_svc_gap_device_appearance_set(0x03c0);
#if defined(RYZ_BLE_HOST_TEST)
    if (!rc) rc=ryz_ble_hid_profile_set_firmware_version("host-test");
#else
    if (!rc) rc=ryz_ble_hid_profile_set_firmware_version(esp_app_get_description()->version);
#endif
    if (!rc) rc=ryz_ble_hid_profile_init(hid_authorized,NULL);
    if (rc) { s_error=ESP_FAIL; s_failure=RYZ_BLE_FAILURE_START; s_sdk_error=rc; }
    ble_npl_event_init(&s_command_event,execute_command,NULL);
    rc=ble_npl_callout_init(&s_poll,nimble_port_get_dflt_eventq(),poll_owner,NULL);
    if (rc) {
        s_error=ESP_FAIL; s_sdk_error=rc; s_failure=RYZ_BLE_FAILURE_START; s_phase=RYZ_BLE_FAILED;
        s_boot_settled=true; s_boot_error=ESP_FAIL; publish();
        (void)nimble_port_deinit(); vTaskDelete(NULL); return;
    }
    portENTER_CRITICAL(&s_guard); s_queue_ready=true; portEXIT_CRITICAL(&s_guard);
    nimble_port_run();
    /* No automatic host restart or re-advertising after ownership ends. */
    on_reset(BLE_HS_EDISABLED); (void)nimble_port_deinit(); vTaskDelete(NULL);
}

esp_err_t ryz_ble_start(void)
{
    portENTER_CRITICAL(&s_guard);
    if (s_started) { portEXIT_CRITICAL(&s_guard); return ESP_OK; }
    s_started=true; s_view.operation_id=s_operation;
    portEXIT_CRITICAL(&s_guard);
    if (xTaskCreate(bootstrap,"ryz_ble",8192,NULL,5,NULL)!=pdPASS) {
        portENTER_CRITICAL(&s_guard);
        s_view.boot_settled=true; s_view.boot_error=ESP_ERR_NO_MEM;
        s_view.last_error=ESP_ERR_NO_MEM; s_view.phase=RYZ_BLE_FAILED; ++s_view.revision;
        portEXIT_CRITICAL(&s_guard); return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
esp_err_t ryz_ble_get_snapshot(ryz_ble_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_guard); *out=s_view; portEXIT_CRITICAL(&s_guard); return ESP_OK;
}
esp_err_t ryz_ble_get_init_diagnostics(ryz_ble_init_diagnostics_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_guard); *out=s_init_diagnostics; portEXIT_CRITICAL(&s_guard); return ESP_OK;
}
esp_err_t ryz_ble_request(ryz_ble_action_t action, uint32_t expected)
{
    portENTER_CRITICAL(&s_guard);
    if (!s_queue_ready || s_command.present || s_committing || expected!=s_view.operation_id ||
        !action_allowed(action,&s_view)) { portEXIT_CRITICAL(&s_guard); return ESP_ERR_INVALID_STATE; }
    if (action==RYZ_BLE_ENABLE && s_view.enabled && s_view.saved_enabled) {
        portEXIT_CRITICAL(&s_guard); return ESP_OK;
    }
    s_command=(command_t){.present=true,.action=action,.operation=expected};
    /* Winning cancellation revokes storage BEFORE the host handles any late
     * SMP callback. No command is accepted while COMMITTING owns the gate. */
    if (action!=RYZ_BLE_PAIR_NEW && action!=RYZ_BLE_RETRY_PAIR && action!=RYZ_BLE_ENABLE) s_write_allowed=false;
    s_view.command_pending=true; ++s_view.revision;
    portEXIT_CRITICAL(&s_guard);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(),&s_command_event); return ESP_OK;
}
esp_err_t ryz_ble_confirm_numeric(uint32_t expected, uint32_t number, bool accept)
{
    portENTER_CRITICAL(&s_guard);
    if (!s_queue_ready || s_command.present || s_committing || expected!=s_view.operation_id ||
        !s_view.compare_pending || s_view.compare_value!=number || s_view.checking_state) {
        portEXIT_CRITICAL(&s_guard); return ESP_ERR_INVALID_STATE;
    }
    s_command=(command_t){.present=true,.numeric=true,.accept=accept,.operation=expected,.number=number};
    if (!accept) s_write_allowed=false;
    s_view.command_pending=true; ++s_view.revision;
    portEXIT_CRITICAL(&s_guard);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(),&s_command_event); return ESP_OK;
}

esp_err_t ryz_ble_hid_open(uint32_t *session_id)
{
    if (!session_id) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_guard);
    esp_err_t rc=ESP_OK;
    if (!s_queue_ready || !s_view.available) rc=ESP_ERR_NOT_SUPPORTED;
    else if (s_hid_session || s_view.hid_busy || s_hid_close_pending) rc=ESP_ERR_NOT_FINISHED;
    else if (!s_view.hid_ready || !s_view.authenticated || s_command.present || s_committing ||
             s_hid_token_counter==UINT32_MAX) rc=ESP_ERR_INVALID_STATE;
    else { s_hid_session=++s_hid_token_counter; *session_id=s_hid_session; }
    portEXIT_CRITICAL(&s_guard); return rc;
}
esp_err_t ryz_ble_hid_tap(uint32_t session_id, ryz_ble_hid_key_t key)
{
    if (!session_id || key<RYZ_BLE_HID_PLAY_PAUSE || key>RYZ_BLE_HID_VOLUME_DOWN) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_guard);
    esp_err_t rc=ESP_OK;
    if (!s_queue_ready || !s_view.available) rc=ESP_ERR_NOT_SUPPORTED;
    else if (session_id!=s_hid_session || !s_view.hid_ready || !s_view.authenticated ||
             s_command.present || s_committing) rc=ESP_ERR_INVALID_STATE;
    else if (s_hid_tap_pending || s_hid_close_pending || s_view.hid_busy) rc=ESP_ERR_NOT_FINISHED;
    else {
        s_hid_key=key; s_hid_tap_epoch=s_hid_public_epoch; s_hid_tap_pending=true;
        s_view.hid_busy=true; ++s_view.revision;
    }
    portEXIT_CRITICAL(&s_guard); return rc;
}
void ryz_ble_hid_close(uint32_t session_id)
{
    portENTER_CRITICAL(&s_guard);
    if (session_id && session_id==s_hid_session) {
        s_hid_session=0; s_hid_tap_pending=false; s_hid_close_pending=true;
        s_view.hid_busy=true; ++s_view.revision;
    }
    portEXIT_CRITICAL(&s_guard);
}
