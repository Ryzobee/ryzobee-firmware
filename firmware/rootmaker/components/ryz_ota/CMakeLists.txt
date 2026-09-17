idf_component_register(
    SRCS "ryz_ota.c" "ryz_ota_esp.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES esp_common
    PRIV_REQUIRES app_update esp_app_format esp_http_client esp_https_ota
                  esp_partition esp_system freertos mbedtls)
