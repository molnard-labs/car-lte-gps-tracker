idf_component_register(SRCS "tracker_store.c" INCLUDE_DIRS "include"
    REQUIRES tracker_core nvs_flash esp_partition freertos)
