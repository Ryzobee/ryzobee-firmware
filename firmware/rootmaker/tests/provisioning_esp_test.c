#define _POSIX_C_SOURCE 200809L
#include "sdk.h"
#include "nvs.h"
#include "ryz_provisioning.h"
#include "ryz_provisioning_platform.h"
#include "ryz_net_traffic.h"
#include "ryz_captive_dns.h"
#include "cJSON.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Production core and ESP Adapter, entered only through public commands and
 * callbacks captured at SDK registration. External SDK/OS/NVS transports are
 * finite test Adapters; no socket/radio, no replacement worker or snapshots. */
static pthread_mutex_t schedule = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
struct fake_task {
    pthread_t thread;
    TaskFunction_t entry;
    void *argument;
    bool live, quit;
    unsigned permits, waits, notifications;
};
struct fake_mutex { pthread_mutex_t mutex; bool live; };
struct fake_queue { bool live, occupied; size_t size; uint8_t bytes[256]; };
struct fake_netif { bool live, ap, up; esp_netif_ip_info_t ip; };
struct fake_handler { bool live; esp_event_base_t base; int32_t id; esp_event_handler_t callback; void *context; };
struct fake_http { bool live; unsigned count; httpd_uri_t handlers[4]; };
struct ryz_captive_dns { bool live; };
static struct fake_task worker;
static struct fake_mutex network_mutex;
static struct fake_queue connect_queue;
static struct fake_netif ap_netif, sta_netif;
static struct fake_handler handlers[2];
static struct fake_http servers[8];
static struct ryz_captive_dns dns_servers[8];
static unsigned server_count, dns_count, stop_calls, wifi_starts, wifi_stops, connects;
static httpd_handle_t stopped_handles[16];
static TickType_t clock_tick;
static wifi_mode_t wifi_mode;
static bool wifi_live, wifi_running, associated;
static wifi_config_t station_config, portal_config;
static wifi_ap_record_t live_ap;
static unsigned scans, scan_reads, scan_clears, scan_stops;
static esp_err_t scan_error, scan_read_error;
static unsigned scan_variant; /* 0 exact, 1 absent, 2 different case, 3 prefix, 4 hidden empty, 5 wrong band */
static char scanned_ssid[33];
static esp_err_t http_stop_error, wifi_stop_error, dns_stop_error;
static esp_err_t wifi_start_error, http_start_error, dns_start_error;
static bool block_http_stop, http_stop_entered, release_http_stop;
static bool block_clock, clock_entered, release_clock;
static esp_err_t ap_info_error, ip_info_error, mac_error;
static esp_err_t global6_error = ESP_FAIL, local6_error = ESP_FAIL;
static esp_ip6_addr_t global6, local6;
static unsigned diagnostic_sdk_calls, monotonic_reads, nvs_reads;
static TickType_t last_wait_ticks;
static bool disconnect_during_mac;
static bool roam_during_mac;
static bool event_during_mac;
static void deliver_disconnect(const char *ssid, uint8_t reason);
static bool fast_post_on_unlock, fast_post_seen, boot_fast_post, fast_worker_on_post;
static void post_after_portal_unlock(void);

typedef struct {
    uint8_t credentials[256]; size_t length; char ap_password[64];
    bool wifi_on_present;
    uint8_t wifi_on;
} stored_t;
struct fake_nvs {
    bool live, write, credentials_dirty, ap_password_dirty, enabled_dirty;
    stored_t data;
};
static struct fake_nvs nvs_handles[8];
static stored_t persisted;
static unsigned credential_writes, credential_erases, commits;
static esp_err_t nvs_open_error, nvs_set_error, nvs_commit_error, nvs_erase_error;
static unsigned nvs_closes, enabled_reads, enabled_writes;
static esp_err_t enabled_get_error, enabled_set_error;
static bool commit_then_error;
static int expected_radio_preference=-1;

/* On macOS the GNU linker does not rewrite these calls. Enter the real wrap
 * functions explicitly, with external SDK delegates preserving arguments and
 * selectable results. Payload contents never enter telemetry or test output. */
esp_err_t __wrap_esp_netif_transmit(esp_netif_t *, void *, size_t);
esp_err_t __wrap_esp_netif_transmit_wrap(esp_netif_t *, void *, size_t, void *);
esp_err_t __wrap_esp_netif_receive(esp_netif_t *, void *, size_t, void *);
static uint8_t traffic_payload[1500], traffic_context;
static esp_err_t traffic_result[3];
static unsigned traffic_calls[3];

static esp_err_t traffic_delegate(unsigned kind, esp_netif_t *netif,
                                   void *data, size_t length, void *context)
{
    assert(kind<3 && (netif==&sta_netif || netif==&ap_netif) && netif->live);
    assert(data==traffic_payload && length<=sizeof(traffic_payload));
    assert(context==(kind ? &traffic_context : NULL));
    ++traffic_calls[kind];
    return traffic_result[kind];
}

esp_err_t __real_esp_netif_transmit(esp_netif_t *netif, void *data, size_t length)
{ return traffic_delegate(0,netif,data,length,NULL); }
esp_err_t __real_esp_netif_transmit_wrap(esp_netif_t *netif, void *data, size_t length, void *pbuf)
{ return traffic_delegate(1,netif,data,length,pbuf); }
esp_err_t __real_esp_netif_receive(esp_netif_t *netif, void *data, size_t length, void *buffer)
{ return traffic_delegate(2,netif,data,length,buffer); }

static void traffic_packet(esp_netif_t *netif, unsigned kind, size_t length, esp_err_t result)
{
    assert(kind<3);
    unsigned calls=traffic_calls[kind];
    traffic_result[kind]=result;
    esp_err_t actual=kind==0 ? __wrap_esp_netif_transmit(netif,traffic_payload,length) :
        kind==1 ? __wrap_esp_netif_transmit_wrap(netif,traffic_payload,length,&traffic_context) :
                  __wrap_esp_netif_receive(netif,traffic_payload,length,&traffic_context);
    assert(actual==result && traffic_calls[kind]==calls+1);
    traffic_result[kind]=ESP_OK;
}

const char PROV_WIFI_EVENT[] = "WIFI_EVENT";
const char PROV_IP_EVENT[] = "IP_EVENT";

static void wait_changed(void)
{
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME,&deadline)==0); deadline.tv_sec+=3;
    assert(pthread_cond_timedwait(&changed,&schedule,&deadline)==0);
}
static uint32_t worker_barrier(void)
{
    assert(pthread_mutex_lock(&schedule)==0);
    ++worker.waits;
    assert(pthread_cond_broadcast(&changed)==0);
    while (!worker.permits && !worker.quit) wait_changed();
    if (worker.quit) { assert(pthread_mutex_unlock(&schedule)==0); pthread_exit(NULL); }
    --worker.permits;
    uint32_t notifications=worker.notifications; worker.notifications=0;
    assert(pthread_mutex_unlock(&schedule)==0);
    return notifications;
}
static void *worker_entry(void *unused)
{ (void)unused; (void)worker_barrier(); worker.entry(worker.argument); abort(); }
static void step_worker(TickType_t now)
{
    assert(pthread_mutex_lock(&schedule)==0);
    assert(worker.live && !worker.permits);
    clock_tick=now;
    unsigned target=worker.waits+1;
    ++worker.permits;
    assert(pthread_cond_broadcast(&changed)==0);
    while (worker.waits<target) wait_changed();
    assert(pthread_mutex_unlock(&schedule)==0);
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    assert(!network_mutex.live);
    assert(pthread_mutex_init(&network_mutex.mutex,NULL)==0);
    network_mutex.live=true; return &network_mutex;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t ticks)
{
    assert(mutex && mutex->live);
    if (!ticks) {
        int result=pthread_mutex_trylock(&mutex->mutex);
        assert(result==0 || result==EBUSY); return result==0 ? pdTRUE : pdFALSE;
    }
    assert(ticks==portMAX_DELAY); assert(pthread_mutex_lock(&mutex->mutex)==0); return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex)
{
    assert(mutex && mutex->live); assert(pthread_mutex_unlock(&mutex->mutex)==0);
    if (fast_post_on_unlock) post_after_portal_unlock();
    return pdTRUE;
}
void vSemaphoreDelete(SemaphoreHandle_t mutex)
{ assert(mutex && mutex->live); assert(pthread_mutex_destroy(&mutex->mutex)==0); mutex->live=false; }
QueueHandle_t xQueueCreate(UBaseType_t count, UBaseType_t size)
{
    assert(count==1 && size && size<=sizeof(connect_queue.bytes) && !connect_queue.live);
    connect_queue=(struct fake_queue){.live=true,.size=size}; return &connect_queue;
}
BaseType_t xQueueOverwrite(QueueHandle_t queue, const void *item)
{
    assert(queue && queue->live && item);
    assert(pthread_mutex_lock(&schedule)==0);
    memcpy(queue->bytes,item,queue->size); queue->occupied=true;
    assert(pthread_mutex_unlock(&schedule)==0); return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks)
{
    assert(queue && queue->live && item && !ticks);
    assert(pthread_mutex_lock(&schedule)==0);
    bool present=queue->occupied;
    if (present) { memcpy(item,queue->bytes,queue->size); queue->occupied=false; }
    assert(pthread_mutex_unlock(&schedule)==0); return present ? pdTRUE : pdFALSE;
}
void vQueueDelete(QueueHandle_t queue) { assert(queue && queue->live); queue->live=false; }
BaseType_t xTaskCreate(TaskFunction_t entry, const char *name, uint32_t stack,
                      void *argument, UBaseType_t priority, TaskHandle_t *out)
{
    assert(entry && !strcmp(name,"ryz_prov") && stack==4096 && !argument && priority==5 && out && !worker.live);
    worker=(struct fake_task){.live=true,.entry=entry,.argument=argument};
    *out=&worker;
    assert(pthread_create(&worker.thread,NULL,worker_entry,NULL)==0);
    assert(pthread_mutex_lock(&schedule)==0);
    while (!worker.waits) wait_changed();
    assert(pthread_mutex_unlock(&schedule)==0); return pdPASS;
}
void vTaskDelete(TaskHandle_t task)
{
    assert(task==&worker && task->live);
    assert(pthread_mutex_lock(&schedule)==0); task->quit=true;
    assert(pthread_cond_broadcast(&changed)==0); assert(pthread_mutex_unlock(&schedule)==0);
    assert(pthread_join(task->thread,NULL)==0); task->live=false;
}
TickType_t xTaskGetTickCount(void)
{
    assert(pthread_mutex_lock(&schedule)==0);
    if (block_clock && pthread_equal(pthread_self(),worker.thread)) {
        clock_entered=true; assert(pthread_cond_broadcast(&changed)==0);
        while (!release_clock) wait_changed();
        block_clock=false;
    }
    TickType_t value=clock_tick;
    assert(pthread_mutex_unlock(&schedule)==0); return value;
}
void xTaskNotifyGive(TaskHandle_t task)
{
    assert(task==&worker && task->live);
    assert(pthread_mutex_lock(&schedule)==0); ++task->notifications;
    assert(pthread_cond_broadcast(&changed)==0); assert(pthread_mutex_unlock(&schedule)==0);
}
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks)
{ last_wait_ticks=ticks; assert(clear==pdTRUE && pthread_equal(pthread_self(),worker.thread)); return worker_barrier(); }

int64_t esp_timer_get_time(void)
{
    assert(pthread_mutex_lock(&schedule)==0);
    ++monotonic_reads;
    int64_t result=(int64_t)clock_tick*1000;
    assert(pthread_mutex_unlock(&schedule)==0);
    return result;
}

