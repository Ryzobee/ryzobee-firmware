/* SDK 5.5.4 ble_store.c passes member-sized output objects cast to the union
 * callback ABI. Exercise the registered production callback, not an oversized
 * union fixture which could hide writes into the caller's stack. */
#define main existing_backend_test_main
#define nimble_port_run oversized_fixture_nimble_port_run
#include "ble_backend_test.c"
#undef nimble_port_run
#undef main

/* Match the SDK's first-boot local IRK object too, including a missing record:
 * the typed helper casts the member pointer; it does not allocate a union. */
void nimble_port_run(void)
{
    union ble_store_key key={0};
    struct ble_store_value_local_irk value={0};
    int rc=ble_hs_cfg.store_read_cb(BLE_STORE_OBJ_TYPE_LOCAL_IRK,&key,
                                  (union ble_store_value *)(void *)&value);
    if (rc==BLE_HS_ENOENT) {
        memset(value.irk,0x77,sizeof(value.irk));
        value.addr.type=BLE_ADDR_PUBLIC;
        assert(ble_hs_cfg.store_write_cb(BLE_STORE_OBJ_TYPE_LOCAL_IRK,
                                        (const union ble_store_value *)(const void *)&value)==0);
    } else assert(rc==0);
    ble_hs_cfg.sync_cb();
    longjmp(host_yield,1);
}

static void missing_leaves_member_unchanged(int type,const union ble_store_key *key,
                                          void *out,size_t size)
{
    uint8_t before[sizeof(union ble_store_value)];
    assert(size<=sizeof(before));
    memset(out,0xa5,size); memcpy(before,out,size);
    assert(ble_hs_cfg.store_read_cb(type,key,(union ble_store_value *)out)==BLE_HS_ENOENT);
    assert(!memcmp(out,before,size));
    assert(ble_hs_cfg.store_read_cb(type,NULL,(union ble_store_value *)out)==BLE_HS_ESTORE_FAIL);
    assert(!memcmp(out,before,size));
    assert(ble_hs_cfg.store_read_cb(type,key,NULL)==BLE_HS_ESTORE_FAIL);
}

static void local_irk_member(void)
{
    seed(false,false);
    start();
    union ble_store_key key={0};
    struct ble_store_value_local_irk out;
    memset(&out,0xa5,sizeof(out));
    assert(ble_hs_cfg.store_read_cb(BLE_STORE_OBJ_TYPE_LOCAL_IRK,&key,
                                  (union ble_store_value *)(void *)&out)==0);
    for (unsigned i=0;i<sizeof(out.irk);++i) assert(out.irk[i]==0x77);
    key.local_irk.idx=1;
    missing_leaves_member_unchanged(BLE_STORE_OBJ_TYPE_LOCAL_IRK,&key,&out,sizeof(out));
}

static void first_boot_member(void)
{
    assert(!exists); start();
    assert(snapshot().available && snapshot().boot_settled);
    assert(snapshot().phase==RYZ_BLE_OFF && !snapshot().bonded);
    assert(writes==1 && commits==1);
}

static void seed_metadata(void)
{
    ryz_ble_record_t r=make_record(false,true);
    r.cccd_count=1;
    r.cccd[0].peer_addr=peer; r.cccd[0].chr_val_handle=0x1234; r.cccd[0].flags=3;
    r.csfc_valid=true; r.csfc.peer_addr=peer; r.csfc.csfc[0]=7;
    assert(ryz_ble_record_save(&r)==ESP_OK);
    ryz_ble_wipe(&r,sizeof(r));
    start();
}

static void cccd_member(void)
{
    seed_metadata();
    union ble_store_key key={0}; key.cccd.peer_addr=peer; key.cccd.chr_val_handle=0x1234;
    struct ble_store_value_cccd out;
    memset(&out,0xa5,sizeof(out));
    assert(ble_hs_cfg.store_read_cb(BLE_STORE_OBJ_TYPE_CCCD,&key,
                                  (union ble_store_value *)(void *)&out)==0);
    assert(ble_addr_cmp(&out.peer_addr,&peer)==0 && out.chr_val_handle==0x1234 && out.flags==3);
    key.cccd.chr_val_handle=0x4321;
    missing_leaves_member_unchanged(BLE_STORE_OBJ_TYPE_CCCD,&key,&out,sizeof(out));
    key.cccd.chr_val_handle=0x1234; key.cccd.idx=1;
    missing_leaves_member_unchanged(BLE_STORE_OBJ_TYPE_CCCD,&key,&out,sizeof(out));
}

