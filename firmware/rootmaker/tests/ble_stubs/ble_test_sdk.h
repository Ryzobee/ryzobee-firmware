#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>

typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(m) pthread_mutex_lock(m)
#define portEXIT_CRITICAL(m) pthread_mutex_unlock(m)
#define pdPASS 1
typedef void (*TaskFunction_t)(void *);
int xTaskCreate(TaskFunction_t,const char *,uint32_t,void *,unsigned,void *);
void vTaskDelete(void *);
int64_t esp_timer_get_time(void);
typedef unsigned nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
esp_err_t nvs_flash_init(void);
esp_err_t nvs_open(const char *,int,nvs_handle_t *);
esp_err_t nvs_get_blob(nvs_handle_t,const char *,void *,size_t *);
esp_err_t nvs_set_blob(nvs_handle_t,const char *,const void *,size_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);

#define BLE_ADDR_PUBLIC 0
#define BLE_ADDR_RANDOM 1
#define BLE_ADDR_PUBLIC_ID 2
#define BLE_ADDR_RANDOM_ID 3
#define BLE_ADDR_ANONYMOUS 0xff
#define BLE_HS_CONN_HANDLE_NONE 0xffff
#define BLE_HS_FOREVER INT32_MAX
#define BLE_HS_EALREADY 2
#define BLE_HS_ENOENT 5
#define BLE_HS_ENOTCONN 7
#define BLE_HS_EAUTHEN 23
#define BLE_HS_EAUTHOR 24
#define BLE_HS_ESTORE_CAP 27
#define BLE_HS_ESTORE_FAIL 28
#define BLE_HS_EDISABLED 30
#define BLE_HS_EDONE 14
#define BLE_HS_EBADDATA 10
#define BLE_HS_ETIMEOUT 13
#define BLE_GATT_CHR_PROP_NOTIFY 0x10
#define BLE_GATT_CHR_PROP_INDICATE 0x20
#define BLE_HS_ADV_F_DISC_GEN 2
#define BLE_HS_ADV_F_BREDR_UNSUP 4
#define BLE_HS_ADV_TX_PWR_LVL_AUTO 127
#define BLE_SM_IOACT_NUMCMP 4
#define BLE_SM_IO_CAP_DISP_YES_NO 1
#define BLE_SM_PAIR_KEY_DIST_ENC 1
#define BLE_SM_PAIR_KEY_DIST_ID 2
#define BLE_ERR_REM_USER_CONN_TERM 0x13
#define BLE_GAP_CONN_MODE_UND 2
#define BLE_GAP_DISC_MODE_GEN 2
#define BLE_GAP_REPEAT_PAIRING_IGNORE 0
typedef struct { uint8_t type,val[6]; } ble_addr_t;
#define BLE_ADDR_ANY (&(ble_addr_t) { 0, {0,0,0,0,0,0} })
static inline int ble_addr_cmp(const ble_addr_t *a,const ble_addr_t *b)
{ if (a->type!=b->type) return (int)a->type-(int)b->type; for (unsigned i=0;i<6;++i) if (a->val[i]!=b->val[i]) return (int)a->val[i]-(int)b->val[i]; return 0; }
struct ble_store_value_sec {
    ble_addr_t peer_addr; uint16_t bond_count; uint8_t key_size; uint16_t ediv;
    uint64_t rand_num; uint8_t ltk[16]; uint8_t ltk_present:1;
    uint8_t irk[16]; uint8_t irk_present:1; uint8_t csrk[16]; uint8_t csrk_present:1;
    uint32_t sign_counter; unsigned authenticated:1; uint8_t sc:1;
};
struct ble_store_value_local_irk { ble_addr_t addr; uint8_t irk[16]; };
struct ble_store_value_rpa_rec { ble_addr_t peer_rpa_addr,peer_addr; };
struct ble_store_value_cccd { ble_addr_t peer_addr; uint16_t chr_val_handle,flags; unsigned value_changed:1; };
struct ble_store_value_csfc { ble_addr_t peer_addr; uint8_t csfc[1]; };
union ble_store_value { struct ble_store_value_sec sec; struct ble_store_value_local_irk local_irk;
    struct ble_store_value_rpa_rec rpa_rec; struct ble_store_value_cccd cccd; struct ble_store_value_csfc csfc; };
