idf_component_register(
    SRCS "telegram_link.c"
    INCLUDE_DIRS "include"
    REQUIRES device_cfg ui net_time motion esp_timer
)
