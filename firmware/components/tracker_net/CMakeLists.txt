idf_component_register(
    SRCS "tracker_net.c"
    INCLUDE_DIRS "include"
    REQUIRES tracker_core tracker_store tracker_output esp_wifi esp_netif esp_event esp_http_client
             esp_timer esp_driver_gpio esp_driver_uart lwip mbedtls esp_modem
)
