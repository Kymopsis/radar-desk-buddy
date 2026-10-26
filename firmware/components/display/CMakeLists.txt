idf_component_register(
    SRCS "display.c"
    INCLUDE_DIRS "include"
    REQUIRES board driver esp_lcd esp_lcd_gc9a01 esp_driver_ledc esp_driver_spi esp_driver_gpio
)
