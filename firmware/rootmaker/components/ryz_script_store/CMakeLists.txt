idf_component_register(
    SRCS "ryz_script_store.c" "ryz_script_store_esp.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES esp_common
    PRIV_REQUIRES spiffs mbedtls heap ryz_time)