const char *esp_err_to_name(esp_err_t error) { return error==ESP_OK ? "ESP_OK" : "SDK_TEST_ERROR"; }
void prov_test_log(const char *tag, const char *format, ...)
{
    assert(tag && format); char line[512]; va_list args; va_start(args,format);
    vsnprintf(line,sizeof(line),format,args); va_end(args);
    assert(!strstr(line,"test-only-pass") && !strstr(line,"second-pass8"));
}
esp_err_t esp_read_mac(uint8_t *out, int type)
{ assert(out && type==ESP_MAC_WIFI_STA); const uint8_t mac[]={2,0,0,0,0x12,0xab}; memcpy(out,mac,sizeof(mac)); return ESP_OK; }
void esp_fill_random(void *out, size_t length) { memset(out,3,length); }
char *inet_ntoa_r(uint32_t address, char *buffer, int length)
{ assert(length>0); return (char *)inet_ntop(AF_INET,&address,buffer,(socklen_t)length); }
char *inet6_ntoa_r(esp_ip6_addr_t address, char *buffer, int length)
{ assert(length>0); return (char *)inet_ntop(AF_INET6,address.addr,buffer,(socklen_t)length); }
esp_err_t esp_netif_init(void) { return ESP_OK; }
esp_netif_t *esp_netif_new(const esp_netif_config_t *config)
{
    assert(config); esp_netif_t *value=config->ap ? &ap_netif : &sta_netif; assert(!value->live);
    *value=(struct fake_netif){.live=true,.ap=config->ap};
    if (value->ap) value->ip.ip.addr=inet_addr("192.168.4.1");
    return value;
}
void esp_netif_destroy(esp_netif_t *netif) { assert(netif && netif->live); netif->live=false; }
void esp_netif_destroy_default_wifi(esp_netif_t *netif) { esp_netif_destroy(netif); }
esp_err_t esp_netif_attach_wifi_ap(esp_netif_t *netif) { assert(netif==&ap_netif); return ESP_OK; }
esp_err_t esp_netif_attach_wifi_station(esp_netif_t *netif) { assert(netif==&sta_netif); return ESP_OK; }
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *out)
{ assert(netif && netif->live && out); ++diagnostic_sdk_calls; *out=netif->ip; return netif==&sta_netif?ip_info_error:ESP_OK; }
bool esp_netif_is_netif_up(esp_netif_t *netif) { assert(netif && netif->live); return netif->up; }
esp_err_t esp_netif_get_ip6_global(esp_netif_t *netif, esp_ip6_addr_t *out)
{ assert(netif==&sta_netif && netif->live && out); ++diagnostic_sdk_calls; *out=global6; return global6_error; }
esp_err_t esp_netif_get_ip6_linklocal(esp_netif_t *netif, esp_ip6_addr_t *out)
{ assert(netif==&sta_netif && netif->live && out); ++diagnostic_sdk_calls; *out=local6; return local6_error; }
esp_err_t esp_netif_dhcps_stop(esp_netif_t *netif) { assert(netif==&ap_netif); return ESP_OK; }
esp_err_t esp_netif_dhcps_start(esp_netif_t *netif) { assert(netif==&ap_netif); return ESP_OK; }
esp_err_t esp_netif_dhcps_option(esp_netif_t *netif, int op, int option, void *value, uint32_t length)
{ assert(netif==&ap_netif && op==ESP_NETIF_OP_SET && option==ESP_NETIF_CAPTIVEPORTAL_URI && value && length); return ESP_OK; }
esp_err_t esp_event_loop_create_default(void) { return ESP_OK; }
esp_err_t esp_event_loop_delete_default(void) { return ESP_OK; }
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
    esp_event_handler_t callback, void *context, esp_event_handler_instance_t *out)
{
    struct fake_handler *value=base==WIFI_EVENT ? &handlers[0] : &handlers[1];
    assert(!value->live && callback && out && ((base==WIFI_EVENT && id==ESP_EVENT_ANY_ID) || (base==IP_EVENT && id==IP_EVENT_STA_GOT_IP)));
    *value=(struct fake_handler){true,base,id,callback,context}; *out=value; return ESP_OK;
}
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t base, int32_t id, esp_event_handler_instance_t instance)
{ assert(instance && instance->live && instance->base==base && instance->id==id); instance->live=false; return ESP_OK; }
esp_err_t esp_wifi_init(const wifi_init_config_t *config) { assert(config && !wifi_live); wifi_live=true; return ESP_OK; }
esp_err_t esp_wifi_deinit(void) { assert(wifi_live && !wifi_running); wifi_live=false; return ESP_OK; }
esp_err_t esp_wifi_set_storage(int storage) { assert(storage==WIFI_STORAGE_RAM); return ESP_OK; }
esp_err_t esp_wifi_set_mode(wifi_mode_t mode) { assert(wifi_live && !wifi_running); wifi_mode=mode; return ESP_OK; }
esp_err_t esp_wifi_set_config(int interface, const wifi_config_t *config)
{ assert(config && (interface==WIFI_IF_STA || interface==WIFI_IF_AP)); if (interface==WIFI_IF_STA) station_config=*config; else portal_config=*config; return ESP_OK; }
esp_err_t esp_wifi_start(void)
{
    assert(wifi_live && !wifi_running);
    if (expected_radio_preference>=0)
        assert(persisted.wifi_on_present && persisted.wifi_on==expected_radio_preference);
    if (wifi_start_error) return wifi_start_error;
    wifi_running=true; ++wifi_starts;
    if (wifi_mode==WIFI_MODE_AP) ap_netif.up=true;
    return ESP_OK;
}
esp_err_t esp_wifi_stop(void)
{
    if (expected_radio_preference>=0)
        assert(persisted.wifi_on_present && persisted.wifi_on==expected_radio_preference);
    ++wifi_stops; if (wifi_stop_error) return wifi_stop_error;
    if (!wifi_running) return ESP_ERR_WIFI_NOT_STARTED;
    wifi_running=false; associated=false; ap_netif.up=sta_netif.up=false;
    sta_netif.ip.ip.addr=0; return ESP_OK;
}
esp_err_t esp_wifi_connect(void) { assert(wifi_running && wifi_mode==WIFI_MODE_STA); ++connects; return ESP_OK; }
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool block)
{
    assert(wifi_running && wifi_mode==WIFI_MODE_STA && !associated);
    assert(config && config->ssid && block && config->show_hidden);
    assert(config->scan_type==WIFI_SCAN_TYPE_ACTIVE && config->scan_time.active.max==100);
    assert(config->channel_bitmap.ghz_2_channels==0x7FFE && !config->channel_bitmap.ghz_5_channels);
    assert(!memcmp(config->ssid, station_config.sta.ssid, strlen((const char *)config->ssid)));
    ryz_provisioning_snapshot_t view;
    assert(ryz_provisioning_get_snapshot(&view)==ESP_OK);
    assert(view.boot_phase==RYZ_PROVISIONING_BOOT_SCANNING && view.boot_restore_pending);
    assert(!connects); /* No association attempt before the scan succeeds. */
    snprintf(scanned_ssid,sizeof(scanned_ssid),"%s",config->ssid);
    ++scans;
    return scan_error;
}
esp_err_t esp_wifi_scan_stop(void) { ++scan_stops; return ESP_OK; }
esp_err_t esp_wifi_clear_ap_list(void) { ++scan_clears; return ESP_OK; }
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *count, wifi_ap_record_t *records)
{
    assert(scans && count && *count==1 && records);
    ++scan_reads;
    if(scan_read_error) return scan_read_error;
    *count=scan_variant==1 ? 0 : 1;
    memset(records,0,sizeof(*records));
    memcpy(records->ssid,scanned_ssid,sizeof(scanned_ssid));
    records->primary=scan_variant==5 ? 36 : 6;
    if(scan_variant==2) records->ssid[0]^=32;
    if(scan_variant==3) records->ssid[strlen(scanned_ssid)-1]=0;
    if(scan_variant==4) records->ssid[0]=0;
    return ESP_OK;
}
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *out)
{ assert(out); ++diagnostic_sdk_calls; if(ap_info_error) return ap_info_error; if (!associated) return ESP_ERR_WIFI_NOT_CONNECT; *out=live_ap; return ESP_OK; }
esp_err_t esp_wifi_get_mac(int interface, uint8_t mac[6])
{
    assert(interface==WIFI_IF_STA && mac && wifi_running);
    ++diagnostic_sdk_calls;
    const uint8_t actual[]={2,0x12,0x34,0x56,0x78,0x9a};
    memcpy(mac,actual,sizeof(actual));
    if(disconnect_during_mac) { associated=false; sta_netif.up=false; disconnect_during_mac=false; }
    if(roam_during_mac) { live_ap.bssid[0]^=1; roam_during_mac=false; }
    if(event_during_mac) { event_during_mac=false; deliver_disconnect("HOST-NET",1); }
    return mac_error;
}
esp_err_t esp_wifi_set_default_wifi_ap_handlers(void) { return ESP_OK; }
esp_err_t esp_wifi_set_default_wifi_sta_handlers(void) { return ESP_OK; }
esp_err_t esp_wifi_clear_default_wifi_driver_and_handlers(esp_netif_t *netif) { assert(netif); return ESP_OK; }

/* Finite NVS commit model; failures normally occur before commit, with one
 * explicit commit-then-error injection. Neither simulates flash/power-cut
 * atomicity. Re-reading uses the real production NVS decoder. */
esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *out)
{
    ++nvs_reads;
    assert(!strcmp(name,"ryz_prov") && out && (mode==NVS_READONLY || mode==NVS_READWRITE));
    if (nvs_open_error) return nvs_open_error;
    for (unsigned i=0;i<8;++i) if (!nvs_handles[i].live) {
        nvs_handles[i]=(struct fake_nvs){.live=true,.write=mode==NVS_READWRITE,.data=persisted}; *out=&nvs_handles[i]; return ESP_OK;
    }
    abort();
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *length)
{
    assert(handle && handle->live && !strcmp(key,"sta_cred") && out && length);
    if (!handle->data.length) return ESP_ERR_NVS_NOT_FOUND;
    if (*length<handle->data.length) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out,handle->data.credentials,handle->data.length); *length=handle->data.length; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t length)
{
    assert(handle && handle->live && handle->write && !strcmp(key,"sta_cred") && data && length<=sizeof(persisted.credentials));
    ++credential_writes; if (nvs_set_error) return nvs_set_error;
    memcpy(handle->data.credentials,data,length); handle->data.length=length;
    handle->credentials_dirty=true; return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out, size_t *length)
{
    assert(handle && handle->live && !strcmp(key,"ap_pass") && out && length);
    if (!handle->data.ap_password[0]) return ESP_ERR_NVS_NOT_FOUND;
    size_t required=strlen(handle->data.ap_password)+1;
    if (*length<required) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out,handle->data.ap_password,required); *length=required; return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *data)
{ assert(handle && handle->live && handle->write && !strcmp(key,"ap_pass") && strlen(data)<sizeof(persisted.ap_password)); strcpy(handle->data.ap_password,data); handle->ap_password_dirty=true; return ESP_OK; }
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out)
{
    assert(handle && handle->live && !handle->write && !strcmp(key,"wifi_on") && out);
    ++enabled_reads;
    if (enabled_get_error) { *out=0xff; return enabled_get_error; }
    if (!handle->data.wifi_on_present) return ESP_ERR_NVS_NOT_FOUND;
    *out=handle->data.wifi_on; return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    assert(handle && handle->live && handle->write && !strcmp(key,"wifi_on"));
    assert(value<=1);
    ++enabled_writes;
    if (enabled_set_error) return enabled_set_error;
    handle->data.wifi_on=value; handle->data.wifi_on_present=true;
    handle->enabled_dirty=true; return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    assert(handle && handle->live && handle->write && !strcmp(key,"sta_cred"));
    ++credential_erases; if (nvs_erase_error) return nvs_erase_error;
    handle->data.length=0; memset(handle->data.credentials,0,sizeof(handle->data.credentials));
    handle->credentials_dirty=true; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle && handle->live && handle->write);
    ++commits;
    if (nvs_commit_error && !commit_then_error) return nvs_commit_error;
    /* Staged writes belong to this handle/key. This models successful commit,
     * not real NVS power-cut atomicity or durability. */
    if (handle->credentials_dirty) {
        memcpy(persisted.credentials,handle->data.credentials,sizeof(persisted.credentials));
        persisted.length=handle->data.length;
    }
    if (handle->ap_password_dirty)
        memcpy(persisted.ap_password,handle->data.ap_password,sizeof(persisted.ap_password));
    if (handle->enabled_dirty) {
        persisted.wifi_on=handle->data.wifi_on;
        persisted.wifi_on_present=handle->data.wifi_on_present;
    }
    handle->credentials_dirty=handle->ap_password_dirty=handle->enabled_dirty=false;
    return nvs_commit_error;
}
void nvs_close(nvs_handle_t handle)
{ assert(handle && handle->live); ++nvs_closes; handle->live=false; }

esp_err_t httpd_start(httpd_handle_t *out, const httpd_config_t *config)
{
    assert(out && config && config->max_open_sockets==4 && config->max_uri_handlers==4 && config->lru_purge_enable);
    if (http_start_error) { *out=NULL; return http_start_error; }
    assert(server_count<8); servers[server_count]=(struct fake_http){.live=true}; *out=&servers[server_count++]; return ESP_OK;
}
esp_err_t httpd_stop(httpd_handle_t handle)
{
    assert(handle && handle->live && stop_calls<16); stopped_handles[stop_calls++]=handle;
    assert(pthread_mutex_lock(&schedule)==0); http_stop_entered=true;
    assert(pthread_cond_broadcast(&changed)==0);
    while (block_http_stop && !release_http_stop) wait_changed();
    assert(pthread_mutex_unlock(&schedule)==0);
    if (http_stop_error) return http_stop_error; /* SDK keeps the handle live. */
    handle->live=false; return ESP_OK;
}
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri)
{ assert(handle && handle->live && uri && uri->handler && handle->count<4); handle->handlers[handle->count++]=*uri; return ESP_OK; }
esp_err_t httpd_register_err_handler(httpd_handle_t handle, httpd_err_code_t code, esp_err_t (*callback)(httpd_req_t *,httpd_err_code_t))
{ assert(handle && handle->live && code==HTTPD_404_NOT_FOUND && callback); return ESP_OK; }
int httpd_req_recv(httpd_req_t *request, char *out, size_t length)
{
    assert(request && out && request->consumed+length<=(size_t)request->content_len);
    memcpy(out,request->body+request->consumed,length); request->consumed+=length; return (int)length;
}
esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status)
{ assert(strlen(status)<sizeof(request->status)); strcpy(request->status,status); return ESP_OK; }
esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type) { assert(request && type); return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *key, const char *value) { assert(request && key && value); return ESP_OK; }
esp_err_t httpd_resp_send(httpd_req_t *request, const char *data, ssize_t length)
{
    assert(request && data); size_t count=length==HTTPD_RESP_USE_STRLEN ? strlen(data) : (size_t)length;
    assert(count<sizeof(request->response)); memcpy(request->response,data,count); request->response[count]=0; return ESP_OK;
}
esp_err_t httpd_resp_send_err(httpd_req_t *request, httpd_err_code_t code, const char *message)
{ snprintf(request->status,sizeof(request->status),"%d",code); return httpd_resp_send(request,message,HTTPD_RESP_USE_STRLEN); }
esp_err_t ryz_captive_dns_start(esp_netif_t *netif, ryz_captive_dns_handle_t *out)
{ assert(netif==&ap_netif && out && dns_count<8); if (dns_start_error) { *out=NULL; return dns_start_error; } dns_servers[dns_count].live=true; *out=&dns_servers[dns_count++]; return ESP_OK; }
esp_err_t ryz_captive_dns_stop(ryz_captive_dns_handle_t handle)
{ if (!handle) return ESP_OK; assert(handle->live); if (dns_stop_error) return dns_stop_error; handle->live=false; return ESP_OK; }

static httpd_req_t request(httpd_handle_t server, const char *path, const char *body)
{
    assert(server && server->live);
    httpd_req_t value={.body=body,.content_len=body ? (int)strlen(body) : 0};
    for (unsigned i=0;i<server->count;++i) if (!strcmp(server->handlers[i].uri,path)) {
        assert(server->handlers[i].handler(&value)==ESP_OK); return value;
    }
    abort();
}

static void post_after_portal_unlock(void)
{
    if (!wifi_running || wifi_mode!=WIFI_MODE_AP || !server_count) return;
    httpd_handle_t server=&servers[server_count-1];
    if (!server->live || server->count!=4) return;
    /* A real HTTP task can run immediately after the platform releases the
     * network mutex, before the core's ready publication. Invoke only the
     * SDK-registered handler here; never synthesize a core event/snapshot. */
    fast_post_on_unlock=false;
    ryz_provisioning_snapshot_t before;
    assert(ryz_provisioning_get_snapshot(&before)==ESP_OK);
    assert(before.state==(boot_fast_post ? RYZ_PROVISIONING_AP_READY : RYZ_PROVISIONING_AP_STARTING));
    httpd_req_t posted=request(server,"/api/provision","{\"ssid\":\"NEXT-NET\",\"password\":\"second-pass8\"}");
    assert(!strcmp(posted.status,"202 Accepted"));
    cJSON *json=cJSON_Parse(posted.response);
    assert(json && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json,"accepted")));
    cJSON_Delete(json);
    fast_post_seen=true;
    if (fast_worker_on_post) { step_worker(0); step_worker(250); }
}

