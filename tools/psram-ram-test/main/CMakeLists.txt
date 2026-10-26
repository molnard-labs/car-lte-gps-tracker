idf_component_register(SRCS "psram_ram_test.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_psram freertos esp_system esp_rom soc)
