#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef struct { int unused; } esp_netif_t;
typedef struct { struct { uint32_t addr; } ip; } esp_netif_ip_info_t;
esp_err_t esp_netif_get_ip_info(esp_netif_t *, esp_netif_ip_info_t *);
