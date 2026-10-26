idf_component_register(SRCS "main.c" "tracker_policy.c"
    INCLUDE_DIRS "."
    REQUIRES tracker_core tracker_store tracker_net tracker_setup
             esp_driver_gpio esp_hw_support esp_timer)
