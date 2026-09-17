idf_component_register(SRCS "ryz_ble.c" "ryz_ble_store.c" "ryz_ble_hid_profile.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${IDF_PATH}/components/bt/host/nimble/nimble/nimble/host/src"
    PRIV_REQUIRES bt nvs_flash freertos esp_timer heap log esp_app_format)

# Bounded first-start diagnostics; no SDK patching or regular-traffic profiling.
target_link_options(${COMPONENT_LIB} INTERFACE
    "LINKER:--wrap=ble_buf_alloc"
    "LINKER:--wrap=esp_vhci_host_register_callback")
