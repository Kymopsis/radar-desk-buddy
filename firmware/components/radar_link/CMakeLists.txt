idf_component_register(
    SRCS "radar_link.c" "radar_protocol.c"
    INCLUDE_DIRS "include"
    REQUIRES board driver esp_driver_uart esp_driver_gpio esp_timer freertos
)
