#include "ble_test_sdk.h"
#include "ryz_ble.h"
#include "ryz_ble_store.h"
#include "ryz_ble_hid_profile.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Production owner + codec + SDK Adapter. Only external NVS/HCI/NPL/task
 * transports are replaced, with callbacks captured at real registration. */
struct ble_hs_cfg ble_hs_cfg;
static TaskFunction_t task;
static struct ble_npl_eventq queue;
static struct ble_npl_event *queued;
static struct ble_npl_callout *timer;
static jmp_buf host_yield;
static uint64_t now;
static bool adv,connected;
static unsigned starts,stops,terminates,commits,writes,read_opens,closed;
static int adv_start_error,adv_stop_error,terminate_error,resolver_error;
static unsigned fail_commit,fail_read_open;
static bool commit_then_error,all_reads_fail;
static int task_error,init_error;
static ble_gap_event_fn *adv_cb,*conn_cb;
static void *adv_arg,*conn_arg;
static struct ble_gap_conn_desc connection;
static struct ble_sm_io last_io;
static uint8_t durable[256];
static bool exists;
static struct { bool open,write,dirty; uint8_t bytes[256]; } handles[4];
static const ble_addr_t peer={.type=BLE_ADDR_PUBLIC,.val={1,2,3,4,5,6}};
static esp_err_t cancel_during_commit;
static bool inject_cancel_commit;
static bool reset_before_sync, callbacks_on_reject;
static unsigned timer_arms;
static int timer_reset_error;
static int adv_fields_error,gatt_error;
static uint16_t adv_interval;
static unsigned scan_responses;
static int scan_response_error;
static unsigned gatt_requests,gatt_writes;
enum { GATT_NONE,GATT_SERVICE,GATT_CHARS,GATT_DESCS,GATT_WRITE };
static int gatt_pending;
static ble_gatt_disc_svc_fn *gatt_svc_cb;
static ble_gatt_chr_fn *gatt_chr_cb;
static ble_gatt_dsc_fn *gatt_dsc_cb;
static ble_gatt_attr_fn *gatt_write_cb;
static void *gatt_arg;
static ble_uuid_any_t gatt_uuid;
static uint16_t gatt_first,gatt_last;

/* Owner integration seam. The real profile's GATT/ATT/report transport has
 * its own ASan tests; this deterministic peer isolates mailbox/lease races. */
static ryz_ble_hid_authorize_fn hid_auth;
static void *hid_auth_ctx;
static uint16_t hid_conn=BLE_HS_CONN_HANDLE_NONE;
static bool hid_subscribed,hid_held,hid_releasing;
static uint64_t hid_due;
static unsigned hid_presses,hid_releases;
static uint8_t hid_bits;
static int hid_poll_error,hid_init_error;
int ryz_ble_hid_profile_init(ryz_ble_hid_authorize_fn auth,void *ctx)
{ hid_auth=auth; hid_auth_ctx=ctx; return hid_init_error; }
int ryz_ble_hid_profile_set_firmware_version(const char *v)
{ assert(!strcmp(v,"host-test")); return 0; }
void ryz_ble_hid_profile_reset(void)
{ hid_conn=BLE_HS_CONN_HANDLE_NONE; hid_subscribed=hid_held=hid_releasing=false; }
void ryz_ble_hid_profile_link(uint16_t h)
{ ryz_ble_hid_profile_reset(); hid_conn=h; }
void ryz_ble_hid_profile_subscribe(uint16_t conn,uint16_t attr,bool notify)
{ if (conn==hid_conn && attr==90) hid_subscribed=notify; }
bool ryz_ble_hid_profile_ready(void)
{ return hid_conn!=BLE_HS_CONN_HANDLE_NONE && hid_subscribed && hid_auth && hid_auth(hid_conn,hid_auth_ctx); }
bool ryz_ble_hid_profile_busy(void) { return hid_held; }
int ryz_ble_hid_profile_tap(uint8_t bits,uint64_t time)
{
    if (!ryz_ble_hid_profile_ready()) return BLE_HS_EAUTHEN;
    assert(!hid_held && bits && !(bits&0x80));
    hid_bits=bits; ++hid_presses; hid_held=true; hid_due=time+100000; return 0;
}
void ryz_ble_hid_profile_release(void) { hid_releasing=true; }
int ryz_ble_hid_profile_poll(uint64_t time)
{
    if (hid_poll_error) return hid_poll_error;
    if (hid_held && (time>=hid_due || hid_releasing)) {
        if (!ryz_ble_hid_profile_ready()) return BLE_HS_EAUTHEN;
        ++hid_releases; hid_held=false;
    }
    hid_releasing=false; return 0;
}
int ryz_ble_hid_profile_last_error(void) { return hid_poll_error; }
void ryz_ble_hid_profile_update_battery(bool valid,uint8_t percent) { (void)valid; (void)percent; }

