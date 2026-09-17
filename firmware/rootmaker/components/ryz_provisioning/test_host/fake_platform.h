#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "ryz_provisioning.h"

void fake_platform_reset(void);
void fake_platform_set_saved_scan_result(esp_err_t error);
unsigned fake_platform_saved_scan_count(void);
void fake_platform_set_enabled_record(esp_err_t result, bool enabled);
void fake_platform_set_save_enabled_result(esp_err_t result, bool writes_on_failure);
void fake_platform_set_save_enabled_hook(void (*hook)(void));
unsigned int fake_platform_enabled_load_count(void);
unsigned int fake_platform_enabled_save_count(void);
bool fake_platform_saved_enabled(void);
void fake_platform_store_credentials(const char *ssid, const char *password);
void fake_platform_set_portal_ip(const char *ipv4);
void fake_platform_set_start_portal_result(esp_err_t result);
void fake_platform_set_load_result(esp_err_t result);
unsigned int fake_platform_load_count(void);
unsigned int fake_platform_clear_count(void);
unsigned int fake_platform_start_portal_count(void);
void fake_platform_set_start_sta_result(esp_err_t result);
void fake_platform_set_ap_identity_result(esp_err_t result);
void fake_platform_set_stop_result(esp_err_t result);
void fake_platform_set_clear_result(esp_err_t result);
void fake_platform_set_stop_hook(void (*hook)(void));
void fake_platform_set_portal_start_hook(void (*hook)(void));
unsigned int fake_platform_stop_count(void);
void fake_platform_submit_credentials(const char *ssid, const char *password);
void fake_platform_begin_connecting(void);
void fake_platform_got_ip(const char *ssid, const char *ipv4);
void fake_platform_disconnect(int reason);
void fake_platform_link_info(const char *ssid, const char *ipv4,
                              const ryz_provisioning_link_info_t *link);
bool fake_platform_credentials_exist(void);
bool fake_platform_portal_start_observed_clean_snapshot(void);
unsigned int fake_platform_init_count(void);
unsigned int fake_platform_deinit_count(void);
unsigned int fake_platform_start_sta_count(void);
void fake_platform_set_local_secret(esp_err_t result, void (*hook)(void));
unsigned int fake_platform_local_secret_calls(void);
