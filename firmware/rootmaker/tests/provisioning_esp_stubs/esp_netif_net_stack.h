#pragma once
#include "sdk.h"

esp_err_t esp_netif_transmit(esp_netif_t *, void *, size_t);
esp_err_t esp_netif_transmit_wrap(esp_netif_t *, void *, size_t, void *);