static ryz_ble_snapshot_t snapshot(void)
{ ryz_ble_snapshot_t s; assert(ryz_ble_get_snapshot(&s)==ESP_OK); return s; }
int ble_uuid_cmp(const ble_uuid_t *a,const ble_uuid_t *b)
{
    if (a->type!=b->type) return a->type-b->type;
    if (a->type==16) return ((const ble_uuid16_t *)a)->value-((const ble_uuid16_t *)b)->value;
    assert(a->type==128); return memcmp(((const ble_uuid128_t *)a)->value,((const ble_uuid128_t *)b)->value,16);
}
int os_mbuf_copydata(const struct os_mbuf *om,int offset,int size,void *out)
{ assert(offset>=0 && size>=0 && offset+size<=om->len && om->len<=4); memcpy(out,om->data+offset,(size_t)size); return 0; }
static int gatt_admit(uint16_t handle,int kind,void *arg)
{
    assert(connected && handle==connection.conn_handle && snapshot().authenticated && !gatt_pending);
    ++gatt_requests;
    if (gatt_error) return gatt_error;
    gatt_pending=kind; gatt_arg=arg; return 0;
}
int ble_gattc_disc_svc_by_uuid(uint16_t h,const ble_uuid_t *uuid,ble_gatt_disc_svc_fn *cb,void *arg)
{
    gatt_svc_cb=cb;
    if (uuid->type==16) gatt_uuid.u16=*(const ble_uuid16_t *)uuid;
    else { assert(uuid->type==128); gatt_uuid.u128=*(const ble_uuid128_t *)uuid; }
    return gatt_admit(h,GATT_SERVICE,arg);
}
int ble_gattc_disc_all_chrs(uint16_t h,uint16_t first,uint16_t last,ble_gatt_chr_fn *cb,void *arg)
{ assert(first && last>=first); gatt_first=first; gatt_last=last; gatt_chr_cb=cb; return gatt_admit(h,GATT_CHARS,arg); }
int ble_gattc_disc_all_dscs(uint16_t h,uint16_t first,uint16_t last,ble_gatt_dsc_fn *cb,void *arg)
{ assert(first==3 && last==5); gatt_first=first; gatt_last=last; gatt_dsc_cb=cb; return gatt_admit(h,GATT_DESCS,arg); }
int ble_gattc_write_flat(uint16_t h,uint16_t attr,const void *data,uint16_t len,ble_gatt_attr_fn *cb,void *arg)
{
    /* Assert the privacy boundary at the real SDK transport. There is no
     * allowed write to ANCS handles/CCCDs, nor any read API fake at all. */
    assert(attr==4 && len==2 && !memcmp(data,"\x02\x00",2));
    ++gatt_writes; gatt_write_cb=cb; return gatt_admit(h,GATT_WRITE,arg);
}
int64_t esp_timer_get_time(void) { return (int64_t)now; }
int xTaskCreate(TaskFunction_t fn,const char *name,uint32_t stack,void *arg,unsigned priority,void *out)
{ assert(name && stack>=8192 && !arg && priority==5 && !out); task=fn; return task_error?0:pdPASS; }
void vTaskDelete(void *arg) { assert(!arg); }
esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *out)
{
    assert(!strcmp(name,"ryz_ble") && out);
    if (mode==NVS_READONLY) { ++read_opens; if (all_reads_fail || read_opens==fail_read_open) return ESP_FAIL; }
    for (unsigned i=0;i<4;++i) if (!handles[i].open) {
        handles[i].open=true; handles[i].write=mode==NVS_READWRITE; handles[i].dirty=false;
        memcpy(handles[i].bytes,durable,256); *out=i+1; return ESP_OK;
    }
    abort();
}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *size)
{ assert(h && handles[h-1].open && !strcmp(key,"record") && *size==256); if (!exists) return ESP_ERR_NVS_NOT_FOUND; memcpy(out,handles[h-1].bytes,256); return ESP_OK; }
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *in,size_t size)
{ assert(h && handles[h-1].open && handles[h-1].write && !strcmp(key,"record") && size==256); ++writes; memcpy(handles[h-1].bytes,in,256); handles[h-1].dirty=true; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h)
{
    assert(h && handles[h-1].open && handles[h-1].dirty); ++commits;
    if (inject_cancel_commit) {
        inject_cancel_commit=false;
        cancel_during_commit=ryz_ble_request(RYZ_BLE_CANCEL_PAIR,snapshot().operation_id);
    }
    if (commits==fail_commit && !commit_then_error) return ESP_FAIL;
    memcpy(durable,handles[h-1].bytes,256); exists=true;
    return commits==fail_commit?ESP_FAIL:ESP_OK;
}
void nvs_close(nvs_handle_t h) { assert(h && handles[h-1].open); handles[h-1].open=false; ++closed; }
void ble_npl_event_init(struct ble_npl_event *e,void (*fn)(struct ble_npl_event *),void *arg) { *e=(struct ble_npl_event){fn,arg}; }
void ble_npl_eventq_put(struct ble_npl_eventq *q,struct ble_npl_event *e) { assert(q==&queue && !queued); queued=e; }
int ble_npl_callout_init(struct ble_npl_callout *t,struct ble_npl_eventq *q,void (*fn)(struct ble_npl_event *),void *arg)
{ assert(q==&queue); t->ev=(struct ble_npl_event){fn,arg}; timer=t; return 0; }
int ble_npl_callout_reset(struct ble_npl_callout *t,uint32_t ticks) { assert(t==timer && ticks==100); ++timer_arms; return timer_reset_error; }
uint32_t ble_npl_time_ms_to_ticks32(uint32_t ms) { return ms; }
struct ble_npl_eventq *nimble_port_get_dflt_eventq(void) { return &queue; }
esp_err_t nimble_port_init(void) { return init_error; }
esp_err_t nimble_port_deinit(void) { return ESP_OK; }
void nimble_port_run(void)
{
    assert(ble_hs_cfg.store_read_cb && ble_hs_cfg.store_write_cb);
    union ble_store_key key={0}; union ble_store_value value={0};
    int rc=ble_hs_cfg.store_read_cb(BLE_STORE_OBJ_TYPE_LOCAL_IRK,&key,&value);
    if (rc==BLE_HS_ENOENT) {
        memset(value.local_irk.irk,0x77,16); value.local_irk.addr.type=BLE_ADDR_PUBLIC;
        (void)ble_hs_cfg.store_write_cb(BLE_STORE_OBJ_TYPE_LOCAL_IRK,&value);
    }
    if (reset_before_sync) ble_hs_cfg.reset_cb(123);
    ble_hs_cfg.sync_cb(); longjmp(host_yield,1);
}
void ble_svc_gap_init(void) {}
void ble_svc_gatt_init(void) {}
int ble_svc_gap_device_name_set(const char *s) { assert(!strcmp(s,"RyzoBee")); return 0; }
int ble_svc_gap_device_appearance_set(uint16_t appearance) { assert(appearance==0x03c0); return 0; }
int ble_hs_id_infer_auto(int privacy,uint8_t *out) { assert(!privacy); *out=BLE_ADDR_PUBLIC; return 0; }
int ble_hs_id_copy_addr(uint8_t type,uint8_t *out,int *rnd) { assert(!type && !rnd); memset(out,0xaa,6); return 0; }
int ble_hs_pvcy_set_our_irk(const uint8_t *irk) { assert(irk); return resolver_error; }
int ble_hs_pvcy_add_entry(const uint8_t *addr,uint8_t type,const uint8_t *irk) { assert(addr && type<=1 && irk); return resolver_error; }
int ble_gap_adv_set_fields(const struct ble_hs_adv_fields *f)
{
    /* Apple ANCS service solicitation (AD type 0x15), little-endian UUID.
     * This is a request for the phone's service, not a local service list. */
    static const uint8_t expected[16]={0xd0,0x00,0x2d,0x12,0x1e,0x4b,0x0f,0xa4,
                                     0x99,0x4e,0xce,0xb5,0x31,0xf4,0x05,0x79};
    assert(f && !f->name && !f->name_len);
    assert(f->num_uuids16==1 && f->uuids16 && f->uuids16->value==0x1812);
    assert(!f->uuids16_is_complete); /* BAS/GAP/GATT/DIS not enumerated here. */
    assert(f->appearance_is_present && f->appearance==0x03c0);
    assert(f->flags==(BLE_HS_ADV_F_DISC_GEN|BLE_HS_ADV_F_BREDR_UNSUP));
    assert(f->sol_num_uuids128==1 && f->sol_uuids128);
    assert(f->sol_uuids128->u.type==128 && !memcmp(f->sol_uuids128->value,expected,16));
    assert(3+4+4+18<=31); /* Flags, HID, appearance, ANCS solicitation = 29. */
    return adv_fields_error;
}
int ble_gap_adv_rsp_set_fields(const struct ble_hs_adv_fields *f)
{
    assert(f && f->name_len==7 && f->name_is_complete && !memcmp(f->name,"RyzoBee",7));
    assert(f->tx_pwr_lvl_is_present && f->tx_pwr_lvl==BLE_HS_ADV_TX_PWR_LVL_AUTO);
    assert(!f->flags && !f->num_uuids16 && !f->sol_num_uuids128);
    ++scan_responses; return scan_response_error;
}
int ble_gap_adv_start(uint8_t type,const ble_addr_t *direct,int32_t duration,const struct ble_gap_adv_params *p,ble_gap_event_fn *fn,void *arg)
{ assert(type==BLE_ADDR_PUBLIC && !direct && duration==BLE_HS_FOREVER && p->conn_mode==BLE_GAP_CONN_MODE_UND && !adv && !connected); assert(p->itvl_min==p->itvl_max && (p->itvl_min==32 || p->itvl_min==244)); adv_interval=p->itvl_min; if (adv_start_error) return adv_start_error; adv=true; ++starts; adv_cb=fn; adv_arg=arg; return 0; }
int ble_gap_adv_stop(void) { ++stops; if (adv_stop_error) return adv_stop_error; bool active=adv; adv=false; return active?0:BLE_HS_EALREADY; }
int ble_gap_adv_active(void) { return adv; }
int ble_gap_conn_find(uint16_t h,struct ble_gap_conn_desc *out)
{ if (!connected || h!=connection.conn_handle) return BLE_HS_ENOTCONN; *out=connection; return 0; }
int ble_gap_set_event_cb(uint16_t h,ble_gap_event_fn *fn,void *arg)
{ assert(connected && h==connection.conn_handle); conn_cb=fn; conn_arg=arg; return 0; }
int ble_gap_security_initiate(uint16_t h) { assert(connected && h==connection.conn_handle); return 0; }
int ble_gap_terminate(uint16_t h,uint8_t reason)
{ assert(connected && h==connection.conn_handle && reason==BLE_ERR_REM_USER_CONN_TERM); ++terminates; return terminate_error; }
int ble_gap_conn_rssi(uint16_t h,int8_t *out) { assert(connected && h==connection.conn_handle); *out=-42; return 0; }
int ble_sm_inject_io(uint16_t h,struct ble_sm_io *io)
{
    assert(connected && h==connection.conn_handle && io->action==BLE_SM_IOACT_NUMCMP); last_io=*io;
    if (!io->numcmp_accept && callbacks_on_reject) {
        struct ble_gap_event p={.type=BLE_GAP_EVENT_PARING_COMPLETE,.pairing_complete={.status=5,.conn_handle=h}};
        struct ble_gap_event e={.type=BLE_GAP_EVENT_ENC_CHANGE,.enc_change={.status=5,.conn_handle=h}};
        assert(conn_cb(&p,conn_arg)==0); assert(conn_cb(&e,conn_arg)==0);
    }
    return 0;
}

