/* Real Lua facade -> real ESP Adapter, with deterministic SDK/device peers.
 * This is not a physical IMU or radio test. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "app_runtime.h"
#include "../components/ryz_runtime/lua_hardware_esp.h"
#include "imu.h"
#include "ryz_provisioning.h"
#include "ryz_ble.h"

static ryz_provisioning_snapshot_t network;
static ryz_ble_snapshot_t ble;
static esp_err_t ble_error;
static esp_err_t hid_open_error, hid_tap_error;
static uint32_t hid_owner, hid_next_owner;
static unsigned hid_opens, hid_taps, hid_closes;
static ryz_ble_hid_key_t hid_last_key;
static uint64_t tick;
static bool cancel_after_hid, timeout_after_hid, cancel_now, host_no_hardware;
static esp_err_t network_error, init_error, read_error, stop_error;
static bool ready;
static unsigned starts, reads, stops;
esp_err_t ryz_provisioning_get_snapshot(ryz_provisioning_snapshot_t *out)
{ *out=network; return network_error; }
esp_err_t ryz_ble_get_snapshot(ryz_ble_snapshot_t *out)
{ *out=ble; return ble_error; }
/* BLE owner is a peer of the runtime. Its radio/report timing is covered by
 * the BLE suite; here it exposes deterministic lease/admission outcomes. */
esp_err_t ryz_ble_hid_open(uint32_t *token)
{
    ++hid_opens;
    if (hid_open_error != ESP_OK) return hid_open_error;
    assert(!hid_owner);
    *token=hid_owner=++hid_next_owner;
    return ESP_OK;
}
esp_err_t ryz_ble_hid_tap(uint32_t token, ryz_ble_hid_key_t key)
{
    assert(token && token==hid_owner);
    ++hid_taps; hid_last_key=key;
    if (timeout_after_hid) tick+=100;
    return hid_tap_error;
}
void ryz_ble_hid_close(uint32_t token)
{
    assert(token && token==hid_owner);
    ++hid_closes; hid_owner=0;
}
esp_err_t ryz_imu_init(void) { ++starts; ready=init_error==ESP_OK; return init_error; }
esp_err_t ryz_imu_get_info(ryz_imu_info_t *out)
{ memset(out,0,sizeof(*out)); out->ready=ready; return ESP_OK; }
esp_err_t ryz_imu_read_sample(ryz_imu_sample_t *out)
{
    ++reads; *out=(ryz_imu_sample_t){123.25f,-456.5f,1000.f,UINT64_C(5000000000),42};
    return read_error;
}
esp_err_t ryz_imu_deinit(void) { ++stops; ready=false; return stop_error; }

static bool cancel_after_hardware;
static ryz_lua_hardware_session_t session;
static uint64_t now(void *ctx) { (void)ctx; return tick; }
static void pause_ms(void *ctx,unsigned ms) { (void)ctx; tick+=ms ? ms : 1; }
static void *resize(void *p,size_t n) { if(!n) { free(p); return NULL; } return realloc(p,n); }
static bool cancelled(void *ctx)
{ (void)ctx; return cancel_now || (cancel_after_hardware && starts>0) ||
    (cancel_after_hid && hid_taps>0); }
static void cleanup(void *ctx) { ryz_lua_hardware_esp_cleanup(ctx); }
static void run(const char *body,const char *phase)
{
    char source[4096]; snprintf(source,sizeof(source),"-- ryz-app/1\n%s",body);
    ryz_app_platform_t platform={.context=&session,.resize=resize,.now_ms=now,
        .pause_ms=pause_ms,.cancelled=cancelled,.cleanup=cleanup,
        .hardware_call=host_no_hardware ? NULL : ryz_lua_hardware_esp_call};
    ryz_lua_result_t result;
    ryz_app_execute(source,strlen(source),"@hardware.lua",100,&platform,&result);
    if(strcmp(result.phase,phase)) fprintf(stderr,"%s: %s\nsource: %s\n",result.phase,result.error,body);
    assert(!strcmp(result.phase,phase) && !session.imu_owned && !session.hid_session && !hid_owner);
}

