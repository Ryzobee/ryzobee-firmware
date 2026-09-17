idf_component_register(
    SRCS "ryz_provisioning.c"
         "ryz_provisioning_esp.c"
         "ryz_provisioning_portal.c"
         "ryz_captive_dns.c"
         "ryz_net_traffic.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES esp_common
    PRIV_REQUIRES esp_event esp_http_server esp_netif esp_timer esp_wifi freertos json lwip nvs_flash ryz_diagnostics)

# Observe the audited ESP-NETIF boundaries without replacing Wi-Fi callbacks or
# changing the SDK. Final target relocation verification is required: Host
# tests exercise the wrappers directly and cannot prove GNU linker coverage.
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_netif_transmit"
    "-Wl,--wrap=esp_netif_transmit_wrap"
    "-Wl,--wrap=esp_netif_receive"
    "-Wl,--undefined=__wrap_esp_netif_transmit"
    "-Wl,--undefined=__wrap_esp_netif_transmit_wrap"
    "-Wl,--undefined=__wrap_esp_netif_receive")
