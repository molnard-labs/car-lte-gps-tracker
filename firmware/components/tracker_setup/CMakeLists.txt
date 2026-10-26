idf_component_register(
    SRCS "tracker_setup.c" "setup_policy.c" "setup_listener.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES tracker_core tracker_store tracker_net tracker_output esp_http_server esp_timer esp_wifi esp_netif esp_hw_support lwip json mbedtls
    EMBED_TXTFILES "setup.html"
)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=lwip_bind")