union ble_store_key {
    struct { ble_addr_t peer_addr; uint8_t idx; } sec;
    struct { ble_addr_t addr; uint8_t idx; } local_irk;
    struct { ble_addr_t peer_rpa_addr; uint8_t idx; } rpa_rec;
    struct { ble_addr_t peer_addr; uint16_t chr_val_handle; uint8_t idx; } cccd;
    struct { ble_addr_t peer_addr; uint8_t idx; } csfc;
};
enum { BLE_STORE_OBJ_TYPE_OUR_SEC=1, BLE_STORE_OBJ_TYPE_PEER_SEC=2, BLE_STORE_OBJ_TYPE_CCCD=3,
    BLE_STORE_OBJ_TYPE_PEER_ADDR=6, BLE_STORE_OBJ_TYPE_LOCAL_IRK=7, BLE_STORE_OBJ_TYPE_CSFC=8 };
struct ble_store_status_event { int type; };
struct ble_gap_sec_state { unsigned encrypted:1,authenticated:1,bonded:1; unsigned key_size:5; };
struct ble_gap_conn_desc { struct ble_gap_sec_state sec_state; ble_addr_t our_id_addr,peer_id_addr,our_ota_addr,peer_ota_addr; uint16_t conn_handle; };
typedef struct { uint8_t type; } ble_uuid_t;
typedef struct { ble_uuid_t u; uint16_t value; } ble_uuid16_t;
typedef struct { ble_uuid_t u; uint8_t value[16]; } ble_uuid128_t;
typedef union { ble_uuid_t u; ble_uuid16_t u16; ble_uuid128_t u128; } ble_uuid_any_t;
#define BLE_UUID16_INIT(v) { {16}, (v) }
#define BLE_UUID128_INIT(...) { {128}, {__VA_ARGS__} }
int ble_uuid_cmp(const ble_uuid_t *,const ble_uuid_t *);
struct ble_gatt_error { uint16_t status,att_handle; };
struct ble_gatt_svc { uint16_t start_handle,end_handle; ble_uuid_any_t uuid; };
struct ble_gatt_chr { uint16_t def_handle,val_handle; uint8_t properties; ble_uuid_any_t uuid; };
struct ble_gatt_dsc { uint16_t handle; ble_uuid_any_t uuid; };
struct os_mbuf { uint16_t len; uint8_t data[4]; };
#define OS_MBUF_PKTLEN(om) ((om)->len)
int os_mbuf_copydata(const struct os_mbuf *,int,int,void *);
struct ble_gatt_attr { uint16_t handle,offset; struct os_mbuf *om; };
typedef int ble_gatt_disc_svc_fn(uint16_t,const struct ble_gatt_error *,const struct ble_gatt_svc *,void *);
typedef int ble_gatt_chr_fn(uint16_t,const struct ble_gatt_error *,const struct ble_gatt_chr *,void *);
typedef int ble_gatt_dsc_fn(uint16_t,const struct ble_gatt_error *,uint16_t,const struct ble_gatt_dsc *,void *);
typedef int ble_gatt_attr_fn(uint16_t,const struct ble_gatt_error *,struct ble_gatt_attr *,void *);
int ble_gattc_disc_svc_by_uuid(uint16_t,const ble_uuid_t *,ble_gatt_disc_svc_fn *,void *);
int ble_gattc_disc_all_chrs(uint16_t,uint16_t,uint16_t,ble_gatt_chr_fn *,void *);
int ble_gattc_disc_all_dscs(uint16_t,uint16_t,uint16_t,ble_gatt_dsc_fn *,void *);
int ble_gattc_write_flat(uint16_t,uint16_t,const void *,uint16_t,ble_gatt_attr_fn *,void *);
struct ble_gap_adv_params { uint8_t conn_mode,disc_mode; uint16_t itvl_min,itvl_max; };
struct ble_hs_adv_fields { uint8_t flags; const uint8_t *name; uint8_t name_len,name_is_complete;
    const ble_uuid128_t *sol_uuids128; uint8_t sol_num_uuids128;
    const ble_uuid16_t *uuids16; uint8_t num_uuids16,uuids16_is_complete;
    uint16_t appearance; uint8_t appearance_is_present;
    int8_t tx_pwr_lvl; uint8_t tx_pwr_lvl_is_present; };
