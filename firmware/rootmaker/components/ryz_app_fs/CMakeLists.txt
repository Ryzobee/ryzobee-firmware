idf_component_register(
    SRCS "ryz_app_fs.c" "ryz_app_fs_esp.c"
    INCLUDE_DIRS "../ryz_runtime/include"
    PRIV_INCLUDE_DIRS "."
    PRIV_REQUIRES spiffs mbedtls heap ryz_script_store)
