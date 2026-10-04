set(srcs "gdey042z98.c"
    "spi_master_example_main.c"
    )

idf_component_register(SRCS ${srcs} "calendar_ui.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES driver esp_wifi esp_event esp_netif nvs_flash esp_adc)