static ryz_ble_record_t make_record(bool enabled,bool bonded)
{
    ryz_ble_record_t r={.enabled=enabled,.local_valid=true,.bonded=bonded,.our_valid=bonded,.peer_valid=bonded};
    memset(r.local.irk,0x77,16);
    if (bonded) {
        r.our.peer_addr=r.peer.peer_addr=peer;
        r.our.key_size=r.peer.key_size=16; r.our.authenticated=r.peer.authenticated=1;
        r.our.sc=r.peer.sc=1; r.our.ltk_present=r.peer.ltk_present=1; r.peer.irk_present=1;
        memset(r.our.ltk,0x33,16); memset(r.peer.ltk,0x33,16); memset(r.peer.irk,0x44,16);
    }
    return r;
}
static void seed(bool enabled,bool bonded)
{ ryz_ble_record_t r=make_record(enabled,bonded); assert(ryz_ble_record_save(&r)==ESP_OK); ryz_ble_wipe(&r,sizeof(r)); }
static void start(void)
{
    assert(ryz_ble_start()==ESP_OK && task);
    if (!setjmp(host_yield)) task(NULL);
    assert(snapshot().available || snapshot().boot_error!=ESP_OK);
    if (snapshot().available) {
        assert(ble_hs_cfg.sm_io_cap==BLE_SM_IO_CAP_DISP_YES_NO && ble_hs_cfg.sm_mitm && ble_hs_cfg.sm_sc_only);
        assert(ble_hs_cfg.sm_sc && ble_hs_cfg.sm_sec_lvl==4 && ble_hs_cfg.sm_bonding);
    }
}
static void run_command(void)
{ assert(queued); struct ble_npl_event *e=queued; queued=NULL; e->fn(e); }
static void command(ryz_ble_action_t action)
{
    assert(ryz_ble_request(action,snapshot().operation_id)==ESP_OK);
    if (queued) run_command();
}
static void tick(uint64_t at) { now=at; assert(timer); timer->ev.fn(&timer->ev); }
static void connect_peer(ble_addr_t identity)
{
    assert(adv && !connected); adv=false; connected=true;
    connection=(struct ble_gap_conn_desc){.conn_handle=7,.peer_id_addr=identity,.peer_ota_addr=identity};
    struct ble_gap_event e={.type=BLE_GAP_EVENT_CONNECT,.connect={.status=0,.conn_handle=7}};
    assert(adv_cb(&e,adv_arg)==0);
}
static void disconnect_peer(void)
{
    assert(connected); struct ble_gap_event e={.type=BLE_GAP_EVENT_DISCONNECT};
    e.disconnect.conn=connection; e.disconnect.reason=BLE_ERR_REM_USER_CONN_TERM;
    connected=false; gatt_pending=GATT_NONE; assert(conn_cb(&e,conn_arg)==0);
}
static void numeric(uint32_t n)
{
    struct ble_gap_event e={.type=BLE_GAP_EVENT_PASSKEY_ACTION};
    e.passkey.conn_handle=7; e.passkey.params.action=BLE_SM_IOACT_NUMCMP; e.passkey.params.numcmp=n;
    assert(conn_cb(&e,conn_arg)==0);
}
static void complete(int status)
{ struct ble_gap_event e={.type=BLE_GAP_EVENT_PARING_COMPLETE,.pairing_complete={.status=status,.conn_handle=7}}; assert(conn_cb(&e,conn_arg)==0); }
static void encryption(bool auth)
{
    connection.sec_state.encrypted=1; connection.sec_state.authenticated=auth; connection.sec_state.key_size=16;
    struct ble_gap_event e={.type=BLE_GAP_EVENT_ENC_CHANGE,.enc_change={.status=0,.conn_handle=7}};
    assert(conn_cb(&e,conn_arg)==0);
}
static int keys(int type)
{
    ryz_ble_record_t r=make_record(true,true); union ble_store_value v={0};
    v.sec=type==BLE_STORE_OBJ_TYPE_OUR_SEC?r.our:r.peer; v.sec.peer_addr=connection.peer_id_addr;
    int rc=ble_hs_cfg.store_write_cb(type,&v); ryz_ble_wipe(&r,sizeof(r)); ryz_ble_wipe(&v,sizeof(v)); return rc;
}
static void begin_pair(void)
{
    seed(true,false); start(); assert(snapshot().phase==RYZ_BLE_OFF && snapshot().boot_settled && !adv);
    command(RYZ_BLE_ENABLE); assert(snapshot().phase==RYZ_BLE_EMPTY && !adv);
    command(RYZ_BLE_PAIR_NEW); assert(snapshot().phase==RYZ_BLE_PAIRING && adv && snapshot().remaining_s==30);
    connect_peer(peer); numeric(1234); assert(snapshot().compare_pending && snapshot().compare_value==1234);
}
static void approve(void)
{
    assert(ryz_ble_confirm_numeric(snapshot().operation_id,1234,true)==ESP_OK); run_command();
    assert(last_io.numcmp_accept==1 && !snapshot().compare_pending);
}
static void stage_keys(void)
{ complete(0); assert(keys(BLE_STORE_OBJ_TYPE_OUR_SEC)==0); assert(keys(BLE_STORE_OBJ_TYPE_PEER_SEC)==0); }
static void pair_success(void)
{ begin_pair(); approve(); stage_keys(); encryption(true); assert(snapshot().phase==RYZ_BLE_PAIRED && snapshot().authenticated && snapshot().bonded); }