static void csfc_member(void)
{
    seed_metadata();
    union ble_store_key key={0}; key.csfc.peer_addr=peer;
    struct ble_store_value_csfc out;
    memset(&out,0xa5,sizeof(out));
    assert(ble_hs_cfg.store_read_cb(BLE_STORE_OBJ_TYPE_CSFC,&key,
                                  (union ble_store_value *)(void *)&out)==0);
    assert(ble_addr_cmp(&out.peer_addr,&peer)==0 && out.csfc[0]==7);
    key.csfc.idx=1;
    missing_leaves_member_unchanged(BLE_STORE_OBJ_TYPE_CSFC,&key,&out,sizeof(out));
}

static void sec_member(int type)
{
    seed(false,true); start();
    union ble_store_key key={0}; key.sec.peer_addr=peer;
    struct ble_store_value_sec out;
    memset(&out,0xa5,sizeof(out));
    assert(ble_hs_cfg.store_read_cb(type,&key,(union ble_store_value *)(void *)&out)==0);
    assert(ble_addr_cmp(&out.peer_addr,&peer)==0 && out.key_size==16 && out.ltk_present);
    assert(out.authenticated && out.sc);
    for (unsigned i=0;i<sizeof(out.ltk);++i) assert(out.ltk[i]==0x33);
    key.sec.idx=1;
    missing_leaves_member_unchanged(type,&key,&out,sizeof(out));
    key.sec.idx=0; key.sec.peer_addr.val[0]^=0x80;
    missing_leaves_member_unchanged(type,&key,&out,sizeof(out));
}

static void rpa_member(void)
{
    begin_pair(); approve(); complete(0);
    struct ble_store_value_rpa_rec value={.peer_rpa_addr={.type=BLE_ADDR_RANDOM,.val={9,8,7,6,5,4}},
                                         .peer_addr=peer};
    assert(ble_hs_cfg.store_write_cb(BLE_STORE_OBJ_TYPE_PEER_ADDR,
                                    (const union ble_store_value *)(const void *)&value)==0);
    union ble_store_key key={0}; key.rpa_rec.peer_rpa_addr=value.peer_rpa_addr;
    struct ble_store_value_rpa_rec out;
    memset(&out,0xa5,sizeof(out));
    assert(ble_hs_cfg.store_read_cb(BLE_STORE_OBJ_TYPE_PEER_ADDR,&key,
                                  (union ble_store_value *)(void *)&out)==0);
    assert(ble_addr_cmp(&out.peer_addr,&peer)==0);
    assert(ble_addr_cmp(&out.peer_rpa_addr,&value.peer_rpa_addr)==0);
    key.rpa_rec.idx=1;
    missing_leaves_member_unchanged(BLE_STORE_OBJ_TYPE_PEER_ADDR,&key,&out,sizeof(out));
}

static void unbonded_and_unknown_members(void)
{
    seed(false,false); start();
    union ble_store_key key={0};
    struct ble_store_value_sec sec;
    struct ble_store_value_cccd cccd;
    struct ble_store_value_csfc csfc;
    struct ble_store_value_rpa_rec rpa;
    missing_leaves_member_unchanged(BLE_STORE_OBJ_TYPE_OUR_SEC,&key,&sec,sizeof(sec));
    missing_leaves_member_unchanged(BLE_STORE_OBJ_TYPE_PEER_SEC,&key,&sec,sizeof(sec));
    missing_leaves_member_unchanged(BLE_STORE_OBJ_TYPE_CCCD,&key,&cccd,sizeof(cccd));
    missing_leaves_member_unchanged(BLE_STORE_OBJ_TYPE_CSFC,&key,&csfc,sizeof(csfc));
    missing_leaves_member_unchanged(BLE_STORE_OBJ_TYPE_PEER_ADDR,&key,&rpa,sizeof(rpa));
    uint8_t unknown;
    missing_leaves_member_unchanged(999,&key,&unknown,sizeof(unknown));
}

int main(int argc,char **argv)
{
    assert(argc==2);
    if (!strcmp(argv[1],"local-irk")) local_irk_member();
    else if (!strcmp(argv[1],"first-boot")) first_boot_member();
    else if (!strcmp(argv[1],"cccd")) cccd_member();
    else if (!strcmp(argv[1],"csfc")) csfc_member();
    else if (!strcmp(argv[1],"our-sec")) sec_member(BLE_STORE_OBJ_TYPE_OUR_SEC);
    else if (!strcmp(argv[1],"peer-sec")) sec_member(BLE_STORE_OBJ_TYPE_PEER_SEC);
    else if (!strcmp(argv[1],"rpa")) rpa_member();
    else if (!strcmp(argv[1],"missing")) unbonded_and_unknown_members();
    else assert(0);
    printf("BLE_STORE_READ_BOUNDS_PASS %s\n",argv[1]);
    return 0;
}