struct ble_sm_io { uint8_t action; uint8_t numcmp_accept; };
enum { BLE_GAP_EVENT_CONNECT=0,BLE_GAP_EVENT_DISCONNECT=1,BLE_GAP_EVENT_ADV_COMPLETE=2,
    BLE_GAP_EVENT_PASSKEY_ACTION=3,BLE_GAP_EVENT_IDENTITY_RESOLVED=4,BLE_GAP_EVENT_PARING_COMPLETE=5,
    BLE_GAP_EVENT_ENC_CHANGE=6,BLE_GAP_EVENT_REPEAT_PAIRING=7,BLE_GAP_EVENT_TERM_FAILURE=8,
    BLE_GAP_EVENT_NOTIFY_RX=9, BLE_GAP_EVENT_SUBSCRIBE=10 };
struct ble_gap_event { int type; union {
    struct { int status; uint16_t conn_handle; } connect,enc_change,pairing_complete,term_failure;
    struct { struct ble_gap_conn_desc conn; int reason; } disconnect;
    struct { uint16_t conn_handle; struct { uint8_t action; uint32_t numcmp; } params; } passkey;
    struct { uint16_t conn_handle; ble_addr_t peer_id_addr; } identity_resolved;
    struct { uint16_t conn_handle; } repeat_pairing;
    struct { struct os_mbuf *om; uint16_t attr_handle,conn_handle; uint8_t indication; } notify_rx;
    struct { uint16_t conn_handle,attr_handle; uint8_t cur_notify,cur_indicate; } subscribe;
}; };
typedef int ble_gap_event_fn(struct ble_gap_event *,void *);
struct ble_hs_cfg {
    void (*reset_cb)(int); void (*sync_cb)(void);
    int (*store_read_cb)(int,const union ble_store_key *,union ble_store_value *);
    int (*store_write_cb)(int,const union ble_store_value *);
    int (*store_delete_cb)(int,const union ble_store_key *);
    int (*store_status_cb)(struct ble_store_status_event *,void *);
    uint8_t sm_io_cap,sm_bonding,sm_mitm,sm_sc,sm_sc_only,sm_sec_lvl,sm_our_key_dist,sm_their_key_dist;
};
extern struct ble_hs_cfg ble_hs_cfg;
struct ble_npl_event { void (*fn)(struct ble_npl_event *); void *arg; };
struct ble_npl_eventq { int unused; };
struct ble_npl_callout { struct ble_npl_event ev; };
void ble_npl_event_init(struct ble_npl_event *,void (*)(struct ble_npl_event *),void *);
void ble_npl_eventq_put(struct ble_npl_eventq *,struct ble_npl_event *);
int ble_npl_callout_init(struct ble_npl_callout *,struct ble_npl_eventq *,void (*)(struct ble_npl_event *),void *);
int ble_npl_callout_reset(struct ble_npl_callout *,uint32_t);
uint32_t ble_npl_time_ms_to_ticks32(uint32_t);
struct ble_npl_eventq *nimble_port_get_dflt_eventq(void);
esp_err_t nimble_port_init(void);
esp_err_t nimble_port_deinit(void);
void nimble_port_run(void);
void ble_svc_gap_init(void);
void ble_svc_gatt_init(void);
int ble_svc_gap_device_name_set(const char *);
int ble_svc_gap_device_appearance_set(uint16_t);
int ble_hs_id_infer_auto(int,uint8_t *);
int ble_hs_id_copy_addr(uint8_t,uint8_t *,int *);
int ble_hs_pvcy_set_our_irk(const uint8_t *);
int ble_hs_pvcy_add_entry(const uint8_t *,uint8_t,const uint8_t *);
int ble_gap_adv_set_fields(const struct ble_hs_adv_fields *);
int ble_gap_adv_rsp_set_fields(const struct ble_hs_adv_fields *);
int ble_gap_adv_start(uint8_t,const ble_addr_t *,int32_t,const struct ble_gap_adv_params *,ble_gap_event_fn *,void *);
int ble_gap_adv_stop(void);
int ble_gap_adv_active(void);
int ble_gap_conn_find(uint16_t,struct ble_gap_conn_desc *);
int ble_gap_set_event_cb(uint16_t,ble_gap_event_fn *,void *);
int ble_gap_security_initiate(uint16_t);
int ble_gap_terminate(uint16_t,uint8_t);
int ble_gap_conn_rssi(uint16_t,int8_t *);
int ble_sm_inject_io(uint16_t,struct ble_sm_io *);