static void start_portal(void)
{
    assert(ryz_provisioning_init()==ESP_OK);
    assert(ryz_provisioning_start()==ESP_OK);
    assert(worker.live && handlers[0].live && handlers[1].live && server_count==1);
    assert(servers[0].live && servers[0].count==4 && wifi_running && wifi_mode==WIFI_MODE_AP);
    assert(ryz_provisioning_diagnostics_ready());
    httpd_req_t status=request(&servers[0],"/api/status",NULL);
    cJSON *json=cJSON_Parse(status.response); assert(json);
    assert(!cJSON_HasObjectItem(json,"ap_password") && !cJSON_HasObjectItem(json,"password")); cJSON_Delete(json);
}
static void http_stop_failure(void)
{
    start_portal(); step_worker(0);
    ryz_provisioning_credentials_t credentials={.ssid="HOST-NET",.password="test-only-pass"};
    http_stop_error=ESP_FAIL;
    esp_err_t error=ryz_provisioning_platform_start_sta(&credentials);
    assert(error==ESP_FAIL); /* RED: old Adapter swallowed stop failure. */
    assert(servers[0].live && stop_calls==1 && stopped_handles[0]==&servers[0]);
    assert(!connects && wifi_starts==1);
    http_stop_error=ESP_OK;
    assert(ryz_provisioning_platform_start_sta(&credentials)==ESP_OK);
    assert(stop_calls==2 && stopped_handles[1]==&servers[0] && !servers[0].live && connects==1);
}

static ryz_provisioning_snapshot_t snapshot(void)
{
    ryz_provisioning_snapshot_t out;
    assert(ryz_provisioning_get_snapshot(&out)==ESP_OK); return out;
}
static void associate(const char *ssid, const char *ip)
{
    assert(wifi_running && wifi_mode==WIFI_MODE_STA && strlen(ssid)<sizeof(live_ap.ssid));
    associated=true; sta_netif.up=true; sta_netif.ip.ip.addr=inet_addr(ip);
    memset(&live_ap,0,sizeof(live_ap)); memcpy(live_ap.ssid,ssid,strlen(ssid));
}
static void deliver_ip(esp_netif_t *netif, const char *ip)
{
    ip_event_got_ip_t event={.esp_netif=netif}; event.ip_info.ip.addr=inet_addr(ip);
    assert(handlers[1].live);
    handlers[1].callback(handlers[1].context,IP_EVENT,IP_EVENT_STA_GOT_IP,&event);
}
static void deliver_disconnect(const char *ssid, uint8_t reason)
{
    wifi_event_sta_disconnected_t event={.reason=reason};
    assert(strlen(ssid)<=sizeof(event.ssid)); event.ssid_len=(uint8_t)strlen(ssid);
    memcpy(event.ssid,ssid,event.ssid_len);
    assert(handlers[0].live);
    handlers[0].callback(handlers[0].context,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&event);
}
static void post_credentials(httpd_handle_t server)
{
    httpd_req_t posted=request(server,"/api/provision","{\"ssid\":\"HOST-NET\",\"password\":\"test-only-pass\"}");
    assert(!strcmp(posted.status,"202 Accepted"));
    cJSON *json=cJSON_Parse(posted.response); assert(json && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json,"accepted")));
    cJSON_Delete(json);
}
static void expect_credentials(void)
{
    ryz_provisioning_credentials_t out;
    assert(ryz_provisioning_platform_load_credentials(&out)==ESP_OK);
    assert(!strcmp(out.ssid,"HOST-NET") && !strcmp(out.password,"test-only-pass"));
}
static void stop_preserves_credentials(void)
{
    start_portal(); post_credentials(&servers[0]);
    assert(snapshot().credentials_stored && !connects && credential_writes==1);
    char ap_password[64]; strcpy(ap_password,persisted.ap_password);
    step_worker(0); /* The real worker has copied the delayed credential job. */
    assert(!connects);
    assert(ryz_provisioning_set_enabled(false)==ESP_OK);
    ryz_provisioning_snapshot_t off=snapshot();
    assert(off.state==RYZ_PROVISIONING_OFF && !off.enabled && off.stop_confirmed && !off.portal_active && !off.ipv4[0]);
    expect_credentials(); assert(!credential_erases && !strcmp(persisted.ap_password,ap_password));
    step_worker(1000); /* Past the cancelled 250 ms deadline. */
    assert(!connects && !wifi_running && !servers[0].live);
    unsigned starts=wifi_starts;
    assert(ryz_provisioning_start()==ESP_OK && wifi_starts==starts && snapshot().state==RYZ_PROVISIONING_OFF);
    assert(ryz_provisioning_set_enabled(true)==ESP_OK);
    assert(connects==1 && wifi_mode==WIFI_MODE_STA && snapshot().state==RYZ_PROVISIONING_CONNECTING);
    associate("HOST-NET","10.0.0.7"); deliver_ip(&sta_netif,"10.0.0.7"); step_worker(1001);
    assert(snapshot().state==RYZ_PROVISIONING_ONLINE && !strcmp(snapshot().ipv4,"10.0.0.7"));
    assert(ryz_provisioning_set_enabled(false)==ESP_OK);
    deliver_ip(&sta_netif,"10.0.0.7"); deliver_disconnect("HOST-NET",WIFI_REASON_AUTH_FAIL);
    step_worker(5000);
    assert(snapshot().state==RYZ_PROVISIONING_OFF && connects==1 && !wifi_running);
    expect_credentials();
}
static esp_err_t transition_result;
static void *stop_on_owner(void *unused)
{ (void)unused; transition_result=ryz_provisioning_set_enabled(false); return NULL; }
static void http_handler_during_stop(void)
{
    start_portal(); block_http_stop=true;
    pthread_t transition;
    assert(pthread_create(&transition,NULL,stop_on_owner,NULL)==0);
    assert(pthread_mutex_lock(&schedule)==0);
    while (!http_stop_entered) wait_changed();
    assert(pthread_mutex_unlock(&schedule)==0);
    ryz_provisioning_snapshot_t pending=snapshot();
    assert(pending.state==RYZ_PROVISIONING_STOPPING && !pending.enabled && !pending.stop_confirmed);
    httpd_req_t rejected=request(&servers[0],"/api/provision","{\"ssid\":\"HOST-NET\",\"password\":\"test-only-pass\"}");
    assert(!strcmp(rejected.status,"409 Conflict") && !credential_writes && !connects);
    assert(ryz_provisioning_set_enabled(true)==ESP_ERR_INVALID_STATE);
    assert(pthread_mutex_lock(&schedule)==0); release_http_stop=true;
    assert(pthread_cond_broadcast(&changed)==0); assert(pthread_mutex_unlock(&schedule)==0);
    assert(pthread_join(transition,NULL)==0 && transition_result==ESP_OK);
    assert(snapshot().state==RYZ_PROVISIONING_OFF && !servers[0].live && !wifi_running);
}
static void stale_events_after_restart(void)
{
    start_portal(); post_credentials(&servers[0]); step_worker(250);
    assert(connects==1 && snapshot().state==RYZ_PROVISIONING_CONNECTING);
    associate("HOST-NET","10.0.0.1");
    deliver_ip(&sta_netif,"10.0.0.1"); /* Old mailbox entry, worker frozen. */
    assert(ryz_provisioning_set_enabled(false)==ESP_OK);
    assert(ryz_provisioning_set_enabled(true)==ESP_OK && connects==2);
    step_worker(251);
    assert(snapshot().state==RYZ_PROVISIONING_CONNECTING && !snapshot().ipv4[0]);
    /* Old SDK event not delivered until after new STA began: it must not get
     * accepted merely because the handler stamps today's generation. */
    deliver_ip(&sta_netif,"10.0.0.1"); step_worker(252);
    assert(snapshot().state==RYZ_PROVISIONING_CONNECTING && !snapshot().ipv4[0]);
    associate("HOST-NET","10.0.0.2");
    deliver_ip(&ap_netif,"10.0.0.2"); step_worker(253);
    deliver_ip(&sta_netif,"10.0.0.1"); step_worker(254);
    assert(snapshot().state==RYZ_PROVISIONING_CONNECTING);
    deliver_ip(&sta_netif,"10.0.0.2"); step_worker(255);
    assert(snapshot().state==RYZ_PROVISIONING_ONLINE && !strcmp(snapshot().ipv4,"10.0.0.2"));
    deliver_disconnect("PREVIOUS-NET",WIFI_REASON_AUTH_FAIL); step_worker(256);
    deliver_disconnect("HOST-NET",WIFI_REASON_AUTH_FAIL); step_worker(257);
    assert(snapshot().state==RYZ_PROVISIONING_ONLINE && connects==2);
    associated=false; sta_netif.up=false; sta_netif.ip.ip.addr=0;
    deliver_disconnect("HOST-NET",1); step_worker(258);
    assert(snapshot().state==RYZ_PROVISIONING_CONNECTING && connects==2);
    assert(ryz_provisioning_set_enabled(false)==ESP_OK); step_worker(5000);
    assert(snapshot().state==RYZ_PROVISIONING_OFF && connects==2);
}
static void failed_stop_is_not_off(unsigned failure)
{
    start_portal(); post_credentials(&servers[0]);
    if (failure==0) http_stop_error=ESP_FAIL;
    else if (failure==1) dns_stop_error=ESP_FAIL;
    else wifi_stop_error=ESP_FAIL;
    assert(ryz_provisioning_set_enabled(false)==ESP_FAIL);
    ryz_provisioning_snapshot_t failed=snapshot();
    assert(failed.state==RYZ_PROVISIONING_FAILED && !failed.enabled && !failed.stop_confirmed && failed.last_error==ESP_FAIL);
    if (!failure) assert(servers[0].live);
    else if (failure==1) assert(dns_servers[0].live);
    else assert(wifi_running);
    assert(ryz_provisioning_set_enabled(true)==ESP_FAIL && wifi_starts==1 && !connects);
    assert(ryz_provisioning_request_reprovision()==ESP_FAIL && !credential_erases);
    expect_credentials();
    http_stop_error=dns_stop_error=wifi_stop_error=ESP_OK;
    assert(ryz_provisioning_set_enabled(false)==ESP_OK);
    assert(snapshot().state==RYZ_PROVISIONING_OFF && snapshot().stop_confirmed && !wifi_running);
    assert(!servers[0].live && !dns_servers[0].live && !connects); expect_credentials();
}
static void unverified_ip_at_retry_deadline(void)
{
    start_portal(); post_credentials(&servers[0]); step_worker(250);
    assert(connects==1 && !associated);
    deliver_disconnect("HOST-NET",1); step_worker(251);
    assert(connects==1 && snapshot().state==RYZ_PROVISIONING_CONNECTING);
    /* Stop at the SDK clock read after this cycle copied its mailbox. The
     * event task then posts a late, unverified IP before reconnect admission. */
    assert(pthread_mutex_lock(&schedule)==0);
    block_clock=true; clock_tick=751;
    unsigned target=worker.waits+1; ++worker.permits;
    assert(pthread_cond_broadcast(&changed)==0);
    while (!clock_entered) wait_changed();
    assert(pthread_mutex_unlock(&schedule)==0);
    deliver_ip(&sta_netif,"10.0.0.99");
    assert(pthread_mutex_lock(&schedule)==0); release_clock=true;
    assert(pthread_cond_broadcast(&changed)==0);
    while (worker.waits<target) wait_changed();
    assert(pthread_mutex_unlock(&schedule)==0);
    assert(connects==2); /* Unverified mailbox data cannot consume the retry. */
    step_worker(752);
    assert(snapshot().state==RYZ_PROVISIONING_CONNECTING && !snapshot().ipv4[0]);
}
static void ignored_coalesced_event_preserves_retry(void)
{
    start_portal(); post_credentials(&servers[0]); step_worker(250);
    assert(connects==1 && !associated);
    deliver_disconnect("HOST-NET",1); step_worker(251);
    /* Neither a late, unverified IP nor a foreign SSID's disconnect proves
     * the current retry is obsolete. Both arrive before the same worker turn. */
    deliver_ip(&sta_netif,"10.0.0.99");
    deliver_disconnect("PREVIOUS-NET",WIFI_REASON_AUTH_FAIL);
    step_worker(252);
    assert(connects==1 && snapshot().state==RYZ_PROVISIONING_CONNECTING);
    step_worker(751);
    assert(connects==2 && !snapshot().ipv4[0]);
}
static void persistence_failure(void)
{
    start_portal(); nvs_commit_error=ESP_FAIL;
    httpd_req_t failed=request(&servers[0],"/api/provision","{\"ssid\":\"HOST-NET\",\"password\":\"test-only-pass\"}");
    assert(!strcmp(failed.status,"500 Internal Server Error") && snapshot().state==RYZ_PROVISIONING_AP_READY);
    assert(!snapshot().credentials_stored && !connect_queue.occupied && !connects);
    ryz_provisioning_credentials_t out;
    assert(ryz_provisioning_platform_load_credentials(&out)==ESP_ERR_NOT_FOUND);
    nvs_commit_error=ESP_OK; post_credentials(&servers[0]); expect_credentials();
    assert(ryz_provisioning_set_enabled(false)==ESP_OK);
    nvs_commit_error=ESP_FAIL;
    assert(ryz_provisioning_request_reprovision()==ESP_FAIL);
    assert(snapshot().state==RYZ_PROVISIONING_FAILED && !snapshot().enabled && !wifi_running);
    expect_credentials(); assert(snapshot().credentials_stored);
    nvs_commit_error=ESP_OK;
    assert(ryz_provisioning_request_reprovision()==ESP_OK);
    assert(snapshot().state==RYZ_PROVISIONING_AP_READY && !snapshot().credentials_stored);
    assert(ryz_provisioning_platform_load_credentials(&out)==ESP_ERR_NOT_FOUND);
}
static void ipv6_value(esp_ip6_addr_t *out, const char *text)
{ memset(out,0,sizeof(*out)); assert(inet_pton(AF_INET6,text,out->addr)==1); }

#if CONFIG_LWIP_IPV6
static void assert_ipv6(const char *actual, const char *expected)
{
    uint8_t a[16], b[16];
    assert(inet_pton(AF_INET6,actual,a)==1 && inet_pton(AF_INET6,expected,b)==1);
    assert(memcmp(a,b,sizeof(a))==0);
}
#endif

