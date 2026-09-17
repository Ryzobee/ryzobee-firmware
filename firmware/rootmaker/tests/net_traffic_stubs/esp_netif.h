#pragma once
#include <stddef.h>
#include "esp_err.h"
typedef struct esp_netif_obj esp_netif_t;
esp_err_t esp_netif_receive(esp_netif_t *, void *, size_t, void *);
