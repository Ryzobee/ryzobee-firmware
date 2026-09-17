#pragma once
#if defined(RYZ_BLE_HOST_TEST)
#include "ble_test_sdk.h"
#else
#include "sdkconfig.h"
#include "esp_timer.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/ble_hs_pvcy.h"
#include "ble_hs_pvcy_priv.h"
#include "nimble/nimble_port.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#if ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(5, 5, 4)
#error "Ryzobee BLE private privacy adapter requires an ESP-IDF 5.5.4 audit"
#endif
#if CONFIG_BT_NIMBLE_STATIC_TO_DYNAMIC || CONFIG_BT_NIMBLE_NVS_PERSIST || CONFIG_BT_NIMBLE_SM_LEGACY || CONFIG_BT_NIMBLE_SMP_ID_RESET
#error "Ryzobee BLE requires its own durable store, no callback replacement/legacy pairing/automatic IRK reset"
#endif
#if CONFIG_BT_NIMBLE_MAX_CONNECTIONS != 1 || CONFIG_BT_NIMBLE_MAX_BONDS != 1
#error "Ryzobee BLE owner requires exactly one connection and one bond"
#endif
#if !CONFIG_BT_NIMBLE_GATT_CLIENT
#error "ANCS discovery requires the NimBLE GATT client"
#endif
#endif
