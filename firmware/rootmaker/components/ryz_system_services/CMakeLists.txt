idf_component_register(
    SRCS "ryz_system_services.c" "ryz_system_services_esp.c"
         "ryz_system_metrics.c" "ryz_system_metrics_esp.c"
    INCLUDE_DIRS "include"
    REQUIRES ryz_provisioning ryz_time
    PRIV_REQUIRES ryz_ota esp_timer freertos log esp_driver_tsens esp_system)