static void start_online_link(void)
{
    start_portal(); post_credentials(&servers[0]); step_worker(250);
    associate("HOST-NET","10.0.0.1"); live_ap.rssi=-51;
    ipv6_value(&global6,"2001:db8::37"); global6_error=ESP_OK;
    deliver_ip(&sta_netif,"10.0.0.1"); step_worker(251);
    ryz_provisioning_snapshot_t value=snapshot();
    assert(value.state==RYZ_PROVISIONING_ONLINE && value.link.mac_valid && value.link.rssi_valid);
    assert(value.link.sampled_at_us==251000 && value.link.last_error==ESP_OK);
    assert(value.link.rssi_dbm==-51 && !strcmp(value.link.mac,"02:12:34:56:78:9A"));
#if CONFIG_LWIP_IPV6
    assert(value.link.ipv6_valid); assert_ipv6(value.link.ipv6,"2001:db8::37");
#else
    assert(!value.link.ipv6_valid && !value.link.ipv6[0]);
#endif
}

static void periodic_link_and_copy_only_snapshot(void)
{
    start_online_link();
    assert(last_wait_ticks>0 && last_wait_ticks<=1000);
    unsigned calls=diagnostic_sdk_calls;
    for(unsigned i=0;i<100;++i) assert(snapshot().link.rssi_dbm==-51);
    assert(diagnostic_sdk_calls==calls); /* Public getter never samples SDK. */
    live_ap.rssi=7; /* SDK allows slightly positive values, never clamp to zero. */
    global6_error=ESP_FAIL; local6_error=ESP_OK; ipv6_value(&local6,"fe80::4");
    step_worker(1250);
    assert(snapshot().link.sampled_at_us==251000 && snapshot().link.rssi_dbm==-51);
    step_worker(1251); /* Timer only: there is no new SDK event/mailbox entry. */
    ryz_provisioning_snapshot_t value=snapshot();
    assert(value.link.rssi_dbm==7 && value.link.sampled_at_us==1251000);
#if CONFIG_LWIP_IPV6
    assert(value.link.ipv6_valid); assert_ipv6(value.link.ipv6,"fe80::4");
#endif
    local6_error=ESP_FAIL;
    step_worker(2251);
    value=snapshot();
    assert(value.link.mac_valid && value.link.rssi_valid && !value.link.ipv6_valid);
    assert(!value.link.ipv6[0] && value.link.last_error==ESP_OK);
    assert(connects==1 && value.state==RYZ_PROVISIONING_ONLINE);
}

static ryz_provisioning_link_info_t traffic_view(void)
{
    ryz_provisioning_link_info_t link=snapshot().link;
    assert(link.traffic_valid && link.traffic_epoch!=0);
    return link;
}

static void traffic_not_valid(void)
{
    ryz_provisioning_link_info_t link=snapshot().link;
    assert(!link.traffic_valid && !link.traffic_epoch);
    assert(!link.traffic_rx_bytes && !link.traffic_tx_bytes);
}

static void traffic_periodic_and_copy_only(void)
{
    start_online_link();
    ryz_provisioning_link_info_t initial=traffic_view();
    assert(!initial.traffic_rx_bytes && !initial.traffic_tx_bytes);
    traffic_packet(&sta_netif,0,600,ESP_OK);
    traffic_packet(&sta_netif,1,40,ESP_OK);
    traffic_packet(&sta_netif,2,900,ESP_OK);
    for(unsigned kind=0;kind<3;++kind) {
        traffic_packet(&sta_netif,kind,73,ESP_FAIL);
        traffic_packet(&ap_netif,kind,1111,ESP_OK);
    }
    unsigned sdk=diagnostic_sdk_calls, timers=monotonic_reads;
    unsigned calls[3]; memcpy(calls,traffic_calls,sizeof(calls));
    for(unsigned i=0;i<100;++i) {
        ryz_provisioning_link_info_t cached=traffic_view();
        assert(cached.traffic_epoch==initial.traffic_epoch);
        assert(!cached.traffic_rx_bytes && !cached.traffic_tx_bytes);
    }
    assert(diagnostic_sdk_calls==sdk && monotonic_reads==timers);
    assert(!memcmp(calls,traffic_calls,sizeof(calls)));
    step_worker(1250);
    assert(traffic_view().sampled_at_us==251000 && !traffic_view().traffic_tx_bytes);
    step_worker(1251);
    ryz_provisioning_link_info_t sampled=traffic_view();
    assert(sampled.traffic_epoch==initial.traffic_epoch && sampled.sampled_at_us==1251000);
    assert(sampled.traffic_tx_bytes==640 && sampled.traffic_rx_bytes==900);
    assert(sampled.mac_valid && sampled.rssi_valid && sampled.last_error==ESP_OK);
    step_worker(2251);
    sampled=traffic_view();
    assert(sampled.traffic_epoch==initial.traffic_epoch);
    assert(sampled.traffic_tx_bytes==640 && sampled.traffic_rx_bytes==900);
    traffic_packet(&sta_netif,2,100,ESP_OK);
    step_worker(3251);
    sampled=traffic_view();
    assert(sampled.traffic_tx_bytes==640 && sampled.traffic_rx_bytes==1000);
    assert(connects==1);
    assert(ryz_provisioning_set_enabled(false)==ESP_OK);
    traffic_not_valid();
}

static void traffic_ap_and_transition_exclusion(void)
{
    start_portal();
    assert(wifi_mode==WIFI_MODE_AP && ap_netif.up);
    for(unsigned kind=0;kind<3;++kind) {
        traffic_packet(&ap_netif,kind,1000,ESP_OK);
        traffic_packet(&sta_netif,kind,500,ESP_OK); /* Not yet verified/bound. */
    }
    step_worker(100);
    assert(snapshot().state==RYZ_PROVISIONING_AP_READY);
    traffic_not_valid();
    post_credentials(&servers[0]); step_worker(350);
    associate("HOST-NET","10.0.0.1");
    deliver_ip(&sta_netif,"10.0.0.1"); step_worker(351);
    ryz_provisioning_link_info_t link=traffic_view();
    assert(!link.traffic_rx_bytes && !link.traffic_tx_bytes);
    traffic_packet(&sta_netif,2,123,ESP_OK);
    step_worker(1351);
    assert(traffic_view().traffic_rx_bytes==123);
    assert(ryz_provisioning_open_portal()==ESP_OK);
    traffic_not_valid();
    for(unsigned kind=0;kind<3;++kind) traffic_packet(&ap_netif,kind,900,ESP_OK);
    step_worker(2351);
    assert(snapshot().state==RYZ_PROVISIONING_AP_READY && wifi_mode==WIFI_MODE_AP);
    traffic_not_valid();
}

static void traffic_disconnect_and_failed_stop(void)
{
    start_online_link();
    uint32_t epoch=traffic_view().traffic_epoch;
    traffic_packet(&sta_netif,0,600,ESP_OK);
    traffic_packet(&sta_netif,2,900,ESP_OK);
    step_worker(1251);
    assert(traffic_view().traffic_tx_bytes==600 && traffic_view().traffic_rx_bytes==900);
    associated=false; sta_netif.up=false; sta_netif.ip.ip.addr=0;
    deliver_disconnect("HOST-NET",1); step_worker(1252);
    assert(snapshot().state==RYZ_PROVISIONING_CONNECTING);
    traffic_not_valid();
    traffic_packet(&sta_netif,0,500,ESP_OK);
    traffic_packet(&sta_netif,2,700,ESP_OK);
    step_worker(1752); assert(connects==2);
    associate("HOST-NET","10.0.0.1");
    deliver_ip(&sta_netif,"10.0.0.1"); step_worker(1753);
    ryz_provisioning_link_info_t link=traffic_view();
    assert(link.traffic_epoch>epoch && !link.traffic_tx_bytes && !link.traffic_rx_bytes);
    epoch=link.traffic_epoch;
    traffic_packet(&sta_netif,2,20,ESP_OK); step_worker(2753);
    assert(traffic_view().traffic_rx_bytes==20);
    wifi_stop_error=ESP_FAIL;
    assert(ryz_provisioning_set_enabled(false)==ESP_FAIL && wifi_running);
    assert(snapshot().state==RYZ_PROVISIONING_FAILED && !snapshot().enabled);
    traffic_not_valid();
    traffic_packet(&sta_netif,1,333,ESP_OK);
    wifi_stop_error=ESP_OK;
    assert(ryz_provisioning_set_enabled(true)==ESP_OK);
    associate("HOST-NET","10.0.0.1");
    deliver_ip(&sta_netif,"10.0.0.1"); step_worker(2754);
    link=traffic_view();
    assert(link.traffic_epoch>epoch && !link.traffic_tx_bytes && !link.traffic_rx_bytes);
    assert(connects==3 && snapshot().state==RYZ_PROVISIONING_ONLINE);
}

static void traffic_roam_and_raw_disconnect(void)
{
    start_online_link();
    uint32_t epoch=traffic_view().traffic_epoch;
    traffic_packet(&sta_netif,0,500,ESP_OK); step_worker(1251);
    assert(traffic_view().traffic_tx_bytes==500);
    live_ap.bssid[0]^=1; step_worker(2251);
    ryz_provisioning_link_info_t link=traffic_view();
    assert(link.traffic_epoch>epoch && !link.traffic_rx_bytes && !link.traffic_tx_bytes);
    epoch=link.traffic_epoch;
    traffic_packet(&sta_netif,0,70,ESP_OK); step_worker(3251);
    assert(traffic_view().traffic_tx_bytes==70);
    /* A raw callback can precede the worker even when the SDK is already
     * associated again. Its old cached snapshot is not a new measurement. */
    deliver_disconnect("HOST-NET",1);
    traffic_packet(&sta_netif,0,40,ESP_OK);
    step_worker(3252);
    assert(snapshot().state==RYZ_PROVISIONING_ONLINE && connects==1);
    assert(traffic_view().sampled_at_us==3251000);
    step_worker(4251);
    link=traffic_view();
    assert(link.traffic_epoch>epoch && !link.traffic_rx_bytes && !link.traffic_tx_bytes);
    epoch=link.traffic_epoch;
    traffic_packet(&sta_netif,0,90,ESP_OK);
    event_during_mac=true; step_worker(5251);
    link=snapshot().link;
    assert(snapshot().state==RYZ_PROVISIONING_ONLINE && link.last_error==ESP_OK);
    assert(link.mac_valid && link.rssi_valid); /* Independent successful telemetry. */
    traffic_not_valid(); /* Do not bind across an event inside live validation. */
    traffic_packet(&sta_netif,2,111,ESP_OK);
    step_worker(6251);
    link=traffic_view();
    assert(link.traffic_epoch>epoch && !link.traffic_rx_bytes && !link.traffic_tx_bytes);
    traffic_packet(&sta_netif,2,12,ESP_OK); step_worker(7251);
    assert(traffic_view().traffic_rx_bytes==12 && !traffic_view().traffic_tx_bytes);
    assert(connects==1);
}

static void link_failures_clear_without_changing_connection(void)
{
    start_online_link();
    TickType_t now=251;
    uint32_t traffic_epoch=traffic_view().traffic_epoch;
    for(unsigned failure=0;failure<9;++failure) {
#if !CONFIG_LWIP_IPV6
        if(failure==3) continue;
#endif
        traffic_packet(&sta_netif,2,100,ESP_OK);
        if(failure==0) ap_info_error=ESP_FAIL;
        else if(failure==1) ip_info_error=ESP_FAIL;
        else if(failure==2) mac_error=ESP_FAIL;
        else if(failure==3) global6_error=ESP_ERR_INVALID_ARG;
        else if(failure==4) sta_netif.up=false;
        else if(failure==5) sta_netif.ip.ip.addr=inet_addr("10.0.0.99");
        else if(failure==6) strcpy((char *)live_ap.ssid,"OTHER-NET");
        else if(failure==7) disconnect_during_mac=true;
        else roam_during_mac=true;
        now+=1000; step_worker(now);
        ryz_provisioning_snapshot_t value=snapshot();
        assert(value.state==RYZ_PROVISIONING_ONLINE && value.last_error==ESP_OK);
        assert(!value.link.mac_valid && !value.link.rssi_valid && !value.link.ipv6_valid);
        assert(!value.link.mac[0] && !value.link.ipv6[0] && !value.link.rssi_dbm);
        assert(value.link.last_error!=ESP_OK && value.link.sampled_at_us==(uint64_t)now*1000);
        traffic_not_valid();
        ap_info_error=ip_info_error=mac_error=ESP_OK;
        global6_error=ESP_OK;
        associate("HOST-NET","10.0.0.1"); live_ap.rssi=-42;
        now+=1000; step_worker(now);
        value=snapshot();
        assert(value.link.mac_valid && value.link.rssi_valid && value.link.rssi_dbm==-42);
        assert(value.link.last_error==ESP_OK && value.state==RYZ_PROVISIONING_ONLINE && connects==1);
        assert(value.link.traffic_valid && value.link.traffic_epoch>traffic_epoch);
        assert(!value.link.traffic_tx_bytes && !value.link.traffic_rx_bytes);
        traffic_epoch=value.link.traffic_epoch;
    }
}

static void link_binding_revoked_by_stop_restart_and_disconnect(void)
{
    start_online_link();
    associated=false; sta_netif.up=false; sta_netif.ip.ip.addr=0;
    deliver_disconnect("HOST-NET",1); step_worker(252);
    assert(snapshot().state==RYZ_PROVISIONING_CONNECTING && !snapshot().link.sampled_at_us);
    assert(!snapshot().link.mac_valid && !snapshot().link.rssi_valid && !snapshot().link.ipv6_valid);
    assert(ryz_provisioning_set_enabled(false)==ESP_OK); step_worker(2500);
    assert(snapshot().state==RYZ_PROVISIONING_OFF && !snapshot().link.sampled_at_us && connects==1);
    assert(ryz_provisioning_set_enabled(true)==ESP_OK); step_worker(2501);
    assert(snapshot().state==RYZ_PROVISIONING_CONNECTING && !snapshot().link.sampled_at_us && connects==2);
    deliver_ip(&sta_netif,"10.0.0.1"); step_worker(2502);
    assert(!snapshot().link.sampled_at_us && !snapshot().link.mac_valid);
    associate("HOST-NET","10.0.0.2"); live_ap.rssi=12;
    ipv6_value(&global6,"2001:db8::42");
    deliver_ip(&sta_netif,"10.0.0.1"); step_worker(2503);
    assert(snapshot().state==RYZ_PROVISIONING_CONNECTING && !snapshot().link.sampled_at_us);
    deliver_ip(&sta_netif,"10.0.0.2"); step_worker(2504);
    assert(snapshot().state==RYZ_PROVISIONING_ONLINE && snapshot().link.rssi_dbm==12);
    assert(snapshot().link.sampled_at_us==2504000);
    wifi_stop_error=ESP_FAIL;
    assert(ryz_provisioning_set_enabled(false)==ESP_FAIL && wifi_running);
    step_worker(9999); /* Failed shutdown leaves radio uncertain, not diagnostics valid. */
    assert(snapshot().state==RYZ_PROVISIONING_FAILED && !snapshot().enabled);
    assert(!snapshot().link.sampled_at_us && !snapshot().link.mac_valid && !snapshot().link.rssi_valid);
    assert(!snapshot().link.ipv6[0] && last_wait_ticks==portMAX_DELAY);
    wifi_stop_error=ESP_OK;
}

