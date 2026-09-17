idf_component_register(
    SRCS "ryz_monitor.c" "ryz_monitor_esp.c" "ryz_monitor_stream.c"
         "ryz_monitor_uart.c" "ryz_monitor_log.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES ryz_tool_pins
    PRIV_REQUIRES esp_driver_uart esp_driver_gpio esp_hw_support esp_timer freertos log ryz_log)
