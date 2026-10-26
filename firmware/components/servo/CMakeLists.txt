idf_component_register(
    SRCS "servo.c"
    INCLUDE_DIRS "include"
    REQUIRES board driver esp_driver_ledc esp_driver_gpio freertos
)