static void all_zero(const void *data, size_t size)
{ const unsigned char *bytes=data; for(size_t i=0;i<size;++i) assert(bytes[i]==0); }

static void no_sta_secret(const void *data, size_t size)
{
    const unsigned char *bytes=data;
    const char secret[]="test-only-pass";
    for(size_t i=0;i+sizeof(secret)-1<=size;++i)
        assert(memcmp(bytes+i,secret,sizeof(secret)-1)!=0);
}

static uint64_t secret_epoch(void)
{
    ryz_provisioning_local_secret_t meta;
    assert(ryz_provisioning_local_secret_try_get(&meta)==ESP_OK);
    assert(meta.available && meta.epoch); return meta.epoch;
}

static void secret_unavailable(uint64_t previous)
{
    ryz_provisioning_local_secret_t meta={.available=true,.epoch=99};
    char out[64]; memset(out,'x',sizeof(out));
    assert(ryz_provisioning_local_secret_try_get(&meta)==ESP_OK && !meta.available && !meta.epoch);
    assert(ryz_provisioning_local_secret_try_copy(previous,out,sizeof(out))==ESP_ERR_INVALID_STATE);
    all_zero(out,sizeof(out));
}

static void secret_api_is_bounded_and_private(void)
{
    ryz_provisioning_local_secret_t meta={.available=true,.epoch=99};
    char out[64]; memset(out,'x',sizeof(out));
    assert(ryz_provisioning_local_secret_try_get(NULL)==ESP_ERR_INVALID_ARG);
    assert(ryz_provisioning_local_secret_try_get(&meta)==ESP_ERR_INVALID_STATE);
    assert(!meta.available && !meta.epoch);
    assert(ryz_provisioning_local_secret_try_copy(1,out,sizeof(out))==ESP_ERR_INVALID_STATE);
    all_zero(out,sizeof(out));
    start_portal(); secret_unavailable(1); /* SoftAP password is never STA. */
    post_credentials(&servers[0]);
    httpd_req_t status=request(&servers[0],"/api/status",NULL);
    no_sta_secret(status.response,strlen(status.response));
    step_worker(250); associate("HOST-NET","10.0.0.1");
    deliver_ip(&sta_netif,"10.0.0.1"); step_worker(251);
    uint64_t epoch=secret_epoch();
    ryz_provisioning_snapshot_t ordinary=snapshot();
    no_sta_secret(&ordinary,sizeof(ordinary));
    assert(strcmp(ordinary.ap_password,"test-only-pass")!=0);
    unsigned sdk=diagnostic_sdk_calls, nvs=nvs_reads, timer=monotonic_reads;
    for(unsigned i=0;i<100;++i) {
        assert(secret_epoch()==epoch);
        assert(ryz_provisioning_local_secret_try_copy(epoch,out,sizeof(out))==ESP_OK);
        assert(!strcmp(out,"test-only-pass"));
    }
    assert(diagnostic_sdk_calls==sdk && nvs_reads==nvs && monotonic_reads==timer+200);
    assert(snapshot().revision==ordinary.revision);
    memset(out,'x',sizeof(out));
    assert(ryz_provisioning_local_secret_try_copy(epoch+1,out,sizeof(out))==ESP_ERR_INVALID_STATE);
    all_zero(out,sizeof(out));
    memset(out,'x',sizeof(out));
    assert(ryz_provisioning_local_secret_try_copy(0,out,sizeof(out))==ESP_ERR_INVALID_ARG);
    all_zero(out,sizeof(out));
    assert(ryz_provisioning_local_secret_try_copy(epoch,NULL,64)==ESP_ERR_INVALID_ARG);
    assert(ryz_provisioning_local_secret_try_copy(epoch,out,0)==ESP_ERR_INVALID_ARG);
    memset(out,'x',sizeof(out));
    assert(ryz_provisioning_local_secret_try_copy(epoch,out,13)==ESP_ERR_INVALID_SIZE);
    all_zero(out,13); assert(out[13]=='x');
    assert(pthread_mutex_lock(&network_mutex.mutex)==0);
    memset(out,'x',sizeof(out)); meta=(ryz_provisioning_local_secret_t){true,epoch};
    assert(ryz_provisioning_local_secret_try_get(&meta)==ESP_ERR_TIMEOUT && !meta.available && !meta.epoch);
    assert(ryz_provisioning_local_secret_try_copy(epoch,out,sizeof(out))==ESP_ERR_TIMEOUT);
    all_zero(out,sizeof(out));
    assert(pthread_mutex_unlock(&network_mutex.mutex)==0);
    assert(secret_epoch()==epoch); /* Busy is not a new association; UI must end its hold. */
}

static void secret_epochs_and_freshness(void)
{
    start_online_link();
    uint64_t epoch=secret_epoch();
    live_ap.rssi=7; step_worker(1251);
    assert(secret_epoch()==epoch); /* Telemetry revisions preserve the hold. */
    live_ap.bssid[0]=42; step_worker(2251);
    uint64_t next=secret_epoch(); assert(next>epoch);
    char out[64]; memset(out,'x',sizeof(out));
    assert(ryz_provisioning_local_secret_try_copy(epoch,out,sizeof(out))==ESP_ERR_INVALID_STATE);
    all_zero(out,sizeof(out)); epoch=next;
    mac_error=ESP_FAIL; step_worker(3251); secret_unavailable(epoch);
    mac_error=ESP_OK; step_worker(4251); next=secret_epoch(); assert(next>epoch); epoch=next;
    /* Advance monotonic time without allowing the real worker another sample. */
    assert(pthread_mutex_lock(&schedule)==0); clock_tick=6751;
    assert(pthread_mutex_unlock(&schedule)==0);
    assert(secret_epoch()==epoch); /* Exactly 2.5 s remains valid. */
    assert(pthread_mutex_lock(&schedule)==0); clock_tick=6752;
    assert(pthread_mutex_unlock(&schedule)==0);
    secret_unavailable(epoch);
    step_worker(6752); next=secret_epoch(); assert(next>epoch); epoch=next;
    /* Even with no getter during the gap, a stale observation cannot renew an old hold. */
    step_worker(9753); next=secret_epoch(); assert(next>epoch); epoch=next;
    associated=false; sta_netif.up=false;
    deliver_disconnect("HOST-NET",1); step_worker(9754); secret_unavailable(epoch);
    step_worker(10254); assert(connects==2);
    associate("HOST-NET","10.0.0.1"); deliver_ip(&sta_netif,"10.0.0.1"); step_worker(10255);
    next=secret_epoch(); assert(next>epoch); epoch=next;
    assert(ryz_provisioning_local_secret_try_copy(epoch,out,sizeof(out))==ESP_OK && !strcmp(out,"test-only-pass"));
    wifi_stop_error=ESP_FAIL;
    assert(ryz_provisioning_set_enabled(false)==ESP_FAIL); secret_unavailable(epoch);
    wifi_stop_error=ESP_OK;
    assert(ryz_provisioning_set_enabled(true)==ESP_OK);
    associate("HOST-NET","10.0.0.1"); deliver_ip(&sta_netif,"10.0.0.1"); step_worker(10256);
    next=secret_epoch(); assert(next>epoch); epoch=next;
    assert(ryz_provisioning_request_reprovision()==ESP_OK); secret_unavailable(epoch);
    httpd_req_t response=request(&servers[1],"/api/provision","{\"ssid\":\"HOST-NET\",\"password\":\"second-pass8\"}");
    assert(!strcmp(response.status,"202 Accepted") && !strstr(response.response,"second-pass8"));
    step_worker(10506); associate("HOST-NET","10.0.0.1");
    deliver_ip(&sta_netif,"10.0.0.1"); step_worker(10507);
    next=secret_epoch(); assert(next>epoch);
    assert(ryz_provisioning_local_secret_try_copy(epoch,out,sizeof(out))==ESP_ERR_INVALID_STATE);
    all_zero(out,sizeof(out));
    assert(ryz_provisioning_local_secret_try_copy(next,out,sizeof(out))==ESP_OK && !strcmp(out,"second-pass8"));
}

static void secret_raw_disconnect_ends_hold_before_worker(void)
{
    start_online_link(); uint64_t epoch=secret_epoch();
    /* Real registered callback arrives after a rapid same-SSID/BSSID/IP
     * reconnect. Network policy correctly ignores its obsolete disconnect,
     * but that cannot renew a local password hold across the connection. */
    deliver_disconnect("HOST-NET",1);
    secret_unavailable(epoch); /* Worker has not consumed its mailbox. */
    step_worker(252);
    assert(snapshot().state==RYZ_PROVISIONING_ONLINE && connects==1);
    secret_unavailable(epoch);
    step_worker(1251); uint64_t next=secret_epoch(); assert(next>epoch); epoch=next;
    event_during_mac=true; step_worker(2251);
    assert(snapshot().state==RYZ_PROVISIONING_ONLINE && snapshot().link.last_error==ESP_OK);
    secret_unavailable(epoch); /* Event during validation must survive it. */
    step_worker(3251); next=secret_epoch(); assert(next>epoch); epoch=next;
    deliver_disconnect("PREVIOUS-NET",WIFI_REASON_AUTH_FAIL);
    secret_unavailable(epoch); /* Conservative hide, never a false new password. */
    step_worker(4251); next=secret_epoch(); assert(next>epoch);
    assert(snapshot().state==RYZ_PROVISIONING_ONLINE && connects==1);
    char out[64]; memset(out,'x',sizeof(out));
    assert(ryz_provisioning_local_secret_try_copy(epoch,out,sizeof(out))==ESP_ERR_INVALID_STATE);
    all_zero(out,sizeof(out));
}

static void secret_open_and_max_password(void)
{
    start_portal();
    httpd_req_t response=request(&servers[0],"/api/provision","{\"ssid\":\"HOST-NET\",\"password\":\"\"}");
    assert(!strcmp(response.status,"202 Accepted"));
    step_worker(250); associate("HOST-NET","10.0.0.1");
    deliver_ip(&sta_netif,"10.0.0.1"); step_worker(251);
    uint64_t epoch=secret_epoch(); char out[64]; memset(out,'x',sizeof(out));
    assert(ryz_provisioning_local_secret_try_copy(epoch,out,1)==ESP_OK && out[0]==0 && out[1]=='x');
    assert(ryz_provisioning_request_reprovision()==ESP_OK);
    char password[64]; memset(password,'q',63); password[63]=0;
    char body[128]; snprintf(body,sizeof(body),"{\"ssid\":\"HOST-NET\",\"password\":\"%s\"}",password);
    response=request(&servers[1],"/api/provision",body); assert(!strcmp(response.status,"202 Accepted"));
    step_worker(501); associate("HOST-NET","10.0.0.1");
    deliver_ip(&sta_netif,"10.0.0.1"); step_worker(502);
    uint64_t next=secret_epoch(); assert(next>epoch);
    assert(ryz_provisioning_local_secret_try_copy(next,out,63)==ESP_ERR_INVALID_SIZE); all_zero(out,63);
    assert(ryz_provisioning_local_secret_try_copy(next,out,64)==ESP_OK && !strcmp(out,password));
}

static void open_portal_preserves_saved_credentials(void)
{
    start_online_link();
    stored_t saved=persisted;
    unsigned writes=credential_writes, erases=credential_erases, committed=commits;
    unsigned connected=connects;
    assert(ryz_provisioning_open_portal()==ESP_OK);
    ryz_provisioning_snapshot_t portal=snapshot();
    assert(portal.enabled && portal.state==RYZ_PROVISIONING_AP_READY);
    assert(portal.credentials_stored && portal.portal_active && !portal.stop_confirmed);
    assert(!portal.ipv4[0] && !portal.link.mac_valid && !portal.link.rssi_valid && !portal.link.ipv6_valid);
    assert(!strcmp(portal.portal_ip,"192.168.4.1"));
    assert(wifi_running && wifi_mode==WIFI_MODE_AP && connects==connected);
    assert(memcmp(&persisted,&saved,sizeof(saved))==0);
    assert(credential_writes==writes && credential_erases==erases && commits==committed);
    expect_credentials();

    unsigned starts=wifi_starts, stops=wifi_stops, servers_started=server_count;
    uint32_t revision=portal.revision;
    assert(ryz_provisioning_open_portal()==ESP_OK);
    assert(snapshot().state==RYZ_PROVISIONING_AP_READY && snapshot().revision==revision);
    assert(wifi_starts==starts && wifi_stops==stops && server_count==servers_started);
    assert(credential_writes==writes && credential_erases==erases && commits==committed);
    assert(ryz_provisioning_set_enabled(false)==ESP_OK);
    assert(ryz_provisioning_set_enabled(true)==ESP_OK);
    assert(snapshot().state==RYZ_PROVISIONING_CONNECTING && connects==connected+1);
    assert(!strcmp((const char *)station_config.sta.ssid,"HOST-NET"));
    assert(!strcmp((const char *)station_config.sta.password,"test-only-pass"));
}

static void open_portal_revokes_station_events_and_retry(void)
{
    start_online_link();
    deliver_ip(&sta_netif,"10.0.0.1"); /* Real event mailbox, worker not released. */
    assert(ryz_provisioning_open_portal()==ESP_OK);
    step_worker(252);
    assert(snapshot().state==RYZ_PROVISIONING_AP_READY && !snapshot().ipv4[0]);
    deliver_ip(&sta_netif,"10.0.0.1");
    deliver_disconnect("HOST-NET",WIFI_REASON_AUTH_FAIL);
    step_worker(1000);
    assert(snapshot().state==RYZ_PROVISIONING_AP_READY && !snapshot().ipv4[0] && connects==1);

    assert(ryz_provisioning_set_enabled(false)==ESP_OK);
    assert(ryz_provisioning_set_enabled(true)==ESP_OK && connects==2);
    deliver_disconnect("HOST-NET",1); step_worker(1001);
    assert(snapshot().state==RYZ_PROVISIONING_CONNECTING && connects==2);
    assert(ryz_provisioning_open_portal()==ESP_OK);
    step_worker(1501); /* The old station's actual retry ticket is now due. */
    deliver_ip(&sta_netif,"10.0.0.1"); step_worker(2000);
    assert(snapshot().state==RYZ_PROVISIONING_AP_READY && !snapshot().ipv4[0]);
    assert(wifi_running && wifi_mode==WIFI_MODE_AP && connects==2);
    expect_credentials(); assert(credential_writes==1 && !credential_erases);
}