static void hid_subscribe(void)
{
    struct ble_gap_event e={.type=BLE_GAP_EVENT_SUBSCRIBE,
        .subscribe={.conn_handle=7,.attr_handle=90,.cur_notify=1}};
    assert(conn_cb(&e,conn_arg)==0);
}
static uint32_t hid_lease(void)
{
    uint32_t token=0; assert(ryz_ble_hid_open(&token)==ESP_OK && token); return token;
}
static void hid_lifecycle(void)
{
    uint32_t token=99;
    assert(ryz_ble_hid_open(NULL)==ESP_ERR_INVALID_ARG);
    assert(ryz_ble_hid_open(&token)==ESP_ERR_NOT_SUPPORTED && token==99);
    begin_pair(); hid_subscribe(); assert(!snapshot().hid_ready);
    assert(ryz_ble_hid_open(&token)==ESP_ERR_INVALID_STATE);
    approve(); stage_keys(); encryption(true); assert(snapshot().hid_ready && !hid_presses);
    token=hid_lease();
    assert(ryz_ble_hid_open(&token)==ESP_ERR_NOT_FINISHED);
    assert(ryz_ble_hid_tap(token,(ryz_ble_hid_key_t)7)==ESP_ERR_INVALID_ARG);
    assert(ryz_ble_hid_tap(token,RYZ_BLE_HID_PLAY_PAUSE)==ESP_OK);
    assert(!hid_presses && snapshot().hid_busy);
    assert(ryz_ble_hid_tap(token,RYZ_BLE_HID_MUTE)==ESP_ERR_NOT_FINISHED);
    tick(100000); assert(hid_presses==1 && hid_bits==1 && hid_held);
    tick(200000); assert(!hid_held && hid_releases==1 && !snapshot().hid_busy);
    assert(ryz_ble_hid_tap(token,RYZ_BLE_HID_VOLUME_DOWN)==ESP_OK);
    ryz_ble_hid_close(token); /* close before dispatch cancels, never presses. */
    tick(300000); assert(hid_presses==1 && !snapshot().hid_busy);
    uint32_t next=hid_lease(); assert(next!=token);
    ryz_ble_hid_close(token); /* stale previous-job cleanup cannot revoke next. */
    assert(ryz_ble_hid_tap(next,RYZ_BLE_HID_VOLUME_UP)==ESP_OK);
    tick(400000); assert(hid_presses==2 && hid_bits==(1U<<RYZ_BLE_HID_VOLUME_UP));
    ryz_ble_hid_close(next);
    assert(ryz_ble_hid_open(&token)==ESP_ERR_NOT_FINISHED);
    tick(400001); assert(!hid_held && hid_releases==2);
    token=hid_lease(); ryz_ble_hid_close(token);
}
static void hid_off_and_epoch(void)
{
    pair_success(); hid_subscribe(); uint32_t token=hid_lease();
    assert(ryz_ble_hid_tap(token,RYZ_BLE_HID_STOP)==ESP_OK);
    assert(ryz_ble_request(RYZ_BLE_DISABLE,snapshot().operation_id)==ESP_OK);
    tick(100000); assert(!hid_presses); run_command(); disconnect_peer();
    assert(!snapshot().hid_ready && !snapshot().hid_busy);
    command(RYZ_BLE_ENABLE); connect_peer(peer); complete(0); encryption(true); hid_subscribe();
    tick(200000); assert(!hid_presses); /* no queued input crosses connection epoch */
    assert(ryz_ble_hid_tap(token,RYZ_BLE_HID_NEXT_TRACK)==ESP_OK);
    connection.sec_state.encrypted=0; tick(300000); assert(!hid_presses && !snapshot().hid_ready);
    ryz_ble_hid_close(token);
}
static void hid_release_failure(void)
{
    pair_success(); hid_subscribe(); uint32_t token=hid_lease();
    assert(ryz_ble_hid_tap(token,RYZ_BLE_HID_MUTE)==ESP_OK);
    tick(100000); assert(hid_held);
    ryz_ble_hid_close(token); hid_poll_error=BLE_HS_EBADDATA; tick(100001);
    assert(terminates && snapshot().phase==RYZ_BLE_STOPPING && snapshot().hid_sdk_error==BLE_HS_EBADDATA);
    assert(ryz_ble_hid_open(&token)!=ESP_OK); disconnect_peer(); assert(!hid_held);
}
static void hid_scan_response_failure(void)
{
    seed(true,false); start(); command(RYZ_BLE_ENABLE);
    scan_response_error=81; command(RYZ_BLE_PAIR_NEW);
    assert(!adv && scan_responses==1 && snapshot().sdk_error==81 && snapshot().phase==RYZ_BLE_PAIR_FAILED);
    scan_response_error=0; command(RYZ_BLE_RETRY_PAIR); assert(adv && scan_responses==2);
}
static void hid_stop_retry(void)
{
    pair_success(); hid_subscribe(); uint32_t token=hid_lease();
    assert(ryz_ble_hid_tap(token,RYZ_BLE_HID_STOP)==ESP_OK); tick(100000);
    terminate_error=71; command(RYZ_BLE_DISABLE);
    assert(hid_releases==1 && !hid_held && terminates==1);
    tick(200000); assert(terminates==2 && snapshot().phase==RYZ_BLE_STOPPING);
    terminate_error=0; tick(300000); assert(terminates==3);
    tick(400000); assert(terminates==3); /* successful request waits for event */
    struct ble_gap_event fail={.type=BLE_GAP_EVENT_TERM_FAILURE,
        .term_failure={.conn_handle=7,.status=72}};
    assert(conn_cb(&fail,conn_arg)==0); tick(500000); assert(terminates==4);
    disconnect_peer(); assert(snapshot().phase==RYZ_BLE_OFF && !snapshot().hid_busy);
    ryz_ble_hid_close(token);
}
static void hid_unsubscribe_persistence(void)
{
    pair_success();
    union ble_store_value v={.cccd={.peer_addr=peer,.chr_val_handle=90,.flags=1}};
    union ble_store_key k={.cccd={.peer_addr=peer,.chr_val_handle=90}};
    assert(!ble_hs_cfg.store_write_cb(BLE_STORE_OBJ_TYPE_CCCD,&v));
    assert(!ble_hs_cfg.store_read_cb(BLE_STORE_OBJ_TYPE_CCCD,&k,&v));
    assert(!ble_hs_cfg.store_delete_cb(BLE_STORE_OBJ_TYPE_CCCD,&k));
    assert(ble_hs_cfg.store_read_cb(BLE_STORE_OBJ_TYPE_CCCD,&k,&v)==BLE_HS_ENOENT);
    ryz_ble_record_t r; assert(ryz_ble_record_load(&r)==ESP_OK && !r.cccd_count && r.bonded);
    assert(!ble_hs_cfg.store_delete_cb(BLE_STORE_OBJ_TYPE_CCCD,&k)); /* already absent */
    assert(ble_hs_cfg.store_delete_cb(BLE_STORE_OBJ_TYPE_OUR_SEC,&k)==BLE_HS_ESTORE_FAIL);
    k.cccd.chr_val_handle=0;
    assert(ble_hs_cfg.store_delete_cb(BLE_STORE_OBJ_TYPE_CCCD,&k)==BLE_HS_ESTORE_FAIL);
    k.cccd.chr_val_handle=90; k.cccd.peer_addr.val[0]^=1;
    assert(ble_hs_cfg.store_delete_cb(BLE_STORE_OBJ_TYPE_CCCD,&k)==BLE_HS_ESTORE_FAIL);
    k.cccd.peer_addr=peer;
    assert(!ble_hs_cfg.store_write_cb(BLE_STORE_OBJ_TYPE_CCCD,&v));
    fail_commit=commits+1;
    assert(ble_hs_cfg.store_delete_cb(BLE_STORE_OBJ_TYPE_CCCD,&k)==BLE_HS_ESTORE_FAIL);
    assert(!snapshot().authenticated); tick(100000); assert(terminates);
}

