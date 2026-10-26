idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES board display ui driver radar_link servo softap_portal motion device_cfg mqtt_link net_time telegram_link
)