static void open_portal_revokes_submitted_connection(bool worker_has_copied)
{
    start_portal(); post_credentials(&servers[0]);
    if (worker_has_copied) step_worker(0);
    stored_t saved=persisted;
    unsigned writes=credential_writes, erases=credential_erases, committed=commits;
    assert(snapshot().state==RYZ_PROVISIONING_CREDENTIALS_RECEIVED);
    assert(ryz_provisioning_open_portal()==ESP_OK);
    assert(!servers[0].live && server_count==2 && servers[1].live);
    step_worker(1000); /* Beyond the revoked HTTP command's 250 ms deadline. */
    assert(snapshot().state==RYZ_PROVISIONING_AP_READY && snapshot().credentials_stored);
    assert(wifi_running && wifi_mode==WIFI_MODE_AP && !connects);
    assert(memcmp(&persisted,&saved,sizeof(saved))==0);
    assert(credential_writes==writes && credential_erases==erases && commits==committed);
    expect_credentials();
}

static void open_portal_stop_failure_preserves_retryable_resources(unsigned failure)
{
    start_portal(); post_credentials(&servers[0]); step_worker(0);
    stored_t saved=persisted;
    unsigned committed=commits;
    if (failure==0) http_stop_error=ESP_FAIL;
    else if (failure==1) dns_stop_error=ESP_FAIL;
    else wifi_stop_error=ESP_FAIL;
    assert(ryz_provisioning_open_portal()==ESP_FAIL);
    ryz_provisioning_snapshot_t failed=snapshot();
    assert(failed.state==RYZ_PROVISIONING_FAILED && !failed.stop_confirmed);
    assert(!failed.enabled && failed.last_error==ESP_FAIL && !failed.ipv4[0]);
    assert(server_count==1 && wifi_starts==1 && !connects);
    if (!failure) assert(servers[0].live && stopped_handles[0]==&servers[0]);
    else if (failure==1) assert(dns_servers[0].live);
    else assert(wifi_running && wifi_mode==WIFI_MODE_AP);
    assert(ryz_provisioning_open_portal()==ESP_FAIL);
    assert(server_count==1 && wifi_starts==1 && !connects);
    step_worker(1000);
    assert(snapshot().state==RYZ_PROVISIONING_FAILED && !connects);
    assert(memcmp(&persisted,&saved,sizeof(saved))==0);
    assert(credential_writes==1 && !credential_erases && commits==committed);
    http_stop_error=dns_stop_error=wifi_stop_error=ESP_OK;
    assert(ryz_provisioning_open_portal()==ESP_OK);
    assert(snapshot().state==RYZ_PROVISIONING_AP_READY && snapshot().credentials_stored);
    assert(!servers[0].live && !dns_servers[0].live && servers[1].live);
    if (!failure) assert(stop_calls==3 && stopped_handles[2]==&servers[0]);
    assert(wifi_starts==2 && wifi_running && wifi_mode==WIFI_MODE_AP && !connects);
    assert(memcmp(&persisted,&saved,sizeof(saved))==0); expect_credentials();
}

static void open_portal_start_failure_is_not_ready(unsigned failure)
{
    start_online_link(); stored_t saved=persisted;
    unsigned committed=commits;
    esp_err_t expected=failure==1 ? ESP_ERR_NO_MEM : ESP_FAIL;
    if (failure==0) wifi_start_error=expected;
    else if (failure==1) http_start_error=expected;
    else dns_start_error=expected;
    assert(ryz_provisioning_open_portal()==expected);
    ryz_provisioning_snapshot_t failed=snapshot();
    assert(failed.state==RYZ_PROVISIONING_FAILED && failed.last_error==expected);
    assert(failed.credentials_stored && !failed.portal_active && !failed.portal_ip[0] && !failed.ipv4[0]);
    assert(!wifi_running && connects==1);
    for (unsigned i=0;i<server_count;++i) assert(!servers[i].live);
    for (unsigned i=0;i<dns_count;++i) assert(!dns_servers[i].live);
    assert(memcmp(&persisted,&saved,sizeof(saved))==0);
    assert(credential_writes==1 && !credential_erases && commits==committed);
    wifi_start_error=http_start_error=dns_start_error=ESP_OK;
    assert(ryz_provisioning_open_portal()==ESP_OK);
    assert(snapshot().state==RYZ_PROVISIONING_AP_READY && snapshot().credentials_stored);
    assert(wifi_running && wifi_mode==WIFI_MODE_AP && connects==1);
    assert(memcmp(&persisted,&saved,sizeof(saved))==0); expect_credentials();
}

static void open_portal_fast_http_submission_wins_over_ready(void)
{
    start_online_link(); unsigned writes=credential_writes, committed=commits;
    fast_post_on_unlock=true;
    assert(ryz_provisioning_open_portal()==ESP_OK);
    assert(fast_post_seen && !fast_post_on_unlock);
    ryz_provisioning_snapshot_t submitted=snapshot();
    assert(submitted.state==RYZ_PROVISIONING_CREDENTIALS_RECEIVED);
    assert(submitted.credentials_stored && !strcmp(submitted.sta_ssid,"NEXT-NET"));
    assert(!submitted.ipv4[0] && connects==1);
    /* This write belongs to the explicit HTTP POST, not OPEN_AP. Existing
     * save-before-connect persistence policy is exercised, not redefined. */
    assert(credential_writes==writes+1 && !credential_erases && commits==committed+1);
    ryz_provisioning_credentials_t saved;
    assert(ryz_provisioning_platform_load_credentials(&saved)==ESP_OK);
    assert(!strcmp(saved.ssid,"NEXT-NET") && !strcmp(saved.password,"second-pass8"));
    step_worker(500); assert(connects==1);
    step_worker(501);
    assert(snapshot().state==RYZ_PROVISIONING_CONNECTING && connects==2);
    assert(!strcmp((const char *)station_config.sta.ssid,"NEXT-NET"));
    associate("NEXT-NET","10.0.0.2"); deliver_ip(&sta_netif,"10.0.0.2"); step_worker(502);
    assert(snapshot().state==RYZ_PROVISIONING_ONLINE && !strcmp(snapshot().ipv4,"10.0.0.2"));
}

static void unexpected_platform_event(const ryz_provisioning_platform_event_t *event)
{ (void)event; abort(); }

static void enabled_platform_roundtrip(void)
{
    assert(ryz_provisioning_platform_init(unexpected_platform_event)==ESP_OK);
    bool enabled=true;
    assert(ryz_provisioning_platform_load_enabled(NULL)==ESP_ERR_INVALID_ARG);
    assert(!nvs_reads && !enabled_reads && !nvs_closes);
    assert(ryz_provisioning_platform_load_enabled(&enabled)==ESP_ERR_NOT_FOUND && !enabled);
    assert(nvs_reads==1 && enabled_reads==1 && nvs_closes==1);
    persisted.credentials[0]=0x5a; persisted.length=1;
    strcpy(persisted.ap_password,"existing-ap-pass");
    assert(ryz_provisioning_platform_save_enabled(false)==ESP_OK);
    assert(persisted.wifi_on_present && !persisted.wifi_on);
    enabled=true;
    assert(ryz_provisioning_platform_load_enabled(&enabled)==ESP_OK && !enabled);
    assert(ryz_provisioning_platform_save_enabled(true)==ESP_OK);
    enabled=false;
    assert(ryz_provisioning_platform_load_enabled(&enabled)==ESP_OK && enabled);
    assert(enabled_writes==2 && commits==2 && nvs_closes==5);
    assert(persisted.length==1 && persisted.credentials[0]==0x5a);
    assert(!strcmp(persisted.ap_password,"existing-ap-pass"));
    assert(!credential_writes && !credential_erases && !wifi_starts && !wifi_stops);
    ryz_provisioning_platform_deinit();
    assert(!wifi_live && !worker.live);
}

static void enabled_boot_off(void)
{
    persisted.wifi_on_present=true; persisted.wifi_on=0;
    assert(ryz_provisioning_init()==ESP_OK);
    ryz_provisioning_snapshot_t initial=snapshot();
    assert(initial.state==RYZ_PROVISIONING_OFF && !initial.enabled && initial.stop_confirmed);
    assert(!enabled_writes && !wifi_starts && !connects);
    unsigned reads=nvs_reads, saved_commits=commits;
    assert(ryz_provisioning_start()==ESP_OK);
    assert(ryz_provisioning_set_enabled(false)==ESP_OK);
    assert(snapshot().state==RYZ_PROVISIONING_OFF && !wifi_running);
    assert(!enabled_writes && nvs_reads==reads && commits==saved_commits);
}

static void enabled_platform_load_errors(void)
{
    assert(ryz_provisioning_platform_init(unexpected_platform_event)==ESP_OK);
    bool enabled=true;
    nvs_open_error=ESP_ERR_NVS_NOT_FOUND;
    assert(ryz_provisioning_platform_load_enabled(&enabled)==ESP_ERR_NOT_FOUND && !enabled);
    assert(!enabled_reads && !nvs_closes);
    nvs_open_error=ESP_FAIL; enabled=true;
    assert(ryz_provisioning_platform_load_enabled(&enabled)==ESP_FAIL && !enabled);
    assert(!enabled_reads && !nvs_closes);
    nvs_open_error=ESP_OK;
    const esp_err_t errors[]={ESP_ERR_NVS_NOT_FOUND,ESP_ERR_NVS_TYPE_MISMATCH,ESP_FAIL};
    for (unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);++i) {
        enabled_get_error=errors[i]; enabled=true;
        esp_err_t expected=errors[i]==ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : errors[i];
        assert(ryz_provisioning_platform_load_enabled(&enabled)==expected && !enabled);
        assert(nvs_closes==i+1);
    }
    enabled_get_error=ESP_OK; persisted.wifi_on_present=true;
    const uint8_t invalid[]={2,255};
    for (unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        persisted.wifi_on=invalid[i]; enabled=true;
        assert(ryz_provisioning_platform_load_enabled(&enabled)==ESP_ERR_INVALID_RESPONSE && !enabled);
    }
    assert(nvs_closes==5 && !enabled_writes && !commits);
    assert(!credential_writes && !credential_erases && !wifi_starts && !wifi_stops);
    ryz_provisioning_platform_deinit();
}

static void enabled_platform_save_errors(void)
{
    assert(ryz_provisioning_platform_init(unexpected_platform_event)==ESP_OK);
    persisted.wifi_on_present=true; persisted.wifi_on=0;
    persisted.length=1; persisted.credentials[0]=0x7b;
    strcpy(persisted.ap_password,"keep-ap-password");
    const stored_t before=persisted;
    for (unsigned stage=0;stage<3;++stage) {
        unsigned closed=nvs_closes, written=enabled_writes, committed=commits;
        const esp_err_t failure=stage==0 ? ESP_ERR_NO_MEM : stage==1 ? ESP_ERR_NVS_INVALID_LENGTH : ESP_FAIL;
        nvs_open_error=stage==0 ? failure : ESP_OK;
        enabled_set_error=stage==1 ? failure : ESP_OK;
        nvs_commit_error=stage==2 ? failure : ESP_OK;
        assert(ryz_provisioning_platform_save_enabled(true)==failure);
        assert(nvs_closes==closed+(stage!=0));
        assert(enabled_writes==written+(stage!=0));
        assert(commits==committed+(stage==2));
        assert(memcmp(&persisted,&before,sizeof(before))==0);
        nvs_open_error=enabled_set_error=nvs_commit_error=ESP_OK;
        bool enabled=true;
        assert(ryz_provisioning_platform_load_enabled(&enabled)==ESP_OK && !enabled);
    }
    assert(!credential_writes && !credential_erases && !wifi_starts && !wifi_stops);
    ryz_provisioning_platform_deinit();
}

static void enabled_boot_invalid(unsigned kind)
{
    persisted.wifi_on_present=true; persisted.wifi_on=kind==1 ? 255 : 2;
    strcpy(persisted.ap_password,"previous-ap-pass");
    esp_err_t expected=ESP_ERR_INVALID_RESPONSE;
    if (kind==2) expected=enabled_get_error=ESP_ERR_NVS_TYPE_MISMATCH;
    if (kind==3) expected=enabled_get_error=ESP_FAIL;
    if (kind==4) expected=nvs_open_error=ESP_ERR_NO_MEM;
    assert(ryz_provisioning_init()==expected);
    ryz_provisioning_snapshot_t absent={0};
    assert(ryz_provisioning_get_snapshot(&absent)==ESP_ERR_INVALID_STATE);
    assert(ryz_provisioning_start()==ESP_ERR_INVALID_STATE);
    assert(!wifi_running && !wifi_starts && !connects && !server_count && !dns_count);
    assert(!worker.live && !wifi_live && !handlers[0].live && !handlers[1].live);
    assert(!enabled_writes && !credential_writes && !credential_erases && !commits);
    for (unsigned i=0;i<8;++i) assert(!nvs_handles[i].live);
    /* Retry real initialization after the external record/error is repaired.
     * No process-reset or power-loss behavior is inferred from this fixture. */
    nvs_open_error=enabled_get_error=ESP_OK; persisted.wifi_on=0;
    assert(ryz_provisioning_init()==ESP_OK);
    assert(snapshot().state==RYZ_PROVISIONING_OFF && !snapshot().enabled);
    assert(ryz_provisioning_start()==ESP_OK && !wifi_starts);
}

static void expect_same_network(const ryz_provisioning_snapshot_t *before)
{
    ryz_provisioning_snapshot_t after=snapshot();
    assert(after.revision==before->revision && after.state==before->state);
    assert(after.enabled==before->enabled && after.stop_confirmed==before->stop_confirmed);
    assert(after.boot_restore_pending==before->boot_restore_pending);
    assert(after.last_error==before->last_error && after.detail==before->detail);
    assert(after.credentials_stored==before->credentials_stored && after.portal_active==before->portal_active);
    assert(!strcmp(after.sta_ssid,before->sta_ssid) && !strcmp(after.ipv4,before->ipv4));
    assert(!strcmp(after.portal_ip,before->portal_ip));
    assert(after.link.sampled_at_us==before->link.sampled_at_us);
    assert(after.link.mac_valid==before->link.mac_valid && after.link.rssi_valid==before->link.rssi_valid);
}

