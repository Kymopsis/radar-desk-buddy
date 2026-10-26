idf_component_register(
    SRCS "softap_portal.c"
    INCLUDE_DIRS "include" "."
    REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_server servo radar_link motion ui device_cfg display dns_server mqtt_link net_time telegram_link
)