static void reset_hid(void)
{
    assert(!hid_owner);
    hid_opens=hid_taps=hid_closes=0;
    hid_open_error=hid_tap_error=ESP_OK;
    cancel_after_hid=timeout_after_hid=cancel_now=host_no_hardware=false;
    ble_error=ESP_OK;
    ble=(ryz_ble_snapshot_t){.available=true,.linked=true,.authenticated=true,
        .bonded=true,.hid_ready=true};
}

static void test_hid(void)
{
    reset_hid();
    ble.available=false;
    run("local h=require('hid'); local ok,err=h.is_ready(); "
        "assert(ok==false and err=='unavailable'); "
        "assert(h.configure==nil and h.send==nil and h.report==nil)","done");
    run("local ok,err=require('hid').tap('play_pause'); "
        "assert(ok==nil and err=='unavailable')","done");
    assert(!hid_opens && !hid_taps && !hid_closes);

    /* False readiness cannot be overridden by other positive snapshot bits. */
    for (unsigned flags=0;flags<32;++flags) {
        ble=(ryz_ble_snapshot_t){.available=(flags&1)!=0,.linked=(flags&2)!=0,
            .authenticated=(flags&4)!=0,.bonded=(flags&8)!=0,.hid_ready=(flags&16)!=0};
        run(!(flags&1) ?
            "local ok,e=require('hid').is_ready(); assert(ok==false and e=='unavailable')" :
            flags==31 ? "local ok,e=require('hid').is_ready(); assert(ok==true and e==nil)" :
            "local ok,e=require('hid').is_ready(); assert(ok==false and e==nil)","done");
        if ((flags&1) && flags!=31)
            run("local ok,e=require('hid').tap('stop'); assert(ok==nil and e=='not_ready')","done");
    }
    assert(!hid_opens && !hid_taps && !hid_closes); /* Status never acquires input. */
    ble_error=ESP_FAIL;
    run("local h=require('hid'); local ok,e=h.is_ready(); assert(ok==false and e=='unavailable'); "
        "ok,e=h.tap('stop'); assert(ok==nil and e=='unavailable')","done");
    assert(!hid_opens);

    reset_hid();
    static const struct { const char *name; ryz_ble_hid_key_t key; } keys[] = {
        {"play_pause", RYZ_BLE_HID_PLAY_PAUSE}, {"stop", RYZ_BLE_HID_STOP},
        {"next_track", RYZ_BLE_HID_NEXT_TRACK}, {"previous_track", RYZ_BLE_HID_PREVIOUS_TRACK},
        {"mute", RYZ_BLE_HID_MUTE}, {"volume_up", RYZ_BLE_HID_VOLUME_UP},
        {"volume_down", RYZ_BLE_HID_VOLUME_DOWN},
    };
    for (unsigned i=0;i<sizeof(keys)/sizeof(keys[0]);++i) {
        char source[128];
        snprintf(source,sizeof(source),"assert(require('hid').tap('%s'))",keys[i].name);
        run(source,"done");
        assert(hid_opens==i+1 && hid_taps==i+1 && hid_closes==i+1 && hid_last_key==keys[i].key);
    }

    reset_hid();
    run("require('hid').is_ready(true)","runtime");
    run("require('hid').tap()","runtime");
    run("require('hid').tap(1)","runtime");
    run("require('hid').tap({})","runtime");
    run("require('hid').tap('stop',true)","runtime");
    run("require('hid').tap('')","runtime");
    run("require('hid').tap('keyboard')","runtime");
    run("require('hid').tap('STOP')","runtime");
    run("require('hid').tap('stop\\0volume_up')","runtime");
    run("require('hid\\0secret')","runtime");
    assert(!hid_opens && !hid_taps && !hid_closes);

    /* Owner results win even after a positive readiness snapshot. */
    static const struct { esp_err_t error; const char *reason; } failures[] = {
        {ESP_ERR_NOT_SUPPORTED,"unavailable"}, {ESP_ERR_INVALID_STATE,"not_ready"},
        {ESP_ERR_NOT_FINISHED,"busy"}, {ESP_FAIL,"failed"},
    };
    for (unsigned i=0;i<sizeof(failures)/sizeof(failures[0]);++i) {
        char source[160];
        snprintf(source,sizeof(source),"local ok,e=require('hid').tap('mute'); assert(ok==nil and e=='%s')",
            failures[i].reason);
        reset_hid(); hid_open_error=failures[i].error;
        run(source,"done");
        assert(hid_opens==1 && !hid_taps && !hid_closes);
        reset_hid(); hid_tap_error=failures[i].error;
        run(source,"done");
        assert(hid_opens==1 && hid_taps==1 && hid_closes==1);
    }

    reset_hid(); hid_tap_error=ESP_ERR_NOT_FINISHED;
    run("local h=require('hid'); local ok,e=h.tap('mute'); assert(ok==nil and e=='busy'); "
        "ok,e=h.tap('volume_up'); assert(ok==nil and e=='busy')","done");
    assert(hid_opens==1 && hid_taps==2 && hid_closes==1); /* Retry keeps the job lease. */

    reset_hid();
    run("local h=require('hid'); local c=coroutine.create(function() "
        "assert(h.tap('stop')); coroutine.yield(); assert(h.tap('volume_up')) end); "
        "assert(coroutine.resume(c)); assert(h.is_ready()); assert(coroutine.resume(c))","done");
    assert(hid_opens==1 && hid_taps==2 && hid_closes==1 && hid_last_key==RYZ_BLE_HID_VOLUME_UP);
    reset_hid();
    run("assert(require('hid').tap('mute')); error('after admission')","runtime");
    assert(hid_opens==1 && hid_taps==1 && hid_closes==1);
    reset_hid(); cancel_now=true;
    run("require('hid').tap('mute')","stopped");
    assert(!hid_opens && !hid_taps && !hid_closes);
    reset_hid(); cancel_after_hid=true;
    run("require('hid').tap('mute'); error('escaped post-call cancellation')","stopped");
    assert(hid_opens==1 && hid_taps==1 && hid_closes==1);
    reset_hid(); cancel_after_hid=true;
    run("local c=coroutine.create(function() require('hid').tap('mute') end); "
        "coroutine.resume(c); error('swallowed cancellation')","stopped");
    assert(hid_opens==1 && hid_taps==1 && hid_closes==1);
    reset_hid(); timeout_after_hid=true;
    run("require('hid').tap('mute'); error('escaped post-call deadline')","timeout");
    assert(hid_opens==1 && hid_taps==1 && hid_closes==1);
    reset_hid();
    run("assert(require('hid').tap('stop')); local t={}; "
        "while true do t[#t+1]=string.rep('x',1024) end","memory");
    assert(hid_opens==1 && hid_taps==1 && hid_closes==1);
    reset_hid(); host_no_hardware=true;
    run("local h=require('hid'); local ok,e=h.is_ready(); assert(ok==false and e=='unavailable'); "
        "ok,e=h.tap('stop'); assert(ok==nil and e=='unavailable')","done");
    assert(!hid_opens && !hid_taps && !hid_closes);
    reset_hid();
    ryz_lua_hardware_esp_cleanup(&session); /* Empty and repeated cleanup is safe. */
    ryz_lua_hardware_esp_cleanup(&session);
    assert(!hid_closes);
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1],"--wait-for-start")) {
        /* Separate loader/sanitizer startup from the strict Lua suite timeout.
         * No test or native peer runs before the parent explicitly sends GO. */
        puts("LUA_HARDWARE_READY");
        fflush(stdout);
        if (getchar() != '\n') return 2;
    } else if (argc != 1) return 2;
    network_error=ESP_ERR_INVALID_STATE;
    run("local ok,err=require('wifi').is_connected(); assert(not ok and err=='unavailable'); "
        "local ok,err=require('ble').is_connected(); assert(not ok and err=='unavailable')","done");
    network_error=ESP_OK;
    for(unsigned state=RYZ_PROVISIONING_UNCONFIGURED;state<=RYZ_PROVISIONING_OFF;++state) {
        network=(ryz_provisioning_snapshot_t){.state=state,.enabled=true};
        strcpy(network.ipv4,"192.0.2.2");
        run(state==RYZ_PROVISIONING_ONLINE ?
            "local ok,err=require('wifi').is_connected(); assert(ok and err==nil)" :
            "local ok,err=require('wifi').is_connected(); assert(not ok and err==nil)","done");
    }
    network.state=RYZ_PROVISIONING_ONLINE; network.enabled=false;
    run("assert(not require('wifi').is_connected())","done");
    network.enabled=true; network.ipv4[0]=0;
    run("assert(not require('wifi').is_connected())","done");
    /* Real Lua facade and ESP Adapter, with all boolean combinations. Saved
     * credentials, GAP link, encryption or a UI phase alone cannot grant true. */
    for (unsigned flags=0; flags<16; ++flags) {
        ble=(ryz_ble_snapshot_t){.available=(flags&1)!=0,.linked=(flags&2)!=0,
            .authenticated=(flags&4)!=0,.bonded=(flags&8)!=0};
        run(!(flags&1) ?
            "local ok,err=require('ble').is_connected(); assert(not ok and err=='unavailable')" :
            flags==15 ? "local ok,err=require('ble').is_connected(); assert(ok and err==nil)" :
            "local ok,err=require('ble').is_connected(); assert(not ok and err==nil)","done");
    }
    ble_error=ESP_FAIL; /* Dirty positive payload accompanying a failed read. */
    run("local ok,err=require('ble').is_connected(); assert(not ok and err=='unavailable')","done");
    ble_error=ESP_OK;
    run("local b=require('ble'); assert(b.scan==nil and b.connect==nil and b.disconnect==nil "
        "and b.pair==nil and b.configure==nil and b.send==nil and b.receive==nil)","done");
    test_hid();
    run("local sample,err=require('imu').read(); assert(sample==nil and err=='not_initialized')","done");
    assert(!starts && !reads && !stops);
    run("local imu=require('imu'); assert(imu.init()); assert(imu.init()); "
        "local s=assert(imu.read()); assert(s.x_mg==123.25 and s.y_mg==-456.5 and s.z_mg==1000); "
        "assert(s.sequence=='42' and s.timestamp_us=='5000000000'); "
        "assert(imu.deinit()); assert(imu.deinit())","done");
    assert(starts==1 && reads==1 && stops==1);
    read_error=ESP_ERR_NOT_FINISHED;
    run("local imu=require('imu'); assert(imu.init()); local s,err=imu.read(); "
        "assert(s==nil and err=='not_ready')","done");
    assert(starts==2 && stops==2);
    read_error=ESP_FAIL;
    run("local imu=require('imu'); assert(imu.init()); local s,err=imu.read(); "
        "assert(s==nil and err=='failed'); error('expected')","runtime");
    assert(stops==3);
    init_error=ESP_FAIL;
    run("local s,err=require('imu').init(); assert(s==nil and err=='failed')","done");
    assert(stops==3);
    init_error=ESP_OK; starts=0; cancel_after_hardware=true;
    run("require('imu').init(); error('ESCAPED')","stopped");
    assert(starts==1 && stops==4);
    cancel_after_hardware=false;
    run("require('wifi').is_connected(true)","runtime");
    run("require('ble').is_connected(true)","runtime");
    run("require('imu').init({hz=100})","runtime");
    run("require('wifi\\0secret')","runtime");
    puts("LUA_HARDWARE_PASS: connectivity, opt-in IMU, HID admission/leases, strict inputs, cancellation and cleanup");
    return 0;
}