static void boot_saved_station(bool enabled, bool fail_start)
{
    /* External persisted fixture, decoded by the production NVS reader. This
     * models an existing v1 saved record, not a synthesized core snapshot. */
    const struct {
        uint32_t version;
        char ssid[RYZ_PROVISIONING_SSID_MAX_LENGTH+1];
        char password[RYZ_PROVISIONING_PASSWORD_MAX_LENGTH+1];
    } record={.version=1,.ssid="HOST-NET",.password="test-only-pass"};
    memcpy(persisted.credentials,&record,sizeof(record)); persisted.length=sizeof(record);
    strcpy(persisted.ap_password,"ABCDEFGHJKLM");
    persisted.wifi_on_present=true; persisted.wifi_on=enabled;
    assert(ryz_provisioning_init()==ESP_OK);
    if (fail_start) wifi_start_error=ESP_ERR_NO_MEM;
    assert(ryz_provisioning_start()==(fail_start ? ESP_ERR_NO_MEM : ESP_OK));
    wifi_start_error=ESP_OK;
    assert(!credential_writes && !credential_erases && !enabled_writes && !commits);
    assert(snapshot().boot_restore_pending==enabled);
    if (enabled) assert(snapshot().credentials_stored && !strcmp(snapshot().sta_ssid,"HOST-NET"));
}

static void boot_fallback_protected_ap_and_stale_events(bool fail_start)
{
    boot_saved_station(true,fail_start); stored_t saved=persisted;
    const unsigned initial_connects=connects;
    if (!fail_start) {
        /* The worker has queued a retry, and another stale IP is in its
         * mailbox when the trusted boot owner reaches its deadline. */
        deliver_disconnect("HOST-NET",1); step_worker(0);
        deliver_ip(&sta_netif,"10.0.0.99");
    }
    assert(ryz_provisioning_boot_fallback()==ESP_OK);
    ryz_provisioning_snapshot_t ready=snapshot();
    assert(ready.state==RYZ_PROVISIONING_AP_READY && ready.enabled && ready.credentials_stored);
    assert(!ready.boot_restore_pending);
    assert(ready.portal_active && !ready.stop_confirmed && !ready.ipv4[0]);
    assert(!strcmp(ready.sta_ssid,"HOST-NET") && !strcmp(ready.portal_ip,"192.168.4.1"));
    assert(!strcmp(ready.ap_password,"ABCDEFGHJKLM"));
    assert(wifi_running && wifi_mode==WIFI_MODE_AP && servers[0].live && dns_servers[0].live);
    assert(portal_config.ap.authmode==WIFI_AUTH_WPA2_PSK);
    assert(!strcmp((const char *)portal_config.ap.password,ready.ap_password));
    assert(ryz_provisioning_diagnostics_ready());
    assert(memcmp(&persisted,&saved,sizeof(saved))==0); expect_credentials();
    step_worker(1000);
    deliver_ip(&sta_netif,"10.0.0.99"); deliver_disconnect("HOST-NET",1); step_worker(2000);
    assert(snapshot().state==RYZ_PROVISIONING_AP_READY && connects==initial_connects);
    ready=snapshot(); unsigned started=wifi_starts, stopped=wifi_stops;
    assert(ryz_provisioning_boot_fallback()==ESP_OK); expect_same_network(&ready);
    assert(wifi_starts==started && wifi_stops==stopped);
    assert(!credential_writes && !credential_erases && !enabled_writes && !commits);
}

static void boot_scan_case(unsigned variant)
{
    struct {
        uint32_t version;
        char ssid[33], password[64];
    } record = {.version=1,.ssid="HOST-NET",.password="test-only-pass"};
    if(variant==8) strcpy(record.ssid,"12345678901234567890123456789012");
    if(variant==9) strcpy(record.ssid,"测试-WiFi");
    memcpy(persisted.credentials,&record,sizeof(record)); persisted.length=sizeof(record);
    strcpy(persisted.ap_password,"ABCDEFGHJKLM");
    persisted.wifi_on_present=true; persisted.wifi_on=1;
    const stored_t saved=persisted;
    scan_variant=variant<=5 ? variant : 0;
    if(variant==6) scan_error=ESP_ERR_TIMEOUT;
    if(variant==7) scan_read_error=ESP_ERR_NO_MEM;
    const esp_err_t expected=variant>=1 && variant<=5 ? ESP_ERR_NOT_FOUND :
        variant==6 ? ESP_ERR_TIMEOUT : variant==7 ? ESP_ERR_NO_MEM : ESP_OK;
    assert(ryz_provisioning_init()==ESP_OK);
    assert(ryz_provisioning_start()==expected);
    assert(scans==1 && scan_reads==(variant==6 ? 0U : 1U));
    assert(scan_clears==(variant==6 || variant==7 ? 1U : 0U));
    assert(scan_stops==(variant==6 ? 1U : 0U));
    if(expected!=ESP_OK) {
        const ryz_provisioning_boot_phase_t phase=expected==ESP_ERR_NOT_FOUND ?
            RYZ_PROVISIONING_BOOT_NOT_FOUND : RYZ_PROVISIONING_BOOT_SCAN_FAILED;
        assert(!connects && snapshot().state==RYZ_PROVISIONING_FAILED);
        assert(snapshot().boot_phase==phase && snapshot().boot_restore_pending);
        assert(ryz_provisioning_boot_fallback()==ESP_OK);
        assert(snapshot().boot_phase==phase && !snapshot().boot_restore_pending);
        assert(snapshot().state==RYZ_PROVISIONING_AP_READY && snapshot().portal_active);
        assert(!connects && wifi_mode==WIFI_MODE_AP && wifi_running);
        deliver_disconnect(record.ssid,1); step_worker(1000); step_worker(2000);
        assert(!connects); /* No hidden retry after a skipped saved SSID. */
    } else {
        assert(connects==1 && snapshot().boot_phase==RYZ_PROVISIONING_BOOT_CONNECTING);
        if(variant==10) {
            deliver_disconnect(record.ssid,WIFI_REASON_AUTH_FAIL); step_worker(1);
            assert(snapshot().boot_phase==RYZ_PROVISIONING_BOOT_CONNECT_FAILED);
        } else if(variant!=11) {
            associate(record.ssid,"10.0.0.7"); deliver_ip(&sta_netif,"10.0.0.7"); step_worker(1);
            assert(snapshot().boot_phase==RYZ_PROVISIONING_BOOT_CONNECTED);
            assert(snapshot().state==RYZ_PROVISIONING_ONLINE && snapshot().ipv4[0]);
        }
        if(variant==10 || variant==11) {
            assert(ryz_provisioning_boot_fallback()==ESP_OK);
            assert(snapshot().boot_phase==RYZ_PROVISIONING_BOOT_CONNECT_FAILED);
            assert(snapshot().state==RYZ_PROVISIONING_AP_READY);
        }
    }
    assert(memcmp(&persisted,&saved,sizeof(saved))==0);
    assert(!credential_writes && !credential_erases && !enabled_writes && !commits);
}

static void boot_fallback_keeps_live_station(bool processed_ip)
{
    boot_saved_station(true,false); stored_t saved=persisted;
    associate("HOST-NET","10.0.0.7"); deliver_ip(&sta_netif,"10.0.0.7");
    if (processed_ip) step_worker(0);
    ryz_provisioning_snapshot_t before=snapshot();
    assert(before.state==(processed_ip ? RYZ_PROVISIONING_ONLINE : RYZ_PROVISIONING_CONNECTING));
    assert(ryz_provisioning_boot_fallback()==ESP_OK);
    assert(snapshot().state==RYZ_PROVISIONING_ONLINE && !strcmp(snapshot().ipv4,"10.0.0.7"));
    assert(!snapshot().boot_restore_pending);
    if (processed_ip) expect_same_network(&before);
    assert(wifi_starts==1 && !wifi_stops && connects==1 && !server_count && !dns_count);
    assert(memcmp(&persisted,&saved,sizeof(saved))==0);
    if (!processed_ip) step_worker(0);
    associated=false; sta_netif.up=false;
    deliver_disconnect("HOST-NET",1); step_worker(1);
    assert(ryz_provisioning_boot_fallback()==ESP_OK);
    step_worker(501); /* Consumed boot API does not alter normal retry policy. */
    assert(connects==2 && wifi_mode==WIFI_MODE_STA && !server_count);
}

static void boot_fallback_superseded(unsigned variant)
{
    boot_saved_station(variant!=0,false);
    if (variant==1) assert(ryz_provisioning_set_enabled(true)==ESP_OK);
    if (variant==2) {
        enabled_set_error=ESP_FAIL;
        assert(ryz_provisioning_set_enabled(false)==ESP_FAIL);
        enabled_set_error=ESP_OK;
    }
    if (variant==3) assert(ryz_provisioning_open_portal()==ESP_OK);
    if (variant==4) assert(ryz_provisioning_request_reprovision()==ESP_OK);
    ryz_provisioning_snapshot_t before=snapshot();
    const unsigned started=wifi_starts, stopped=wifi_stops, reads=nvs_reads;
    assert(ryz_provisioning_boot_fallback()==ESP_OK); expect_same_network(&before);
    assert(wifi_starts==started && wifi_stops==stopped && nvs_reads==reads);
    if (!variant) assert(!wifi_running && !connects && !server_count);
}

static void boot_fallback_rejects_unverified_ip(unsigned variant)
{
    boot_saved_station(true,false);
    associate(variant==1 ? "FOREIGN-NET" : "HOST-NET",variant==2 ? "0.0.0.0" : "10.0.0.7");
    if (variant==0) sta_netif.up=false; /* esp-netif may retain a down-interface IP. */
    deliver_ip(&sta_netif,"10.0.0.7");
    unsigned reads=nvs_reads;
    assert(ryz_provisioning_boot_fallback()==ESP_OK);
    assert(snapshot().state==RYZ_PROVISIONING_AP_READY && !snapshot().ipv4[0]);
    assert(wifi_mode==WIFI_MODE_AP && wifi_stops==1 && nvs_reads==reads);
    step_worker(1000);
    assert(snapshot().state==RYZ_PROVISIONING_AP_READY && connects==1);
}

static void boot_fallback_failure(unsigned stage)
{
    boot_saved_station(true,false); stored_t saved=persisted;
    if (stage<2) associate("HOST-NET","10.0.0.7");
    esp_err_t expected=stage==4 ? ESP_ERR_NO_MEM : ESP_FAIL;
    if (stage==0) ap_info_error=expected;
    if (stage==1) ip_info_error=expected;
    if (stage==2) wifi_stop_error=expected;
    if (stage==3) wifi_start_error=expected;
    if (stage==4) http_start_error=expected;
    if (stage==5) dns_start_error=expected;
    assert(ryz_provisioning_boot_fallback()==expected);
    ryz_provisioning_snapshot_t failed=snapshot();
    assert(failed.state==RYZ_PROVISIONING_FAILED && failed.last_error==expected);
    assert(failed.enabled && failed.credentials_stored && !failed.portal_active && !failed.ipv4[0]);
    if (stage<2) assert(wifi_running && wifi_stops==0 && !server_count);
    if (stage==2) assert(wifi_running && wifi_stops==1 && !server_count);
    if (stage>=3) assert(!wifi_running);
    assert(memcmp(&persisted,&saved,sizeof(saved))==0);
    const unsigned stopped=wifi_stops, started=wifi_starts;
    assert(ryz_provisioning_boot_fallback()==ESP_OK); expect_same_network(&failed);
    assert(wifi_stops==stopped && wifi_starts==started); /* No hidden retry. */
    ap_info_error=ip_info_error=wifi_stop_error=wifi_start_error=http_start_error=dns_start_error=ESP_OK;
    if (stage<2) {
        deliver_ip(&sta_netif,"10.0.0.7"); step_worker(1);
        assert(snapshot().state==RYZ_PROVISIONING_ONLINE && !wifi_stops);
    } else {
        deliver_ip(&sta_netif,"10.0.0.7"); step_worker(1000);
        assert(snapshot().state==RYZ_PROVISIONING_FAILED && connects==1);
        assert(ryz_provisioning_open_portal()==ESP_OK);
        assert(snapshot().state==RYZ_PROVISIONING_AP_READY);
    }
    assert(memcmp(&persisted,&saved,sizeof(saved))==0);
}

static void boot_fallback_fast_http_submission(void)
{
    boot_saved_station(true,false);
    fast_post_on_unlock=boot_fast_post=true;
    assert(ryz_provisioning_boot_fallback()==ESP_OK && fast_post_seen);
    assert(snapshot().state==RYZ_PROVISIONING_CREDENTIALS_RECEIVED);
    assert(!strcmp(snapshot().sta_ssid,"NEXT-NET") && credential_writes==1);
    step_worker(0); step_worker(250);
    assert(snapshot().state==RYZ_PROVISIONING_CONNECTING && connects==2);
}

static void initial_ap_fast_submission_is_not_boot_restore(void)
{
    assert(ryz_provisioning_init()==ESP_OK);
    fast_post_on_unlock=fast_worker_on_post=true;
    assert(ryz_provisioning_start()==ESP_OK && fast_post_seen);
    ryz_provisioning_snapshot_t before=snapshot();
    assert(before.state==RYZ_PROVISIONING_CONNECTING && !before.boot_restore_pending);
    const unsigned stopped=wifi_stops, started=wifi_starts;
    assert(ryz_provisioning_boot_fallback()==ESP_OK); expect_same_network(&before);
    assert(wifi_stops==stopped && wifi_starts==started && connects==1);
}

static void boot_observed_online_then_dropped_is_not_boot_restore(void)
{
    boot_saved_station(true,false);
    associate("HOST-NET","10.0.0.7"); deliver_ip(&sta_netif,"10.0.0.7"); step_worker(0);
    assert(snapshot().state==RYZ_PROVISIONING_ONLINE && !snapshot().boot_restore_pending);
    associated=false; sta_netif.up=false;
    deliver_disconnect("HOST-NET",1); step_worker(1);
    ryz_provisioning_snapshot_t before=snapshot();
    assert(before.state==RYZ_PROVISIONING_CONNECTING && !before.boot_restore_pending);
    assert(ryz_provisioning_boot_fallback()==ESP_OK); expect_same_network(&before);
    step_worker(501);
    assert(connects==2 && wifi_mode==WIFI_MODE_STA && !server_count && !wifi_stops);
}