static void boot_off(void)
{
    seed(false,true); uint8_t before[256]; memcpy(before,durable,256); unsigned n=commits;
    start(); assert(snapshot().phase==RYZ_BLE_OFF && !snapshot().enabled && !snapshot().saved_enabled);
    assert(snapshot().boot_settled && snapshot().bonded && !starts && !adv && commits==n);
    assert(!memcmp(before,durable,256));
}
static void boot_no_peer(void)
{ seed(true,false); start(); assert(snapshot().boot_settled && !snapshot().enabled && snapshot().saved_enabled && !starts); }
static void boot_timeout(void)
{
    seed(true,true); uint8_t before[256]; memcpy(before,durable,256); unsigned n=commits;
    start(); assert(adv && !snapshot().boot_settled && snapshot().remaining_s==10);
    tick(9999999); assert(adv && !snapshot().boot_settled); tick(10000000);
    assert(snapshot().boot_settled && snapshot().boot_error==ESP_ERR_TIMEOUT && snapshot().phase==RYZ_BLE_OFF);
    assert(!adv && snapshot().bonded && snapshot().saved_enabled && commits==n && !memcmp(before,durable,256));
}
static void restore(void)
{
    seed(true,true); start(); uint32_t op=snapshot().operation_id;
    command(RYZ_BLE_ENABLE); assert(snapshot().operation_id==op && !queued && adv); /* Idempotent ON preserves callback epoch. */
    connect_peer(peer); complete(0); assert(!terminates && snapshot().linked && !snapshot().authenticated);
    assert(keys(BLE_STORE_OBJ_TYPE_OUR_SEC)==BLE_HS_ESTORE_FAIL); /* Restore cannot open a new-key window. */
    encryption(true); assert(snapshot().authenticated && snapshot().phase==RYZ_BLE_LINKED && snapshot().boot_settled);
    assert(!snapshot().service_known && !snapshot().service_ready);
    tick(11000000); assert(connected && snapshot().authenticated && !terminates);
    disconnect_peer(); assert(adv && snapshot().phase==RYZ_BLE_SAVED_WAITING && !snapshot().remaining_valid);
}
static void pair_durable(void)
{
    pair_success(); ryz_ble_record_t loaded={0}; assert(ryz_ble_record_load(&loaded)==ESP_OK && loaded.bonded);
    assert(loaded.our.authenticated && loaded.our.sc && loaded.our.key_size==16);
    assert(loaded.enabled && !snapshot().service_ready); ryz_ble_wipe(&loaded,sizeof(loaded));
}
static void no_numeric(void)
{
    begin_pair(); complete(0); assert(keys(BLE_STORE_OBJ_TYPE_OUR_SEC)==BLE_HS_ESTORE_FAIL);
    encryption(true); disconnect_peer(); assert(!snapshot().bonded && !snapshot().authenticated);
    ryz_ble_record_t r; assert(ryz_ble_record_load(&r)==ESP_OK && !r.bonded);
}
static void cancel_late(bool reject)
{
    begin_pair(); if (!reject) { approve(); stage_keys(); }
    uint32_t op=snapshot().operation_id;
    assert((reject?ryz_ble_confirm_numeric(op,1234,false):ryz_ble_request(RYZ_BLE_CANCEL_PAIR,op))==ESP_OK);
    assert(keys(BLE_STORE_OBJ_TYPE_OUR_SEC)==BLE_HS_ESTORE_FAIL);
    complete(0); encryption(true); assert(!snapshot().bonded && !snapshot().authenticated);
    run_command(); assert(snapshot().phase==RYZ_BLE_STOPPING && snapshot().linked);
    encryption(true); disconnect_peer(); assert(snapshot().phase==RYZ_BLE_EMPTY && !snapshot().bonded && !snapshot().authenticated);
    ryz_ble_record_t r; assert(ryz_ble_record_load(&r)==ESP_OK && !r.bonded);
}
static void pairing_timeout(void)
{
    begin_pair(); approve(); stage_keys(); tick(30000000);
    assert(snapshot().phase==RYZ_BLE_STOPPING && terminates==1);
    assert(keys(BLE_STORE_OBJ_TYPE_PEER_SEC)==BLE_HS_ESTORE_FAIL); encryption(true); disconnect_peer();
    assert(snapshot().phase==RYZ_BLE_PAIR_TIMEOUT && !snapshot().bonded);
}
static void save_failure(bool ambiguous,bool readback)
{
    begin_pair(); approve(); stage_keys();
    if (readback) fail_read_open=read_opens+1;
    else { fail_commit=commits+1; commit_then_error=ambiguous; }
    encryption(true); assert(!snapshot().authenticated && !snapshot().bonded);
    assert(snapshot().failure==RYZ_BLE_FAILURE_SAVE && snapshot().last_error!=ESP_OK);
    disconnect_peer(); assert(snapshot().phase==RYZ_BLE_PAIR_FAILED);
    ryz_ble_record_t r; assert(ryz_ble_record_load(&r)==ESP_OK && !r.bonded);
}
static void commit_gate_test(void)
{
    begin_pair(); approve(); stage_keys(); inject_cancel_commit=true; encryption(true);
    assert(cancel_during_commit==ESP_ERR_INVALID_STATE && snapshot().phase==RYZ_BLE_PAIRED && snapshot().authenticated);
}
static void stale_number(void)
{
    begin_pair(); unsigned n=commits; uint32_t op=snapshot().operation_id;
    assert(ryz_ble_confirm_numeric(op-1,1234,true)==ESP_ERR_INVALID_STATE);
    assert(ryz_ble_confirm_numeric(op,4321,true)==ESP_ERR_INVALID_STATE);
    assert(!queued && snapshot().compare_pending && commits==n);
}
static void forget(bool replace,bool fail)
{
    pair_success(); unsigned n=commits;
    command(replace?RYZ_BLE_REPLACE:RYZ_BLE_FORGET);
    assert(snapshot().phase==RYZ_BLE_FORGETTING && snapshot().bonded && commits==n);
    complete(5); encryption(true);
    assert(snapshot().phase==RYZ_BLE_FORGETTING && !snapshot().authenticated && commits==n);
    if (fail) fail_commit=commits+1;
    disconnect_peer();
    if (fail) { assert(snapshot().phase==RYZ_BLE_FORGET_FAILED && snapshot().bonded && !adv); command(RYZ_BLE_RETRY_FORGET); }
    ryz_ble_record_t r; assert(ryz_ble_record_load(&r)==ESP_OK && !r.bonded && r.enabled);
    assert(snapshot().old_bond_removed && !snapshot().bonded);
    if (replace) assert(snapshot().phase==RYZ_BLE_PAIRING && adv && snapshot().remaining_s==30);
    else assert(snapshot().phase==RYZ_BLE_EMPTY && !adv);
}
static void off_preserves(void)
{
    pair_success(); command(RYZ_BLE_DISABLE);
    assert(snapshot().phase==RYZ_BLE_STOPPING && snapshot().linked && !snapshot().authenticated);
    complete(5); encryption(true); assert(snapshot().phase==RYZ_BLE_STOPPING);
    ryz_ble_record_t r; assert(ryz_ble_record_load(&r)==ESP_OK && !r.enabled && r.bonded);
    disconnect_peer(); assert(snapshot().phase==RYZ_BLE_OFF && !snapshot().enabled && snapshot().bonded);
}
static void stop_failure(void)
{
    seed(true,true); start(); adv_stop_error=123; command(RYZ_BLE_DISABLE);
    assert(snapshot().phase==RYZ_BLE_STOPPING && snapshot().checking_state && adv);
    tick(5000000); assert(snapshot().phase==RYZ_BLE_STOPPING && snapshot().last_error==ESP_ERR_TIMEOUT && adv);
}
static void foreign_peer(void)
{
    seed(true,true); start(); ble_addr_t foreign=peer; foreign.val[0]^=1; connect_peer(foreign);
    complete(0); encryption(true); assert(!snapshot().authenticated && terminates);
    disconnect_peer(); assert(snapshot().phase==RYZ_BLE_OFF && snapshot().bonded);
}
static void bad_record(void)
{
    seed(true,true); durable[8]^=1; start();
    assert(!snapshot().available && !snapshot().bond_known && snapshot().boot_settled && !starts);
}
static void synchronous_reject_off(void)
{
    begin_pair(); callbacks_on_reject=true; command(RYZ_BLE_DISABLE);
    assert(snapshot().phase==RYZ_BLE_STOPPING && !snapshot().saved_enabled);
    disconnect_peer(); assert(snapshot().phase==RYZ_BLE_OFF && !snapshot().enabled && !snapshot().bonded);
}
static void reset_then_pair_timeout(void)
{
    seed(true,false); reset_before_sync=true; start(); assert(timer_arms && snapshot().available);
    command(RYZ_BLE_ENABLE); command(RYZ_BLE_PAIR_NEW); tick(30000000);
    assert(snapshot().phase==RYZ_BLE_PAIR_TIMEOUT && !adv);
}
static void unknown_read_cleanup(void)
{
    begin_pair(); approve(); stage_keys(); all_reads_fail=true;
    encryption(true); assert(!snapshot().bond_known && !snapshot().authenticated && snapshot().checking_state);
    disconnect_peer(); assert(snapshot().phase==RYZ_BLE_FAILED && !snapshot().enabled);
    all_reads_fail=false; ryz_ble_record_t r;
    assert(ryz_ble_record_load(&r)==ESP_OK && !r.bonded && r.enabled);
}
static void reused_handle_old_callbacks(void)
{
    begin_pair(); approve(); stage_keys(); void *old_context=conn_arg;
    command(RYZ_BLE_CANCEL_PAIR); disconnect_peer(); command(RYZ_BLE_PAIR_NEW);
    connect_peer(peer); numeric(1234); approve(); assert(conn_arg!=old_context);
    struct ble_gap_event p={.type=BLE_GAP_EVENT_PARING_COMPLETE,.pairing_complete={.status=0,.conn_handle=7}};
    assert(conn_cb(&p,old_context)==0 && keys(BLE_STORE_OBJ_TYPE_OUR_SEC)==BLE_HS_ESTORE_FAIL);
    struct ble_gap_event e={.type=BLE_GAP_EVENT_ENC_CHANGE,.enc_change={.status=0,.conn_handle=7}};
    assert(conn_cb(&e,old_context)==0 && !snapshot().authenticated && !snapshot().bonded);
    stage_keys(); encryption(true); assert(snapshot().authenticated && snapshot().bonded);
}
static void identity_resolution_before_keys(void)
{
    seed(true,false); start(); command(RYZ_BLE_ENABLE); command(RYZ_BLE_PAIR_NEW);
    ble_addr_t rpa={.type=BLE_ADDR_RANDOM,.val={7,8,9,10,11,0x40}};
    connect_peer(rpa); numeric(1234); approve(); complete(0);
    connection.peer_id_addr=peer;
    struct ble_gap_event e={.type=BLE_GAP_EVENT_IDENTITY_RESOLVED,.identity_resolved={.conn_handle=7,.peer_id_addr=peer}};
    assert(conn_cb(&e,conn_arg)==0);
    assert(keys(BLE_STORE_OBJ_TYPE_OUR_SEC)==0 && keys(BLE_STORE_OBJ_TYPE_PEER_SEC)==0); encryption(true);
    ryz_ble_record_t r; assert(ryz_ble_record_load(&r)==ESP_OK && r.bonded && ryz_ble_addr_equal(&r.peer.peer_addr,&peer));
}
static void late_connect_after_off(void)
{
    seed(true,true); start(); void *old_adv_arg=adv_arg;
    command(RYZ_BLE_DISABLE); assert(snapshot().phase==RYZ_BLE_OFF && !adv);
    connected=true; connection=(struct ble_gap_conn_desc){.conn_handle=7,.peer_id_addr=peer};
    struct ble_gap_event e={.type=BLE_GAP_EVENT_CONNECT,.connect={.status=0,.conn_handle=7}};
    assert(adv_cb(&e,old_adv_arg)==0);
    assert(snapshot().phase==RYZ_BLE_STOPPING && snapshot().linked && terminates);
    complete(0); encryption(true); assert(!snapshot().authenticated);
    disconnect_peer(); assert(snapshot().phase==RYZ_BLE_OFF && !snapshot().linked);
}
static void missing_deadline_timer_fails_closed(void)
{
    seed(true,true); timer_reset_error=55; start();
    assert(snapshot().boot_settled && snapshot().boot_error!=ESP_OK && !adv && !starts);
    assert(snapshot().phase==RYZ_BLE_FAILED && !snapshot().enabled);
}
static void pump(void) { tick(now+100000U); }
static void service_reply(bool present)
{
    assert(gatt_pending==GATT_SERVICE);
    if (present) {
        struct ble_gatt_svc svc={.start_handle=gatt_uuid.u.type==16?1:10,
                                 .end_handle=gatt_uuid.u.type==16?8:25,.uuid=gatt_uuid};
        const struct ble_gatt_error ok={0};
        assert(gatt_svc_cb(7,&ok,&svc,gatt_arg)==0);
    }
    gatt_pending=GATT_NONE;
    const struct ble_gatt_error done={.status=BLE_HS_EDONE};
    (void)gatt_svc_cb(7,&done,NULL,gatt_arg);
}
static void chars_done(void)
{
    assert(gatt_pending==GATT_CHARS); gatt_pending=GATT_NONE;
    const struct ble_gatt_error done={.status=BLE_HS_EDONE};
    (void)gatt_chr_cb(7,&done,NULL,gatt_arg);
}
static void watch_service_changed(void)
{
    pump(); assert(gatt_pending==GATT_SERVICE && gatt_uuid.u.type==16 && gatt_uuid.u16.value==0x1801);
    service_reply(true); pump();
    assert(gatt_pending==GATT_CHARS && gatt_first==1 && gatt_last==8);
    const struct ble_gatt_error ok={0};
    struct ble_gatt_chr chr={.def_handle=2,.val_handle=3,.properties=BLE_GATT_CHR_PROP_INDICATE,
                             .uuid.u16=BLE_UUID16_INIT(0x2a05)};
    assert(gatt_chr_cb(7,&ok,&chr,gatt_arg)==0);
    chr.def_handle=6; chr.val_handle=7; chr.uuid.u16.value=0x2b29;
    assert(gatt_chr_cb(7,&ok,&chr,gatt_arg)==0);
    chars_done(); pump();
    assert(gatt_pending==GATT_DESCS);
    struct ble_gatt_dsc dsc={.handle=4,.uuid.u16=BLE_UUID16_INIT(0x2902)};
    assert(gatt_dsc_cb(7,&ok,3,&dsc,gatt_arg)==0);
    gatt_pending=GATT_NONE;
    const struct ble_gatt_error done={.status=BLE_HS_EDONE};
    assert(gatt_dsc_cb(7,&done,3,NULL,gatt_arg)==0); pump();
    assert(gatt_pending==GATT_WRITE); gatt_pending=GATT_NONE;
    struct ble_gatt_attr attr={.handle=4};
    assert(gatt_write_cb(7,&ok,&attr,gatt_arg)==0);
    assert(snapshot().ancs_service_changed_subscribed && snapshot().ancs_state==RYZ_BLE_ANCS_DISCOVERING);
}
static void finish_ancs(bool present)
{
    pump(); assert(gatt_pending==GATT_SERVICE && gatt_uuid.u.type==128);
    const ble_uuid128_t expected=BLE_UUID128_INIT(
        0xd0,0x00,0x2d,0x12,0x1e,0x4b,0x0f,0xa4,0x99,0x4e,0xce,0xb5,0x31,0xf4,0x05,0x79);
    assert(!ble_uuid_cmp(&gatt_uuid.u,&expected.u));
    service_reply(present);
    if (!present) { assert(snapshot().ancs_state==RYZ_BLE_ANCS_UNAVAILABLE); return; }
    pump(); assert(gatt_pending==GATT_CHARS && gatt_first==10 && gatt_last==25);
    const struct ble_gatt_error ok={0};
    struct ble_gatt_chr chr={.def_handle=11,.val_handle=12,.properties=BLE_GATT_CHR_PROP_NOTIFY,
        .uuid.u128=BLE_UUID128_INIT(0xbd,0x1d,0xa2,0x99,0xe6,0x25,0x58,0x8c,0xd9,0x42,0x01,0x63,0x0d,0x12,0xbf,0x9f)};
    assert(gatt_chr_cb(7,&ok,&chr,gatt_arg)==0); chars_done();
    assert(snapshot().ancs_state==RYZ_BLE_ANCS_DISCOVERED && !snapshot().ancs_sdk_error);
    assert(!snapshot().service_ready && !snapshot().service_known); /* Discovery != data access. */
}
static void changed(uint16_t handle,bool indication,unsigned length)
{
    struct os_mbuf om={.len=(uint16_t)length,.data={1,0,0xff,0xff}};
    struct ble_gap_event event={.type=BLE_GAP_EVENT_NOTIFY_RX,
        .notify_rx={.conn_handle=7,.attr_handle=handle,.indication=indication,.om=&om}};
    assert(conn_cb(&event,conn_arg)==0);
}
static void ancs_discovery(void)
{
    begin_pair(); pump(); assert(!gatt_requests); approve(); stage_keys(); pump(); assert(!gatt_requests);
    encryption(true); watch_service_changed(); finish_ancs(true);
    unsigned before=gatt_requests; pump(); changed(12,false,4); changed(3,false,4); changed(3,true,3); pump();
    assert(gatt_requests==before && gatt_writes==1 && snapshot().ancs_state==RYZ_BLE_ANCS_DISCOVERED);
    disconnect_peer(); assert(snapshot().ancs_state==RYZ_BLE_ANCS_IDLE && !snapshot().ancs_service_changed_subscribed);
}
static void ancs_changed(bool in_flight)
{
    pair_success(); watch_service_changed();
    if (in_flight) { pump(); assert(gatt_pending==GATT_SERVICE); changed(3,true,4); service_reply(false); }
    else { finish_ancs(false); changed(3,true,4); }
    assert(snapshot().ancs_state==RYZ_BLE_ANCS_DISCOVERING);
    watch_service_changed(); finish_ancs(true);
    assert(gatt_writes==2);
    changed(3,true,4); watch_service_changed(); finish_ancs(false); /* iOS unpublishes ANCS. */
    assert(snapshot().authenticated && snapshot().bonded && !terminates);
}
static void ancs_request_failure(bool timeout)
{
    pair_success(); if (!timeout) gatt_error=BLE_HS_EAUTHOR;
    pump(); assert(gatt_requests==1);
    if (timeout) tick(now+10000000U);
    assert(snapshot().ancs_state==RYZ_BLE_ANCS_ERROR && snapshot().ancs_sdk_error==(timeout?BLE_HS_ETIMEOUT:BLE_HS_EAUTHOR));
    assert(snapshot().authenticated && snapshot().bonded && !terminates); /* Keep valid bond. */
    if (timeout) service_reply(false); /* Late terminal reply cannot turn failure into success. */
    pump(); assert(gatt_requests==1 && snapshot().ancs_state==RYZ_BLE_ANCS_ERROR);
}
static void ancs_late_after_off(void)
{
    pair_success(); pump();
    ble_gatt_disc_svc_fn *old_cb=gatt_svc_cb; void *old_arg=gatt_arg;
    assert(ryz_ble_request(RYZ_BLE_DISABLE,snapshot().operation_id)==ESP_OK);
    const struct ble_gatt_error done={.status=BLE_HS_EDONE};
    assert(old_cb(7,&done,NULL,old_arg)==BLE_HS_EDONE); /* Revoked at admission. */
    run_command(); disconnect_peer(); assert(snapshot().ancs_state==RYZ_BLE_ANCS_IDLE);
    command(RYZ_BLE_ENABLE); connect_peer(peer); complete(0); encryption(true); pump();
    assert(gatt_arg!=old_arg); unsigned before=gatt_requests;
    assert(old_cb(7,&done,NULL,old_arg)==BLE_HS_EDONE);
    assert(snapshot().ancs_state==RYZ_BLE_ANCS_DISCOVERING && gatt_requests==before);
    service_reply(false); pump(); service_reply(false);
    assert(snapshot().ancs_state==RYZ_BLE_ANCS_UNAVAILABLE);
}
static void ancs_malformed(void)
{
    pair_success(); pump(); const struct ble_gatt_error ok={0};
    struct ble_gatt_svc bad={.start_handle=8,.end_handle=1,.uuid=gatt_uuid};
    assert(gatt_svc_cb(7,&ok,&bad,gatt_arg)==BLE_HS_EDONE); gatt_pending=GATT_NONE;
    assert(snapshot().ancs_state==RYZ_BLE_ANCS_ERROR && snapshot().ancs_sdk_error==BLE_HS_EBADDATA);
    pump(); assert(gatt_requests==1 && snapshot().authenticated);
}
static void ancs_advertise_failure(void)
{
    seed(true,false); start(); command(RYZ_BLE_ENABLE); adv_fields_error=BLE_HS_EBADDATA;
    command(RYZ_BLE_PAIR_NEW); assert(!adv && !starts && snapshot().phase==RYZ_BLE_PAIR_FAILED);
    assert(snapshot().sdk_error==BLE_HS_EBADDATA);
    adv_fields_error=0; command(RYZ_BLE_RETRY_PAIR); assert(adv && adv_interval==32);
}
static void ancs_missing_source(void)
{
    pair_success(); watch_service_changed(); pump(); service_reply(true); pump(); chars_done();
    assert(snapshot().ancs_state==RYZ_BLE_ANCS_ERROR && snapshot().ancs_sdk_error==BLE_HS_EBADDATA);
    assert(snapshot().authenticated && gatt_writes==1); /* Service UUID alone is not enough. */
}
static void ancs_watch_failure(void)
{
    pair_success(); watch_service_changed(); finish_ancs(false); changed(3,true,4);
    pump(); service_reply(true); pump();
    const struct ble_gatt_error ok={0};
    struct ble_gatt_chr chr={.def_handle=2,.val_handle=3,.properties=BLE_GATT_CHR_PROP_INDICATE,
                             .uuid.u16=BLE_UUID16_INIT(0x2a05)};
    assert(gatt_chr_cb(7,&ok,&chr,gatt_arg)==0);
    chr.def_handle=6; chr.val_handle=7; chr.uuid.u16.value=0x2b29;
    assert(gatt_chr_cb(7,&ok,&chr,gatt_arg)==0);
    chars_done(); pump();
    struct ble_gatt_dsc dsc={.handle=4,.uuid.u16=BLE_UUID16_INIT(0x2902)};
    assert(gatt_dsc_cb(7,&ok,3,&dsc,gatt_arg)==0); gatt_pending=GATT_NONE;
    const struct ble_gatt_error done={.status=BLE_HS_EDONE};
    assert(gatt_dsc_cb(7,&done,3,NULL,gatt_arg)==0); pump();
    assert(gatt_pending==GATT_WRITE); gatt_pending=GATT_NONE;
    const struct ble_gatt_error denied={.status=BLE_HS_EAUTHOR};
    assert(gatt_write_cb(7,&denied,NULL,gatt_arg)==0);
    assert(snapshot().ancs_state==RYZ_BLE_ANCS_ERROR && !snapshot().ancs_service_changed_subscribed);
    assert(snapshot().authenticated && snapshot().bonded && !terminates);
}
int main(int argc,char **argv)
{
    assert(argc==2);
    if (!strcmp(argv[1],"boot_off")) boot_off();
    else if (!strcmp(argv[1],"boot_empty")) boot_no_peer();
    else if (!strcmp(argv[1],"boot_timeout")) boot_timeout();
    else if (!strcmp(argv[1],"restore")) restore();
    else if (!strcmp(argv[1],"pair")) pair_durable();
    else if (!strcmp(argv[1],"no_numeric")) no_numeric();
    else if (!strcmp(argv[1],"cancel")) cancel_late(false);
    else if (!strcmp(argv[1],"reject")) cancel_late(true);
    else if (!strcmp(argv[1],"pair_timeout")) pairing_timeout();
    else if (!strcmp(argv[1],"save_fail")) save_failure(false,false);
    else if (!strcmp(argv[1],"save_unknown")) save_failure(true,false);
    else if (!strcmp(argv[1],"readback_fail")) save_failure(false,true);
    else if (!strcmp(argv[1],"commit_gate")) commit_gate_test();
    else if (!strcmp(argv[1],"number")) stale_number();
    else if (!strcmp(argv[1],"forget")) forget(false,false);
    else if (!strcmp(argv[1],"replace")) forget(true,false);
    else if (!strcmp(argv[1],"forget_fail")) forget(false,true);
    else if (!strcmp(argv[1],"replace_fail")) forget(true,true);
    else if (!strcmp(argv[1],"off")) off_preserves();
    else if (!strcmp(argv[1],"stop_fail")) stop_failure();
    else if (!strcmp(argv[1],"foreign")) foreign_peer();
    else if (!strcmp(argv[1],"bad_record")) bad_record();
    else if (!strcmp(argv[1],"sync_reject")) synchronous_reject_off();
    else if (!strcmp(argv[1],"reset_timeout")) reset_then_pair_timeout();
    else if (!strcmp(argv[1],"unknown_cleanup")) unknown_read_cleanup();
    else if (!strcmp(argv[1],"reused_handle")) reused_handle_old_callbacks();
    else if (!strcmp(argv[1],"identity")) identity_resolution_before_keys();
    else if (!strcmp(argv[1],"late_connect")) late_connect_after_off();
    else if (!strcmp(argv[1],"timer_fail")) missing_deadline_timer_fails_closed();
    else if (!strcmp(argv[1],"ancs")) ancs_discovery();
    else if (!strcmp(argv[1],"ancs_changed")) ancs_changed(false);
    else if (!strcmp(argv[1],"ancs_dirty")) ancs_changed(true);
    else if (!strcmp(argv[1],"ancs_error")) ancs_request_failure(false);
    else if (!strcmp(argv[1],"ancs_timeout")) ancs_request_failure(true);
    else if (!strcmp(argv[1],"ancs_late")) ancs_late_after_off();
    else if (!strcmp(argv[1],"ancs_bad")) ancs_malformed();
    else if (!strcmp(argv[1],"ancs_adv_error")) ancs_advertise_failure();
    else if (!strcmp(argv[1],"ancs_missing_source")) ancs_missing_source();
    else if (!strcmp(argv[1],"ancs_watch_error")) ancs_watch_failure();
    else if (!strcmp(argv[1],"hid_lifecycle")) hid_lifecycle();
    else if (!strcmp(argv[1],"hid_off_epoch")) hid_off_and_epoch();
    else if (!strcmp(argv[1],"hid_release_failure")) hid_release_failure();
    else if (!strcmp(argv[1],"hid_scan_response_failure")) hid_scan_response_failure();
    else if (!strcmp(argv[1],"hid_stop_retry")) hid_stop_retry();
    else if (!strcmp(argv[1],"hid_unsubscribe_persistence")) hid_unsubscribe_persistence();
    else abort();
    for (unsigned i=0;i<4;++i) assert(!handles[i].open);
    puts("RYZ_BLE_BACKEND_PASS"); return 0;
}
