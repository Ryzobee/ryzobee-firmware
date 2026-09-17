#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <sys/time.h>
#include "esp_err.h"
typedef void (*esp_sntp_time_cb_t)(struct timeval *value);
typedef struct {
    bool smooth_sync,server_from_dhcp,wait_for_sync,start;
    esp_sntp_time_cb_t sync_cb;
    bool renew_servers_after_new_IP;
    int ip_event_to_renew;
    size_t index_of_first_server,num_of_servers;
    const char *servers[1];
} esp_sntp_config_t;
/* Relevant defaults copied from the read-only ESP-IDF 5.5.4 public header. */
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(server) { \
    .smooth_sync=false,.wait_for_sync=true,.start=true, \
    .num_of_servers=1,.servers={server} }
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config);
esp_err_t esp_netif_sntp_start(void);