static void enabled_mutation_save_failure(bool turn_on, unsigned stage)
{
    persisted.wifi_on_present=true; persisted.wifi_on=turn_on ? 0 : 1;
    if (turn_on) assert(ryz_provisioning_init()==ESP_OK);
    else start_online_link();
    ryz_provisioning_snapshot_t before=snapshot();
    stored_t prior=persisted;
    unsigned starts=wifi_starts, stops=wifi_stops, closed=nvs_closes;
    unsigned written=enabled_writes, committed=commits, connected=connects;
    const esp_err_t failure=stage==0 ? ESP_ERR_NO_MEM : stage==1 ? ESP_ERR_NVS_INVALID_LENGTH : ESP_FAIL;
    nvs_open_error=stage==0 ? failure : ESP_OK;
    enabled_set_error=stage==1 ? failure : ESP_OK;
    nvs_commit_error=stage==2 ? failure : ESP_OK;
    assert(ryz_provisioning_set_enabled(turn_on)==failure);
    expect_same_network(&before);
    assert(wifi_starts==starts && wifi_stops==stops && connects==connected);
    assert(nvs_closes==closed+(stage!=0) && enabled_writes==written+(stage!=0));
    assert(commits==committed+(stage==2));
    assert(memcmp(&persisted,&prior,sizeof(prior))==0); /* This injection failed before commit. */
    nvs_open_error=enabled_set_error=nvs_commit_error=ESP_OK;
    bool enabled=turn_on;
    assert(ryz_provisioning_platform_load_enabled(&enabled)==ESP_OK && enabled!=turn_on);
    expected_radio_preference=turn_on ? 1 : 0;
    assert(ryz_provisioning_set_enabled(turn_on)==ESP_OK);
    assert(snapshot().enabled==turn_on && wifi_running==turn_on);
    written=enabled_writes; committed=commits;
    assert(ryz_provisioning_set_enabled(turn_on)==ESP_OK);
    assert(enabled_writes==written && commits==committed);
    assert(prior.length==persisted.length && !memcmp(prior.credentials,persisted.credentials,sizeof(prior.credentials)));
    assert(!strcmp(prior.ap_password,persisted.ap_password));
    expected_radio_preference=-1;
}

static void enabled_first_explicit_on(void)
{
    start_portal();
    assert(!persisted.wifi_on_present && !enabled_writes);
    unsigned committed=commits, starts=wifi_starts;
    assert(ryz_provisioning_start()==ESP_OK && !enabled_writes && commits==committed);
    assert(ryz_provisioning_set_enabled(true)==ESP_OK);
    assert(persisted.wifi_on_present && persisted.wifi_on==1 && enabled_writes==1);
    assert(commits==committed+1 && wifi_starts==starts);
    assert(ryz_provisioning_set_enabled(true)==ESP_OK && enabled_writes==1);
    expected_radio_preference=0;
    assert(ryz_provisioning_set_enabled(false)==ESP_OK);
    assert(persisted.wifi_on==0 && enabled_writes==2 && !wifi_running);
    assert(ryz_provisioning_set_enabled(false)==ESP_OK && enabled_writes==2);
    expected_radio_preference=-1;
}

static void enabled_ambiguous_commit_does_not_trust_old_cache(void)
{
    persisted.wifi_on_present=true; persisted.wifi_on=1;
    start_portal();
    ryz_provisioning_snapshot_t before=snapshot();
    unsigned starts=wifi_starts, stops=wifi_stops;
    nvs_commit_error=ESP_FAIL; commit_then_error=true;
    assert(ryz_provisioning_set_enabled(false)==ESP_FAIL);
    assert(persisted.wifi_on==0 && enabled_writes==1);
    expect_same_network(&before);
    assert(wifi_starts==starts && wifi_stops==stops && wifi_running);
    nvs_commit_error=ESP_OK; commit_then_error=false;
    /* The caller still sees ON. A new explicit ON must repair the unknown
     * preference instead of trusting the pre-failure cached value of ON. */
    assert(ryz_provisioning_set_enabled(true)==ESP_OK);
    assert(persisted.wifi_on==1 && enabled_writes==2);
    assert(ryz_provisioning_set_enabled(true)==ESP_OK && enabled_writes==2);
    assert(wifi_starts==starts && wifi_stops==stops);
}

static void enabled_radio_failure_retains_preference(bool turn_on)
{
    persisted.wifi_on_present=true; persisted.wifi_on=turn_on ? 0 : 1;
    if (turn_on) assert(ryz_provisioning_init()==ESP_OK);
    else start_online_link();
    expected_radio_preference=turn_on ? 1 : 0;
    if (turn_on) wifi_start_error=ESP_FAIL;
    else wifi_stop_error=ESP_FAIL;
    assert(ryz_provisioning_set_enabled(turn_on)==ESP_FAIL);
    assert(enabled_writes==1 && persisted.wifi_on==(uint8_t)turn_on);
    ryz_provisioning_snapshot_t failed=snapshot();
    assert(failed.state==RYZ_PROVISIONING_FAILED && failed.last_error==ESP_FAIL);
    assert(failed.enabled==turn_on);
    bool enabled=!turn_on;
    assert(ryz_provisioning_platform_load_enabled(&enabled)==ESP_OK && enabled==turn_on);
    unsigned committed=commits;
    wifi_start_error=wifi_stop_error=ESP_OK;
    assert(ryz_provisioning_set_enabled(turn_on)==ESP_OK);
    assert(enabled_writes==1 && commits==committed && wifi_running==turn_on);
    expected_radio_preference=-1;
}

static void enabled_temporary_portals_preserve_off_preference(void)
{
    persisted.wifi_on_present=true; persisted.wifi_on=0;
    assert(ryz_provisioning_init()==ESP_OK);
    char ap_password[64]; strcpy(ap_password,persisted.ap_password);
    unsigned committed=commits;
    expected_radio_preference=0;
    assert(ryz_provisioning_open_portal()==ESP_OK);
    assert(snapshot().state==RYZ_PROVISIONING_AP_READY && snapshot().enabled);
    assert(wifi_running && !enabled_writes && persisted.wifi_on==0 && commits==committed);
    bool enabled=true;
    assert(ryz_provisioning_platform_load_enabled(&enabled)==ESP_OK && !enabled);
    assert(ryz_provisioning_set_enabled(false)==ESP_OK && !enabled_writes);
    assert(ryz_provisioning_request_reprovision()==ESP_OK);
    assert(snapshot().state==RYZ_PROVISIONING_AP_READY && snapshot().enabled);
    assert(wifi_running && !enabled_writes && persisted.wifi_on==0);
    assert(ryz_provisioning_platform_load_enabled(&enabled)==ESP_OK && !enabled);
    assert(!strcmp(ap_password,persisted.ap_password));
    /* Temporary effective ON must not suppress the first persistent ON. */
    expected_radio_preference=1;
    assert(ryz_provisioning_set_enabled(true)==ESP_OK);
    assert(enabled_writes==1 && persisted.wifi_on==1 && snapshot().enabled);
    expected_radio_preference=-1;
}

int main(int argc, char **argv)
{
    assert(argc==2);
    if (!strncmp(argv[1],"boot_scan_",10)) boot_scan_case((unsigned)atoi(argv[1]+10));
    else if (!strcmp(argv[1],"boot_fallback_ap")) boot_fallback_protected_ap_and_stale_events(false);
    else if (!strcmp(argv[1],"boot_fallback_start_failed")) boot_fallback_protected_ap_and_stale_events(true);
    else if (!strcmp(argv[1],"boot_fallback_live")) boot_fallback_keeps_live_station(false);
    else if (!strcmp(argv[1],"boot_fallback_online")) boot_fallback_keeps_live_station(true);
    else if (!strncmp(argv[1],"boot_fallback_superseded_",25)) boot_fallback_superseded((unsigned)atoi(argv[1]+25));
    else if (!strncmp(argv[1],"boot_fallback_failure_",22)) boot_fallback_failure((unsigned)atoi(argv[1]+22));
    else if (!strncmp(argv[1],"boot_fallback_unverified_",25)) boot_fallback_rejects_unverified_ip((unsigned)atoi(argv[1]+25));
    else if (!strcmp(argv[1],"boot_fallback_fast_post")) boot_fallback_fast_http_submission();
    else if (!strcmp(argv[1],"boot_initial_fast_post")) initial_ap_fast_submission_is_not_boot_restore();
    else if (!strcmp(argv[1],"boot_online_dropped")) boot_observed_online_then_dropped_is_not_boot_restore();
    else if (!strcmp(argv[1],"enabled_platform")) enabled_platform_roundtrip();
    else if (!strcmp(argv[1],"enabled_boot_off")) enabled_boot_off();
    else if (!strcmp(argv[1],"enabled_load_errors")) enabled_platform_load_errors();
    else if (!strcmp(argv[1],"enabled_save_errors")) enabled_platform_save_errors();
    else if (!strcmp(argv[1],"enabled_bad2")) enabled_boot_invalid(0);
    else if (!strcmp(argv[1],"enabled_bad255")) enabled_boot_invalid(1);
    else if (!strcmp(argv[1],"enabled_badtype")) enabled_boot_invalid(2);
    else if (!strcmp(argv[1],"enabled_get_failure")) enabled_boot_invalid(3);
    else if (!strcmp(argv[1],"enabled_open_failure")) enabled_boot_invalid(4);
    else if (!strcmp(argv[1],"enabled_on_open")) enabled_mutation_save_failure(true,0);
    else if (!strcmp(argv[1],"enabled_on_set")) enabled_mutation_save_failure(true,1);
    else if (!strcmp(argv[1],"enabled_on_commit")) enabled_mutation_save_failure(true,2);
    else if (!strcmp(argv[1],"enabled_off_open")) enabled_mutation_save_failure(false,0);
    else if (!strcmp(argv[1],"enabled_off_set")) enabled_mutation_save_failure(false,1);
    else if (!strcmp(argv[1],"enabled_off_commit")) enabled_mutation_save_failure(false,2);
    else if (!strcmp(argv[1],"enabled_first_on")) enabled_first_explicit_on();
    else if (!strcmp(argv[1],"enabled_commit_unknown")) enabled_ambiguous_commit_does_not_trust_old_cache();
    else if (!strcmp(argv[1],"enabled_radio_on")) enabled_radio_failure_retains_preference(true);
    else if (!strcmp(argv[1],"enabled_radio_off")) enabled_radio_failure_retains_preference(false);
    else if (!strcmp(argv[1],"enabled_temporary")) enabled_temporary_portals_preserve_off_preference();
    else if (!strcmp(argv[1],"http_stop_failure")) http_stop_failure();
    else if (!strcmp(argv[1],"stop_credentials")) stop_preserves_credentials();
    else if (!strcmp(argv[1],"http_try_lock")) http_handler_during_stop();
    else if (!strcmp(argv[1],"stale_events")) stale_events_after_restart();
    else if (!strcmp(argv[1],"stop_http")) failed_stop_is_not_off(0);
    else if (!strcmp(argv[1],"stop_dns")) failed_stop_is_not_off(1);
    else if (!strcmp(argv[1],"stop_wifi")) failed_stop_is_not_off(2);
    else if (!strcmp(argv[1],"retry_late_ip")) unverified_ip_at_retry_deadline();
    else if (!strcmp(argv[1],"retry_ignored_batch")) ignored_coalesced_event_preserves_retry();
    else if (!strcmp(argv[1],"link_periodic")) periodic_link_and_copy_only_snapshot();
    else if (!strcmp(argv[1],"traffic_periodic")) traffic_periodic_and_copy_only();
    else if (!strcmp(argv[1],"traffic_ap")) traffic_ap_and_transition_exclusion();
    else if (!strcmp(argv[1],"traffic_restart")) traffic_disconnect_and_failed_stop();
    else if (!strcmp(argv[1],"traffic_raw")) traffic_roam_and_raw_disconnect();
    else if (!strcmp(argv[1],"link_failures")) link_failures_clear_without_changing_connection();
    else if (!strcmp(argv[1],"link_restart")) link_binding_revoked_by_stop_restart_and_disconnect();
    else if (!strcmp(argv[1],"secret_api")) secret_api_is_bounded_and_private();
    else if (!strcmp(argv[1],"secret_epochs")) secret_epochs_and_freshness();
    else if (!strcmp(argv[1],"secret_open")) secret_open_and_max_password();
    else if (!strcmp(argv[1],"secret_event")) secret_raw_disconnect_ends_hold_before_worker();
    else if (!strcmp(argv[1],"open_preserves")) open_portal_preserves_saved_credentials();
    else if (!strcmp(argv[1],"open_stale_events")) open_portal_revokes_station_events_and_retry();
    else if (!strcmp(argv[1],"open_queued")) open_portal_revokes_submitted_connection(false);
    else if (!strcmp(argv[1],"open_delayed")) open_portal_revokes_submitted_connection(true);
    else if (!strcmp(argv[1],"open_stop_http")) open_portal_stop_failure_preserves_retryable_resources(0);
    else if (!strcmp(argv[1],"open_stop_dns")) open_portal_stop_failure_preserves_retryable_resources(1);
    else if (!strcmp(argv[1],"open_stop_wifi")) open_portal_stop_failure_preserves_retryable_resources(2);
    else if (!strcmp(argv[1],"open_start_wifi")) open_portal_start_failure_is_not_ready(0);
    else if (!strcmp(argv[1],"open_start_http")) open_portal_start_failure_is_not_ready(1);
    else if (!strcmp(argv[1],"open_start_dns")) open_portal_start_failure_is_not_ready(2);
    else if (!strcmp(argv[1],"open_fast_post")) open_portal_fast_http_submission_wins_over_ready();
    else { assert(!strcmp(argv[1],"persistence")); persistence_failure(); }
    if (worker.live) assert(ryz_provisioning_set_enabled(false)==ESP_OK);
    assert(!wifi_running);
    for (unsigned i=0;i<server_count;++i) assert(!servers[i].live);
    for (unsigned i=0;i<dns_count;++i) assert(!dns_servers[i].live);
    if (worker.live) vTaskDelete(&worker);
    for (unsigned i=0;i<8;++i) assert(!nvs_handles[i].live);
    printf("PROVISIONING_ESP_PASS %s\n",argv[1]); return 0;
}
