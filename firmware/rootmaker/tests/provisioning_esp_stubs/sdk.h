#pragma once

/* Minimal SDK transport/OS values for the real provisioning ESP Adapter.
 * No socket, radio, event-loop implementation or production worker is copied. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <pthread.h>
#include <arpa/inet.h>

#ifndef CONFIG_LWIP_IPV6
#define CONFIG_LWIP_IPV6 1
#endif

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_NOT_FINISHED 0x10c
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c
#define ESP_ERR_WIFI_NOT_STARTED 0x3002
#define ESP_ERR_WIFI_NOT_CONNECT 0x300f
#define ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED 0x5003
#define ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED 0x5004
const char *esp_err_to_name(esp_err_t error);
void prov_test_log(const char *tag, const char *format, ...);
#define ESP_LOGI(tag, ...) prov_test_log(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) prov_test_log(tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) prov_test_log(tag, __VA_ARGS__)

typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
typedef void (*TaskFunction_t)(void *);
typedef struct fake_task *TaskHandle_t;
typedef struct fake_queue *QueueHandle_t;
typedef struct fake_mutex *SemaphoreHandle_t;
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(p) assert(pthread_mutex_lock(p) == 0)
#define portEXIT_CRITICAL(p) assert(pthread_mutex_unlock(p) == 0)
#define pdPASS 1
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex);
void vSemaphoreDelete(SemaphoreHandle_t mutex);
QueueHandle_t xQueueCreate(UBaseType_t count, UBaseType_t size);
BaseType_t xQueueOverwrite(QueueHandle_t queue, const void *item);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks);
void vQueueDelete(QueueHandle_t queue);
BaseType_t xTaskCreate(TaskFunction_t entry, const char *name, uint32_t stack,
                      void *argument, UBaseType_t priority, TaskHandle_t *out);
void vTaskDelete(TaskHandle_t task);
TickType_t xTaskGetTickCount(void);
void xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks);

typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { uint32_t addr[4]; uint8_t zone; } esp_ip6_addr_t;
typedef struct { esp_ip4_addr_t ip, netmask, gw; } esp_netif_ip_info_t;
typedef struct fake_netif esp_netif_t;
typedef struct { bool ap; } esp_netif_config_t;
#define ESP_NETIF_DEFAULT_WIFI_AP() ((esp_netif_config_t){.ap=true})
#define ESP_NETIF_DEFAULT_WIFI_STA() ((esp_netif_config_t){.ap=false})
#define ESP_NETIF_OP_SET 1
#define ESP_NETIF_CAPTIVEPORTAL_URI 114
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(ip) (unsigned)(((ip)->addr) & 255U), (unsigned)(((ip)->addr >> 8) & 255U), \
    (unsigned)(((ip)->addr >> 16) & 255U), (unsigned)(((ip)->addr >> 24) & 255U)
char *inet_ntoa_r(uint32_t address, char *buffer, int length);
char *inet6_ntoa_r(esp_ip6_addr_t address, char *buffer, int length);
int64_t esp_timer_get_time(void);
esp_err_t esp_netif_init(void);
esp_netif_t *esp_netif_new(const esp_netif_config_t *config);
void esp_netif_destroy(esp_netif_t *netif);
void esp_netif_destroy_default_wifi(esp_netif_t *netif);
esp_err_t esp_netif_attach_wifi_ap(esp_netif_t *netif);
esp_err_t esp_netif_attach_wifi_station(esp_netif_t *netif);
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *out);
bool esp_netif_is_netif_up(esp_netif_t *netif);
esp_err_t esp_netif_get_ip6_global(esp_netif_t *netif, esp_ip6_addr_t *out);
esp_err_t esp_netif_get_ip6_linklocal(esp_netif_t *netif, esp_ip6_addr_t *out);
esp_err_t esp_netif_dhcps_stop(esp_netif_t *netif);
esp_err_t esp_netif_dhcps_start(esp_netif_t *netif);
esp_err_t esp_netif_dhcps_option(esp_netif_t *netif, int op, int option,
                               void *value, uint32_t length);

typedef const char *esp_event_base_t;
extern const char PROV_WIFI_EVENT[], PROV_IP_EVENT[];
#define WIFI_EVENT PROV_WIFI_EVENT
#define IP_EVENT PROV_IP_EVENT
#define ESP_EVENT_ANY_ID (-1)
#define WIFI_EVENT_STA_DISCONNECTED 5
#define IP_EVENT_STA_GOT_IP 0
typedef void (*esp_event_handler_t)(void *, esp_event_base_t, int32_t, void *);
typedef struct fake_handler *esp_event_handler_instance_t;
typedef struct { esp_netif_t *esp_netif; esp_netif_ip_info_t ip_info; bool ip_changed; } ip_event_got_ip_t;
typedef struct { uint8_t ssid[32]; uint8_t ssid_len; uint8_t bssid[6]; uint8_t reason; int8_t rssi; } wifi_event_sta_disconnected_t;
esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_loop_delete_default(void);
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
    esp_event_handler_t callback, void *context, esp_event_handler_instance_t *out);
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t base, int32_t id,
    esp_event_handler_instance_t instance);

typedef enum { WIFI_MODE_NULL=0, WIFI_MODE_STA, WIFI_MODE_AP } wifi_mode_t;
enum { WIFI_IF_STA=0, WIFI_IF_AP=1, WIFI_STORAGE_RAM=1, WIFI_AUTH_OPEN=0, WIFI_AUTH_WPA2_PSK=3 };
typedef struct { bool capable, required; } wifi_pmf_config_t;
typedef union {
    struct { uint8_t ssid[32], password[64]; struct { int authmode; } threshold; wifi_pmf_config_t pmf_cfg; } sta;
    struct { uint8_t ssid[32], password[64], ssid_len, channel, max_connection; int authmode; wifi_pmf_config_t pmf_cfg; } ap;
} wifi_config_t;
typedef struct { int ignored; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
typedef struct { uint8_t bssid[6], ssid[33], primary; int8_t rssi; } wifi_ap_record_t;
enum { WIFI_SCAN_TYPE_ACTIVE = 0 };
typedef struct {
    uint8_t *ssid;
    bool show_hidden;
    int scan_type;
    struct { struct { uint32_t min, max; } active; } scan_time;
    struct { uint16_t ghz_2_channels; uint32_t ghz_5_channels; } channel_bitmap;
} wifi_scan_config_t;
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool block);
esp_err_t esp_wifi_scan_stop(void);
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *count, wifi_ap_record_t *records);
esp_err_t esp_wifi_clear_ap_list(void);
enum { WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT=15, WIFI_REASON_IE_IN_4WAY_DIFFERS=17,
    WIFI_REASON_GROUP_CIPHER_INVALID, WIFI_REASON_PAIRWISE_CIPHER_INVALID, WIFI_REASON_AKMP_INVALID,
    WIFI_REASON_UNSUPP_RSN_IE_VERSION, WIFI_REASON_INVALID_RSN_IE_CAP, WIFI_REASON_802_1X_AUTH_FAILED,
    WIFI_REASON_CIPHER_SUITE_REJECTED, WIFI_REASON_BAD_CIPHER_OR_AKM,
    WIFI_REASON_AUTH_FAIL=202, WIFI_REASON_HANDSHAKE_TIMEOUT=204,
    WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY=210, WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD=211 };
esp_err_t esp_wifi_init(const wifi_init_config_t *config);
esp_err_t esp_wifi_deinit(void);
esp_err_t esp_wifi_set_storage(int storage);
esp_err_t esp_wifi_set_mode(wifi_mode_t mode);
esp_err_t esp_wifi_set_config(int interface, const wifi_config_t *config);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *out);
esp_err_t esp_wifi_get_mac(int interface, uint8_t mac[6]);
esp_err_t esp_wifi_set_default_wifi_ap_handlers(void);
esp_err_t esp_wifi_set_default_wifi_sta_handlers(void);
esp_err_t esp_wifi_clear_default_wifi_driver_and_handlers(esp_netif_t *netif);
enum { ESP_MAC_WIFI_STA=0 };
esp_err_t esp_read_mac(uint8_t *out, int type);
void esp_fill_random(void *out, size_t length);

typedef struct fake_nvs *nvs_handle_t;
enum { NVS_READONLY=0, NVS_READWRITE=1 };
esp_err_t nvs_flash_init(void);
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *out);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *length);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t length);
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out, size_t *length);
esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *data);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key);
esp_err_t nvs_commit(nvs_handle_t handle);
void nvs_close(nvs_handle_t handle);

typedef struct fake_http *httpd_handle_t;
typedef struct { int content_len; const char *body; size_t consumed; char status[64], response[8192]; } httpd_req_t;
typedef int httpd_err_code_t;
typedef struct { unsigned max_open_sockets, max_uri_handlers; bool lru_purge_enable; } httpd_config_t;
#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){0})
enum { HTTP_GET=0, HTTP_POST=1, HTTPD_404_NOT_FOUND=404, HTTPD_500_INTERNAL_SERVER_ERROR=500 };
#define HTTPD_RESP_USE_STRLEN (-1)
typedef struct { const char *uri; int method; esp_err_t (*handler)(httpd_req_t *); } httpd_uri_t;
esp_err_t httpd_start(httpd_handle_t *out, const httpd_config_t *config);
esp_err_t httpd_stop(httpd_handle_t handle);
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri);
esp_err_t httpd_register_err_handler(httpd_handle_t handle, httpd_err_code_t code,
                                    esp_err_t (*callback)(httpd_req_t *, httpd_err_code_t));
int httpd_req_recv(httpd_req_t *request, char *out, size_t length);
esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status);
esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type);
esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *key, const char *value);
esp_err_t httpd_resp_send(httpd_req_t *request, const char *data, ssize_t length);
esp_err_t httpd_resp_send_err(httpd_req_t *request, httpd_err_code_t code, const char *message);
