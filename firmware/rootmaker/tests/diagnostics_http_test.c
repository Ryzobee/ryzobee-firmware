/* Contract harness: diagnostics_adapter.inc is the exact production handler
 * block extracted during test setup, not a rewritten/mock implementation. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "cJSON.h"
#include "ryz_diagnostics.h"

typedef struct { const char *uri; int status; char body[65536]; } httpd_req_t;
typedef struct {struct {uint32_t addr;} ip,netmask;} esp_netif_ip_info_t;
static void *s_network_mutex=(void*)1,*s_ap_netif=(void*)2,*s_http_server=(void*)3;
static bool s_wifi_started=true,s_portal_session=true;
static char s_ap_password[]="sample-key12";
static bool lock_available=true,ip_available=true,socket_available=true;
static struct sockaddr_in local_address,peer_address;
static esp_netif_ip_info_t ap_address;
#define pdTRUE 1
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
#define HTTPD_RESP_USE_STRLEN -1
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
static int xSemaphoreTake(void *mutex,int wait){assert(mutex&&wait==0);return lock_available;}
static void xSemaphoreGive(void *mutex){assert(mutex);}
static int httpd_req_to_sockfd(httpd_req_t *r){assert(r);return socket_available?7:-1;}
static int test_getsockname(int fd,struct sockaddr *out,socklen_t *n){assert(fd==7&&*n==sizeof(local_address));memcpy(out,&local_address,*n);return 0;}
static int test_getpeername(int fd,struct sockaddr *out,socklen_t *n){assert(fd==7&&*n==sizeof(peer_address));memcpy(out,&peer_address,*n);return 0;}
#define getsockname test_getsockname
#define getpeername test_getpeername
static esp_err_t esp_netif_get_ip_info(void *netif,esp_netif_ip_info_t *out){assert(netif);*out=ap_address;return ip_available?ESP_OK:ESP_FAIL;}
static void set_common_headers(httpd_req_t *r){assert(r);}
static void httpd_resp_set_status(httpd_req_t *r,const char *status){assert(!strcmp(status,"410 Gone"));r->status=410;}
static void httpd_resp_set_type(httpd_req_t *r,const char *type){assert(r&&type);}
static esp_err_t httpd_resp_send(httpd_req_t *r,const char *body,int length){assert(length==-1&&strlen(body)<sizeof(r->body));strcpy(r->body,body);return ESP_OK;}
static esp_err_t httpd_resp_send_err(httpd_req_t *r,int status,const char *body){r->status=status;strcpy(r->body,body);return ESP_OK;}
static esp_err_t httpd_req_get_url_query_str(httpd_req_t *r,char *out,size_t capacity){const char *q=strchr(r->uri,'?');if(!q||strlen(++q)>=capacity)return ESP_FAIL;strcpy(out,q);return ESP_OK;}
static int64_t clock_us;
int64_t ryz_diagnostics_test_time(void){return clock_us;}
void ryz_diagnostics_test_random(void *out,size_t n){memset(out,0xab,n);}
const char ryz_diagnostics_html[]="<!doctype html>REPORT SHELL";
#include "diagnostics_adapter.inc"

static void reset_request(httpd_req_t *r,const char *uri){memset(r,0,sizeof(*r));r->uri=uri;r->status=200;}
static void *no_memory(size_t size){(void)size;return NULL;}
static void ignore_free(void *p){(void)p;}
int main(void){
    local_address.sin_family=peer_address.sin_family=AF_INET;
    ap_address.ip.addr=local_address.sin_addr.s_addr=inet_addr("192.168.4.1");
    ap_address.netmask.addr=inet_addr("255.255.255.0");peer_address.sin_addr.s_addr=inet_addr("192.168.4.2");
    ryz_diagnostics_input_t input={.phase="runtime",.code="LUA-R01",.summary="SAMPLE",.source_file="sample.lua",.job_id="sample17",.firmware_version="SAMPLE",.error="<script>\"test\"</script>\n中文",.output="captured\noutput",.line=-1,.duration_ms=-1,.peak_bytes=-1,.cleanup_ok=-1,.output_truncated=true};
    assert(ryz_diagnostics_publish(&input)==ESP_OK);char path[64],uri[100];assert(ryz_diagnostics_get_path(path,sizeof(path))==ESP_OK);snprintf(uri,sizeof(uri),"/api/diagnostics/%s",path+3);
    httpd_req_t r;reset_request(&r,uri);assert(diagnostics_json_get_handler(&r)==ESP_OK&&r.status==200);
    cJSON *json=cJSON_Parse(r.body);assert(json&&cJSON_GetArraySize(json)==19);assert(!strcmp(cJSON_GetObjectItemCaseSensitive(json,"error")->valuestring,input.error));assert(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(json,"line")));assert(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(json,"trace")));assert(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(json,"cleanup_ok")));assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json,"output_truncated")));assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(json,"text_sanitized")));cJSON_Delete(json);
    assert(!strstr(r.body,"abababab"));
    const char *invalid[]={"/api/diagnostics/latest","/api/diagnostics/00000001", "/api/diagnostics/00000001?cap=bad", "/api/diagnostics/00000001?cap=abababababababababababababababab&cap=abababababababababababababababab", "/api/diagnostics/00000001x?cap=abababababababababababababababab", "/api/diagnostics/00000001?cap=00000000000000000000000000000000"};
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++){reset_request(&r,invalid[i]);assert(diagnostics_json_get_handler(&r)==ESP_OK&&r.status==410);assert(!strstr(r.body,"captured"));}
    local_address.sin_addr.s_addr=inet_addr("192.168.1.64");reset_request(&r,uri);assert(diagnostics_json_get_handler(&r)==ESP_OK&&r.status==410);local_address.sin_addr.s_addr=ap_address.ip.addr;
    peer_address.sin_addr.s_addr=inet_addr("192.168.1.2");reset_request(&r,uri);assert(diagnostics_json_get_handler(&r)==ESP_OK&&r.status==410);peer_address.sin_addr.s_addr=inet_addr("192.168.4.2");
    bool *flags[]={&s_wifi_started,&s_portal_session,&lock_available,&ip_available,&socket_available};for(size_t i=0;i<sizeof(flags)/sizeof(flags[0]);i++){*flags[i]=false;reset_request(&r,uri);assert(diagnostics_json_get_handler(&r)==ESP_OK&&r.status==410);*flags[i]=true;}
    s_http_server=NULL;reset_request(&r,uri);assert(diagnostics_json_get_handler(&r)==ESP_OK&&r.status==410);s_http_server=(void*)3;
    s_ap_password[0]=0;reset_request(&r,uri);assert(diagnostics_json_get_handler(&r)==ESP_OK&&r.status==410);strcpy(s_ap_password,"sample-key12");
    reset_request(&r,path);assert(diagnostics_page_get_handler(&r)==ESP_OK&&r.status==200);assert(!strcmp(r.body,ryz_diagnostics_html));
    cJSON_Hooks hooks={.malloc_fn=no_memory,.free_fn=ignore_free};cJSON_InitHooks(&hooks);reset_request(&r,uri);assert(diagnostics_json_get_handler(&r)==ESP_OK&&r.status==500);cJSON_InitHooks(NULL);
    clock_us=600000000;reset_request(&r,uri);assert(diagnostics_json_get_handler(&r)==ESP_OK&&r.status==410);
    puts("DIAGNOSTICS_HTTP_PASS protected_ap destination peer capability exact_query immutable_id null escaped readonly ttl oom");return 0;
}
