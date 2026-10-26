idf_component_register(
    SRCS "net_time.c" "net_http.c"
    INCLUDE_DIRS "include"
    REQUIRES device_cfg ui esp_http_client esp-tls esp_netif esp_event esp_wifi mbedtls
)
