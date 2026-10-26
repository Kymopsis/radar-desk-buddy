idf_component_register(
    SRCS "mqtt_link.c"
    INCLUDE_DIRS "include"
    REQUIRES device_cfg ui net_time esp_http_client esp-tls esp_netif esp_wifi
             esp_event nvs_flash servo motion esp_app_format radar_link esp_timer
             mbedtls
)
